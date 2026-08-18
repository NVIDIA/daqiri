#!/usr/bin/env bash
set -euo pipefail

IMAGE_TAG="${IMAGE_TAG:-daqiri:local}"
BASE_TARGET="${BASE_TARGET:-dpdk}"
BASE_IMAGE="${BASE_IMAGE:-cuda}"
DAQIRI_ENGINE="${DAQIRI_ENGINE:-dpdk ibverbs}"
DAQIRI_BUILD_PYTHON="${DAQIRI_BUILD_PYTHON:-OFF}"
DAQIRI_ENABLE_S3="${DAQIRI_ENABLE_S3:-OFF}"
BUILD_SHARED_LIBS="${BUILD_SHARED_LIBS:-ON}"
DAQIRI_ENABLE_OTEL_METRICS="${DAQIRI_ENABLE_OTEL_METRICS:-OFF}"
AWS_SDK_CPP_VERSION="${AWS_SDK_CPP_VERSION:-1.11.822}"

# CUDA toolkit version for the default (BASE_IMAGE=cuda) base image. Override this when the
# host driver is older than the default: a container whose CUDA runtime is newer than the host
# driver cannot initialize CUDA at all. IGX Thor, for example, ships driver 580.00 / CUDA 13.0
# and needs CUDA_VERSION=13.0.0 (cuda-compat forward compatibility is datacenter-only and does
# not apply to Tegra).
CUDA_VERSION="${CUDA_VERSION:-13.1.0}"
UBUNTU_VERSION="${UBUNTU_VERSION:-ubuntu24.04}"

# DAQIRI_OS_BASE_IMAGE may be set directly to pin an arbitrary base image; when it is, the
# BASE_IMAGE selector and CUDA_VERSION are not consulted.
if [[ -z "${DAQIRI_OS_BASE_IMAGE:-}" ]]; then
  case "${BASE_IMAGE}" in
    cuda)
      DAQIRI_OS_BASE_IMAGE="nvcr.io/nvidia/cuda:${CUDA_VERSION}-devel-${UBUNTU_VERSION}"
      ;;
    torch)
      DAQIRI_OS_BASE_IMAGE="nvcr.io/nvidia/pytorch:26.01-py3"
      ;;
    *)
      echo "ERROR: invalid BASE_IMAGE='${BASE_IMAGE}'. Choose from: cuda, torch" >&2
      exit 1
      ;;
  esac
fi

echo "Building ${IMAGE_TAG} on ${DAQIRI_OS_BASE_IMAGE} (target ${BASE_TARGET})"

docker build \
  --target runtime \
  --build-arg DAQIRI_BASE_TARGET="${BASE_TARGET}" \
  --build-arg DAQIRI_OS_BASE_IMAGE="${DAQIRI_OS_BASE_IMAGE}" \
  --build-arg DAQIRI_ENGINE="${DAQIRI_ENGINE}" \
  --build-arg DAQIRI_BUILD_PYTHON="${DAQIRI_BUILD_PYTHON}" \
  --build-arg DAQIRI_ENABLE_S3="${DAQIRI_ENABLE_S3}" \
  --build-arg BUILD_SHARED_LIBS="${BUILD_SHARED_LIBS}" \
  --build-arg DAQIRI_ENABLE_OTEL_METRICS="${DAQIRI_ENABLE_OTEL_METRICS}" \
  --build-arg AWS_SDK_CPP_VERSION="${AWS_SDK_CPP_VERSION}" \
  -t "${IMAGE_TAG}" \
  .

echo "Built ${IMAGE_TAG}"
