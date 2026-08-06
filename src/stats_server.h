/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <signal.h>

#include "flow_table.h"

/* Configuration for the stats server (runs on the main lcore). */
struct stats_server_cfg {
    const char            *socket_path;
    struct flow_table     *ft;
    volatile sig_atomic_t *stop;
};

/* Blocking loop on the calling (main) lcore; returns when *stop != 0. */
void stats_server_run(const struct stats_server_cfg *cfg);
