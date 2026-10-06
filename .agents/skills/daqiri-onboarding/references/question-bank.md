# Question Bank

Use these questions selectively. Ask enough to choose a valid path, but do not dump the whole list.

## Goal

- What do you want the first successful result to prove: install, GPUDirect raw TX/RX, cabled throughput, hardware-loopback throughput, UDP/TCP baseline, or RoCE/RDMA?
- Are you optimizing for a smoke test, a trustworthy performance number, or debugging a failure?
- Do you need to compare against Linux sockets, RDMA verbs, or raw Ethernet?

## Location and Privileges

- Are you running on the host, inside the DAQIRI container, or on a remote machine?
- Can we run privileged Docker containers or root networking commands on this system?
- Is this a single-host, cabled loopback, or two-host setup?

## Hardware

- Is a ConnectX NIC available? If yes, what generation or `ibdev2netdev -v` output do you have?
- Is a cable loopback or peer host available?
- Is an NVIDIA GPU visible in `nvidia-smi`?
- On hybrid systems, which GPU UUID should DAQIRI use?
- Which netdevs and PCIe addresses correspond to the ports you want to use?
- What are the MAC addresses and IPs for each side?

## Build and Runtime

- Has the DAQIRI container image already been built?
- Are benchmark binaries available under `/opt/daqiri/bin`, `./build/examples`, or `./build-socket-rdma/examples`?
- Which `DAQIRI_ENGINE` was used for the build?
- Are hugepages mounted and sized for the raw config?

## Configuration

- Are you using a checked-in YAML, a generated YAML, or a custom config?
- Do any `<angle-bracket>` placeholders remain?
- For socket/RDMA, are endpoint URI schemes `udp://`, `tcp://`, or `roce://` as intended?
- For raw runs, is the RX side started before TX in split-role setups?
- Are CPU cores, queue IDs, memory regions, and GPU affinity appropriate for the machine?

## Results and Debugging

- Did DAQIRI report non-zero TX/RX packets or completions?
- Did any `NO_FREE_BURST_BUFFERS`, `NO_FREE_PACKET_BUFFERS`, CQ, retry, or RDMA-CM errors appear?
- For cabled tests, did TX and RX PHY counters increase by matching packet counts?
- For hardware loopback, did `vport_loopback_bytes` increase while PHY counters stayed flat?
- For UDP, did kernel UDP/IP/NIC error counters remain flat?
- For TCP, were there retransmissions or socket errors?
