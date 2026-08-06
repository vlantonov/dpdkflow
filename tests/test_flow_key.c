/* SPDX-License-Identifier: BSD-3-Clause
 * Tests for flow_key struct layout — no DPDK dependency.
 * Validates that the key is exactly 16 bytes with the expected field offsets
 * so rte_hash hashes the full fixed-size key consistently (including padding).
 */
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#include "flow_table.h"

int main(void)
{
    /* 4+4+2+2+1+3 == 16; rte_hash requires a fixed, SIMD-friendly key size */
    assert(sizeof(struct flow_key) == 16);

    assert(offsetof(struct flow_key, src_ip)   == 0);
    assert(offsetof(struct flow_key, dst_ip)   == 4);
    assert(offsetof(struct flow_key, src_port) == 8);
    assert(offsetof(struct flow_key, dst_port) == 10);
    assert(offsetof(struct flow_key, proto)    == 12);
    assert(offsetof(struct flow_key, pad)      == 13);

    /* Verify that memset-to-zero produces a zero key (pad bytes are clean). */
    struct flow_key k;
    memset(&k, 0, sizeof(k));
    assert(k.src_ip   == 0);
    assert(k.dst_ip   == 0);
    assert(k.src_port == 0);
    assert(k.dst_port == 0);
    assert(k.proto    == 0);
    assert(k.pad[0]   == 0 && k.pad[1] == 0 && k.pad[2] == 0);

    return 0;
}
