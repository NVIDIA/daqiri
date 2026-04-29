---
hide:
  - navigation
---
# Benchmarking Examples

DAQIRI provides a benchmarking application named `daqiri_bench_default` that can be used to test the performance of the networking configuration. In this section, we'll walk you through the steps needed to configure the application for your NIC for Tx and Rx, and run a loopback test between the two interfaces with a [physical SFP cable](https://www.nvidia.com/en-us/networking/interconnect/) connecting them.

Make sure to [build the DAQIRI library](../getting-started.md#building-from-source) beforehand.

!!! note "Prerequisites"

    Before running the benchmarking application, ensure your system has been fully configured per the [System Configuration](system_configuration.md) page.

## Running the DAQIRI container

If you built DAQIRI using the container approach, use the following command to launch the container with DPDK and GPU support. The host system must be fully configured (see [System Configuration](system_configuration.md)) before the container can access the NIC and GPU hardware.

```bash
docker run --rm -it --privileged \
  --runtime=nvidia \
  --network=host \
  -v /dev/hugepages:/dev/hugepages \
  daqiri:local bash
```

??? info "Explanation of container flags"

    | Flag | Purpose |
    |------|---------|
    | `--privileged` | DPDK requires raw access to NIC hardware (PCI devices, hugepage files). Also covers `/dev/infiniband` for RDMA. |
    | `--runtime=nvidia` | Makes the host GPU visible inside the container via the NVIDIA Container Toolkit |
    | `--network=host` | Shares the host network namespace so DPDK can discover the physical NIC interfaces and their PCIe topology |
    | `-v /dev/hugepages:/dev/hugepages` | Mounts the hugepage filesystem for DPDK memory allocation (`--privileged` alone does not cover mounted filesystems) |

## Update the loopback configuration

The benchmark executables and example YAML configurations are located at:

| | Binaries | YAML configs |
|---|---|---|
| **Container** | `/opt/daqiri/bin/` | `/opt/daqiri/bin/` |
| **From source** | `./build/examples/` | `./examples/` |

The fields in the YAML configs will be explained in more detail in [Understanding the Configuration File](configuration.md). For now, we'll stick to modifying the strict minimum required fields to run the application as-is on your system.

##### Identify your NIC's PCIe addresses

Retrieve the PCIe addresses of both ports of your NIC. We'll arbitrarily use the first for Tx and the second for Rx here:

=== "ibdev2netdev"

    ```bash
    sudo ibdev2netdev -v | awk '{print $1}'
    ```

=== "lspci"

    ```bash
    # `0200` is the PCI-SIG class code for NICs
    # `15b3` is the Vendor ID for Mellanox
    lspci -n | awk '$2 == "0200:" && $3 ~ /^15b3:/ {print $1}'
    ```

??? abstract "See an example output"

    ```
    0005:03:00.0
    0005:03:00.1
    ```

##### Configure the NIC for Tx and Rx

Set the NIC addresses in the `interfaces` section of the `daqiri` configuration, making sure to remove the template brackets `< >`. This configures your NIC independently of your application:

- Set the `address` field of the `tx_port` interface to one of these addresses. That interface will be able to transmit ethernet packets.
- Set the `address` field of the `rx_port` interface to the other address. This interface will be able to receive ethernet packets.

```yaml hl_lines="3 7"
interfaces:
    - name: "tx_port"
    address: <0000:00:00.0>       # The BUS address of the interface doing Tx
    tx:
        ...
    - name: "rx_port"
    address: <0000:00:00.0>       # The BUS address of the interface doing Rx
    rx:
        ...
```

???+ abstract "See an example yaml"

    ```yaml hl_lines="3 7"
    interfaces:
        - name: "tx_port"
        address: 0005:03:00.0       # The BUS address of the interface doing Tx
        tx:
            ...
        - name: "rx_port"
        address: 0005:03:00.1       # The BUS address of the interface doing Rx
        rx:
            ...
    ```

##### Configure the application

To run the benchmarking application to run a loopback on your system, you'll need to modify the `bench_tx` section which configures the application itself, to create the packet headers and direct the packets to the NIC. Make sure to remove the template brackets `< >`.

- `eth_dst_addr` with the MAC address (and not the PCIe address) of the NIC interface you want to use for Rx. You can get the MAC address of your `if_name` interface with `#!bash cat /sys/class/net/$if_name/address`:

```yaml hl_lines="4"
bench_tx:
    interface_name: "tx_port" # Name of the TX port from the daqiri config
    ...
    eth_dst_addr: <00:00:00:00:00:00> # Destination MAC address - required when Rx flow_isolation=true
    ...
```

???+ abstract "See an example yaml"

    ```yaml hl_lines="4"
    bench_tx:
        interface_name: "tx_port" # Name of the TX port from the daqiri config
        ...
        eth_dst_addr: 48:b0:2d:ee:83:ad # Destination MAC address - required when Rx flow_isolation=true
        ...
    ```

??? info "Show explanation"

    - `eth_dst_addr` - the destination ethernet MAC address - will be embedded in the packet headers by the application. This is required here because the Rx interface above has `flow_isolation: true` (explained in more details below). In that configuration, only the packets listing the adequate destination MAC address will be accepted by the Rx interface.
    - We ignore the IP fields (`ip_src_addr`, `ip_dst_addr`) for now, as we are testing on a layer 2 network by just connecting a cable between the two interfaces on our system, therefore having mock values has no impact.
    - You might have noted the lack of a `eth_src_addr` field in this `bench_tx` section. This is because the source Ethernet MAC address can be inferred automatically by the DAQIRI library from the PCIe address of the Tx interface referenced above.

## Run the loopback test

After having modified the configuration file, ensure you have connected an SFP cable between the two interfaces of your NIC, then run the application with the command below:

=== "Containerized"

    [Launch the DAQIRI container](#running-the-daqiri-container), then inside:

    ```bash
    /opt/daqiri/bin/daqiri_bench_default /opt/daqiri/bin/daqiri_bench_default_tx_rx.yaml
    ```

=== "From source"

    This assumes you have built DAQIRI and its dependencies locally on your system.

    ```bash
    sudo ./build/examples/daqiri_bench_default examples/daqiri_bench_default_tx_rx.yaml
    ```

The application will run indefinitely. You can stop it gracefully with `Ctrl-C`. You can also uncomment and set the `max_duration_ms` field in the `scheduler` section of the configuration file to limit the duration of the run automatically.

??? abstract "See an example output — PLACEHOLDER"

    !!! warning "Placeholder"

        The log output below is from the legacy Advanced Networking Operator and will be updated with DAQIRI output in a future revision. The structure and flow of the output is representative, but binary names, log prefixes (`adv_network_*`, `ANO`), and some configuration details will differ.

    ```log
    [info] [fragment.cpp:599] Loading extensions from configs...
    [info] [gxf_executor.cpp:264] Creating context
    [info] [main.cpp:35] Initializing advanced network operator
    [info] [main.cpp:40] Using ANO manager dpdk
    [info] [adv_network_rx.cpp:35] Adding output port bench_rx_out
    [info] [adv_network_rx.cpp:51] AdvNetworkOpRx::initialize()
    [info] [adv_network_common.h:607] Finished reading advanced network operator config
    [info] [adv_network_dpdk_mgr.cpp:373] Attempting to use 2 ports for high-speed network
    [info] [adv_network_dpdk_mgr.cpp:382] Setting DPDK log level to: Info
    [info] [adv_network_dpdk_mgr.cpp:402] DPDK EAL arguments: adv_net_operator --file-prefix=nwlrbbmqbh -l 3,11,9 --log-level=9 --log-level=pmd.net.mlx5:info -a 0005:03:00.0,txq_inline_max=0,dv_flow_en=2 -a 0005:03:00.1,txq_inline_max=0,dv_flow_en=2
    Log level 9 higher than maximum (8)
    EAL: Detected CPU lcores: 12
    EAL: Detected NUMA nodes: 1
    EAL: Detected shared linkage of DPDK
    EAL: Multi-process socket /var/run/dpdk/nwlrbbmqbh/mp_socket
    EAL: Selected IOVA mode 'VA'
    EAL: 1 hugepages of size 1073741824 reserved, but no mounted hugetlbfs found for that size
    EAL: Probe PCI driver: mlx5_pci (15b3:1021) device: 0005:03:00.0 (socket -1)
    mlx5_net: PCI information matches for device "mlx5_0"
    mlx5_net: enhanced MPS is enabled
    mlx5_net: port 0 MAC address is 48:B0:2D:EE:83:AC
    EAL: Probe PCI driver: mlx5_pci (15b3:1021) device: 0005:03:00.1 (socket -1)
    mlx5_net: PCI information matches for device "mlx5_1"
    mlx5_net: enhanced MPS is enabled
    mlx5_net: port 1 MAC address is 48:B0:2D:EE:83:AD
    TELEMETRY: No legacy callbacks, legacy socket not created
    [info] [adv_network_dpdk_mgr.cpp:298] Port 0 has no RX queues. Creating dummy queue.
    [info] [adv_network_dpdk_mgr.cpp:165] Adjusting buffer size to 9228 for headroom
    [info] [adv_network_dpdk_mgr.cpp:165] Adjusting buffer size to 9128 for headroom
    [info] [adv_network_dpdk_mgr.cpp:165] Adjusting buffer size to 9128 for headroom
    [info] [adv_network_mgr.cpp:116] Registering memory regions
    [info] [adv_network_mgr.cpp:178] Successfully allocated memory region MR_Unused_P0 at 0x100fa0000 type 2 with 9100 bytes (32768 elements @ 9228 bytes total 302383104)
    [info] [adv_network_mgr.cpp:178] Successfully allocated memory region Data_RX_GPU at 0xffff4fc00000 type 3 with 9000 bytes (51200 elements @ 9128 bytes total 467402752)
    [info] [adv_network_mgr.cpp:178] Successfully allocated memory region Data_TX_GPU at 0xffff33e00000 type 3 with 9000 bytes (51200 elements @ 9128 bytes total 467402752)
    [info] [adv_network_mgr.cpp:191] Finished allocating memory regions
    [info] [adv_network_dpdk_mgr.cpp:223] Successfully registered external memory for Data_TX_GPU
    [info] [adv_network_dpdk_mgr.cpp:223] Successfully registered external memory for Data_RX_GPU
    [info] [adv_network_dpdk_mgr.cpp:193] Mapped external memory descriptor for 0xffff4fc00000 to device 0
    [info] [adv_network_dpdk_mgr.cpp:193] Mapped external memory descriptor for 0xffff33e00000 to device 0
    [info] [adv_network_dpdk_mgr.cpp:193] Mapped external memory descriptor for 0xffff4fc00000 to device 1
    [info] [adv_network_dpdk_mgr.cpp:193] Mapped external memory descriptor for 0xffff33e00000 to device 1
    [info] [adv_network_dpdk_mgr.cpp:454] DPDK init (0005:03:00.0) -- RX: ENABLED TX: ENABLED
    [info] [adv_network_dpdk_mgr.cpp:464] Configuring RX queue: UNUSED_P0_Q0 (0) on port 0
    [info] [adv_network_dpdk_mgr.cpp:513] Created mempool RXP_P0_Q0_MR0 : mbufs=32768 elsize=9228 ptr=0x10041c380
    [info] [adv_network_dpdk_mgr.cpp:523] Max packet size needed for RX: 9100
    [info] [adv_network_dpdk_mgr.cpp:564] Configuring TX queue: ADC Samples (0) on port 0
    [info] [adv_network_dpdk_mgr.cpp:607] Created mempool TXP_P0_Q0_MR0 : mbufs=51200 elsize=9000 ptr=0x100c1fc00
    [info] [adv_network_dpdk_mgr.cpp:621] Max packet size needed with TX: 9100
    [info] [adv_network_dpdk_mgr.cpp:632] Setting port config for port 0 mtu:9082
    [info] [adv_network_dpdk_mgr.cpp:663] Initializing port 0 with 1 RX queues and 1 TX queues...
    mlx5_net: port 0 Tx queues number update: 0 -> 1
    mlx5_net: port 0 Rx queues number update: 0 -> 1
    [info] [adv_network_dpdk_mgr.cpp:679] Successfully configured ethdev
    [info] [adv_network_dpdk_mgr.cpp:689] Successfully set descriptors to 8192/8192
    [info] [adv_network_dpdk_mgr.cpp:704] Port 0 not in isolation mode
    [info] [adv_network_dpdk_mgr.cpp:713] Setting up port:0, queue:0, Num scatter:1 pool:0x10041c380
    [info] [adv_network_dpdk_mgr.cpp:734] Successfully setup RX port 0 queue 0
    [info] [adv_network_dpdk_mgr.cpp:756] Successfully set up TX queue 0/0
    [info] [adv_network_dpdk_mgr.cpp:761] Enabling promiscuous mode for port 0
    mlx5_net: [mlx5dr_cmd_query_caps]: Failed to query wire port regc value
    mlx5_net: port 0 Rx queues number update: 1 -> 1
    [info] [adv_network_dpdk_mgr.cpp:775] Successfully started port 0
    [info] [adv_network_dpdk_mgr.cpp:778] Port 0, MAC address: 48:B0:2D:EE:83:AC
    [info] [adv_network_dpdk_mgr.cpp:1111] Applying tx_eth_src offload for port 0
    [info] [adv_network_dpdk_mgr.cpp:454] DPDK init (0005:03:00.1) -- RX: ENABLED TX: DISABLED
    [info] [adv_network_dpdk_mgr.cpp:464] Configuring RX queue: Data (0) on port 1
    [info] [adv_network_dpdk_mgr.cpp:513] Created mempool RXP_P1_Q0_MR0 : mbufs=51200 elsize=9128 ptr=0x125a5b940
    [info] [adv_network_dpdk_mgr.cpp:523] Max packet size needed for RX: 9000
    [info] [adv_network_dpdk_mgr.cpp:621] Max packet size needed with TX: 9000
    [info] [adv_network_dpdk_mgr.cpp:632] Setting port config for port 1 mtu:8982
    [info] [adv_network_dpdk_mgr.cpp:663] Initializing port 1 with 1 RX queues and 0 TX queues...
    mlx5_net: port 1 Rx queues number update: 0 -> 1
    [info] [adv_network_dpdk_mgr.cpp:679] Successfully configured ethdev
    [info] [adv_network_dpdk_mgr.cpp:689] Successfully set descriptors to 8192/8192
    [info] [adv_network_dpdk_mgr.cpp:701] Port 1 in isolation mode
    [info] [adv_network_dpdk_mgr.cpp:713] Setting up port:1, queue:0, Num scatter:1 pool:0x125a5b940
    [info] [adv_network_dpdk_mgr.cpp:734] Successfully setup RX port 1 queue 0
    [info] [adv_network_dpdk_mgr.cpp:764] Not enabling promiscuous mode on port 1 since flow isolation is enabled
    mlx5_net: [mlx5dr_cmd_query_caps]: Failed to query wire port regc value
    mlx5_net: port 1 Rx queues number update: 1 -> 1
    [info] [adv_network_dpdk_mgr.cpp:775] Successfully started port 1
    [info] [adv_network_dpdk_mgr.cpp:778] Port 1, MAC address: 48:B0:2D:EE:83:AD
    [info] [adv_network_dpdk_mgr.cpp:790] Adding RX flow ADC Samples
    [info] [adv_network_dpdk_mgr.cpp:998] Adding IPv4 length match for 1050
    [info] [adv_network_dpdk_mgr.cpp:1018] Adding UDP port match for src/dst 4096/4096
    [info] [adv_network_dpdk_mgr.cpp:814] Setting up RX burst pool with 8191 batches of size 81920
    [info] [adv_network_dpdk_mgr.cpp:833] Setting up RX burst pool with 8191 batches of size 20480
    [info] [adv_network_dpdk_mgr.cpp:875] Setting up TX ring TX_RING_P0_Q0
    [info] [adv_network_dpdk_mgr.cpp:901] Setting up TX burst pool TX_BURST_POOL_P0_Q0 with 10240 pointers at 0x125a0d4c0
    [info] [adv_network_dpdk_mgr.cpp:1186] Config validated successfully
    [info] [adv_network_dpdk_mgr.cpp:1199] Starting advanced network workers
    [info] [adv_network_dpdk_mgr.cpp:1278] Flushing packet on port 1
    [info] [adv_network_dpdk_mgr.cpp:1478] Starting RX Core 9, port 1, queue 0, socket 0
    [info] [adv_network_dpdk_mgr.cpp:1268] Done starting workers
    [info] [default_bench_op_tx.h:79] AdvNetworkingBenchDefaultTxOp::initialize()
    [info] [adv_network_dpdk_mgr.cpp:1637] Starting TX Core 11, port 0, queue 0 socket 0 using burst pool 0x125a0d4c0 ring 0x127690740
    [info] [default_bench_op_tx.h:113] Initialized 4 streams and events
    [info] [default_bench_op_tx.h:130] AdvNetworkingBenchDefaultTxOp::initialize() complete
    [info] [default_bench_op_rx.h:67] AdvNetworkingBenchDefaultRxOp::initialize()
    [info] [gxf_executor.cpp:1797] creating input IOSpec named 'burst_in'
    [info] [default_bench_op_rx.h:104] AdvNetworkingBenchDefaultRxOp::initialize() complete
    [info] [adv_network_tx.cpp:46] AdvNetworkOpTx::initialize()
    [info] [gxf_executor.cpp:1797] creating input IOSpec named 'burst_in'
    [info] [adv_network_common.h:607] Finished reading advanced network operator config
    [info] [gxf_executor.cpp:2208] Activating Graph...
    [info] [gxf_executor.cpp:2238] Running Graph...
    [info] [multi_thread_scheduler.cpp:300] MultiThreadScheduler started worker thread [pool name: default_pool, thread uid: 0]
    [info] [multi_thread_scheduler.cpp:300] MultiThreadScheduler started worker thread [pool name: default_pool, thread uid: 1]
    [info] [multi_thread_scheduler.cpp:300] MultiThreadScheduler started worker thread [pool name: default_pool, thread uid: 2]
    [info] [gxf_executor.cpp:2240] Waiting for completion...
    [info] [multi_thread_scheduler.cpp:300] MultiThreadScheduler started worker thread [pool name: default_pool, thread uid: 3]
    [info] [multi_thread_scheduler.cpp:300] MultiThreadScheduler started worker thread [pool name: default_pool, thread uid: 4]
    ^C[info] [multi_thread_scheduler.cpp:636] Stopping multithread scheduler
    [info] [multi_thread_scheduler.cpp:694] Stopping all async jobs
    [info] [multi_thread_scheduler.cpp:218] Dispatcher thread has stopped checking jobs
    [info] [multi_thread_scheduler.cpp:679] Waiting to join all async threads
    [info] [multi_thread_scheduler.cpp:316] Worker Thread [pool name: default_pool, thread uid: 1] exiting.
    [info] [multi_thread_scheduler.cpp:702] *********************** DISPATCHER EXEC TIME : 476345.364000 ms

    [info] [multi_thread_scheduler.cpp:316] Worker Thread [pool name: default_pool, thread uid: 0] exiting.
    [info] [multi_thread_scheduler.cpp:316] Worker Thread [pool name: default_pool, thread uid: 3] exiting.
    [info] [multi_thread_scheduler.cpp:371] Event handler thread exiting.
    [info] [multi_thread_scheduler.cpp:703] *********************** DISPATCHER WAIT TIME : 47339.961000 ms

    [info] [multi_thread_scheduler.cpp:704] *********************** DISPATCHER COUNT : 197630449

    [info] [multi_thread_scheduler.cpp:316] Worker Thread [pool name: default_pool, thread uid: 2] exiting.
    [info] [multi_thread_scheduler.cpp:705] *********************** WORKER EXEC TIME : 983902.800000 ms

    [info] [multi_thread_scheduler.cpp:706] *********************** WORKER WAIT TIME : 1634522.159000 ms

    [info] [multi_thread_scheduler.cpp:707] *********************** WORKER COUNT : 11817369

    [info] [multi_thread_scheduler.cpp:316] Worker Thread [pool name: default_pool, thread uid: 4] exiting.
    [info] [multi_thread_scheduler.cpp:688] All async worker threads joined, deactivating all entities
    [info] [adv_network_rx.cpp:46] AdvNetworkOpRx::stop()
    [info] [adv_network_dpdk_mgr.cpp:1928] DPDK ANO shutdown called 2
    [info] [adv_network_tx.cpp:41] AdvNetworkOpTx::stop()
    [info] [adv_network_dpdk_mgr.cpp:1928] DPDK ANO shutdown called 1
    [info] [adv_network_dpdk_mgr.cpp:1133] Port 0:
    [info] [adv_network_dpdk_mgr.cpp:1135]  - Received packets:    0
    [info] [adv_network_dpdk_mgr.cpp:1136]  - Transmit packets:    6005066864
    [info] [adv_network_dpdk_mgr.cpp:1137]  - Received bytes:      0
    [info] [adv_network_dpdk_mgr.cpp:1138]  - Transmit bytes:      6389391347584
    [info] [adv_network_dpdk_mgr.cpp:1139]  - Missed packets:      0
    [info] [adv_network_dpdk_mgr.cpp:1140]  - Errored packets:     0
    [info] [adv_network_dpdk_mgr.cpp:1141]  - RX out of buffers:   0
    [info] [adv_network_dpdk_mgr.cpp:1143]    ** Extended Stats **
    [info] [adv_network_dpdk_mgr.cpp:1173]       tx_good_packets:          6005070000
    [info] [adv_network_dpdk_mgr.cpp:1173]       tx_good_bytes:            6389394480000
    [info] [adv_network_dpdk_mgr.cpp:1173]       tx_q0_packets:            6005070000
    [info] [adv_network_dpdk_mgr.cpp:1173]       tx_q0_bytes:              6389394480000
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_multicast_bytes:               9589
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_multicast_packets:             22
    [info] [adv_network_dpdk_mgr.cpp:1173]       tx_unicast_bytes:         6389394480000
    [info] [adv_network_dpdk_mgr.cpp:1173]       tx_multicast_bytes:               9589
    [info] [adv_network_dpdk_mgr.cpp:1173]       tx_unicast_packets:               6005070000
    [info] [adv_network_dpdk_mgr.cpp:1173]       tx_multicast_packets:             22
    [info] [adv_network_dpdk_mgr.cpp:1173]       tx_phy_packets:           6005070022
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_phy_packets:           24
    [info] [adv_network_dpdk_mgr.cpp:1173]       tx_phy_bytes:             6413414769677
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_phy_bytes:             9805
    [info] [adv_network_dpdk_mgr.cpp:1133] Port 1:
    [info] [adv_network_dpdk_mgr.cpp:1135]  - Received packets:    6004323692
    [info] [adv_network_dpdk_mgr.cpp:1136]  - Transmit packets:    0
    [info] [adv_network_dpdk_mgr.cpp:1137]  - Received bytes:      6388600255072
    [info] [adv_network_dpdk_mgr.cpp:1138]  - Transmit bytes:      0
    [info] [adv_network_dpdk_mgr.cpp:1139]  - Missed packets:      746308
    [info] [adv_network_dpdk_mgr.cpp:1140]  - Errored packets:     0
    [info] [adv_network_dpdk_mgr.cpp:1141]  - RX out of buffers:   5047027287
    [info] [adv_network_dpdk_mgr.cpp:1143]    ** Extended Stats **
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_good_packets:          6004323692
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_good_bytes:            6388600255072
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_missed_errors:         746308
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_mbuf_allocation_errors:                5047027287
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_q0_packets:            6004323692
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_q0_bytes:              6388600255072
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_q0_errors:             5047027287
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_unicast_bytes:         6389394480000
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_multicast_bytes:               9589
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_unicast_packets:               6005070000
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_multicast_packets:             22
    [info] [adv_network_dpdk_mgr.cpp:1173]       tx_multicast_bytes:               9589
    [info] [adv_network_dpdk_mgr.cpp:1173]       tx_multicast_packets:             22
    [info] [adv_network_dpdk_mgr.cpp:1173]       tx_phy_packets:           24
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_phy_packets:           6005070022
    [info] [adv_network_dpdk_mgr.cpp:1173]       tx_phy_bytes:             9805
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_phy_bytes:             6413414769677
    [info] [adv_network_dpdk_mgr.cpp:1173]       rx_out_of_buffer:         746308
    [info] [adv_network_dpdk_mgr.cpp:1935] ANO DPDK manager shutting down
    [info] [adv_network_dpdk_mgr.cpp:1622] Total packets received by application (port/queue 1/0): 6004323692
    [info] [adv_network_dpdk_mgr.cpp:1698] Total packets transmitted by application (port/queue 0/0): 6005070000
    [info] [multi_thread_scheduler.cpp:645] Multithread scheduler stopped.
    [info] [multi_thread_scheduler.cpp:664] Multithread scheduler finished.
    [info] [gxf_executor.cpp:2243] Deactivating Graph...
    [info] [multi_thread_scheduler.cpp:491] TOTAL EXECUTION TIME OF SCHEDULER : 523694.460857 ms

    [info] [gxf_executor.cpp:2251] Graph execution finished.
    [info] [adv_network_dpdk_mgr.cpp:1928] DPDK ANO shutdown called 0
    [info] [default_bench_op_tx.h:51] ANO benchmark TX op shutting down
    [info] [default_bench_op_rx.h:56] Finished receiver with 6388570603520/6004295680 bytes/packets received and 0 packets dropped
    [info] [default_bench_op_rx.h:61] ANO benchmark RX op shutting down
    [info] [default_bench_op_rx.h:108] AdvNetworkingBenchDefaultRxOp::freeResources() start
    [info] [default_bench_op_rx.h:116] AdvNetworkingBenchDefaultRxOp::freeResources() complete
    [info] [gxf_executor.cpp:294] Destroying context
    ```

To inspect the speed the data is moving through the NIC, run `mlnx_perf` on one of the interfaces in a separate terminal, concurrently with the application running:

```bash
sudo mlnx_perf -i $if_name
```

??? abstract "See an example output"

    On IGX with RTX A6000, we are able to hit close to the 100 Gbps linerate with this configuration:
    ```log
      rx_vport_unicast_packets: 11,614,900
        rx_vport_unicast_bytes: 12,358,253,600 Bps   = 98,866.2 Mbps
                rx_packets_phy: 11,614,847
                  rx_bytes_phy: 12,404,657,664 Bps   = 99,237.26 Mbps
     rx_1024_to_1518_bytes_phy: 11,614,936
                rx_prio0_bytes: 12,404,738,832 Bps   = 99,237.91 Mbps
              rx_prio0_packets: 11,614,923
    ```

??? tip "Troubleshooting"

    ??? failure "Cannot create HWS action since HWS is not supported"

        Example error:

        ```log
        mlx5_net: [mlx5dr_action_create_generic_bulk]: Cannot create HWS action since HWS is not supported
        mlx5_net: Failed to start port 0 0005:03:00.0: fail to configure port
        [CRITICAL] Cannot start device err=-95, port=0
        ```

        The DPDK backend uses Hardware Steering (HWS) via the `dv_flow_en=2` mlx5 device argument. HWS requires compatible versions of both the NIC firmware and the host's MLNX_OFED kernel modules. Per the [DPDK mlx5 documentation](https://doc.dpdk.org/guides/nics/mlx5.html), the minimum requirements are ConnectX-6 Dx or later with firmware `xx.35.1012`+, but the host's OFED/kernel driver must also support the HWS features expected by the DPDK version in use.

        Check your OFED and firmware versions:

        ```bash
        cat /sys/module/mlx5_core/version   # Host OFED kernel module version
        ethtool -i <interface_name> | grep firmware  # NIC firmware version
        ```

        To resolve, update your NIC firmware and OFED drivers, or contact the DAQIRI team for guidance on compatible version combinations.

    ??? failure "EAL: failed to parse device"

        Make sure to set valid PCIe addresses in the `address` fields in `interfaces`, per [instructions above](#configure-the-nic-for-tx-and-rx).

    ??? failure "Invalid MAC address format"

        Make sure to set a valid MAC address in the `eth_dst_addr` field in `bench_tx`, per [instructions above](#configure-the-application).

    ??? failure "mlx5_common: Fail to create MR for address [...] Could not DMA map EXT memory"

        Example error:

        ```log
        mlx5_common: Fail to create MR for address (0xffff2fc00000)
        mlx5_common: Device 0005:03:00.0 unable to DMA map
        [critical] [adv_network_dpdk_mgr.cpp:188] Could not DMA map EXT memory: -1 err=Invalid argument
        [critical] [adv_network_dpdk_mgr.cpp:430] Failed to map MRs
        ```

        [Make sure that `nvidia-peermem` is loaded](system_configuration.md#enable-gpudirect).

    ??? failure "EAL: Couldn't get fd on hugepage file [..] error allocating rte services array"

        Example error:

        ```log
        EAL: get_seg_fd(): open '/mnt/huge/nwlrbbmqbhmap_0' failed: Permission denied
        EAL: Couldn't get fd on hugepage file
        EAL: error allocating rte services array
        EAL: FATAL: rte_service_init() failed
        EAL: rte_service_init() failed
        ```

        Ensure you run as root, using `sudo`.

    ??? failure "EAL: Cannot get hugepage information."

        ```log
        EAL: x hugepages of size x reserved, no mounted hugetlbfs found for that size
        ```

        Ensure your [hugepages are mounted](system_configuration.md#step-4-enable-huge-pages).

        ```log
        EAL: No free x kB hugepages reported on node 0
        ```

        - Ensure you have [allocated hugepages](system_configuration.md#step-4-enable-huge-pages).
        - If you have already, check if they are any free left with `grep Huge /proc/meminfo`.

            ??? abstract "See an example output"

                No more space here!

                ```
                HugePages_Total:       2
                HugePages_Free:        0
                HugePages_Rsvd:        0
                HugePages_Surp:        0
                Hugepagesize:    1048576 kB
                Hugetlb:         2097152 kB
                ```

        - If not, you can delete dangling hugepages under your hugepage mount point. That happens when your previous application run crashes.

            ```bash
            sudo rm -rf /dev/hugepages/* # default mount point
            sudo rm -rf /mnt/huge/*      # custom mount point
            ```

    ??? failure "Could not allocate x MB of GPU memory [...] Failed to allocate GPU memory"

        Check your GPU utilization:

        ```bash
        nvidia-smi pmon -c 1
        ```

        You might need to kill some of the listed processes to free up GPU VRAM.

---
**Previous:** [System Configuration](system_configuration.md)  
**Next:** [Understanding the Configuration File](configuration.md) — deep dive into the YAML parameters
