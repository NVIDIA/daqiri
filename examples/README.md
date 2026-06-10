# DAQIRI Bench Examples

Standalone benchmark applications for testing performance of DAQIRI with various configurations:

- `daqiri_bench_raw_gpudirect`: raw TX/RX with one device-memory packet segment
- `daqiri_bench_raw_hds`: raw TX/RX with header-data split
- `daqiri_bench_raw_reorder_seq`: raw RX sequence-number reorder benchmark
- `daqiri_bench_raw_reorder_quantize`: raw RX sequence reorder with payload conversion
- `daqiri_example_pcap_writer`: RX pcap writer with optional GPUDirect demo TX traffic
- `daqiri_bench_rdma`: RDMA benchmark logic (former `rdma_bench.h`)
- `daqiri_bench_socket`: TCP/UDP socket benchmark logic
- `daqiri_example_gds_write`: one-shot capture that demonstrates synchronous and
  asynchronous raw or PCAP device-memory burst writes with cuFile. It includes a
  software loopback config and a hardware TX/RX config for packets that leave the NIC
  and are received back through a real port.

Build from repository root:

```bash
cmake -S . -B build -DDAQIRI_BUILD_EXAMPLES=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_MGR="dpdk socket"
cmake --build build -j
```

Build with GPUDirect Storage support for `daqiri_example_gds_write`, whose config uses GPU
RX buffers:

```bash
cmake -S . -B build -DDAQIRI_BUILD_EXAMPLES=ON -DDAQIRI_ENABLE_GDS=ON -DDAQIRI_MGR="dpdk socket"
cmake --build build -j
```

For CUDA device-memory output, the runtime must have a working cuFile/GDS stack. In
regular `nvidia-fs` mode, verify that the kernel module is loaded and the destination
storage is supported before running the example:

```bash
lsmod | grep nvidia_fs
/usr/local/cuda/gds/tools/gdscheck.py -p
```

For local NVMe, `gdscheck.py -p` should report `NVMe : Supported`. Host-memory output
paths do not require GDS and use POSIX writes instead.

Run:

```bash
./build/examples/daqiri_bench_raw_gpudirect ./build/examples/daqiri_bench_raw_tx_rx.yaml --seconds 10
./build/examples/daqiri_bench_raw_gpudirect ./build/examples/daqiri_bench_raw_tx_rx_4q.yaml --seconds 10
./build/examples/daqiri_bench_raw_hds ./build/examples/daqiri_bench_raw_tx_rx_hds.yaml --seconds 10
./build/examples/daqiri_bench_raw_reorder_seq ./build/examples/daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml --seconds 10
./build/examples/daqiri_bench_raw_reorder_quantize ./build/examples/daqiri_bench_raw_tx_rx_reorder_quantize_seq_batch.yaml --seconds 10
./build/examples/daqiri_example_pcap_writer ./build/examples/daqiri_example_pcap_writer_sw_loopback.yaml /tmp/daqiri-capture.pcap --tx
./build/examples/daqiri_bench_rdma ./build/examples/daqiri_bench_rdma_tx_rx.yaml --seconds 10 --mode both
./build/examples/daqiri_bench_socket ./build/examples/daqiri_bench_socket_udp_tx_rx.yaml --seconds 10 --mode both
./build/examples/daqiri_bench_socket ./build/examples/daqiri_bench_socket_tcp_tx_rx.yaml --seconds 10 --mode both
./build/examples/daqiri_example_gds_write ./build/examples/daqiri_example_gds_write_sw_loopback.yaml /mnt/nvme/capture packet_group_0 --mode both --format raw --offset 60
./build/examples/daqiri_example_gds_write ./build/examples/daqiri_example_gds_write_sw_loopback.yaml /mnt/nvme/capture packet_group_0 --mode both --format pcap
```

For real wire traffic, replace the PCIe, MAC, and IP placeholders in
`daqiri_example_gds_write_tx_rx.yaml`. Set `eth_dst_addr` to the MAC address of the
port that will receive the frame, and use `eth_src_addr` for the TX port MAC. The
example also enables `tx_eth_src`, so hardware TX will use the actual TX port source
MAC if the backend fills it. The generated Ethernet/IPv4/UDP headers include EtherType
`0x0800`, TTL `64`, correct IPv4 total length/checksum, UDP length/checksum, and the
configured addresses and ports.

The app-level `bench_tx.cpu_core`, `bench_rx.cpu_core`,
`socket_bench_*.cpu_core`, and `rdma_bench_*.cpu_core` fields pin the benchmark
application threads. These are separate from the DAQIRI queue `cpu_core` values that
pin the library's worker threads.

Included configs:

