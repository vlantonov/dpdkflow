# dpdkflow

`dpdkflow` is a DPDK 23.11 LTS portfolio application that attaches to a Linux network interface via the `eth_af_packet` virtual PMD (no SR-IOV NIC required), captures all incoming Ethernet frames on a dedicated polling lcore, extracts the IPv4 5-tuple (source IP, destination IP, source port, destination port, IP protocol) from each frame, and maintains per-flow packet counters in a `rte_hash` table backed by hugepage memory. Live counters are exposed on demand over a UNIX domain socket as a JSON response; non-IPv4 frames are accumulated in a separate aggregate counter.

## Requirements

| Dependency | Version |
|---|---|
| DPDK | 23.11 LTS (discoverable via `pkg-config libdpdk`) |
| Meson | ≥ 1.1 |
| GCC | ≥ 11, **or** Clang ≥ 14 |
| Linux kernel | ≥ 4.15 (AF_PACKET TPACKET_V3 required by DPDK `eth_af_packet`) |
| Hugepages | ≥ 64 × 2 MiB pages pre-allocated (see below) |

## Hugepage Setup (one-time, as root)

```bash
echo 64 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

## Build

```bash
meson setup build
ninja -C build
```

The binary is placed at `build/dpdkflow`.

## Running

```bash
sudo ./build/dpdkflow \
  -l 0-1 --in-memory --no-pci \
  --vdev "net_af_packet0,iface=eth0" \
  -- \
  --socket /tmp/dpdkflow.sock
```

Replace `eth0` with the interface you want to monitor. The `--in-memory` EAL flag avoids writing DPDK runtime files that require elevated permissions.

### Application arguments (after `--`)

| Argument | Default | Description |
|---|---|---|
| `--socket <path>` | `/tmp/dpdkflow.sock` | UNIX domain socket path |
| `--nbmbufs <N>` | `8191` | Mbuf pool size (must be 2ⁿ − 1) |
| `--max-flows <N>` | `65536` | Maximum distinct 5-tuples (must be a power of two) |

## Querying Counters

```bash
echo "GET /counters" | nc -U /tmp/dpdkflow.sock
```

Example response:

```json
{"flows":[{"src_ip":"192.168.1.10","dst_ip":"93.184.216.34","proto":6,"src_port":54321,"dst_port":80,"packets":42}],"other_packets":7}
```

Send `SIGINT` or `Ctrl-C` to stop the application gracefully; it will release all DPDK resources and remove the socket file before exiting.

## Running Tests

The unit tests cover DPDK-independent logic (struct layout and IP formatting) and require only a C17 compiler — no hugepages or hardware needed:

```bash
meson setup build
meson test -C build
```

## Architecture

See [docs/design/architecture.md](docs/design/architecture.md) for the full component diagram and per-module breakdown.

## Licence

BSD-3-Clause — see [LICENSE](LICENSE).
