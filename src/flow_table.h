/* SPDX-License-Identifier: BSD-3-Clause */
#pragma once

#include <stdint.h>

/*
 * 5-tuple key used as the rte_hash key.  All fields are in network byte order.
 * pad[] must be zeroed before use; rte_hash hashes the raw bytes including
 * padding, so a dirty pad produces a different hash for the same logical key.
 * Total size: 16 bytes (SIMD-friendly for rte_hash CRC32c/AES-NI paths).
 */
struct flow_key {
    uint32_t src_ip;   /* network byte order */
    uint32_t dst_ip;   /* network byte order */
    uint16_t src_port; /* network byte order; 0 for non-TCP/UDP (e.g. ICMP) */
    uint16_t dst_port; /* network byte order; 0 for non-TCP/UDP */
    uint8_t  proto;    /* IP protocol number */
    uint8_t  pad[3];   /* explicit zero-pad to 16 bytes */
};

typedef uint64_t flow_count_t;

struct flow_table; /* opaque */

/* Allocates hash + entry pool from DPDK hugepage heap. */
int  flow_table_init(struct flow_table **out, uint32_t max_flows);
void flow_table_destroy(struct flow_table *ft);

/* Hot path — called only from the single RX lcore. */
void flow_table_update(struct flow_table *ft, const struct flow_key *key);
void flow_table_incr_other(struct flow_table *ft);

/* Stats path — called from main/stats lcore. */
typedef void (*flow_iter_cb)(const struct flow_key *key,
                             flow_count_t count, void *arg);
void     flow_table_foreach(struct flow_table *ft,
                             flow_iter_cb cb, void *arg);
uint64_t flow_table_other_count(const struct flow_table *ft);
