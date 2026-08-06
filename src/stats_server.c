/* SPDX-License-Identifier: BSD-3-Clause */
#include "stats_server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <netinet/in.h>

/* ---- streaming JSON state passed through flow_table_foreach callback ---- */

struct json_ctx {
    int fd;
    int first; /* 1 before the first entry is written; 0 after */
};

static void write_flow_cb(const struct flow_key *key,
                          flow_count_t count, void *arg)
{
    struct json_ctx *ctx = (struct json_ctx *)arg;

    char src[INET_ADDRSTRLEN];
    char dst[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &key->src_ip, src, sizeof(src));
    inet_ntop(AF_INET, &key->dst_ip, dst, sizeof(dst));

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "%s{\"src_ip\":\"%s\",\"dst_ip\":\"%s\","
                     "\"proto\":%u,\"src_port\":%u,\"dst_port\":%u,"
                     "\"packets\":%"PRIu64"}",
                     ctx->first ? "" : ",",
                     src, dst,
                     (unsigned)key->proto,
                     (unsigned)ntohs(key->src_port),
                     (unsigned)ntohs(key->dst_port),
                     count);

    if (n <= 0 || (size_t)n >= sizeof(buf))
        return; /* truncation; skip this entry */

    /* Ignore partial-write errors — client may disconnect; avoid abort. */
    (void)write(ctx->fd, buf, (size_t)n);
    ctx->first = 0;
}

/*
 * Stream the full counter snapshot as JSON directly to 'fd'.
 * Response format: {"flows":[...entries...],"other_packets":N}\n
 * Uses a 256-byte per-entry stack buffer; no heap allocation.
 */
static void write_json_flows(int fd, struct flow_table *ft)
{
    static const char open[] = "{\"flows\":[";
    (void)write(fd, open, sizeof(open) - 1);

    struct json_ctx ctx = { .fd = fd, .first = 1 };
    flow_table_foreach(ft, write_flow_cb, &ctx);

    char tail[64];
    int n = snprintf(tail, sizeof(tail),
                     "],\"other_packets\":%"PRIu64"}\n",
                     flow_table_other_count(ft));
    if (n > 0 && (size_t)n < sizeof(tail))
        (void)write(fd, tail, (size_t)n);
}

/* ---- per-connection handling -------------------------------------------- */

static void handle_client(int client_fd, struct flow_table *ft)
{
    char req[128];
    ssize_t n = read(client_fd, req, sizeof(req) - 1);
    if (n <= 0)
        return;
    req[n] = '\0';

    if (strncmp(req, "GET /counters", 13) == 0) {
        write_json_flows(client_fd, ft);
    } else {
        static const char err[] = "{\"error\":\"unknown request\"}\n";
        (void)write(client_fd, err, sizeof(err) - 1);
    }
}

/* ---- socket lifecycle ---------------------------------------------------- */

static int server_setup(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    /* Remove any stale socket file from a previous run. */
    unlink(path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }

    /* Non-blocking so that accept() returns EAGAIN instead of blocking
     * indefinitely; the loop polls every 10 ms (NFR-002). */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void server_teardown(int fd, const char *path)
{
    close(fd);
    unlink(path); /* FR-018 */
}

/* ---- main server loop ---------------------------------------------------- */

void stats_server_run(const struct stats_server_cfg *cfg)
{
    int listen_fd = server_setup(cfg->socket_path);
    if (listen_fd < 0)
        return;

    while (!*cfg->stop) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000); /* 10 ms poll; worst-case accept latency <<  NFR-002 */
                continue;
            }
            if (errno == EINTR) {
                /* Signal fired (SA_RESTART not set); re-check stop flag. */
                continue;
            }
            break; /* unexpected error */
        }

        handle_client(client_fd, cfg->ft);
        close(client_fd);
    }

    server_teardown(listen_fd, cfg->socket_path);
}
