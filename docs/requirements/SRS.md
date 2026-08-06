# Software Requirements Specification — dpdkflow

**Version:** 0.1.0  
**Date:** 2026-08-06  
**Status:** Draft — awaiting System Architect review  

---

## 1. Purpose & Scope

### 1.1 Purpose

`dpdkflow` is a DPDK 23.11 LTS sample application that performs per-5-tuple packet counting at line rate on a developer laptop. It is a portfolio project demonstrating competency in high-performance networking (DPDK lcore/queue model, hugepages, mempools) and systems programming in C.

### 1.2 What the Application Does

- Captures packets from a Linux network interface using the `eth_af_packet` virtual device (no SR-IOV NIC required).
- Extracts the 5-tuple (source IP, destination IP, source port, destination port, IP protocol) from each packet.
- Maintains per-5-tuple packet counters in a lock-minimised data structure.
- Exposes the live counters over a UNIX domain socket as JSON on demand (poll/query model).
- Shuts down gracefully on `SIGINT` or `SIGTERM`.

### 1.3 What the Application Does NOT Do

- Does not modify, forward, drop, or inject packets.
- Does not perform deep-packet inspection beyond the 5-tuple header fields.
- Does not provide a persistent store, database, or file export of counters.
- Does not support Windows, macOS, or non-Linux platforms.
- Does not bundle or vendor the DPDK source tree.
- Does not implement a push/streaming model on the socket API (stretch goal).

---

## 2. Stakeholders & Use Cases

### 2.1 Stakeholders

| Stakeholder | Interest |
|---|---|
| Developer (author) | Portfolio demonstration; learn DPDK internals |
| Interviewer / reviewer | Verify DPDK, C networking, and systems skills |
| CI pipeline | Build-time verification; functional smoke tests |

### 2.2 Use Cases

| ID | Name | Actor | Summary |
|---|---|---|---|
| UC-01 | Start and capture | Developer | Launch `dpdkflow` against a named interface; packet counting begins immediately |
| UC-02 | Query counters | Client tool (e.g., `curl`, custom client) | Connect to the UNIX socket and request all current 5-tuple counters as JSON |
| UC-03 | Graceful stop | Developer / CI | Send `SIGINT` or `SIGTERM`; application drains in-flight work and exits cleanly |
| UC-04 | Benchmark throughput | Developer | Inject traffic via a software traffic generator and observe ≥ 1 Mpps on a single RX lcore |

---

## 3. Functional Requirements

### 3.1 Packet Capture

| ID | Requirement |
|---|---|
| FR-001 | The application shall attach to exactly one Ethernet interface, identified by name on the command line, using the DPDK `eth_af_packet` virtual device. |
| FR-002 | The application shall create at least one RX queue on the vdev with a configurable ring size (default: 1024 descriptors). |
| FR-003 | The application shall configure a mempool of configurable size (default: 8192 mbufs) from hugepage-backed memory for packet storage. |
| FR-004 | The application shall use DPDK's lcore abstraction: a dedicated lcore shall perform burst RX (`rte_eth_rx_burst`) in a polling loop. |

### 3.2 5-Tuple Extraction and Counting

| ID | Requirement |
|---|---|
| FR-005 | The application shall parse each received packet's Ethernet header to determine the EtherType. |
| FR-006 | For EtherType `0x0800` (IPv4), the application shall extract: source IP address, destination IP address, IP protocol number, source port (TCP/UDP), and destination port (TCP/UDP). For protocols without ports (e.g., ICMP), source and destination port fields shall be recorded as `0`. |
| FR-007 | The application shall maintain a per-5-tuple packet counter, incrementing atomically (or lock-protected) on each matching packet. |
| FR-008 | Non-IPv4 frames (ARP, IPv6, etc.) shall be counted under a single "other" aggregate counter and not broken down by 5-tuple. |
| FR-009 | _(Stretch)_ For EtherType `0x86DD` (IPv6), the application shall extract the IPv6 5-tuple (128-bit src/dst addresses, next header protocol, src/dst ports) and maintain per-5-tuple counters with the same semantics as FR-006–FR-007. |

