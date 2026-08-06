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
"""Packetize CIFAR-10 images as int8 wire frames for DAQIRI config reorder.

Each image is resized to 224x224 RGB uint8, quantized to signed int8 as
(pixel - INT8_OFFSET) — lossless for 0..255 — and split into out_payload_len
byte packets (default 1176). Sequence numbers span a full inference batch
(img_in_batch * packets_per_image + pkt) and are written as big-endian bits
into the header at seq_bit_offset (no 4-byte payload prefix). Normalization
is NOT applied here; the ONNX front-end folds offset + ImageNet norm.

Shared constants with export_resnet_onnx.py (must stay in sync):
  INT8_OFFSET, IMAGENET_MEAN/STD channel order = R,G,B = NCHW 0,1,2.

  python3 prepare_cifar10_pcap.py --num-images 256 --images-per-batch 32 \\
      --out data/cifar10_resnet.pcap
"""

import argparse
import os
import socket
import struct

# Keep in sync with export_resnet_onnx.py.
INT8_OFFSET = 128
IMAGENET_MEAN = (0.485, 0.456, 0.406)  # documented; applied in ONNX, not here
IMAGENET_STD = (0.229, 0.224, 0.225)

# Silence unused-import lint for the documented shared constants.
_ = (IMAGENET_MEAN, IMAGENET_STD)


def ip_checksum(header: bytes) -> int:
    s = 0
    for i in range(0, len(header), 2):
        s += (header[i] << 8) | header[i + 1]
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def set_bits_be(data: bytearray, bit_offset: int, bit_width: int, value: int) -> None:
    """Write `value` as big-endian bits starting at bit_offset (matches DAQIRI)."""
    for i in range(bit_width):
        src_shift = bit_width - 1 - i
        bit = (value >> src_shift) & 1
        bit_idx = bit_offset + i
        byte_idx = bit_idx // 8
        bit_pos = 7 - (bit_idx % 8)
        mask = 1 << bit_pos
        data[byte_idx] = (data[byte_idx] & ~mask) | (bit << bit_pos)


