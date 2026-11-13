#include "signal.hpp"

#include <csignal>

volatile sig_atomic_t g_shutdown_requested = 0;

namespace {

/// Only a sig_atomic_t store is async-signal-safe. stop_source::request_stop()
/// runs later on the normal polling path when this flag is set.
void signal_handler(int) { g_shutdown_requested = 1; }

} // namespace

void setup_shutdown_handlers() {
    struct sigaction action{};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}
