#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

DAQIRI_OS_BASE_IMAGE="${DAQIRI_OS_BASE_IMAGE:-nvcr.io/nvidia/cuda:13.1.0-devel-ubuntu24.04}"
DOCKER_TARGET="${DOCKER_TARGET:-base}"

args=(
  build
  --target "${DOCKER_TARGET}"
  --build-arg "DAQIRI_OS_BASE_IMAGE=${DAQIRI_OS_BASE_IMAGE}"
)

if [ "${NO_CACHE:-0}" = "1" ]; then
  args+=(--no-cache)
fi

args+=(".")

docker "${args[@]}"
