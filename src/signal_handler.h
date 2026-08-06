/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <signal.h>

/* Set to 1 by the SIGINT/SIGTERM handler; checked by both lcore loops. */
extern volatile sig_atomic_t g_stop;

/* Install SIGINT and SIGTERM handlers via sigaction (SA_RESTART cleared). */
void signal_handler_install(void);
