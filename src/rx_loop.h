/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <stdint.h>
#include <signal.h>

#include "flow_table.h"

/* Configuration passed to rx_loop_run via rte_eal_remote_launch. */
struct rx_loop_cfg {
    uint16_t               port_id;
    struct flow_table     *ft;
    volatile sig_atomic_t *stop;
};

/* lcore entry point; arg must point to a struct rx_loop_cfg. */
int rx_loop_run(void *arg);
