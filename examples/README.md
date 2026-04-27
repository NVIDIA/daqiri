# DAQIRI Bench Examples

Standalone benchmark applications for testing performance of DAQIRI with various configurations:

- `daqiri_bench_raw_gpudirect`: raw TX/RX with one device-memory packet segment
- `daqiri_bench_raw_hds`: raw TX/RX with header-data split
- `daqiri_bench_raw_reorder_seq`: raw RX sequence-number reorder benchmark
- `daqiri_bench_rdma`: RDMA benchmark logic (former `rdma_bench.h`)
- `daqiri_bench_socket`: TCP/UDP socket benchmark logic

Build from repository root:

```bash
cmake -S . -B build -DDAQIRI_BUILD_EXAMPLES=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_MGR="dpdk socket"
cmake --build build -j
```

Run:

```bash
./build/examples/daqiri_bench_raw_gpudirect ./build/examples/daqiri_bench_raw_tx_rx.yaml --seconds 10
./build/examples/daqiri_bench_raw_hds ./build/examples/daqiri_bench_raw_tx_rx_hds.yaml --seconds 10
./build/examples/daqiri_bench_raw_reorder_seq ./build/examples/daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml --seconds 10
./build/examples/daqiri_bench_rdma ./build/examples/daqiri_bench_rdma_tx_rx.yaml --seconds 10 --mode both
./build/examples/daqiri_bench_socket ./build/examples/daqiri_bench_socket_udp_tx_rx.yaml --seconds 10 --mode both
./build/examples/daqiri_bench_socket ./build/examples/daqiri_bench_socket_tcp_tx_rx.yaml --seconds 10 --mode both
```

Included configs:

| Config file | Benchmark |
|-------------|-----------|
| `daqiri_bench_raw_tx_rx.yaml` | `daqiri_bench_raw_gpudirect` |
| `daqiri_bench_raw_sw_loopback.yaml` | `daqiri_bench_raw_gpudirect` |
| `daqiri_bench_raw_rx_multi_q.yaml` | `daqiri_bench_raw_gpudirect` |
| `daqiri_bench_raw_tx_rx_hds.yaml` | `daqiri_bench_raw_hds` |
| `daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml` | `daqiri_bench_raw_reorder_seq` |
| `daqiri_bench_raw_tx_rx_reorder_seq_1024_cpu.yaml` | `daqiri_bench_raw_reorder_seq` |
| `daqiri_bench_raw_sw_loopback_reorder_seq_1024.yaml` | `daqiri_bench_raw_reorder_seq` |
| `daqiri_bench_raw_rx_reorder_seq_ppb.yaml` | `daqiri_bench_raw_reorder_seq` |
| `daqiri_bench_raw_rx_reorder_seq_batch.yaml` | `daqiri_bench_raw_reorder_seq` |
| `daqiri_bench_rdma_tx_rx.yaml` | `daqiri_bench_rdma` |
| `daqiri_bench_socket_udp_tx_rx.yaml` | `daqiri_bench_socket` |
| `daqiri_bench_socket_tcp_tx_rx.yaml` | `daqiri_bench_socket` |

Configs named `raw_rx_*` are RX-only. They initialize the RX path and wait for matching
external traffic; when run by themselves they may exit successfully with `0` packets. The
TX/RX reorder configs are full closed-loop examples. The CPU reorder config is a throughput
stress case, so dropped-packet counters can increase when the sender outruns CPU reorder.
