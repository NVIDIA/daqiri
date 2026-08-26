#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Config rewriters for the RTX PRO 6000 benchmark runner, sourced by
# run_rtx_pro_bench.sh. Structure-aware awk rather than sed: the fields being
# replaced (PCIe addresses, GPU ordinals, cores) repeat per queue and per memory
# region, so a line-oriented substitution rewrites the wrong one.
#
# Callers must set: TX_BDF RX_BDF TX_GPU RX_GPU TX_NUMA RX_NUMA CPU_MASTER
# CPU_TX CPU_RX CPU_TX_WORK CPU_RX_WORK IS_SW, and ETH_DST_ADDR for the wire.

# Rewrite the machine-specific fields of a raw-Ethernet bench config.
#
# Substitution is keyed on YAML structure -- which interface block an `address`
# sits in, which memory region an `affinity` belongs to -- rather than on the
# literal values that happen to be in the template. Matching on literals is how
# the templates and the runner drifted apart before: the config named one card
# and the runner searched for another, so the rewrite quietly did nothing and
# the run went out over whatever the template said.
#
# `affinity` is overloaded in the schema: a CUDA ordinal for kind: device, a
# NUMA node for kind: huge / host_pinned. Region names carry _TX_ or _RX_, and
# `kind` always precedes `affinity` in these templates.
rewrite_raw_topology() {
  local file="$1"
  awk -v tx_bdf="$TX_BDF" -v rx_bdf="$RX_BDF" \
      -v tx_gpu="$TX_GPU" -v rx_gpu="$RX_GPU" \
      -v tx_numa="$TX_NUMA" -v rx_numa="$RX_NUMA" \
      -v master="$CPU_MASTER" \
      -v tx_poll="$CPU_TX" -v rx_poll="$CPU_RX" \
      -v tx_work="$CPU_TX_WORK" -v rx_work="$CPU_RX_WORK" \
      -v is_sw="$IS_SW" '
    # Indent levels differ between these configs (some nest memory_regions and
    # interfaces one level deeper), so anchor on structure, not column counts.
    function indent_of(line,   pad) { pad = line; sub(/[^ ].*$/, "", pad); return length(pad) }
    function value_of(line,   v) {
      v = line; sub(/^[^:]*:[[:space:]]*/, "", v); gsub(/^"|"$/, "", v); return v
    }
    function set_scalar(key, value,   pad) {
      pad = $0; sub(/[^ ].*$/, "", pad)
      print pad key ": " value
    }
    # Guarded: `memory_regions:` also appears inside each queue as a list of
    # region names to attach, and that nested key must not reopen the section.
    /^ *memory_regions:/ && section != "if" { section = "mr"; iface = ""; item_indent = -1 }
    /^ *interfaces:/     { section = "if"; region = ""; kind = ""; item_indent = -1 }
    /^bench_tx:/         { section = "bench_tx"; iface = ""; region = "" }
    /^bench_rx:/         { section = "bench_rx"; iface = ""; region = "" }

    # The first list entry in a section fixes the indent of its siblings, which
    # separates interface entries from the queue and flow entries nested inside.
    (section == "mr" || section == "if") && /^ *- name:/ {
      if (item_indent < 0) item_indent = indent_of($0)
      if (indent_of($0) == item_indent) {
        if (section == "mr") { region = value_of($0); kind = "" }
        else                 { iface = value_of($0) }
      } else if (section == "if") {
        qname = value_of($0)
      }
    }
    section == "mr" && /^ +kind:/ { kind = value_of($0) }
    section == "mr" && /^ +affinity:/ {
      if (region ~ /_TX_|_TX$/)      { set_scalar("affinity", kind == "device" ? tx_gpu : tx_numa); next }
      else if (region ~ /_RX_|_RX$/) { set_scalar("affinity", kind == "device" ? rx_gpu : rx_numa); next }
    }

    # SW loopback keeps address: "loopback"; there is no NIC to name.
    section == "if" && /^ +address:/ && is_sw != "1" {
      if (iface == "tx_port") { set_scalar("address", tx_bdf); next }
      if (iface == "rx_port") { set_scalar("address", rx_bdf); next }
    }
    # Prefer the interface name. The software-loopback config carries both
    # directions on one pseudo-interface, so fall back to the queue name there.
    section == "if" && /^ +cpu_core:/ {
      if (iface == "tx_port")      { set_scalar("cpu_core", tx_poll); next }
      if (iface == "rx_port")      { set_scalar("cpu_core", rx_poll); next }
      if (qname ~ /^tx_/)          { set_scalar("cpu_core", tx_poll); next }
      if (qname ~ /^(rx_|rq_)/)    { set_scalar("cpu_core", rx_poll); next }
    }
    section == "bench_tx" && /^ +cpu_core:/ { set_scalar("cpu_core", tx_work); next }
    section == "bench_rx" && /^ +cpu_core:/ { set_scalar("cpu_core", rx_work); next }

    /^ *master_core:/ { set_scalar("master_core", master); next }
    { print }
  ' "$file" > "${file}.tmp" && mv "${file}.tmp" "$file"
}

