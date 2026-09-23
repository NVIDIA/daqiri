#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Emit the supported GVSP image profile as raw Ethernet frames from a cabled peer.

This is a narrow validation fixture, not a camera simulator: it implements no GVCP,
discovery, resend, or GenICam control. It requires root/CAP_NET_RAW and an Ethernet peer
whose frames enter the DAQIRI receiver NIC.
"""

from __future__ import annotations

import argparse
import random
import socket
import struct
import time


def mac(text: str) -> bytes:
    value = bytes(int(part, 16) for part in text.split(":"))
    if len(value) != 6:
        raise argparse.ArgumentTypeError("MAC address must contain six octets")
    return value


def checksum(data: bytes) -> int:
    if len(data) & 1:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    total = (total & 0xFFFF) + (total >> 16)
    total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def gvsp(block_id: int, content_type: int, packet_id: int, data: bytes) -> bytes:
    packet_info = (content_type << 24) | packet_id
    return struct.pack("!HHI", 0, block_id & 0xFFFF, packet_info) + data


def leader(block_id: int, timestamp: int, pixel_format: int, width: int, height: int) -> bytes:
    data = struct.pack(
        "!HHQIIIII",
        0,
        1,  # image payload
        timestamp,
        pixel_format,
        width,
        height,
        0,
        0,
    )
    return gvsp(block_id, 1, 0, data)


def ethernet_udp(
    payload: bytes,
    source_mac: bytes,
    destination_mac: bytes,
    source_ip: str,
    destination_ip: str,
    source_port: int,
    destination_port: int,
    identification: int,
) -> bytes:
    udp = struct.pack("!HHHH", source_port, destination_port, 8 + len(payload), 0) + payload
    source_ip_bytes = socket.inet_aton(source_ip)
    destination_ip_bytes = socket.inet_aton(destination_ip)
    ip_without_checksum = struct.pack(
        "!BBHHHBBH4s4s",
        0x45,
        0,
        20 + len(udp),
        identification & 0xFFFF,
        0x4000,
        64,
        socket.IPPROTO_UDP,
        0,
        source_ip_bytes,
        destination_ip_bytes,
    )
    ip = ip_without_checksum[:10] + struct.pack("!H", checksum(ip_without_checksum)) + ip_without_checksum[12:]
    return destination_mac + source_mac + struct.pack("!H", 0x0800) + ip + udp


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", required=True)
    parser.add_argument("--source-mac", required=True, type=mac)
    parser.add_argument("--destination-mac", required=True, type=mac)
    parser.add_argument("--source-ip", required=True)
    parser.add_argument("--destination-ip", required=True)
    parser.add_argument("--source-port", type=int, default=50000)
    parser.add_argument("--destination-port", type=int, required=True)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--frame-bytes", type=int, default=1920 * 1080)
    parser.add_argument("--data-payload-bytes", type=int, required=True)
    parser.add_argument("--pixel-format", type=lambda value: int(value, 0), default=0x01080001)
    parser.add_argument("--frames", type=int, default=10)
    parser.add_argument("--inter-packet-us", type=float, default=20.0)
    parser.add_argument("--drop-data-packet", type=int, default=0)
    parser.add_argument("--duplicate-data-packet", type=int, default=0)
    parser.add_argument("--shuffle-data", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = arguments()
    if args.frame_bytes <= 0 or args.data_payload_bytes <= 0 or args.frames <= 0:
        raise SystemExit("frame sizes and --frames must be positive")

    raw_socket = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
    raw_socket.bind((args.interface, 0))
    delay = args.inter_packet_us / 1_000_000.0
    identification = 0

    for frame_index in range(args.frames):
        block_id = (frame_index % 0xFFFF) + 1
        pixels = bytes((frame_index + offset) & 0xFF for offset in range(args.frame_bytes))
        packets = [leader(block_id, time.monotonic_ns(), args.pixel_format, args.width, args.height)]
        data_packets = []
        for offset in range(0, len(pixels), args.data_payload_bytes):
            packet_id = offset // args.data_payload_bytes + 1
            if packet_id == args.drop_data_packet:
                continue
            packet = gvsp(block_id, 3, packet_id, pixels[offset : offset + args.data_payload_bytes])
            data_packets.append((packet_id, packet))
            if packet_id == args.duplicate_data_packet:
                data_packets.append((packet_id, packet))
        if args.shuffle_data:
            random.shuffle(data_packets)
        packets.extend(packet for _, packet in data_packets)
        expected_data_packets = (args.frame_bytes + args.data_payload_bytes - 1) // args.data_payload_bytes
        packets.append(gvsp(block_id, 2, expected_data_packets + 1, b"\0" * 8))

        for payload in packets:
            frame = ethernet_udp(
                payload,
                args.source_mac,
                args.destination_mac,
                args.source_ip,
                args.destination_ip,
                args.source_port,
                args.destination_port,
                identification,
            )
            raw_socket.send(frame)
            identification += 1
            if delay > 0:
                time.sleep(delay)

    print(f"sent {args.frames} synthetic GVSP frame(s) on {args.interface}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
