# dpdkflow — Architecture Document

**Version:** 0.1.2  
**Date:** 2026-08-07  
**Status:** Released — v0.1.2  
**SRS Reference:** [docs/requirements/SRS.md](../requirements/SRS.md) v0.1.0

---

## 1. Overview

`dpdkflow` is a single-binary Linux C17 application that attaches to a named network interface via DPDK 23.11's `eth_af_packet` virtual PMD (no SR-IOV NIC required), captures all incoming Ethernet frames at line rate on a dedicated polling lcore, parses each IPv4 frame's 5-tuple (source IP, destination IP, source port, destination port, IP protocol), and maintains per-flow packet counters in a DPDK `rte_hash` table backed by a hugepage-resident entry pool. A second concurrent path — running on the main lcore — serves a UNIX domain socket: on receipt of a `GET /counters\n` request it iterates the hash table, serialises every live counter as a JSON object array, and streams the response directly to the client socket with no intermediate heap allocation. Non-IPv4 frames increment a single aggregate "other" counter returned alongside the flow array. The binary is launched with `--in-memory` EAL mode to avoid writing privileged runtime files, and runs with only `CAP_NET_RAW` once hugepages are pre-configured. It shuts down gracefully on `SIGINT` or `SIGTERM`, releasing all DPDK resources before returning exit code 0.

---

## 2. Component Diagram

```
                  SIGINT / SIGTERM
                         │
                         ▼
                ┌─────────────────┐
                │ signal_handler  │──── g_stop = 1
                └─────────────────┘

 Network iface (AF_PACKET kernel ring)
         │
         ▼ rte_eth_rx_burst
 ┌──────────────────────────────────────────────────────────────┐
 │                    dpdkflow process                          │
 │                                                              │
 │  lcore 1 ── rx_loop                                          │
 │  ┌──────────────────────────────────────────────────────┐   │
 │  │  rte_eth_rx_burst (up to 32 mbufs per iteration)     │   │
 │  │    └─► parse_5tuple ──► IPv4?                        │   │
 │  │                           ├─yes─► flow_table_update  │   │
 │  │                           └─no──► flow_table_incr_other│  │
 │  │  rte_pktmbuf_free (all mbufs)                        │   │
 │  │  check g_stop → exit loop                            │   │
 │  └────────────────────┬─────────────────────────────────┘   │
 │                       │ rte_hash_lookup / rte_hash_add_key   │
 │                       │ atomic_fetch_add_explicit on count   │
 │              ┌────────▼──────────────┐                      │
 │              │      flow_table       │                      │
 │              │  rte_hash (key→idx)   │                      │
 │              │  flow_entry_t pool[]  │                      │
 │              │  _Atomic uint64_t     │                      │
 │              │    other              │                      │
 │              └────────▲──────────────┘                      │
 │                       │ rte_hash_iterate                     │
 │                       │ atomic_load_explicit on count field  │
 │  lcore 0 ── main + stats_server                              │
 │  ┌──────────────────────────────────────────────────────┐   │
 │  │  UNIX socket accept (O_NONBLOCK + 10 ms usleep poll) │   │
 │  │    └─► read request line                             │   │
 │  │    └─► flow_table_foreach (streaming JSON)           │   │
 │  │    └─► write(fd, ...) per entry (256-byte stack buf) │   │
 │  │    └─► close client fd                               │   │
 │  │  check g_stop → exit loop                            │   │
 │  └──────────────────────────────────────────────────────┘   │
 │                                                              │
 └──────────────────────────────────────────────────────────────┘
                        │
              /tmp/dpdkflow.sock  (UNIX domain socket)
                        │
               client (curl / custom tool)
```

Data-flow summary:
1. Frames arrive from the kernel AF_PACKET ring into the DPDK mbuf pool.
2. RX lcore calls `rte_eth_rx_burst`, parses each mbuf, updates `flow_table`, frees mbufs.
3. Stats lcore accepts a client, snapshots `flow_table` via iteration callback, streams JSON, closes.
4. `SIGINT`/`SIGTERM` sets `g_stop`; both lcore loops exit; main tears down DPDK and returns 0.

---

## 3. Module Breakdown

### `src/main.c`

**Purpose:** Program entry point; orchestrates EAL init, vdev/mempool/port setup, lcore launch, stats server execution on the main lcore, and full teardown.

