// HTTP server for video control.

#include <pthread.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <system_error>

#include <CLI/App.hpp>
#include <CLI/Config.hpp>
#include <CLI/Formatter.hpp>
#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include <httplib/httplib.h>

#include "display_output.h"
#include "logging_policy.h"
#include "script_data.h"
#include "script_runner.h"
#include "unix_system.h"

namespace pivid {

namespace {

std::shared_ptr<log::logger> const& server_logger() {
    static const auto logger = make_logger("server");
    return logger;
}

struct ServerContext {
    std::shared_ptr<UnixSystem> sys;
    std::shared_ptr<DisplayDriver> driver;
    std::unique_ptr<ScriptRunner> runner;
    double default_zero_time = 0.0;
    bool trust_network = false;
    int port = 31415;
};

class Server {
  public:
    ~Server() {
        std::unique_lock lock{mutex};
        if (thread.joinable()) {
            DEBUG(logger, "Stopping main loop thread");
            shutdown = true;
            wakeup_mono->set();
            thread.join();
        }
    }

    void run(ServerContext&& context) {
        using namespace std::placeholders;
        cx = std::move(context);

        http.Get("/media(/.*)", [&](auto const& q, auto& s) {on_media(q, s);});
        http.Get("/screens", [&](auto const& q, auto& s) {on_screens(q, s);});
        http.Post("/quit", [&](auto const& q, auto& s) {on_quit(q, s);});
        http.Post("/play", [&](auto const& q, auto& s) {on_play(q, s);});

        http.set_logger([&](auto const& q, auto const& s) {log_hook(q, s);});
        http.set_exception_handler(
            [&](auto const& q, auto& s, auto& e) {error_hook(q, s, e);}
        );

        DEBUG(logger, "Launching main loop thread");
        wakeup_mono = cx.sys->make_flag(CLOCK_MONOTONIC);
        thread = std::thread(&Server::main_loop_thread, this);
        if (cx.trust_network) {
            logger->info("Listening to WHOLE NETWORK on port {}", cx.port);
            http.listen("0.0.0.0", cx.port);
        } else {
            logger->info("Listening to localhost on port {}", cx.port);
            http.listen("localhost", cx.port);
        }
        logger->info("Stopped listening");
    }

    void main_loop_thread() {
        pthread_setname_np(pthread_self(), "pivid:mainloop");
        TRACE(logger, "Starting main loop thread");

        double last_mono = 0.0;
        std::unique_lock lock{mutex};
        while (!shutdown) {
            if (!script) {
                TRACE(logger, "UPDATE (wait for script)");
                lock.unlock();
                wakeup_mono->sleep();
                lock.lock();
                continue;
            }

            ASSERT(script->main_loop_hz > 0.0);
            double const period = 1.0 / script->main_loop_hz;
            double const mono = cx.sys->clock(CLOCK_MONOTONIC);
            if (mono < last_mono + period) {
                TRACE(
                    logger, "UPDATE (sleep {:.3f}s)",
                    last_mono + period - mono
                );
                lock.unlock();
                wakeup_mono->sleep_until(last_mono + period);
                lock.lock();
                continue;
            }

            DEBUG(logger, "UPDATE (mono={:.3f}s)", mono);
            last_mono = std::max(last_mono + period, mono - period);
            auto const copy = script;
            lock.unlock();
            cx.runner->update(*copy);
            lock.lock();
        }

        TRACE(logger, "Main loop thread stopped");
    }

  private:
    // Constant during run
    std::shared_ptr<log::logger> const logger = server_logger();
    ServerContext cx;
    httplib::Server http;
    std::thread thread;
    std::shared_ptr<SyncFlag> wakeup_mono;

    // Guarded by mutex
    std::mutex mutable mutex;
    bool shutdown = false;
    std::shared_ptr<Script const> script;

    void on_media(httplib::Request const& req, httplib::Response& res) {
        nlohmann::json j = {{"req", req.path}};

        try {
            DEBUG(logger, "INFO \"{}\"", std::string(req.matches[1]));
            auto const info = cx.runner->file_info(req.matches[1]);
            nlohmann::json media_j;
            if (!info.filename.empty())
                media_j["filename"] = info.filename;
            if (!info.container_type.empty())
                media_j["container_type"] = info.container_type;
            if (!info.pixel_format.empty())
                media_j["pixel_format"] = info.pixel_format;
            if (!info.codec_name.empty())
                media_j["codec_name"] = info.codec_name;
            if (info.size)
                media_j["size"] = {info.size->x, info.size->y};
            if (info.frame_rate)
                media_j["frame_rate"] = *info.frame_rate;
            if (info.bit_rate)
                media_j["bit_rate"] = *info.bit_rate;
            if (info.duration)
                media_j["duration"] = *info.duration;
            j["media"] = media_j;
            j["ok"] = true;
        } catch (std::system_error const& e) {
            if (e.code() == std::errc::no_such_file_or_directory) {
                res.status = 404;
                j["error"] = e.what();
            } else {
                throw;
            }
        }

        res.set_content(j.dump(), "application/json");
    }