rewrite_eth_dst() {
  local file="$1"
  [[ -n "${ETH_DST_ADDR:-}" ]] || return 0
  sed -E \
    -e "s|<00:00:00:00:00:00>|$ETH_DST_ADDR|g" \
    -e "s|^( *eth_dst_addr: ).*|\1$ETH_DST_ADDR|" \
    "$file" > "${file}.tmp" && mv "${file}.tmp" "$file"
}

# Put this host's cores in a single-role netns config (socket or RoCE).
#
# The checked-in netns bases carry the core numbers of the machine they were
# written for, and a role config has three consumers to place: the queue poller,
# the engine manager thread (which pins to the TX queue core), and the bench
# worker. The worker must NOT land on the manager's core -- co-locating them
# livelocks at small message sizes -- so each role gets a poll core and a
# separate work core.
rewrite_netns_cores() {
  local file="$1" poll="$2" work="$3"
  awk -v poll="$poll" -v work="$work" -v master="$CPU_MASTER" '
    function set_scalar(key, value,   pad) {
      pad = $0; sub(/[^ ].*$/, "", pad)
      print pad key ": " value
    }
    /^ *interfaces:/                  { section = "if" }
    /^[a-z_]+_bench_(server|client):/ { section = "bench" }

    # A one-way test uses one direction per role (client TX, server RX) and the
    # other direction only for bookkeeping, so both queues share the poll core
    # and the bench worker gets the work core to itself.
    section == "if"    && / cpu_core:/   { set_scalar("cpu_core", poll); next }
    section == "bench" && /^ *cpu_core:/ { set_scalar("cpu_core", work); next }
    /^ *master_core:/ { set_scalar("master_core", master); next }
    { print }
  ' "$file" > "${file}.tmp" && mv "${file}.tmp" "$file"
}

# The RoCE config is host_pinned throughout, so every affinity is a NUMA node,
# and its interfaces are named by IP rather than by PCIe address.
rewrite_rdma_affinity() {
  local file="$1"
  awk -v tx_numa="$TX_NUMA" -v rx_numa="$RX_NUMA" -v master="$CPU_MASTER" '
    /^    - name:/ {
      region = $0; sub(/.*name:[[:space:]]*"?/, "", region); sub(/".*$/, "", region)
    }
    # Role wins over direction: DATA_RX_GPU_CLIENT is the client role receiving,
    # so it belongs on the client port, not on the server one.
    /^ +affinity:/ {
      indent = $0; sub(/[^ ].*$/, "", indent)
      if (region ~ /CLIENT|Client/) { print indent "affinity: " tx_numa; next }
      if (region ~ /SERVER|Server/) { print indent "affinity: " rx_numa; next }
      if (region ~ /_TX_|_TX$/)     { print indent "affinity: " tx_numa; next }
      if (region ~ /_RX_|_RX$/)     { print indent "affinity: " rx_numa; next }
    }
    /^    master_core:/ {
      indent = $0; sub(/[^ ].*$/, "", indent)
      print indent "master_core: " master; next
    }
    { print }
  ' "$file" > "${file}.tmp" && mv "${file}.tmp" "$file"
}