**Key exports:** none (translation-unit-local helpers only)

**Key functions:**
```c
int  main(int argc, char **argv);
/* internal */
static int  port_init(uint16_t port_id, struct rte_mempool *mp,
                      uint16_t ring_size);
static void parse_app_args(int argc, char **argv, struct app_cfg *cfg);
static void teardown(uint16_t port_id, struct rte_mempool *mp,
                     struct flow_table *ft);
```

`struct app_cfg` fields: `socket_path`, `nbmbufs` (Mersenne-validated), `max_flows` (power-of-two), `rx_lcore_id`.

**Dependencies:** all other modules; `librte_eal`, `librte_ethdev`, `librte_mempool`, `librte_mbuf`

---

### `src/rx_loop.h` / `src/rx_loop.c`

**Purpose:** DPDK lcore entry point for the RX polling loop; performs burst RX, IPv4 5-tuple parsing, and per-flow counter updates.

**Key exports:**
```c
struct rx_loop_cfg {
    uint16_t              port_id;
    struct flow_table    *ft;
    volatile sig_atomic_t *stop;
};

/* lcore entry: signature required by rte_eal_remote_launch */
int rx_loop_run(void *arg);
```

**Key internal functions:**
```c
/* Returns 1 for valid IPv4 (key populated), 0 for non-IPv4 */
static int parse_5tuple(const struct rte_mbuf *m, struct flow_key *out);
```

RX burst loop sketch (pseudocode):
```
while (*stop == 0):
    n = rte_eth_rx_burst(port_id, queue=0, mbufs[], BURST_SIZE=32)
    for i in 0..n:
        if parse_5tuple(mbufs[i], &key):
            flow_table_update(ft, &key)
        else:
            flow_table_incr_other(ft)
        rte_pktmbuf_free(mbufs[i])
```

Ports read in network byte order; `rte_be_to_cpu_*` is used for protocol-number comparisons only, not for storing keys (keys stay in NBO for hash consistency).

**Dependencies:** `flow_table.h`, `signal_handler.h`; `librte_ethdev`, `librte_mbuf`, `librte_net`

---

### `src/flow_table.h` / `src/flow_table.c`

**Purpose:** Wraps `rte_hash`; provides a fixed-capacity per-5-tuple packet counter table and an aggregate non-IPv4 counter. This is the only module that touches `rte_hash` directly.

**Key exports:**
```c
/* --- key type (rte_hash key; must be fixed-size) --- */
struct flow_key {
    uint32_t src_ip;    /* network byte order */
    uint32_t dst_ip;    /* network byte order */
    uint16_t src_port;  /* network byte order; 0 for non-TCP/UDP */
    uint16_t dst_port;  /* network byte order; 0 for non-TCP/UDP */
    uint8_t  proto;     /* IP protocol number */
    uint8_t  pad[3];    /* zero-pad to 16 bytes; rte_hash fixed-key requirement */
};

typedef uint64_t flow_count_t;

/* --- opaque handle --- */
struct flow_table;

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
```

**Internal layout (implementation-private):**
```c
struct flow_entry {
    struct flow_key      key;
    _Atomic flow_count_t count;
};

struct flow_table {
    struct rte_hash   *hash;
    struct flow_entry *entries;    /* rte_zmalloc'd; size = max_flows */
    _Atomic uint32_t   pool_used;  /* monotonic slot allocator */
    _Atomic uint64_t   other;
    uint32_t           max_flows;
};
```

`flow_table_update` algorithm:
1. `rte_hash_lookup_data(ft->hash, key, (void **)&entry)` — fast read path
2. If found: `atomic_fetch_add_explicit(&entry->count, 1, memory_order_relaxed)`
3. If `ENOENT` (new flow): atomically claim next pool slot via `atomic_fetch_add_explicit(&ft->pool_used, 1, ...)`; if slot < `max_flows`: initialise entry, `rte_hash_add_key_data()`; else log warning once and drop silently.

**Dependencies:** `librte_hash`, `librte_malloc`; no other project modules

---

### `src/stats_server.h` / `src/stats_server.c`

**Purpose:** UNIX domain socket server; accepts one client connection at a time, dispatches on the request line, iterates `flow_table` to stream a JSON response, and closes the connection.

