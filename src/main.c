/* SPDX-License-Identifier: BSD-3-Clause */
#include "signal_handler.h"
#include "flow_table.h"
#include "rx_loop.h"
#include "stats_server.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_log.h>

#define MBUF_CACHE_SIZE  256
#define DEFAULT_NB_MBUFS 8191u
#define DEFAULT_MAX_FLOWS 65536u
#define DEFAULT_SOCK_PATH "/tmp/dpdkflow.sock"
#define RX_RING_SIZE      1024

struct app_cfg {
    char     socket_path[108]; /* sizeof sun_path */
    uint32_t nbmbufs;
    uint32_t max_flows;
    unsigned rx_lcore_id;
};

/* ---- argument parsing ---------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [EAL options] -- "
            "[--socket <path>] [--nbmbufs <N>] [--max-flows <N>]\n"
            "  --socket    UNIX socket path (default: %s)\n"
            "  --nbmbufs   mbuf pool size, must be 2^N-1 (default: %u)\n"
            "  --max-flows max distinct 5-tuples, must be power of 2 (default: %u)\n",
            prog, DEFAULT_SOCK_PATH, DEFAULT_NB_MBUFS, DEFAULT_MAX_FLOWS);
}

static void parse_app_args(int argc, char **argv, struct app_cfg *cfg)
{
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0) {
            if (i + 1 >= argc)
                rte_exit(EXIT_FAILURE, "--socket requires an argument\n");
            snprintf(cfg->socket_path, sizeof(cfg->socket_path),
                     "%s", argv[++i]);

        } else if (strcmp(argv[i], "--nbmbufs") == 0) {
            if (i + 1 >= argc)
                rte_exit(EXIT_FAILURE, "--nbmbufs requires an argument\n");
            unsigned long v = strtoul(argv[++i], NULL, 10);
            if (v == 0 || (v & (v + 1)) != 0)
                rte_exit(EXIT_FAILURE,
                         "--nbmbufs must be a Mersenne number (2^N - 1): %lu\n",
                         v);
            cfg->nbmbufs = (uint32_t)v;

        } else if (strcmp(argv[i], "--max-flows") == 0) {
            if (i + 1 >= argc)
                rte_exit(EXIT_FAILURE, "--max-flows requires an argument\n");
            unsigned long v = strtoul(argv[++i], NULL, 10);
            if (v == 0 || (v & (v - 1)) != 0)
                rte_exit(EXIT_FAILURE,
                         "--max-flows must be a power of two: %lu\n", v);
            cfg->max_flows = (uint32_t)v;

        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h")     == 0) {
            usage(argv[0]);
            rte_eal_cleanup();
            exit(EXIT_SUCCESS);
        }
    }
}

/* ---- port initialisation ------------------------------------------------- */

static void port_init(uint16_t port_id, struct rte_mempool *mp,
                      uint16_t ring_size)
{
    struct rte_eth_conf port_conf;
    memset(&port_conf, 0, sizeof(port_conf));
    port_conf.rxmode.mtu = RTE_ETHER_MAX_LEN;

    int ret = rte_eth_dev_configure(port_id, /*nb_rxq=*/1, /*nb_txq=*/0,
                                    &port_conf);
    if (ret < 0)
        rte_exit(EXIT_FAILURE,
                 "rte_eth_dev_configure(port %u) failed: %d\n",
                 port_id, ret);

    ret = rte_eth_rx_queue_setup(port_id, /*queue_id=*/0, ring_size,
                                 rte_eth_dev_socket_id(port_id),
                                 NULL, mp);
    if (ret < 0)
        rte_exit(EXIT_FAILURE,
                 "rte_eth_rx_queue_setup(port %u) failed: %d\n",
                 port_id, ret);

    ret = rte_eth_dev_start(port_id);
    if (ret < 0)
        rte_exit(EXIT_FAILURE,
                 "rte_eth_dev_start(port %u) failed: %d\n",
                 port_id, ret);

    /* Promiscuous mode so all frames reach the RX queue. */
    ret = rte_eth_promiscuous_enable(port_id);
    if (ret < 0)
        RTE_LOG(WARNING, USER1,
                "rte_eth_promiscuous_enable(port %u) failed: %d\n",
                port_id, ret);
}

/* ---- teardown ------------------------------------------------------------ */