def build_frame(chunk: bytes, seq: int, header_size: int, payload_byte_offset: int,
                seq_bit_offset: int, seq_bit_width: int, eth_src: bytes,
                eth_dst: bytes, ip_src: int, ip_dst: int, sport: int,
                dport: int) -> bytes:
    if payload_byte_offset != header_size:
        raise ValueError("payload_byte_offset must equal header_size")
    filler = header_size - 42
    udp_payload = bytearray(b"\x00" * filler + chunk)
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
    udp = struct.pack(">HHHH", sport, dport, udp_len, 0)

    frame = bytearray(eth + bytes(ip) + udp + udp_payload)
    set_bits_be(frame, seq_bit_offset, seq_bit_width, seq)
    return bytes(frame)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--num-images", type=int, default=256)
    ap.add_argument("--images-per-batch", type=int, default=32,
                    help="pad image count up to a multiple of this (full reorder batches)")
    ap.add_argument("--out", default="data/cifar10_resnet.pcap")
    ap.add_argument("--data-root", default="data/cifar10")
    ap.add_argument("--out-payload-len", type=int, default=1176,
                    help="int8 sample bytes per packet (must divide 150528)")
    ap.add_argument("--header-size", type=int, default=64)
    ap.add_argument("--payload-byte-offset", type=int, default=64)
    ap.add_argument("--seq-bit-offset", type=int, default=128)
    ap.add_argument("--seq-bit-width", type=int, default=12)
    ap.add_argument("--packets-per-image", type=int, default=128)
    ap.add_argument("--udp-port", type=int, default=4096)
    ap.add_argument("--eth-src", default="02:00:00:00:00:01")
    ap.add_argument("--eth-dst", default="02:00:00:00:00:02",
                    help="placeholder; the replayer patches the real RX MAC at TX time")
    ap.add_argument("--ip-src", default="1.1.1.1")
    ap.add_argument("--ip-dst", default="2.2.2.2")
    args = ap.parse_args()

    import torch
    import torchvision
    import torchvision.transforms as T

    elems_per_image = 3 * 224 * 224  # int8 samples
    if elems_per_image % args.out_payload_len != 0:
        raise SystemExit(f"--out-payload-len {args.out_payload_len} must divide "
                         f"{elems_per_image}")
    packets_per_image = elems_per_image // args.out_payload_len
    if args.packets_per_image != packets_per_image:
        raise SystemExit(f"--packets-per-image {args.packets_per_image} != "
                         f"{packets_per_image} from geometry")
    if args.header_size < 42:
        raise SystemExit("--header-size must be >= 42 (ETH+IP+UDP)")
    if args.payload_byte_offset != args.header_size:
        raise SystemExit("--payload-byte-offset must equal --header-size for this layout")
    if args.seq_bit_offset + args.seq_bit_width > args.payload_byte_offset * 8:
        raise SystemExit("seq field must end at or before payload_byte_offset")

    transform = T.Compose([
        T.Resize(224),
        T.ToTensor(),  # float [0,1] CHW — we re-quantize to uint8 then int8
    ])
    ds = torchvision.datasets.CIFAR10(root=args.data_root, train=False,
                                      download=True, transform=transform)
    n = min(args.num_images, len(ds))
    # Pad to a multiple of images_per_batch so every reorder batch completes.
    ipb = args.images_per_batch
    if ipb > 0 and n % ipb != 0:
        n = ((n + ipb - 1) // ipb) * ipb
        n = min(n, len(ds))
        # If still short, wrap by repeating from the start after writing.
    n_write = min(args.num_images, len(ds))
    if ipb > 0 and n_write % ipb != 0:
        n_pad = ((n_write + ipb - 1) // ipb) * ipb
    else:
        n_pad = n_write

    eth_src = bytes(int(b, 16) for b in args.eth_src.split(":"))
    eth_dst = bytes(int(b, 16) for b in args.eth_dst.split(":"))
    ip_src = struct.unpack(">I", socket.inet_aton(args.ip_src))[0]
    ip_dst = struct.unpack(">I", socket.inet_aton(args.ip_dst))[0]

    packets_per_batch = packets_per_image * ipb

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    labels = []
    frames_written = 0
    with open(args.out, "wb") as pcap:
        pcap.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        for idx in range(n_pad):
            img, label = ds[idx % len(ds)]
            # uint8 pixels 0..255 from float tensor, then signed int8 = pixel - 128.
            u8 = (img.clamp(0, 1) * 255.0).round().to(torch.uint8).numpy()  # CHW
            i8 = (u8.astype("int16") - INT8_OFFSET).astype("int8")
            data = i8.tobytes()  # 150528 bytes, NCHW channel order R,G,B
            assert len(data) == elems_per_image
            labels.append(int(label) if idx < n_write else int(label))
            img_in_batch = idx % ipb
            for pkt in range(packets_per_image):
                chunk = data[pkt * args.out_payload_len:(pkt + 1) * args.out_payload_len]
                seq = img_in_batch * packets_per_image + pkt
                assert 0 <= seq < packets_per_batch
                frame = build_frame(chunk, seq, args.header_size, args.payload_byte_offset,
                                    args.seq_bit_offset, args.seq_bit_width,
                                    eth_src, eth_dst, ip_src, ip_dst,
                                    args.udp_port, args.udp_port)
                pcap.write(struct.pack("<IIII", idx, pkt, len(frame), len(frame)))
                pcap.write(frame)
                frames_written += 1

    labels_path = args.out + ".labels"
    with open(labels_path, "w") as f:
        for lbl in labels:
            f.write(f"{lbl}\n")

    print(f"Wrote {frames_written} frames ({n_pad} images x {packets_per_image} pkts) "
          f"to {args.out}")
    print(f"Wrote {len(labels)} labels to {labels_path}")
    print(f"frame_bytes={args.header_size + args.out_payload_len} "
          f"packets_per_batch={packets_per_batch} "
          f"(set Reorder_RX_GPU buf_size >= "
          f"{packets_per_batch * args.out_payload_len * 2})")


if __name__ == "__main__":
    main()
