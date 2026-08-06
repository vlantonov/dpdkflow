/* SPDX-License-Identifier: BSD-3-Clause */
#include "signal_handler.h"

#include <signal.h>

volatile sig_atomic_t g_stop = 0;

static void handler(int sig)
{
    (void)sig;
    g_stop = 1;
}

void signal_handler_install(void)
{
    struct sigaction sa = {0};

    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* SA_RESTART intentionally omitted: accept() returns EINTR */

    if (sigaction(SIGINT,  &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0)
        perror("sigaction");
}
