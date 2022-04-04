// Simple command line tool to print available drivers, connectors and modes.

#include <CLI/App.hpp>
#include <CLI/Config.hpp>
#include <CLI/Formatter.hpp>
#include <fmt/core.h>

#include "display_output.h"

namespace pivid {

// Main program, parses flags and calls the decoder loop.
extern "C" int main(int const argc, char const* const* const argv) {
    std::string log_arg;

    CLI::App app("Print video drivers, connectors and modes");
    CLI11_PARSE(app, argc, argv);

    std::shared_ptr sys = global_system();

    try {
        for (auto const& listing : list_display_drivers(sys)) {
            fmt::print("## {}\n", debug(listing));
            auto driver = open_display_driver(sys, listing.dev_file);
            for (auto const& screen : driver->scan_screens()) {
                fmt::print(
                    "Screen #{:<3} {} {}\n",
                    screen.id, screen.connector,
                    screen.display_detected ? "[connected]" : "[no connection]"
                );
                if (screen.active_mode.nominal_hz)
                    fmt::print("  {} [ACTIVE]\n", debug(screen.active_mode));
                for (auto const& mode : screen.modes) {
                    if (debug(mode) != debug(screen.active_mode))
                        fmt::print("  {}\n", debug(mode));
                }
                fmt::print("\n");
            }
        }
    } catch (std::exception const& e) {
        fmt::print("*** {}\n", e.what());
    }

    return 0;
}

}  // namespace pivid