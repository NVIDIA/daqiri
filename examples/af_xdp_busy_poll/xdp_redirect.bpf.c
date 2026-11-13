// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

struct endpoint {
  __u32 ip;
  __u16 port;
  __u8 mac[6];
};
struct {
  __uint(type, BPF_MAP_TYPE_XSKMAP);
  __uint(max_entries, 64);
  __type(key, __u32);
  __type(value, __u32);
} xsks SEC(".maps");
struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __uint(max_entries, 1);
  __type(key, __u32);
  __type(value, struct endpoint);
} endpoints SEC(".maps");

SEC("xdp") int echo_redirect(struct xdp_md* ctx) {
  unsigned char* p = (void*)(long)ctx->data;
  unsigned char* end = (void*)(long)ctx->data_end;
  __u32 zero = 0;
  struct endpoint* local = bpf_map_lookup_elem(&endpoints, &zero);
  if (!local || p + 60 > end || p[12] != 8 || p[13] != 0 || p[14] != 0x45 || p[23] != 17)
    return XDP_PASS;
#pragma unroll
  for (int i = 0; i < 6; ++i)
    if (p[i] != local->mac[i]) return XDP_PASS;
  __u32 ip;
  __builtin_memcpy(&ip, p + 30, 4);
  __u16 port;
  __builtin_memcpy(&port, p + 36, 2);
  if (ip != local->ip || port != bpf_htons(local->port)) return XDP_PASS;
  return bpf_redirect_map(&xsks, ctx->rx_queue_index, XDP_PASS);
}
char LICENSE[] SEC("license") = "GPL";
