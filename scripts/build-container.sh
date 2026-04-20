#!/usr/bin/env bash
set -euo pipefail

IMAGE_TAG="${IMAGE_TAG:-daqiri:local}"
BASE_TARGET="${BASE_TARGET:-dpdk}"
DAQIRI_MGR="${DAQIRI_MGR:-dpdk socket}"
DAQIRI_BUILD_PYTHON="${DAQIRI_BUILD_PYTHON:-OFF}"
BUILD_SHARED_LIBS="${BUILD_SHARED_LIBS:-ON}"

docker build \
  --target runtime \
  --build-arg DAQIRI_BASE_TARGET="${BASE_TARGET}" \
  --build-arg DAQIRI_MGR="${DAQIRI_MGR}" \
  --build-arg DAQIRI_BUILD_PYTHON="${DAQIRI_BUILD_PYTHON}" \
  --build-arg BUILD_SHARED_LIBS="${BUILD_SHARED_LIBS}" \
  -t "${IMAGE_TAG}" \
  .

echo "Built ${IMAGE_TAG}"