**Key exports:**
```c
struct stats_server_cfg {
    const char            *socket_path;
    struct flow_table     *ft;
    volatile sig_atomic_t *stop;
};

/* Blocking loop on the calling (main) lcore; returns when *stop != 0. */
void stats_server_run(const struct stats_server_cfg *cfg);
```

**Key internal functions:**
```c
static int  server_setup(const char *path);   /* socket+bind+listen+O_NONBLOCK */
static void handle_client(int client_fd, struct flow_table *ft);
static void write_json_flows(int fd, struct flow_table *ft);
static void server_teardown(int fd, const char *path); /* close+unlink */
```

`write_json_flows` uses a stack-allocated 256-byte buffer per entry and streams directly to `fd`; no heap allocation (satisfies NFR-008).

**Dependencies:** `flow_table.h`, `signal_handler.h`; POSIX sockets (`sys/un.h`, `sys/socket.h`, `arpa/inet.h`)

---

### `src/signal_handler.h` / `src/signal_handler.c`

**Purpose:** Installs `SIGINT`/`SIGTERM` handlers that set the global stop flag, enabling graceful shutdown across both lcores.

**Key exports:**
```c
extern volatile sig_atomic_t g_stop;

/* Install handlers for SIGINT and SIGTERM via sigaction(). */
void signal_handler_install(void);
```

**Internal:**
```c
static void _handler(int sig) { (void)sig; g_stop = 1; }
```

`sigaction()` is called with `sa.sa_flags = 0` (i.e., `SA_RESTART` is **not** set). This causes `accept()` to return `EINTR` when a signal fires, so the stats server loop detects `g_stop` without a polling delay.

**Dependencies:** none (POSIX only: `signal.h`)

---

### `meson.build`

**Purpose:** Meson build definition; locates DPDK 23.11 via `pkg-config` (`libdpdk`), compiles all source files, links the `dpdkflow` executable.

**Dependencies:** Meson ≥ 1.1; DPDK 23.11 `libdpdk.pc` discoverable by `pkg-config`

---

## 4. Key Data Structures

### 4.1 Flow Key

```c
/* 5-tuple flow key — used as rte_hash key; must be fixed-size */
struct flow_key {
    uint32_t src_ip;    /* network byte order */
    uint32_t dst_ip;    /* network byte order */
    uint16_t src_port;  /* network byte order; 0 for non-TCP/UDP (e.g., ICMP) */
    uint16_t dst_port;  /* network byte order; 0 for non-TCP/UDP */
    uint8_t  proto;     /* IP protocol number (IPPROTO_TCP=6, IPPROTO_UDP=17, …) */
    uint8_t  pad[3];    /* explicit zero-padding to 16 bytes total */
};

/* per-flow packet counter stored in the entry pool */
typedef uint64_t flow_count_t;
```

Total key size: 16 bytes. The `pad` bytes must be zeroed before use; `rte_hash` hashes the raw bytes including padding. 16 bytes is also SIMD-friendly for `rte_hash`'s internal CRC32/AES-NI acceleration.

### 4.2 Why `rte_hash` Over a Plain Hash Map

| Criterion | `rte_hash` | `uthash` / glib / plain open-addressing |
|---|---|---|
| Thread-safety | Built-in per-bucket RW spinlock (`RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY`) | Requires external locking wrapping every call |
| NUMA placement | Allocated via DPDK `rte_malloc` (socket-local hugepage heap) | `malloc` — no NUMA control |
| Hash function | CRC32c/AES-NI hardware-accelerated | Software only |
| DPDK API fit | Returns slot index; integrates with `rte_hash_iterate` | N/A |
| Dependency | Already pulled in by `libdpdk` | Extra library |

`RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY` adds a per-bucket spinlock. Concurrent reads (stats lcore iterating) take a shared lock per bucket; writes (RX lcore inserting a new flow) take an exclusive lock on that bucket only. Counter increments for *existing* flows bypass the hash entirely — they use `_Atomic` directly on the `flow_entry_t` in the pre-allocated pool — so the hash lock is only contended on the rare event of a new-flow insertion.

### 4.3 Flow Entry Pool

```c
struct flow_entry {
    struct flow_key      key;    /* copy stored for JSON serialisation */
    _Atomic flow_count_t count;
};
```

