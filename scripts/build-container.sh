#!/usr/bin/env bash
set -euo pipefail

IMAGE_TAG="${IMAGE_TAG:-daqiri:local}"
BASE_TARGET="${BASE_TARGET:-dpdk}"
BASE_IMAGE="${BASE_IMAGE:-cuda}"
DAQIRI_MGR="${DAQIRI_MGR:-dpdk socket}"
DAQIRI_BUILD_PYTHON="${DAQIRI_BUILD_PYTHON:-OFF}"
BUILD_SHARED_LIBS="${BUILD_SHARED_LIBS:-ON}"

case "${BASE_IMAGE}" in
  cuda)
    DAQIRI_OS_BASE_IMAGE="nvcr.io/nvidia/cuda:13.1.0-devel-ubuntu24.04"
    ;;
  torch)
    DAQIRI_OS_BASE_IMAGE="nvcr.io/nvidia/pytorch:26.01-py3"
    ;;
  *)
    echo "ERROR: invalid BASE_IMAGE='${BASE_IMAGE}'. Choose from: cuda, torch" >&2
    exit 1
    ;;
esac

docker build \
  --target runtime \
  --build-arg DAQIRI_BASE_TARGET="${BASE_TARGET}" \
  --build-arg DAQIRI_OS_BASE_IMAGE="${DAQIRI_OS_BASE_IMAGE}" \
  --build-arg DAQIRI_MGR="${DAQIRI_MGR}" \
  --build-arg DAQIRI_BUILD_PYTHON="${DAQIRI_BUILD_PYTHON}" \
  --build-arg BUILD_SHARED_LIBS="${BUILD_SHARED_LIBS}" \
  -t "${IMAGE_TAG}" \
  .

echo "Built ${IMAGE_TAG}"
