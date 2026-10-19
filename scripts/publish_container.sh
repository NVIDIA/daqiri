#!/usr/bin/env bash
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

IMAGE="${IMAGE:-nvcr.io/nvstaging/holoscan/daqiri}"
PUSH="${PUSH:-0}"
PUBLISH_MANIFEST="${PUBLISH_MANIFEST:-0}"
ARCH="${ARCH:-$(uname -m)}"

case "${ARCH}" in
  x86_64) ARCH=amd64 ;;
  aarch64|arm64) ARCH=arm64 ;;
  amd64) ;;
  *)
    echo "ERROR: unsupported architecture '${ARCH}'" >&2
    exit 1
    ;;
esac

version="$(cat VERSION)"
if ! printf '%s' "${version}" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
  echo "ERROR: VERSION '${version}' is not YYYY.MM.PATCH" >&2
  exit 1
fi

login_registry() {
  if [ -z "${NGC_API_KEY:-}" ]; then
    echo "ERROR: NGC_API_KEY is required to publish" >&2
    exit 1
  fi

  printf '%s' "${NGC_API_KEY}" | docker login nvcr.io -u '$oauthtoken' --password-stdin
}

if [ "${PUBLISH_MANIFEST}" = "1" ]; then
  login_registry
  docker buildx imagetools create \
    -t "${IMAGE}:${version}" \
    "${IMAGE}:${version}-amd64" \
    "${IMAGE}:${version}-arm64"
  docker buildx imagetools inspect "${IMAGE}:${version}"
  exit 0
fi

IMAGE_TAG="${IMAGE}:${version}-${ARCH}" scripts/build-container.sh

if [ "${PUSH}" != "1" ]; then
  echo "Built ${IMAGE}:${version}-${ARCH}; set PUSH=1 to publish."
  exit 0
fi

login_registry
docker push "${IMAGE}:${version}-${ARCH}"