| Config file | Benchmark |
|-------------|-----------|
| `daqiri_bench_raw_tx_rx.yaml` | `daqiri_bench_raw_gpudirect` |
| `daqiri_bench_raw_tx_rx_4q.yaml` | `daqiri_bench_raw_gpudirect` |
| `daqiri_bench_raw_sw_loopback.yaml` | `daqiri_bench_raw_gpudirect` |
| `daqiri_example_gds_write_sw_loopback.yaml` | `daqiri_example_gds_write` |
| `daqiri_example_gds_write_tx_rx.yaml` | `daqiri_example_gds_write` |
| `daqiri_bench_raw_rx_multi_q.yaml` | `daqiri_bench_raw_gpudirect` |
| `daqiri_example_pcap_writer_sw_loopback.yaml` | `daqiri_example_pcap_writer` |
| `daqiri_example_pcap_writer_tx_rx.yaml` | `daqiri_example_pcap_writer` |
| `daqiri_bench_raw_tx_rx_hds.yaml` | `daqiri_bench_raw_hds` |
| `daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml` | `daqiri_bench_raw_reorder_seq` |
| `daqiri_bench_raw_tx_rx_reorder_seq_1024_cpu.yaml` | `daqiri_bench_raw_reorder_seq` |
| `daqiri_bench_raw_sw_loopback_reorder_seq_1024.yaml` | `daqiri_bench_raw_reorder_seq` |
| `daqiri_bench_raw_rx_reorder_seq_ppb.yaml` | `daqiri_bench_raw_reorder_seq` |
| `daqiri_bench_raw_rx_reorder_seq_batch.yaml` | `daqiri_bench_raw_reorder_seq` |
| `daqiri_bench_raw_tx_rx_reorder_quantize_seq_batch.yaml` | `daqiri_bench_raw_reorder_quantize` |
| `daqiri_bench_rdma_tx_rx.yaml` | `daqiri_bench_rdma` |
| `daqiri_bench_socket_udp_tx_rx.yaml` | `daqiri_bench_socket` |
| `daqiri_bench_socket_tcp_tx_rx.yaml` | `daqiri_bench_socket` |

Configs named `raw_rx_*` are RX-only. They initialize the RX path and wait for matching
external traffic; when run by themselves they may exit successfully with `0` packets. The
TX/RX reorder configs are full closed-loop examples. The CPU reorder config is a throughput
stress case, so dropped-packet counters can increase when the sender outruns CPU reorder.

## PCAP Writer Example

`daqiri_example_pcap_writer` is an RX-first capture example. It runs until Ctrl+C,
writes a classic Ethernet `.pcap` file as RX bursts arrive, and then closes the file so
tools such as `tcpdump -r` or Wireshark can read it. The bundled configs use device
memory for both TX and RX packet buffers so the network path uses GPUDirect. The PCAP
writer stages device packet data through pinned host memory before writing the file, so
sustained capture throughput is dictated by the device-to-host copy rate and by the
filesystem or block device accepting the writes.

The software-loopback config includes a transmitter only to make the example self
contained:

```bash
./build/examples/daqiri_example_pcap_writer \
  ./build/examples/daqiri_example_pcap_writer_sw_loopback.yaml \
  /tmp/daqiri-capture.pcap \
  --tx
```

For a tcpdump-like RX-only utility, remove the `bench_tx` block from the config or omit
`--tx`, point the `bench_rx` entry at the RX port, and leave the process running until
you press Ctrl+C:

```bash
./build/examples/daqiri_example_pcap_writer \
  ./build/examples/daqiri_example_pcap_writer_tx_rx.yaml \
  /mnt/nvme/daqiri-capture.pcap
```

If storage IO is the bottleneck, a RAM-backed filesystem can make the example run much
faster while you test the capture path. `ramfs` has no hard size limit, so it can exhaust
system memory; use it only on a controlled test machine and copy the capture out before
unmounting:

```bash
sudo mkdir -p /mnt/daqiri-pcap-ramfs
sudo mount -t ramfs ramfs /mnt/daqiri-pcap-ramfs
./build/examples/daqiri_example_pcap_writer \
  ./build/examples/daqiri_example_pcap_writer_sw_loopback.yaml \
  /mnt/daqiri-pcap-ramfs/daqiri-capture.pcap \
  --tx
sudo cp /mnt/daqiri-pcap-ramfs/daqiri-capture.pcap /mnt/nvme/
sudo umount /mnt/daqiri-pcap-ramfs
```

For a capped RAM filesystem, use `tmpfs` instead:

```bash
sudo mkdir -p /mnt/daqiri-pcap-tmpfs
sudo mount -t tmpfs -o size=16G,mode=0777 tmpfs /mnt/daqiri-pcap-tmpfs
```
