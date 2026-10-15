#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

variant="${BASE_IMAGE:-cuda}"
case "${variant}" in
  cuda) base="${DAQIRI_OS_BASE_IMAGE:-nvcr.io/nvidia/cuda:13.1.0-devel-ubuntu24.04}" ;;
  torch) base="${DAQIRI_OS_BASE_IMAGE:-nvcr.io/nvidia/pytorch:26.01-py3}" ;;
  *) echo "BASE_IMAGE must be cuda or torch" >&2; exit 1 ;;
esac
# Record the immutable multi-architecture manifest actually selected for this
# build, even when the caller supplies a mutable tag.
if [[ "${base}" != *@sha256:* ]]; then
  manifest="$(docker buildx imagetools inspect "${base}" --format '{{json .Manifest}}')"
  digest="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["digest"])' <<< "${manifest}")"
  base="${base}@${digest}"
fi
version="$(cat VERSION)"
image="${IMAGE_TAG:-daqiri:${version}-${variant}-release}"
args=(build -f Dockerfile_release --target release
  --build-arg "DAQIRI_OS_BASE_IMAGE=${base}"
  --build-arg "DAQIRI_REVISION=$(git rev-parse HEAD)"
  --build-arg "DAQIRI_VERSION=${version}"
  --build-arg "BUILD_JOBS=${BUILD_JOBS:-8}"
  -t "${image}")
if [[ -n "${CMAKE_CUDA_ARCHITECTURES:-}" ]]; then
  args+=(--build-arg "CMAKE_CUDA_ARCHITECTURES=${CMAKE_CUDA_ARCHITECTURES}")
fi
if [[ -n "${PLATFORM:-}" ]]; then
  args+=(--platform "${PLATFORM}")
fi
args+=(.)
docker "${args[@]}"
echo "Built ${image}; validate with scripts/check-release-container.sh ${image}"