### 3.3 Thread / Lcore Model

| ID | Requirement |
|---|---|
| FR-010 | The application shall run packet capture (RX burst, 5-tuple extraction, counter update) exclusively on one or more dedicated RX lcores. |
| FR-011 | Counter reads for the JSON API shall execute on a separate lcore (or the main lcore), never blocking the RX lcore. |
| FR-012 | The mapping of lcores to roles (RX vs. stats/API) shall be configurable via command-line arguments passed after DPDK's EAL `--` separator. |

### 3.4 UNIX Socket JSON API

| ID | Requirement |
|---|---|
| FR-013 | The application shall create a UNIX domain socket at a configurable path (default: `/tmp/dpdkflow.sock`) and listen for client connections on startup. |
| FR-014 | The application shall accept one client connection at a time (sequential, not concurrent). |
| FR-015 | Upon receiving a newline-terminated request string `GET /counters\n`, the application shall respond with a JSON document listing every observed 5-tuple and its current packet count, then close the connection. |
| FR-016 | The JSON response format shall be an array of objects, each containing keys: `src_ip`, `dst_ip`, `proto`, `src_port`, `dst_port`, `packets`. |
| FR-017 | Upon receiving an unrecognised request, the application shall respond with `{"error":"unknown request"}\n` and close the connection. |
| FR-018 | The UNIX socket file shall be removed on graceful shutdown. |

### 3.5 Graceful Shutdown

| ID | Requirement |
|---|---|
| FR-019 | The application shall install signal handlers for `SIGINT` and `SIGTERM`. |
| FR-020 | On receipt of either signal, the application shall: stop the RX polling loop, flush any in-progress counter updates, close and remove the UNIX socket, release the mempool, stop and close all DPDK ports, and call `rte_eal_cleanup()` before exiting. |
| FR-021 | The application shall exit with status code `0` on clean shutdown and non-zero on fatal error. |

### 3.6 Hugepage and Mempool Configuration

| ID | Requirement |
|---|---|
| FR-022 | The application shall require hugepages to be pre-allocated by the operator before launch (it shall not attempt to allocate hugepages itself). |
| FR-023 | The application shall configure the DPDK EAL with `--in-memory` to avoid writing DPDK runtime files that require elevated permissions beyond `CAP_NET_RAW`. |
| FR-024 | The number of mbufs in the mempool shall be user-configurable via a command-line argument; the value shall be validated as a power of two minus one (Mersenne form required by DPDK). |

---

## 4. Non-Functional Requirements

| ID | Requirement |
|---|---|
| NFR-001 | **Throughput baseline:** The application shall sustain ≥ 1 000 000 packets per second (1 Mpps) on a single RX lcore processing minimum-size (64-byte) Ethernet frames, as measured by a software traffic generator on the same machine via a veth or AF_PACKET loopback setup. |
| NFR-002 | **Latency of counter reads:** A `GET /counters` response shall be returned within 100 ms of the client request under normal operating conditions. |
| NFR-003 | **Platform:** Linux only; minimum kernel version 4.15 (required by `AF_PACKET` TPACKET_V3 and DPDK 23.11). |
| NFR-004 | **DPDK version:** DPDK 23.11 LTS (exact minor patch TBD); no other DPDK version is supported. |
| NFR-005 | **Build system:** The project shall be built with Meson ≥ 1.1 and locate DPDK via `pkg-config` (`libdpdk`). DPDK source shall not be vendored or submoduled. |
| NFR-006 | **Compiler:** GCC ≥ 11 or Clang ≥ 14; C17 standard (`-std=c17`). |
| NFR-007 | **Privilege model:** No `sudo` or `CAP_SYS_ADMIN` shall be required at runtime beyond hugepage setup. The binary shall run with `CAP_NET_RAW` (granted via `setcap` or the invoking shell). |
| NFR-008 | **Memory:** The application shall not dynamically allocate heap memory outside of DPDK mempools after initialisation completes. |
| NFR-009 | **Portability of build:** A fresh `meson setup build && ninja -C build` on a machine with DPDK 23.11 installed shall produce a working binary with no manual path configuration. |
| NFR-010 | **Licence:** Source files shall be licensed under BSD-3-Clause, consistent with the DPDK project's own licence. |

