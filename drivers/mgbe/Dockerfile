# SPDX-License-Identifier: Apache-2.0
# Build the repository's normal project image first, then add runtime test tools.
ARG BASE_IMAGE=daqiri:local
FROM ${BASE_IMAGE}
RUN apt-get update && apt-get install -y --no-install-recommends \
    iproute2 ethtool psmisc kmod && rm -rf /var/lib/apt/lists/*
