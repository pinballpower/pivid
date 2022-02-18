#include "frame_player.h"

#include <condition_variable>
#include <mutex>
#include <thread>

#include <fmt/chrono.h>
#include <fmt/core.h>

#include "logging_policy.h"

namespace pivid {

namespace {

std::shared_ptr<log::logger> const& player_logger() {
    static const auto logger = make_logger("player");
    return logger;
}

class ThreadFramePlayer : public FramePlayer {
  public:
    virtual ~ThreadFramePlayer() {
        std::unique_lock lock{mutex};
        if (thread.joinable()) {
            logger->debug("Stopping frame player...");
            shutdown = true;
            lock.unlock();
            wakeup.notify_all();
            thread.join();
        }
    }

    virtual void set_timeline(Timeline timeline) {
        std::unique_lock lock{mutex};

        // Avoid thread wakeup if the wakeup schedule doesn't change.
        bool const same_keys = 
            timeline.size() == this->timeline.size() && std::equal(
                timeline.begin(), timeline.end(), this->timeline.begin(),
                [] (auto const& a, auto const& b) { return a.first == b.first; }
            );

        if (logger->should_log(log_level::trace)) {
            if (timeline.empty()) {
                logger->trace("Set timeline empty");
            } else {
                using namespace std::chrono_literals;
                logger->trace(
                    "Set timeline {}f {:.3f}s~{:.3f}s {}",
                    timeline.size(),
                    timeline.begin()->first.time_since_epoch() / 1.0s,
                    timeline.rbegin()->first.time_since_epoch() / 1.0s,
                    same_keys ? "[same]" : "[diff]"
                );
            }
        }

        this->timeline = std::move(timeline);
        if (!this->timeline.empty() && !same_keys) {
            lock.unlock();
            wakeup.notify_all();
        }
    }

    virtual Timeline::key_type last_shown() const {
        std::lock_guard const lock{mutex};
        return shown;
    }

    void start(
        std::shared_ptr<UnixSystem> sys,
        DisplayDriver* driver,
        uint32_t connector_id,
        DisplayMode mode
    ) {
        logger->info("Launching frame player...");
        thread = std::thread(
            &ThreadFramePlayer::player_thread,
            this,
            std::move(sys),
            driver,
            connector_id,
            std::move(mode)
        );
    }

    void player_thread(
        std::shared_ptr<UnixSystem> sys,
        DisplayDriver* driver,
        uint32_t connector_id,
        DisplayMode mode
    ) {
        using namespace std::chrono_literals;
        logger->debug("Frame player thread running...");
        std::unique_lock lock{mutex};
        while (!shutdown) {
            if (timeline.empty()) {
                logger->trace("PLAY (no frames, waiting for wakeup)");
                wakeup.wait(lock);
                continue;
            }

            if (logger->should_log(log_level::trace)) {
                logger->trace(
                    "PLAY timeline {}f {:.3f}s~{:.3f}s",
                    timeline.size(),
                    timeline.begin()->first.time_since_epoch() / 1.0s,
                    timeline.rbegin()->first.time_since_epoch() / 1.0s
                );
            }

            auto const now = sys->steady_time();
            auto show = timeline.upper_bound(now);
            if (show != timeline.begin()) {
                auto before = show;
                --before;
                if (before->first > shown) show = before;
            }

            for (auto s = timeline.upper_bound(shown); s != show; ++s) {
                if (logger->should_log(log_level::warn)) {
                    logger->warn(
                        "Skip frame sched={:.3f}s ({}ms old)",
                        s->first.time_since_epoch() / 1.0s,
                        (now - s->first) / 1ms
                    );
                }
                shown = s->first;
            }

            if (show == timeline.end()) {
                logger->trace("> (no more frames, waiting for wakeup)");
                wakeup.wait(lock);
                continue;
            }

            if (show->first > now) {
                if (logger->should_log(log_level::trace)) {
                    auto const delay = show->first - now;
                    logger->trace("> (waiting {}ms for frame)", delay / 1ms);
                }
                sys->wait_until(show->first, &wakeup, &lock);
                continue;
            }

            auto const done = driver->update_done_yet(connector_id);
            if (!done) {
                logger->trace("> (update pending, waiting 5ms)");
                auto const try_again = now + 5ms;
                sys->wait_until(try_again, &wakeup, &lock);
                continue;
            }

            driver->update(connector_id, mode, show->second);
            shown = show->first;
            if (logger->should_log(log_level::debug)) {
                auto const lag = now - shown;
                logger->debug(
                    "Show frame sched={:.3f}s ({}ms old)",
                    shown.time_since_epoch() / 1.0s, lag / 1ms
                );
            }
        }
        logger->debug("Frame player thread ending...");
    }

  private:
    // Constant from start to ~
    std::shared_ptr<log::logger> const logger = player_logger();
    std::thread thread;
    std::mutex mutable mutex;

    // Guarded by mutex
    bool shutdown = false;
    std::condition_variable wakeup;
    Timeline timeline;
    Timeline::key_type shown = {};
};

}  // anonymous namespace

std::unique_ptr<FramePlayer> start_frame_player(
    std::shared_ptr<UnixSystem> sys,
    DisplayDriver* driver,
    uint32_t connector_id,
    DisplayMode mode
) {
    auto player = std::make_unique<ThreadFramePlayer>();
    player->start(std::move(sys), driver, connector_id, std::move(mode));
    return player;
}

}  // namespace pivid