---

## 5. Constraints & Assumptions

| # | Constraint / Assumption |
|---|---|
| C-01 | The target machine is a Linux developer laptop (x86-64); ARM64 support is a stretch goal. |
| C-02 | At least 256 MB of 2 MB hugepages (128 pages) must be pre-configured by the operator (`/sys/kernel/mm/hugepages/`). |
| C-03 | DPDK 23.11 is installed system-wide or in a prefix discoverable by `pkg-config` (e.g., `/usr/local`). |
| C-04 | The `eth_af_packet` PMD is compiled into the installed DPDK (it is enabled by default in upstream builds). |
| C-05 | The network interface passed on the command line exists and is up before the application is launched. |
| C-06 | Only one instance of `dpdkflow` runs per host at a time (single DPDK primary process). |
| C-07 | The 5-tuple counter table is held entirely in process memory; loss of counters on restart is acceptable. |
| C-08 | IPv4 packets only are required for the initial release; IPv6 is a labelled stretch goal (FR-009). |

---

## 6. Out of Scope

- Packet forwarding, modification, or injection.
- IPv6 support in the initial release (tracked as FR-009 stretch goal).
- Persistent storage or export of counters to disk/database.
- Distributed or multi-process DPDK secondary-process architectures.
- Windows, macOS, BSD, or container-specific build targets.
- A graphical or web-based dashboard (the JSON API is the sole interface).
- Push/streaming counter updates over the socket (only pull/query is required).
- SR-IOV or physical NIC PMDs (only `eth_af_packet` vdev is in scope).
- Unit-test framework integration (GoogleTest/Catch2) — this is a functional demo; basic smoke testing in CI is sufficient.
- TLS/authentication on the UNIX socket.

---

## 7. Open Questions

| ID | Question | Impact if unresolved |
|---|---|---|
| OQ-01 | What is the maximum number of distinct 5-tuples the counter table must support (e.g., 64 K, 1 M)? Affects data structure choice and memory sizing. | NFR-008, FR-007 |
| OQ-02 | Should the counter table evict old entries (LRU, time-based)? Or grow unboundedly until restart? | FR-007, FR-015 |
| OQ-03 | Is per-5-tuple byte counting (in addition to packet counting) required, or is packet count sufficient? | FR-007, FR-016 |
| OQ-04 | Should `GET /counters` support filtering (e.g., by IP prefix or protocol), or always return the full table? | FR-015, NFR-002 |
| OQ-05 | What is the expected maximum socket API request rate (queries per second)? This determines whether a separate stats lcore is needed or the main lcore suffices. | FR-011 |
| OQ-06 | Is a `RESET /counters` API command (zero all counts without restart) required? | FR-013–FR-017 |
| OQ-07 | Should the application log to `stderr`, `syslog`, or a file? What is the minimum log level for production use? | Operational |
| OQ-08 | Is VLAN-tagged traffic (802.1Q) in scope? If so, should the VLAN tag be part of the 5-tuple or stripped transparently? | FR-005–FR-006 |
| OQ-09 | Must the meson build also produce a `compile_commands.json` for IDE/clangd integration, or is that optional? | NFR-005, NFR-009 |
| OQ-10 | Is there a CI environment (GitHub Actions, local runner) where the build must pass? If so, is DPDK 23.11 available in that environment, or must the CI use a Docker image? | NFR-005 |

---

*End of SRS — dpdkflow v0.1.0*
