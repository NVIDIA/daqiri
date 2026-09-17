# Generate and Validate Configurations

`scripts/gen_daqiri_config.py` is the deterministic path from deployment
parameters to DAQIRI YAML. It preserves application-owned top-level sections and
writes byte-identical output for identical inputs. The generator checks its own
profile arguments; the C++ `parse_network_config` implementation used by DAQIRI
is the single authority for the emitted configuration.

The generator requires Python 3 and PyYAML. They are installed in the DAQIRI
development container. For a host Python environment:

```bash
python3 -m pip install pyyaml
```

A CMake install exposes the same entry point as
`/opt/daqiri/bin/gen_daqiri_config.py`; commands below use the source-tree path.

## Generate a raw-Ethernet pair

Supply the actual local topology rather than editing a copied YAML. This example
uses the two ports and discrete GPU on an IGX-style loopback system:

```bash
python3 scripts/gen_daqiri_config.py raw-pair \
  --tx-address 0005:03:00.0 --rx-address 0005:03:00.1 \
  --master-core 3 --engine ibverbs --memory-kind device \
  --tx-queue-cores 4 --rx-queue-cores 5 \
  --tx-worker-cores 6 --rx-worker-cores 7 \
  --eth-dst-addr 48:b0:2d:f4:04:24 \
  --ip-src-addr 1.1.1.1 --ip-dst-addr 2.2.2.2 \
  --output raw-loopback.yaml
```

`--role loopback` is the default and emits one document containing TX and RX.
For two hosts, generate one independently runnable file per role:

```bash
python3 scripts/gen_daqiri_config.py raw-pair \
  --tx-address 0000:01:00.0 --rx-address 0000:01:00.0 \
  --master-core 8 --engine dpdk --memory-kind host_pinned \
  --tx-queue-cores 17 --rx-queue-cores 18 \
  --tx-worker-cores 16 --rx-worker-cores 19 \
  --eth-dst-addr 4c:bb:47:2a:ea:ee \
  --role both --output-dir generated/raw-xhost
```

The TX file contains only the TX interface, TX memory, and `bench_tx`; the RX
file contains only the RX equivalents. Generate from the local and peer system
facts, copy the file for the remote role to that host, start RX first, and then
start TX. File transfer and remote process control intentionally remain outside
the deterministic generator.

Comma-separated core lists create multi-queue matrices. For example,
`--tx-queue-cores 16,19 --tx-worker-cores 15,6` creates two TX queues. The
generator derives memory regions, flow-to-queue routing, and benchmark entries
from the queue counts. `examples/run_spark_mq_bench.sh` uses this interface for
all four 1×1, 1×2, 2×1, and 2×2 cells.

Add one of `--transform vlan`, `--transform vxlan`, `--transform gre`, or
`--transform nvgre` to generate the corresponding raw hardware encap/decap
configuration. Transform profiles currently require one TX and one RX queue.
They also require an explicit `--engine dpdk` or `--engine ibverbs`, because the
two engines express the transform's RX match differently.

Use `--daqiri-only` when generating a production library config rather than a
benchmark input. This omits `bench_tx` and `bench_rx`.

## Generate UDP, TCP, or RoCE roles

`socket-pair` emits separate TX/client and RX/server documents. The same command
works with `--transport udp`, `tcp`, or `roce`:

Transport-specific options are checked rather than ignored: `--rx-batch-size`
and `--iterations` apply only to TCP/UDP, while `--rx-num-bufs`,
`--tx-num-bufs`, `--rx-depth`, `--tx-depth`, and `--roce-transport-mode` apply
only to RoCE.

```bash
python3 scripts/gen_daqiri_config.py socket-pair \
  --transport udp \
  --client-address 10.250.0.1 --server-address 10.250.0.2 \
  --client-port 5101 --server-port 5001 \
  --client-master-core 8 --server-master-core 8 \
  --client-rx-core 17 --client-tx-core 17 \
  --server-rx-core 16 --server-tx-core 16 \
  --client-worker-core 17 --server-worker-core 16 \
  --message-size 8000 --buffer-size 65536 \
  --num-bufs 1024 --rx-batch-size 32 \
  --role both --output-dir generated/udp
```

For RoCE, use host-pinned memory and size receive/transmit windows explicitly
when needed:

```bash
python3 scripts/gen_daqiri_config.py socket-pair \
  --transport roce \
  --client-address 10.250.0.1 --server-address 10.250.0.2 \
  --client-port 4096 --server-port 4096 \
  --client-master-core 8 --server-master-core 8 \
  --client-rx-core 18 --client-tx-core 17 \
  --server-rx-core 19 --server-tx-core 16 \
  --client-worker-core 18 --server-worker-core 19 \
  --message-size 8000000 --buffer-size 8000000 --num-bufs 128 \
  --rx-num-bufs 512 --tx-num-bufs 128 \
  --rx-depth 512 --tx-depth 128 --memory-kind host_pinned \
  --role both --output-dir generated/roce
```

