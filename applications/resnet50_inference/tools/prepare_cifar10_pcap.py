#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Packetize preprocessed CIFAR-10 images into a standard .pcap for replay.

Each image is resized to 224x224, ImageNet-normalized, and serialized as FP32
NCHW (3*224*224*4 = 602112 bytes). The bytes are split into fixed-size chunks
(out_payload_len) and framed into ETH/IP/UDP packets. Each packet carries a
per-image sequence number (0 .. packets_per_image-1) at the payload start so the
DAQIRI RX-side reorder kernel reassembles the image. A "<pcap>.labels" sidecar
(one CIFAR class id per image, in send order) drives the per-class feature stats.

The frame layout mirrors the bench's populate_udp_ipv4_headers: a real
14B Ethernet + 20B IPv4 + 8B UDP header, zero filler up to header_size, then the
4-byte sequence number, then the image chunk. Preprocessing is baked in here so
the runtime pipeline stays pure reorder -> infer. No scapy dependency.

  python3 prepare_cifar10_pcap.py --num-images 256 --out data/cifar10_resnet.pcap
"""

import argparse
import os
import socket
import struct

IMAGENET_MEAN = (0.485, 0.456, 0.406)
IMAGENET_STD = (0.229, 0.224, 0.225)


def ip_checksum(header: bytes) -> int:
    s = 0
    for i in range(0, len(header), 2):
        s += (header[i] << 8) | header[i + 1]
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def build_frame(chunk: bytes, seq: int, header_size: int, eth_src: bytes,
                eth_dst: bytes, ip_src: int, ip_dst: int, sport: int,
                dport: int) -> bytes:
    # UDP payload = filler (header_size - 42) + 4-byte seq + chunk.
    filler = header_size - 42
    udp_payload = b"\x00" * filler + struct.pack(">I", seq) + chunk
    udp_len = 8 + len(udp_payload)
    ip_total = 20 + udp_len

    eth = eth_dst + eth_src + struct.pack(">H", 0x0800)

    ip = bytearray(struct.pack(">BBHHHBBH4s4s",
                               0x45, 0x00, ip_total, 0x0000, 0x0000, 64,
                               socket.IPPROTO_UDP, 0,
                               struct.pack(">I", ip_src), struct.pack(">I", ip_dst)))
    chk = ip_checksum(bytes(ip))
    ip[10] = (chk >> 8) & 0xFF
    ip[11] = chk & 0xFF

    # IPv4 UDP checksum is optional; 0 means "not computed" (the NIC flow matches
    # ports, not the checksum), which keeps this tool simple.
    udp = struct.pack(">HHHH", sport, dport, udp_len, 0)

    return eth + bytes(ip) + udp + udp_payload


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--num-images", type=int, default=256)
    ap.add_argument("--out", default="data/cifar10_resnet.pcap")
    ap.add_argument("--data-root", default="data/cifar10")
    ap.add_argument("--out-payload-len", type=int, default=7168,
                    help="image-pixel bytes per packet (must divide 602112)")
    ap.add_argument("--header-size", type=int, default=64,
                    help="logical payload offset (>=42); matches the YAML header_size")
    ap.add_argument("--udp-port", type=int, default=4096)
    ap.add_argument("--eth-src", default="02:00:00:00:00:01")
    ap.add_argument("--eth-dst", default="02:00:00:00:00:02",
                    help="placeholder; the replayer patches the real RX MAC at TX time")
    ap.add_argument("--ip-src", default="1.2.3.4")
    ap.add_argument("--ip-dst", default="5.6.7.8")
    args = ap.parse_args()

    import torch
    import torchvision
    import torchvision.transforms as T

    image_bytes = 3 * 224 * 224 * 4
    if image_bytes % args.out_payload_len != 0:
        raise SystemExit(f"--out-payload-len {args.out_payload_len} must divide "
                         f"{image_bytes}")
    packets_per_image = image_bytes // args.out_payload_len
    if args.header_size < 42:
        raise SystemExit("--header-size must be >= 42 (ETH+IP+UDP)")

    transform = T.Compose([
        T.Resize(224),
        T.ToTensor(),
        T.Normalize(mean=IMAGENET_MEAN, std=IMAGENET_STD),
    ])
    ds = torchvision.datasets.CIFAR10(root=args.data_root, train=False,
                                      download=True, transform=transform)
    n = min(args.num_images, len(ds))

    eth_src = bytes(int(b, 16) for b in args.eth_src.split(":"))
    eth_dst = bytes(int(b, 16) for b in args.eth_dst.split(":"))
    ip_src = struct.unpack(">I", socket.inet_aton(args.ip_src))[0]
    ip_dst = struct.unpack(">I", socket.inet_aton(args.ip_dst))[0]

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    labels = []
    frames_written = 0
    with open(args.out, "wb") as pcap:
        # pcap global header: magic, ver 2.4, thiszone, sigfigs, snaplen, linktype=1.
        pcap.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        for idx in range(n):
            img, label = ds[idx]
            data = img.numpy().astype("<f4").tobytes()  # FP32 NCHW (CHW), 602112 B
            assert len(data) == image_bytes
            labels.append(int(label))
            for seq in range(packets_per_image):
                chunk = data[seq * args.out_payload_len:(seq + 1) * args.out_payload_len]
                frame = build_frame(chunk, seq, args.header_size, eth_src, eth_dst,
                                    ip_src, ip_dst, args.udp_port, args.udp_port)
                # pcap record header: ts_sec, ts_usec, incl_len, orig_len.
                pcap.write(struct.pack("<IIII", idx, seq, len(frame), len(frame)))
                pcap.write(frame)
                frames_written += 1

    labels_path = args.out + ".labels"
    with open(labels_path, "w") as f:
        for lbl in labels:
            f.write(f"{lbl}\n")

    print(f"Wrote {frames_written} frames ({n} images x {packets_per_image} pkts) "
          f"to {args.out}")
    print(f"Wrote {len(labels)} labels to {labels_path}")
    print(f"frame_bytes={args.header_size + 4 + args.out_payload_len} "
          f"(set memory_regions buf_size >= this)")


if __name__ == "__main__":
    main()