`flow_table_init()` allocates `max_flows` entries with `rte_zmalloc()` once at startup. Pool slots are claimed with an atomic increment of `pool_used` (monotonic; no deallocation). This satisfies NFR-008: no dynamic allocation after initialisation.

---

## 5. Threading Model

### 5.1 Lcore Assignment

| Lcore | Role | How it runs |
|---|---|---|
| 0 (main) | EAL init · app init · stats server loop · teardown | `main()` directly; `stats_server_run()` is a blocking call |
| 1 (worker) | RX burst · 5-tuple parse · counter update | `rte_eal_remote_launch(rx_loop_run, &rx_cfg, rx_lcore)` |

Minimum EAL arg: `-l 0-1`. The RX lcore id is discovered at runtime with `rte_get_next_lcore(rte_get_main_lcore(), /*skip_main=*/1, /*wrap=*/0)` and stored in `app_cfg.rx_lcore_id`. This makes the assignment robust to non-contiguous lcore masks.

### 5.2 Stop Flag

```c
volatile sig_atomic_t g_stop = 0;
```

`volatile sig_atomic_t` is the POSIX-mandated type for signal-handler-visible state:
- `volatile` prevents the compiler from caching the value in a register across loop iterations.
- `sig_atomic_t` guarantees that the assignment in the signal handler is indivisible (no torn write) on all POSIX platforms.
- The flag is written exactly once (to `1`) by the signal handler and never reset. For a single monotonic write, `volatile sig_atomic_t` is sufficient and is the idiom used in DPDK's own example applications.

**Trade-off:** C11 `_Atomic int` with `memory_order_release`/`memory_order_acquire` is formally more correct for cross-thread visibility under the C memory model. However, `volatile sig_atomic_t` is universally portable, requires no `<stdatomic.h>`, and is valid in signal-handler context on all Linux versions targeted (kernel ≥ 4.15, GCC ≥ 11). The C11 variant is the more principled choice if the project later adopts a stricter concurrency analysis tool (e.g., ThreadSanitizer).

### 5.3 Counter Atomics

Counter increment (RX lcore, hot path):
```c
atomic_fetch_add_explicit(&entry->count, 1, memory_order_relaxed);
```

Counter read (stats lcore):
```c
flow_count_t c = atomic_load_explicit(&entry->count, memory_order_relaxed);
```

`memory_order_relaxed` is sufficient: the stats snapshot is best-effort (a count may lag by one or two packets during the read). This is standard practice for monitoring counters and avoids unnecessary memory fences on the hot path.

### 5.4 Hash Table Concurrency

| Operation | Lcore | Lock behaviour |
|---|---|---|
| `rte_hash_lookup_data` (existing flow) | RX (1) | Per-bucket shared read lock |
| `rte_hash_add_key_data` (new flow) | RX (1) | Per-bucket exclusive write lock |
| `rte_hash_iterate` (snapshot) | Stats (0) | Per-bucket shared read lock per step |

Because only lcore 1 ever writes to the hash, write contention is zero. The exclusive lock on new-flow insertion (rare event) briefly blocks any simultaneous `rte_hash_iterate` on that bucket only, not the full table.

---

## 6. UNIX Socket JSON API

### 6.1 Wire Protocol

All messages are UTF-8 text. There is no HTTP framing — the protocol is a raw line-oriented UNIX stream.

**Request (client → server):**
```
GET /counters\n
```

**Success response (server → client):**
```
{"flows":[{"src_ip":"192.168.1.1","dst_ip":"10.0.0.1","proto":6,"src_port":12345,"dst_port":80,"packets":42},{"src_ip":"...","dst_ip":"...","proto":17,"src_port":5353,"dst_port":5353,"packets":7}],"other_packets":3}\n
```

**Unknown request response:**
```
{"error":"unknown request"}\n
```

> **Design deviation from FR-016:** FR-016 specifies a bare JSON array. This design returns a JSON object `{"flows":[…],"other_packets":N}` to satisfy FR-008 (aggregate non-IPv4 counter) without a second API call. The `flows` array elements retain all fields specified in FR-016. If a strict bare-array format is mandated, `other_packets` can be relocated to a separate `GET /stats` endpoint in a future iteration — the addition is backward-compatible.

### 6.2 IP Address Formatting

`flow_key.src_ip` / `dst_ip` are stored in **network byte order** as received off the wire. `inet_ntop(AF_INET, &addr, buf, INET_ADDRSTRLEN)` is used for serialisation — it is thread-safe and expects NBO input.

