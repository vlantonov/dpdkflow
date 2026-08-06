/* SPDX-License-Identifier: BSD-3-Clause
 * Tests for IPv4 address formatting used by write_flow_cb in stats_server.c.
 * Exercises inet_ntop with representative addresses stored in network byte
 * order (as flow_key fields are), without any DPDK dependency.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>

int main(void)
{
    char buf[INET_ADDRSTRLEN];

    /* 192.168.1.1 stored in NBO (as flow_key.src_ip / dst_ip are) */
    uint32_t addr = htonl(0xC0A80101u);
    assert(inet_ntop(AF_INET, &addr, buf, sizeof(buf)) != NULL);
    assert(strcmp(buf, "192.168.1.1") == 0);

    /* All-zeros — should produce "0.0.0.0" */
    uint32_t zero = 0u;
    assert(inet_ntop(AF_INET, &zero, buf, sizeof(buf)) != NULL);
    assert(strcmp(buf, "0.0.0.0") == 0);

    /* Broadcast — should produce "255.255.255.255" */
    uint32_t broad = htonl(0xFFFFFFFFu);
    assert(inet_ntop(AF_INET, &broad, buf, sizeof(buf)) != NULL);
    assert(strcmp(buf, "255.255.255.255") == 0);

    /* Loopback 127.0.0.1 */
    uint32_t lo = htonl(0x7F000001u);
    assert(inet_ntop(AF_INET, &lo, buf, sizeof(buf)) != NULL);
    assert(strcmp(buf, "127.0.0.1") == 0);

    /* Verify INET_ADDRSTRLEN (16) is large enough for max-length address */
    assert(INET_ADDRSTRLEN >= 16);

    return 0;
}
