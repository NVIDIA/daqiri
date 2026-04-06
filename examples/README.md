# DAQIRI Bench Examples

Standalone benchmark applications for testing performance of DAQIRI with various configurations:

- `daqiri_bench_default`: default TX/RX benchmark logic (former `default_bench_op_tx.h` / `default_bench_op_rx.h`)
- `daqiri_bench_rdma`: RDMA benchmark logic (former `rdma_bench.h`)

Build from repository root:

```bash
cmake -S . -B build -DDAQIRI_BUILD_EXAMPLES=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_MGR="dpdk rdma"
cmake --build build -j
```

Run:

```bash
./build/examples/daqiri_bench_default ./build/examples/daqiri_bench_default_tx_rx.yaml --seconds 10
./build/examples/daqiri_bench_rdma ./build/examples/daqiri_bench_rdma_tx_rx.yaml --seconds 10 --mode both
```

Included configs:

- `daqiri_bench_default_tx_rx.yaml`
- `daqiri_bench_default_tx_rx_hds.yaml`
- `daqiri_bench_default_sw_loopback.yaml`
- `daqiri_bench_default_rx_multi_q.yaml`
- `daqiri_bench_rdma_tx_rx.yaml`