`examples/run_spark_bench.sh` uses these profiles directly for its namespace
and benchmark matrix. It does not maintain or mutate Spark-specific base YAMLs.

## Render any DAQIRI configuration

The profiles cover common deployment pairs. For HDS, reorder, dynamic-flow, or
application-specific documents, `render` is the general path: it accepts either
a complete document or a bare `daqiri.cfg` mapping and emits the canonical
deterministic serialization. Existing values can be replaced with repeatable
JSON Pointer assignments; assignment values are parsed as YAML 1.2 scalars or
collections.

```bash
python3 scripts/gen_daqiri_config.py render \
  examples/daqiri_bench_raw_tx_rx.yaml \
  --set /daqiri/cfg/master_core=3 \
  --set /daqiri/cfg/interfaces/0/address=0005:03:00.0 \
  --set /daqiri/cfg/interfaces/1/address=0005:03:00.1 \
  --set /daqiri/cfg/interfaces/0/tx/queues/0/cpu_core=4 \
  --set /daqiri/cfg/interfaces/1/rx/queues/0/cpu_core=5 \
  --set /bench_tx/0/cpu_core=6 \
  --set /bench_rx/0/cpu_core=7 \
  --set /bench_tx/0/eth_dst_addr=48:b0:2d:f4:04:24 \
  --set /bench_tx/0/ip_src_addr=1.1.1.1 \
  --set /bench_tx/0/ip_dst_addr=2.2.2.2 \
  --output generated/raw.yaml
```

An override may replace only an existing path. A misspelled path fails instead
of silently adding a new key. The final document must contain concrete values;
unresolved angle-bracket placeholders are rejected during rendering.

## Authoritative validation

The generator does not maintain a second model of the DAQIRI configuration
language. After building DAQIRI, validate configurations with the parse-only
executable, which calls the same `parse_network_config` implementation used by
`daqiri_init()` without allocating packet memory or touching a NIC:

```bash
python3 scripts/check_daqiri_configs.py \
  --validator build/examples/daqiri_config_validate
python3 scripts/check_generated_configs.py \
  --validator build/examples/daqiri_config_validate
```

The first command materializes only the typed placeholders in the canonical
teaching configurations before parsing them. The second checks deterministic
generation across independent Python processes, parses every socket, RoCE, raw,
transform, multi-queue, and cross-host role configuration, and verifies focused
invalid cases are rejected. Unknown keys, required fields, types, ranges, and
semantic constraints are all owned by the C++ decoder.

Pull-request CI builds the ibverbs engines and validates all compatible
checked-in and generated configurations. The full container-publish build also
validates the explicitly DPDK configurations; both jobs invoke this same C++
decoder rather than implementing validation in Python.

## Spark verification checklist

After copying this branch to a Spark and rebuilding the container, first run the
portable generator tests and hardware-free parser checks:

```bash
python3 -m pytest
python3 scripts/check_daqiri_configs.py \
  --validator build/examples/daqiri_config_validate
python3 scripts/check_generated_configs.py \
  --validator build/examples/daqiri_config_validate
```

Then run one generated cell per transport. Bring the namespace wire loopback up
for RoCE/TCP/UDP and down for DPDK as described by each harness:

```bash
# Default namespace, physical p0-to-p1 cable
ETH_DST_ADDR="$(cat /sys/class/net/enP2p1s0f1np1/address)" \
  RUN_SECONDS=10 examples/run_spark_bench.sh dpdk smoke

# dq_wire_client / dq_wire_server namespaces
RUN_SECONDS=10 examples/run_spark_bench.sh rdma smoke
RUN_SECONDS=10 PAIRS_OVERRIDE=1 examples/run_spark_bench.sh socket-udp smoke
RUN_SECONDS=10 PAIRS_OVERRIDE=1 examples/run_spark_bench.sh socket-tcp smoke
```

Finally, exercise every raw multi-queue topology at one payload:

```bash
ETH_DST_ADDR="$(cat /sys/class/net/enP2p1s0f1np1/address)" \
  PAYLOADS=8000 RUN_SECONDS=10 examples/run_spark_mq_bench.sh
```

For each hardware run, retain both application completion output and physical
NIC counter deltas. Non-zero application counts without matching physical
counters can indicate a local shortcut rather than the intended cable path.