    void on_play(httplib::Request const& req, httplib::Response& res) {
        auto new_script = std::make_shared<Script>(
            parse_script(req.body, cx.default_zero_time)
        );

        int layer_count = 0;
        for (auto const& [name, screen] : new_script->screens)
            layer_count += screen.layers.size();

        DEBUG(
            logger, "PLAY scr={} lay={} med={} t0={}",
            new_script->screens.size(), layer_count, new_script->media.size(),
            format_realtime(new_script->zero_time)
        );

        TRACE(logger, "  Script: {}", req.body);

        std::unique_lock lock{mutex};
        script = std::move(new_script);
        wakeup_mono->set();

        nlohmann::json const j = {{"req", req.path}, {"ok", true}};
        res.set_content(j.dump(), "application/json");
    }

    void on_screens(httplib::Request const& req, httplib::Response& res) {
        nlohmann::json j = {{"req", req.path}, {"ok", true}};
        auto* screens_j = &j["screens"];
        for (auto const& screen : cx.driver->scan_screens()) {
            nlohmann::json screen_j;
            screen_j["detected"] = screen.display_detected;

            auto const& am = screen.active_mode;
            if (am.nominal_hz)
                screen_j["active_mode"] = {am.size.x, am.size.y, am.nominal_hz};

            auto* modes_j = &screen_j["modes"];
            std::set<std::tuple<int, int, int>> added;
            for (auto const& m : screen.modes) {
                auto [it, f] = added.emplace(m.size.x, m.size.y, m.nominal_hz);
                if (f) modes_j->push_back(*it);
            }

            (*screens_j)[screen.connector] = screen_j;
        }

        res.set_content(j.dump(), "application/json");
    }

    void on_quit(httplib::Request const& req, httplib::Response& res) {
        std::unique_lock lock{mutex};
        DEBUG(logger, "STOP");
        http.stop();
        shutdown = true;
        wakeup_mono->set();

        nlohmann::json const j = {{"req", req.path}, {"ok", true}};
        res.set_content(j.dump(), "application/json");
    }

    void log_hook(httplib::Request const& req, httplib::Response const& res) {
        logger->info(
            "[{}] {} {} {}",
            res.status, req.remote_addr, req.method, req.path
        );
    }

    void error_hook(
        httplib::Request const& req, httplib::Response& res, std::exception& e
    ) {
        res.status = dynamic_cast<std::invalid_argument*>(&e) ? 400 : 500;
        nlohmann::json j = {{"req", req.path}, {"error", e.what()}};
        res.set_content(j.dump(), "application/json");
    }
};

}  // anonymous namespace

extern "C" int main(int const argc, char const* const* const argv) {
    std::string dev_arg;
    std::string log_arg;
    std::string media_root_arg;

    ScriptContext script_cx;
    ServerContext server_cx;

    CLI::App app("Serve HTTP REST API for video playback");
    app.add_option("--dev", dev_arg, "DRM driver /dev file or hardware path");
    app.add_option("--log", log_arg, "Log level/configuration");
    app.add_option("--port", server_cx.port, "TCP port to listen on");
    app.add_option(
        "--media_root", script_cx.root_dir, "Media directory"
    )->required();
    app.add_flag(
        "--trust_network", server_cx.trust_network,
        "Allow non-localhost connections"
    );
    CLI11_PARSE(app, argc, argv);
    configure_logging(log_arg);
    auto const logger = server_logger();

    try {
        server_cx.sys = global_system();
        for (auto const& dev : list_display_drivers(server_cx.sys)) {
            auto const text = debug(dev);
            if (text.find(dev_arg) == std::string::npos) continue;
            server_cx.driver = open_display_driver(server_cx.sys, dev.dev_file);
            break;
        }
        CHECK_RUNTIME(server_cx.driver, "No DRM device for \"{}\"", dev_arg);

        script_cx.sys = server_cx.sys;
        script_cx.driver = server_cx.driver;
        script_cx.file_base = script_cx.root_dir;
        server_cx.default_zero_time = server_cx.sys->clock();

        logger->info("Media root: {}", script_cx.root_dir);
        logger->info("Start: {}", format_realtime(server_cx.default_zero_time));
        server_cx.runner = make_script_runner(std::move(script_cx));

        Server server;
        server.run(std::move(server_cx));
    } catch (std::exception const& e) {
        logger->critical("{}", e.what());
        return 1;
    }

    return 0;
}

}  // namespace pivid
