#include "signal.hpp"
#include <csignal>

volatile sig_atomic_t g_shutdown_requested = 0;

static void signal_handler(int) { g_shutdown_requested = 1; }

void setup_shutdown_handlers() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
}