Ports are displayed in **host byte order**: `rte_be_to_cpu_16(key.src_port)`.

### 6.3 Streaming Serialisation (No Heap Allocation)

To comply with NFR-008, no intermediate buffer holding the full JSON response is allocated. The stats server streams JSON directly to the client `fd`:

1. `write(fd, "{\"flows\":[", …)` — open object + array
2. For each flow (via `flow_iter_cb`): format one object into a 256-byte stack buffer with `snprintf`, `write(fd, buf, len)`; prepend `,` for all entries after the first
3. `write(fd, "],"other_packets":N}\n", …)` — close array + object + newline

### 6.4 Connection Model

- After `listen()`, the server socket is set `O_NONBLOCK`.
- Accept loop: `accept()` → if `EAGAIN`, `usleep(10000)` (10 ms), check `g_stop`, repeat.
- `accept()` returns `EINTR` on signal delivery (because `SA_RESTART` is not set); the loop treats `EINTR` equivalently to `EAGAIN` after checking `g_stop`.
- Worst-case accept latency: 10 ms (≪ NFR-002 budget of 100 ms).
- One client at a time (FR-014): client `fd` is closed before returning to the accept loop.
- On `g_stop`: `server_teardown()` closes the listen socket and calls `unlink(socket_path)` (FR-018).

---

## 7. Build System Design

### 7.1 `meson.build`

```meson
project('dpdkflow', 'c',
  version         : '0.1.2',
  default_options : ['c_std=c17', 'warning_level=2'],
)

dpdk = dependency('libdpdk', required : true)

sources = files(
  'src/main.c',
  'src/rx_loop.c',
  'src/flow_table.c',
  'src/stats_server.c',
  'src/signal_handler.c',
)

executable('dpdkflow',
  sources      : sources,
  dependencies : [dpdk],
  install      : false,
)
```

`dependency('libdpdk')` invokes `pkg-config --cflags --libs libdpdk`, which resolves all DPDK compile flags (`-march=native`, `-msse4.2`, etc.) and link flags automatically from the system-installed `libdpdk.pc`. No manual `-I` or `-L` paths are needed (NFR-009).

### 7.2 Build and Run

```bash
# One-time hugepage setup (operator, not application — FR-022):
echo 64 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# Build
meson setup build
ninja -C build

# Grant CAP_NET_RAW once after build (NFR-007):
sudo setcap cap_net_raw+ep build/dpdkflow

# Run (no sudo required after setcap):
build/dpdkflow \
  -l 0-1 --in-memory --no-pci \
  --vdev "net_af_packet0,iface=eth0" \
  -- --socket /tmp/dpdkflow.sock --nbmbufs 8191 --max-flows 65536
```

### 7.3 Notable EAL Flags

| Flag | Purpose | Requirement |
|---|---|---|
| `--in-memory` | Avoids writing files in `/var/run/dpdk/`; no `CAP_SYS_ADMIN` needed | FR-023, NFR-007 |
| `--no-pci` | Skips PCI bus scan; avoids `/dev/mem` access (vdev-only app) | NFR-007 |
| `--vdev net_af_packet0,iface=<IFACE>` | Creates the AF_PACKET virtual Ethernet device | FR-001 |
| `-l 0-1` | Enables lcores 0 (main) and 1 (RX worker) | FR-010, FR-012 |

Application arguments (after `--`):

| Arg | Default | Validated |
|---|---|---|
| `--socket <path>` | `/tmp/dpdkflow.sock` | path length |
| `--nbmbufs <N>` | `8191` | `(N & (N+1)) == 0` (Mersenne) |
| `--max-flows <N>` | `65536` | power of two |

### 7.4 `compile_commands.json`

Meson writes `build/compile_commands.json` automatically; no extra configuration. For clangd:
```bash
ln -s build/compile_commands.json .
```

---

## 8. Startup Sequence

