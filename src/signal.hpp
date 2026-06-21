#pragma once
#include <csignal>

/// Shared shutdown signal — set by SIGINT/SIGTERM, polled by long-running operations.
extern volatile sig_atomic_t g_shutdown_requested;

inline bool is_shutdown_requested() { return g_shutdown_requested != 0; }

/// Install signal handlers (SIGINT, SIGTERM) that set g_shutdown_requested.
void setup_shutdown_handlers();
