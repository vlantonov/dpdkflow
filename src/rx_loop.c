/* SPDX-License-Identifier: BSD-3-Clause */
#include "rx_loop.h"

#include <string.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_ether.h>
#include <rte_byteorder.h>

#define BURST_SIZE 32

/*
 * Parse the Ethernet frame in 'm' into a 5-tuple 'out'.
 * Returns 1 if the frame is IPv4 and the key was populated, 0 otherwise.
 *
 * INVARIANT: 'out' is always memset to zero first so pad[3] is clean;
 * rte_hash hashes raw bytes including padding.
 */
static int parse_5tuple(const struct rte_mbuf *m, struct flow_key *out)
{
    memset(out, 0, sizeof(*out));

    uint32_t pkt_len = rte_pktmbuf_pkt_len(m);
    if (pkt_len < sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr))
        return 0;

    const struct rte_ether_hdr *eth =
        rte_pktmbuf_mtod(m, const struct rte_ether_hdr *);

    if (rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4)
        return 0;

    const struct rte_ipv4_hdr *ip =
        (const struct rte_ipv4_hdr *)(eth + 1);

    out->src_ip = ip->src_addr;
    out->dst_ip = ip->dst_addr;
    out->proto  = ip->next_proto_id;

    /* IHL field is the lower 4 bits of version_ihl, in 32-bit words. */
    uint8_t  ihl     = (ip->version_ihl & 0x0fu) * 4u;
    uint32_t ip_off  = sizeof(struct rte_ether_hdr) + ihl;
    const uint8_t *l4 = (const uint8_t *)eth + ip_off;

    if (ip->next_proto_id == IPPROTO_TCP) {
        if (pkt_len < ip_off + sizeof(struct rte_tcp_hdr))
            return 0;
        const struct rte_tcp_hdr *tcp = (const struct rte_tcp_hdr *)l4;
        out->src_port = tcp->src_port;
        out->dst_port = tcp->dst_port;
    } else if (ip->next_proto_id == IPPROTO_UDP) {
        if (pkt_len < ip_off + sizeof(struct rte_udp_hdr))
            return 0;
        const struct rte_udp_hdr *udp = (const struct rte_udp_hdr *)l4;
        out->src_port = udp->src_port;
        out->dst_port = udp->dst_port;
    }
    /* For other protocols (e.g. ICMP), src_port and dst_port remain 0. */

    return 1;
}

int rx_loop_run(void *arg)
{
    struct rx_loop_cfg *cfg    = (struct rx_loop_cfg *)arg;
    uint16_t            portid = cfg->port_id;
    struct flow_table  *ft     = cfg->ft;

    struct rte_mbuf *mbufs[BURST_SIZE];

    while (!*cfg->stop) {
        uint16_t n = rte_eth_rx_burst(portid, 0, mbufs, BURST_SIZE);

        for (uint16_t i = 0; i < n; i++) {
            struct flow_key key;
            if (parse_5tuple(mbufs[i], &key))
                flow_table_update(ft, &key);
            else
                flow_table_incr_other(ft);

            rte_pktmbuf_free(mbufs[i]);
        }
    }

    return 0;
}