1. `main()` receives `argc`/`argv`.
2. **Pre-scan:** `parse_app_args()` walks argv after the `--` separator; extracts `--socket`, `--nbmbufs`, `--max-flows`; stores in `struct app_cfg`. Reports error and exits non-zero on invalid values.
3. **EAL init:** `rte_eal_init(argc, argv)` — DPDK parses EAL flags (`-l`, `--in-memory`, `--no-pci`, `--vdev`), pins lcore threads, maps hugepages via anonymous `mmap`. Returns the number of consumed args.
4. **Signal handlers:** `signal_handler_install()` — `sigaction()` installs `_handler` for `SIGINT` and `SIGTERM`; `SA_RESTART` not set.
5. **Mempool:** `rte_pktmbuf_pool_create("mp", nbmbufs, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id())` — allocates hugepage-backed mbuf pool. Aborts if `nbmbufs` is not Mersenne.
6. **Flow table:** `flow_table_init(&ft, max_flows)` — `rte_zmalloc` the `flow_entry_t` pool (16 × `max_flows` bytes on hugepages); `rte_hash_create()` with `RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY`.
7. **Port init:** `port_init(port_id=0, mp, ring_size=1024)` — `rte_eth_dev_configure` (1 RX queue, 0 TX queues), `rte_eth_rx_queue_setup`, `rte_eth_dev_start`. Port 0 is the first vdev bound by `--vdev`.
8. **RX lcore launch:** `rte_eal_remote_launch(rx_loop_run, &rx_cfg, rx_lcore_id)` — lcore 1 begins its polling loop immediately; the AF_PACKET ring may be empty but the lcore spins in `rte_eth_rx_burst`.
9. **Stats server setup:** `stats_server_run()` creates the UNIX socket, `bind`s, `listen`s, sets `O_NONBLOCK`, then enters the accept loop.
10. **First packet received:** kernel delivers a frame to the AF_PACKET ring → DPDK copies into a free mbuf → `rte_eth_rx_burst` returns it → `parse_5tuple` extracts the 5-tuple → `flow_table_update` finds no existing entry → claims pool slot 0 → `rte_hash_add_key_data` inserts key → `atomic_fetch_add_explicit` sets count to 1.

---

## 9. Shutdown Sequence

1. **Signal fires:** OS delivers `SIGINT` or `SIGTERM`; `_handler()` executes `g_stop = 1`.
2. **Stats server unblocks:** the blocked `accept()` (or `usleep`) returns; the stats loop reads `g_stop == 1` and exits `stats_server_run()`. `server_teardown()` calls `close(listen_fd)` then `unlink(socket_path)` — satisfies FR-018.
3. **RX lcore unblocks:** on the next iteration's `g_stop` check, `rx_loop_run` exits its polling loop and returns `0` to the EAL scheduler.
4. **Join RX lcore:** `rte_eal_wait_lcore(rx_lcore_id)` — main blocks until lcore 1 has fully exited. Any in-flight counter increments complete before this returns.
5. **Port stop:** `rte_eth_dev_stop(port_id)` — drains in-flight RX descriptors; no new mbufs are filled after this returns.
6. **Port close:** `rte_eth_dev_close(port_id)` — releases the vdev and its resources.
7. **Flow table destroy:** `flow_table_destroy(ft)` — `rte_hash_free(ft->hash)`, `rte_free(ft->entries)`, `rte_free(ft)`.
8. **Mempool free:** `rte_mempool_free(mp)` — returns hugepage memory to EAL.
9. **EAL cleanup:** `rte_eal_cleanup()` — unmaps hugepages, joins and destroys lcore pthreads, removes EAL runtime state.
10. **Return:** `main()` returns `0` — satisfies FR-021.

---

## 10. Open Questions Resolved