static void teardown(uint16_t port_id, struct rte_mempool *mp,
                     struct flow_table *ft)
{
    int ret = rte_eth_dev_stop(port_id);
    if (ret < 0)
        RTE_LOG(WARNING, USER1,
                "rte_eth_dev_stop(port %u) failed: %d\n", port_id, ret);
    rte_eth_dev_close(port_id);
    flow_table_destroy(ft);
    rte_mempool_free(mp);
    rte_eal_cleanup();
}

/* ---- entry point --------------------------------------------------------- */

int main(int argc, char **argv)
{
    /* EAL init consumes its own arguments (everything up to and including --).
     * It returns the number of args parsed; we advance past them to reach the
     * application arguments. */
    int eal_ret = rte_eal_init(argc, argv);
    if (eal_ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eal_init failed: %d\n", eal_ret);

    argc -= eal_ret;
    argv += eal_ret;

    /* Defaults */
    struct app_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.socket_path, sizeof(cfg.socket_path), "%s", DEFAULT_SOCK_PATH);
    cfg.nbmbufs   = DEFAULT_NB_MBUFS;
    cfg.max_flows = DEFAULT_MAX_FLOWS;

    parse_app_args(argc, argv, &cfg);

    /* Discover the first non-main lcore for RX work. */
    unsigned main_lcore = rte_get_main_lcore();
    unsigned rx_lcore   = rte_get_next_lcore(main_lcore, /*skip_main=*/1,
                                             /*wrap=*/0);
    if (rx_lcore == RTE_MAX_LCORE)
        rte_exit(EXIT_FAILURE,
                 "No worker lcore available; launch with -l 0-1\n");
    cfg.rx_lcore_id = rx_lcore;

    RTE_LOG(INFO, USER1, "dpdkflow: main lcore %u, RX lcore %u\n",
            main_lcore, rx_lcore);

    /* Install SIGINT / SIGTERM handlers before touching any blocking calls. */
    signal_handler_install();

    /* Mempool (hugepage-backed). */
    struct rte_mempool *mp =
        rte_pktmbuf_pool_create("mbuf_pool", cfg.nbmbufs,
                                MBUF_CACHE_SIZE, 0,
                                RTE_MBUF_DEFAULT_BUF_SIZE,
                                (int)rte_socket_id());
    if (!mp)
        rte_exit(EXIT_FAILURE, "rte_pktmbuf_pool_create failed\n");

    /* Flow table. */
    struct flow_table *ft = NULL;
    if (flow_table_init(&ft, cfg.max_flows) < 0)
        rte_exit(EXIT_FAILURE, "flow_table_init failed\n");

    /* Port 0 is the vdev created by --vdev net_af_packet0,iface=<IFACE>. */
    uint16_t port_id = 0;
    if (!rte_eth_dev_is_valid_port(port_id))
        rte_exit(EXIT_FAILURE,
                 "Port 0 not found. Did you pass --vdev net_af_packet0,iface=<IFACE>?\n");

    port_init(port_id, mp, RX_RING_SIZE);
    RTE_LOG(INFO, USER1, "dpdkflow: port %u started\n", port_id);

    /* Launch RX loop on the worker lcore. */
    struct rx_loop_cfg rx_cfg = {
        .port_id = port_id,
        .ft      = ft,
        .stop    = &g_stop,
    };

    int ret = rte_eal_remote_launch(rx_loop_run, &rx_cfg, rx_lcore);
    if (ret < 0)
        rte_exit(EXIT_FAILURE,
                 "rte_eal_remote_launch failed: %d\n", ret);

    RTE_LOG(INFO, USER1,
            "dpdkflow: listening on %s — send SIGINT/SIGTERM to stop\n",
            cfg.socket_path);

    /* Stats server runs blocking on the main lcore until g_stop fires. */
    struct stats_server_cfg stats_cfg = {
        .socket_path = cfg.socket_path,
        .ft          = ft,
        .stop        = &g_stop,
    };
    stats_server_run(&stats_cfg);

    /* Ensure the stop flag is set in case stats_server_run returned early. */
    g_stop = 1;

    /* Wait for the RX lcore to finish its current burst and exit cleanly. */
    rte_eal_wait_lcore(rx_lcore);
    RTE_LOG(INFO, USER1, "dpdkflow: RX lcore joined\n");

    teardown(port_id, mp, ft);
    return 0;
}
