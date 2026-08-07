/* SPDX-License-Identifier: BSD-3-Clause */
#include "flow_table.h"

#include <stdatomic.h>
#include <string.h>
#include <errno.h>

#include <rte_hash.h>
#include <rte_malloc.h>
#include <rte_log.h>

/* Per-flow entry stored in the pre-allocated hugepage pool. */
struct flow_entry {
    struct flow_key      key;
    _Atomic flow_count_t count;
};

struct flow_table {
    struct rte_hash   *hash;
    struct flow_entry *entries;   /* rte_zmalloc'd; size = max_flows */
    _Atomic uint32_t   pool_used; /* monotonic slot allocator; sole writer = RX lcore */
    _Atomic uint64_t   other;     /* non-IPv4 aggregate counter */
    uint32_t           max_flows;
};

int flow_table_init(struct flow_table **out, uint32_t max_flows)
{
    struct flow_table *ft = rte_zmalloc("flow_table", sizeof(*ft), 0);
    if (!ft)
        return -ENOMEM;

    ft->entries = rte_zmalloc("flow_entries",
                              sizeof(struct flow_entry) * max_flows,
                              RTE_CACHE_LINE_SIZE);
    if (!ft->entries) {
        rte_free(ft);
        return -ENOMEM;
    }

    struct rte_hash_parameters params = {
        .name      = "dpdkflow_hash",
        .entries   = max_flows,
        .key_len   = sizeof(struct flow_key),
        .socket_id = (int)rte_socket_id(),
        .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY,
    };

    ft->hash = rte_hash_create(&params);
    if (!ft->hash) {
        rte_free(ft->entries);
        rte_free(ft);
        return -ENOMEM;
    }

    ft->max_flows = max_flows;
    *out = ft;
    return 0;
}

void flow_table_destroy(struct flow_table *ft)
{
    if (!ft)
        return;
    rte_hash_free(ft->hash);
    rte_free(ft->entries);
    rte_free(ft);
}

void flow_table_update(struct flow_table *ft, const struct flow_key *key)
{
    void *data;
    int ret = rte_hash_lookup_data(ft->hash, key, &data);
    if (ret >= 0) {
        /* Existing flow: hot path — no hash lock, just atomic counter bump. */
        struct flow_entry *entry = (struct flow_entry *)data;
        atomic_fetch_add_explicit(&entry->count, 1, memory_order_relaxed);
        return;
    }

    /* New flow: claim the next pool slot with an atomic increment. */
    uint32_t slot = atomic_fetch_add_explicit(&ft->pool_used, 1, memory_order_relaxed);
    if (slot >= ft->max_flows) {
        /* Table full; emit one warning then silently drop new flows. */
        static int warned = 0;
        if (!warned) {
            RTE_LOG(WARNING, USER1,
                    "dpdkflow: flow table full (%u entries); "
                    "new flows will be dropped\n",
                    ft->max_flows);
            warned = 1;
        }
        return;
    }

    struct flow_entry *entry = &ft->entries[slot];
    entry->key = *key;
    atomic_store_explicit(&entry->count, 1, memory_order_relaxed);

    ret = rte_hash_add_key_data(ft->hash, key, entry);
    if (ret < 0)
        RTE_LOG(WARNING, USER1,
                "dpdkflow: rte_hash_add_key_data failed (ret=%d)\n", ret);
}

void flow_table_incr_other(struct flow_table *ft)
{
    atomic_fetch_add_explicit(&ft->other, 1, memory_order_relaxed);
}

void flow_table_foreach(struct flow_table *ft, flow_iter_cb cb, void *arg)
{
    uint32_t iter = 0;
    const void *key;
    void *data;

    while (rte_hash_iterate(ft->hash, &key, &data, &iter) >= 0) {
        struct flow_entry *entry = (struct flow_entry *)data;
        flow_count_t count = atomic_load_explicit(&entry->count, memory_order_relaxed);
        cb((const struct flow_key *)key, count, arg);
    }
}

uint64_t flow_table_other_count(const struct flow_table *ft)
{
    return atomic_load_explicit(&ft->other, memory_order_relaxed);
}
