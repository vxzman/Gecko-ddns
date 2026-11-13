#pragma once
#include <csignal>

/// Shared shutdown signal — set by SIGINT/SIGTERM, polled by long-running operations.
extern volatile sig_atomic_t g_shutdown_requested;

[[nodiscard]] inline bool is_shutdown_requested() { return g_shutdown_requested != 0; }

/// Install sigaction handlers (SIGINT, SIGTERM) with SA_RESTART.
/// The handler only sets g_shutdown_requested. Callers turn that into stop_source::request_stop().
void setup_shutdown_handlers();