| OQ | Resolution | Rationale |
|---|---|---|
| **OQ-01** Max distinct 5-tuples | **64 K (65 536)** default; overridable via `--max-flows N` (N must be a power of two for `rte_hash`). When full, new flows are silently dropped and a single `RTE_LOG_WARNING` is emitted. | 64 K × 24 bytes (`flow_entry_t`) = 1.5 MB of hugepage memory — negligible. Covers all realistic laptop test scenarios. |
| **OQ-02** Eviction policy | **None.** Table grows until `max_flows`, then drops new-flow insertions. Counters are lost on restart (C-07 permits this). | No LRU complexity needed for a portfolio demo. Restart clears counters. |
| **OQ-03** Byte counting | **Packet count only** for initial release. `flow_entry_t` layout is easily extended with `_Atomic uint64_t bytes` later. | FR-007 and FR-016 require only packet counts. Adding bytes is a one-field change. |
| **OQ-04** Filtering on `GET /counters` | **No filtering.** Full table always returned. | Simplest correct implementation; NFR-002 (100 ms) is easily met for ≤ 64 K flows with streaming JSON. |
| **OQ-05** Socket API QPS | **Main lcore handles stats.** No dedicated third lcore. Accept loop polls every 10 ms. | Portfolio demo load is negligible. Keeping 2 lcores simplifies topology and conserves cores on a laptop. |
| **OQ-06** `RESET /counters` | **Not implemented.** | Explicitly out of scope (SRS Section 6). |
| **OQ-07** Logging | **`rte_log` to `stderr`** via `RTE_LOGTYPE_USER1` at default level `RTE_LOG_INFO`. Adjustable at runtime with the standard `--log-level` EAL argument. | Native DPDK logging; zero external dependency; familiar to DPDK practitioners. |
| **OQ-08** VLAN-tagged traffic | **Out of scope.** 802.1Q frames are counted under `other_packets`. | SRS Section 6 explicitly excludes VLAN handling. |
| **OQ-09** `compile_commands.json` | **Automatic.** Meson writes `build/compile_commands.json` with no extra configuration; symlink it to the project root for clangd. | No work required; already a Meson default. |
| **OQ-10** CI environment | **Recommended:** GitHub Actions workflow using a Docker image (`ubuntu:22.04` + DPDK 23.11 installed from source or a pre-built apt overlay). Pipeline step: `meson setup build && ninja -C build`. DPDK 23.11 is not in stock Ubuntu apt repos; a Docker image with a cached DPDK install ensures reproducibility. | Flags this as a future task outside the initial implementation scope. |

---

## 11. Design Decisions & Trade-offs

| Decision | Choice | Alternative | Reason chosen |
|---|---|---|---|
| Hash table | `rte_hash` + `RW_CONCURRENCY` | `uthash`, glib | DPDK-native, NUMA-aware, SIMD hash, built-in RW concurrency, already in `libdpdk` |
| Stats lcore | Reuse lcore 0 (main) | Dedicated third lcore | Stats QPS is negligible; 2-lcore topology saves a core on laptops |
| Stop flag type | `volatile sig_atomic_t` | `_Atomic int` | POSIX idiomatic, signal-safe on all platforms, DPDK convention |
| JSON serialisation | Manual `snprintf` streaming | `cJSON`, `jansson` | No extra dependency; format is trivially simple; no heap alloc after init |
| Counter storage | Pool array indexed by hash slot | Pointer stored as hash value | O(1) pool allocation; no `malloc` after init; clean DPDK pattern |
| Eviction | None (table full → drop) | LRU | No complexity needed for demo scope; C-07 permits loss on restart |
| `accept()` blocking model | `O_NONBLOCK` + 10 ms poll | `epoll` | Simpler code path; polling overhead is negligible on a dedicated lcore |
| Response format | `{"flows":[…],"other_packets":N}` | Bare array per FR-016 | Satisfies FR-008 without a second API call; trivially backward-compatible |

---

## 12. Risks

| Risk | Mitigation |
|---|---|
| `rte_hash_iterate` snapshot is not globally atomic — a concurrent new-flow insertion may appear or not appear in the current snapshot | Acceptable for monitoring; document as an eventually-consistent snapshot in the README |
| Flow table fills (64 K cap) during high-diversity traffic — new flows silently dropped | Emit a one-time `RTE_LOG_WARNING`; expose `pool_used` in a future `GET /stats` endpoint |
| `eth_af_packet` PMD kernel-copy overhead limits throughput to ~1–2 Mpps on modern hardware | Documented in README; within NFR-001 (1 Mpps target); no SR-IOV needed for demo |
| `libdpdk.pc` not discoverable if DPDK installed in a non-standard prefix | Document `PKG_CONFIG_PATH=/path/to/dpdk/lib/pkgconfig` override in README |
| `CAP_IPC_LOCK` may be required for hugepage `mlock()` on some kernel configurations | Document in README; `--in-memory` behaviour varies by kernel; test on the target machine |
| `pad[3]` in `flow_key` not zeroed before hash insert — hash produces incorrect results for the same logical key | `parse_5tuple` must `memset(&key, 0, sizeof(key))` before populating fields; document as a required invariant in `rx_loop.c` |

---

*End of architecture document — dpdkflow v0.1.2*
