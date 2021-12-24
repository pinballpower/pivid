// Simple command line tool to exercise video decoding and playback.

#include <CLI/App.hpp>
#include <CLI/Config.hpp>
#include <CLI/Formatter.hpp>
#include <fmt/core.h>

#include <cmath>
#include <thread>

#include "display_output.h"
#include "media_decoder.h"

// Main program, parses flags and calls the decoder loop.
int main(int const argc, char const* const* const argv) {
    std::string media_arg;
    std::string dev_arg = "gpu";

    CLI::App app("Decode and show a media file");
    app.add_option("--media", media_arg, "Media file or URL");
    app.add_option("--dev", dev_arg, "DRM driver /dev file or hardware path");
    CLI11_PARSE(app, argc, argv);

    try {
        fmt::print("=== Video drivers ===\n");
        std::vector<std::filesystem::path> dev_files;
        for (auto const& d : pivid::list_display_drivers()) {
            fmt::print(
                "{} ({}): {}", d.dev_file.native(), d.driver, d.system_path
            );
            if (!d.driver_bus_id.empty()) fmt::print(" ({})", d.driver_bus_id);
            if (d.dev_file.native().find(dev_arg) != std::string::npos ||
                d.system_path.find(dev_arg) != std::string::npos ||
                d.driver.find(dev_arg) != std::string::npos ||
                d.driver_bus_id.find(dev_arg) != std::string::npos) {
                dev_files.push_back(d.dev_file);
                fmt::print(" [SELECTED]");
            }
            fmt::print("\n");
        }
        if (dev_files.size() != 1) {
            throw std::runtime_error(fmt::format(
                "{} driver matches for --dev=\"{}\"\n",
                dev_files.size(), dev_arg
            ));
        }
        fmt::print("\n");

        fmt::print("=== Display outputs ===\n");
        auto const driver = pivid::open_display_driver(dev_files[0]);
        for (auto const& output : driver->scan_outputs()) {
            fmt::print(
                "#{:<3} {}{}\n", output.connector_id, output.name,
                output.connected.value_or(false) ? " [connected]" : ""
            );

            std::string active;
            if (output.active_mode) {
                active = output.active_mode->format();
                fmt::print("  {} active\n", active);
            }
            for (auto const& mode : output.modes) {
                auto const line = mode.format();
                if (line != active) fmt::print("  {}\n", line);
            }
            fmt::print("\n");
        }

        if (media_arg.empty()) {
            fmt::print("*** No --media file specified\n");
            exit(1);
        }

        auto const decoder = pivid::new_media_decoder(media_arg);
        while (!decoder->at_eof()) {
            auto const frame = decoder->next_frame();
            if (frame) {
                fmt::print("FRAME\n");
            } else {
                using namespace std::chrono_literals;
                std::this_thread::sleep_for(0.01s);
            }
        }
    } catch (std::exception const& e) {
        fmt::print("*** {}\n", e.what());
    }

    return 0;
}
