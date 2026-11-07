#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
image=${MGBE_IMAGE:-daqiri:mgbe}
vendor=${MGBE_VENDOR_SOURCE:-/tmp/tegra-oot}
identity=(--user "$(id -u):$(id -g)")
if [[ ${1:-} == --root ]]; then
  identity=(--user 0:0 --pid=host)
  shift
fi
[[ $# -gt 0 ]] || { echo "usage: $0 [--root] COMMAND [ARGS...]" >&2; exit 2; }
common_git=$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)
mounts=(-v "$root:$root" -v "$common_git:$common_git:ro"
        -v /usr/src:/usr/src:ro -v /lib/modules:/lib/modules:ro -v /dev:/dev)
if [[ -d $vendor ]]; then
  mounts+=(-v "$vendor:$vendor:ro")
fi
if [[ -d /opt/daqiri ]]; then
  mounts+=(-v /opt/daqiri:/opt/daqiri:ro)
fi
exec docker run --rm --privileged --network host --runtime=nvidia \
  -e NVIDIA_VISIBLE_DEVICES=all -e NVIDIA_DISABLE_REQUIRE=true \
  -e GIT_OPTIONAL_LOCKS=0 "${identity[@]}" "${mounts[@]}" -w "$root" "$image" "$@"
