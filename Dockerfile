# syntax=docker/dockerfile:1

# SPDX-FileCopyrightText: Copyright (c) 2024-2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

ARG DAQIRI_BASE_TARGET=dpdk
ARG DAQIRI_MGR="dpdk socket"
ARG DAQIRI_BUILD_PYTHON=OFF
ARG DAQIRI_ENABLE_S3=OFF
ARG BUILD_SHARED_LIBS=ON
ARG DAQIRI_ENABLE_OTEL_METRICS=OFF
ARG AWS_SDK_CPP_VERSION=1.11.822
ARG DAQIRI_OS_BASE_IMAGE=nvcr.io/nvidia/cuda:13.1.0-devel-ubuntu24.04

# ============================================================
# base: Base layer
# ============================================================
FROM ${DAQIRI_OS_BASE_IMAGE} AS base
SHELL ["/bin/bash", "-eou", "pipefail", "-c"]

# Basic system dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl \
        ca-certificates \
        infiniband-diags \
        gnupg \
        python3-pip \
        git \
        build-essential \
        clang-format \
    && OS_CODENAME=$(. /etc/os-release && echo "$VERSION_CODENAME") \
    && KW_KEYRING="/usr/share/keyrings/kitware-archive-keyring.gpg" \
    && curl -fsSL "https://apt.kitware.com/keys/kitware-archive-latest.asc" \
        | gpg --dearmor -o "$KW_KEYRING" \
    && echo "deb [signed-by=$KW_KEYRING] https://apt.kitware.com/ubuntu/ $OS_CODENAME main" \
        > /etc/apt/sources.list.d/kitware.list \
    && apt-get update \
    && rm "$KW_KEYRING" \
    && CMAKE_VERSION="$(apt-cache madison cmake | awk '$3 ~ /^3[.]/ { print $3; exit }')" \
    && test -n "$CMAKE_VERSION" \
    && apt-get install --no-install-recommends -y \
        kitware-archive-keyring \
        "cmake=${CMAKE_VERSION}" \
        "cmake-data=${CMAKE_VERSION}" \
    && rm -rf /var/lib/apt/lists/*

# ==============================================================
# base-deps: base dependencies for the DAQIRI lib
# ==============================================================
FROM base AS base-deps

ARG TARGETARCH
ARG CACHEBUST=1
ARG DEBIAN_FRONTEND=noninteractive
ARG DOCA_VERSION=3.2.1
ARG AWS_SDK_CPP_VERSION

WORKDIR /opt

# Configure the DOCA APT repository so libibverbs / librdmacm / libmlx5
# (and later mft, mlnx-ofed-kernel-utils in the dpdk stage) resolve to
# MLNX OFED packages with intact -dev symlinks. Required because some
# upstream base images (notably nvcr.io/nvidia/pytorch) ship the stock
# Ubuntu packages with files stripped, leaving dpkg's database in a state
# where the package is "installed" but the -dev symlinks are missing on
# disk and the .deb is not re-downloadable.
RUN if [ "${TARGETARCH}" = "amd64" ]; then \
        DOCA_ARCH="x86_64"; \
    elif [ "$TARGETARCH" = "arm64" ]; then \
        DOCA_ARCH="arm64-sbsa"; \
    else \
        echo "Unknown architecture: $TARGETARCH"; \
        exit 1; \
    fi \
    && DISTRO=$(. /etc/os-release && echo ${ID}${VERSION_ID}) \
    && DOCA_REPO_LINK=https://linux.mellanox.com/public/repo/doca/${DOCA_VERSION}/${DISTRO}/${DOCA_ARCH} \
    && echo "Using DOCA_REPO_LINK=${DOCA_REPO_LINK}" \
    && LOCAL_GPG_KEY_PATH="/usr/share/keyrings/mellanox-archive-keyring.gpg" \
    && curl -fsSL ${DOCA_REPO_LINK}/GPG-KEY-Mellanox.pub | gpg --dearmor | tee ${LOCAL_GPG_KEY_PATH} > /dev/null \
    && echo "deb [signed-by=${LOCAL_GPG_KEY_PATH}] ${DOCA_REPO_LINK} ./" | tee /etc/apt/sources.list.d/mellanox.list

# APT installs for base dependencies
# - libibverbs-dev, librdmacm-dev: RDMA/ibverbs support for Mellanox NICs
# - libmlx5-1: Mellanox ConnectX driver
# - ibverbs-utils: utilities
# - python3-dev: for building python bindings
RUN apt-get update && apt-get install -y --no-install-recommends \
        libibverbs-dev \
        librdmacm-dev \
        libmlx5-1 \
        ibverbs-utils \
        python3-dev \
        pybind11-dev \
        libcurl4-openssl-dev \
        libssl-dev \
        uuid-dev \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Build only the AWS SDK for C++ S3 component. DAQIRI links this SDK only when
# configured with -DDAQIRI_ENABLE_S3=ON, but installing it here keeps the
# recommended container build path self-contained.
RUN git clone --depth 1 --recurse-submodules --shallow-submodules \
        --branch "${AWS_SDK_CPP_VERSION}" \
        https://github.com/aws/aws-sdk-cpp.git /tmp/aws-sdk-cpp \
    && cmake -S /tmp/aws-sdk-cpp -B /tmp/aws-sdk-cpp/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_ONLY=s3 \
        -DBUILD_SHARED_LIBS=ON \
        -DENABLE_TESTING=OFF \
        -DAUTORUN_UNIT_TESTS=OFF \
        -DCUSTOM_MEMORY_MANAGEMENT=OFF \
    && cmake --build /tmp/aws-sdk-cpp/build -j "$(nproc)" \
    && cmake --install /tmp/aws-sdk-cpp/build \
    && ldconfig \
    && rm -rf /tmp/aws-sdk-cpp

# PIP installs
# - pytest: test harness
# - pyyaml: to parse yaml configs in tests
# - scapy: for debugging and mocking network packets for tests
RUN python3 -m pip install --no-cache-dir --break-system-packages \
        pytest \
        pyyaml \
        scapy

# ==============================================================
# dpdk-deps: Build upstream DPDK from source
# ==============================================================
FROM base-deps AS dpdk

# DPDK version to download and build
ARG DPDK_VERSION=25.11
ARG DPDK_BUILD_DIR=/opt/dpdk-build
ARG DPDK_INSTALL_PREFIX=/usr/local

COPY dpdk_patches /tmp/dpdk_patches


# - ninja-build: for cmake build
# - pkgconf: to import dpdk in CMake
# - meson: for building DPDK
# - python3-pyelftools: required for DPDK build
# - libnuma-dev: NUMA support for DPDK
RUN apt-get update && apt-get install -y --no-install-recommends \
        ninja-build \
        pkgconf \
        meson \
        python3-pyelftools \
        libnuma-dev \
    && rm -rf /var/lib/apt/lists/*

# Download, build and install DPDK using meson/ninja
RUN curl -fsSL https://fast.dpdk.org/rel/dpdk-${DPDK_VERSION}.tar.xz -o /tmp/dpdk-${DPDK_VERSION}.tar.xz \
    && tar xf /tmp/dpdk-${DPDK_VERSION}.tar.xz -C /tmp \
    && cd /tmp/dpdk-${DPDK_VERSION} \
    && for patch in /tmp/dpdk_patches/*.patch; do \
         patch_name="$(basename "${patch}")"; \
         echo "Applying DPDK patch: ${patch_name}"; \
         if [[ "${patch_name}" == "dmabuf.patch" ]]; then \
           git apply \
             --exclude=.mailmap \
             --exclude=doc/guides/rel_notes/release_26_03.rst \
             "${patch}"; \
         else \
           git apply "${patch}"; \
         fi; \
       done \
    && meson setup ${DPDK_BUILD_DIR} \
        --prefix=${DPDK_INSTALL_PREFIX} \
        -Dtests=false \
        -Dplatform=generic \
        -Denable_docs=false \
        -Ddisable_drivers=baseband/*,bus/ifpga/*,common/cpt,common/dpaax,common/iavf,common/octeontx,common/octeontx2,crypto/nitrox,net/ark,net/atlantic,net/avp,net/axgbe,net/bnx2x,net/bnxt,net/cxgbe,net/e1000,net/ena,net/enic,net/fm10k,net/hinic,net/hns3,net/i40e,net/ixgbe,vdpa/ifc,net/igc,net/liquidio,net/mana,net/netvsc,net/nfp,net/qede,net/sfc,net/thunderx,net/vdev_netvsc,net/vmxnet3,regex/octeontx2 \
    && cd ${DPDK_BUILD_DIR} \
    && ninja \
    && meson install \
    && ldconfig \
    && rm -rf /tmp/dpdk-${DPDK_VERSION} /tmp/dpdk-${DPDK_VERSION}.tar.xz ${DPDK_BUILD_DIR} /tmp/dpdk_patches

# DOCA APT repository is configured in the base-deps stage above.
# - mlnx-ofed-kernel-utils for utilities including ibdev2netdev used by tune_system.py
# - mft for Mellanox Firmware Tools (mlxconfig, flint, etc.)
RUN apt-get update && apt-get install -y --no-install-recommends \
    mlnx-ofed-kernel-utils \
    mft \
    && rm -rf /var/lib/apt/lists/*

ARG DAQIRI_ENABLE_OTEL_METRICS
ARG OPENTELEMETRY_CPP_VERSION=v1.27.0
RUN if [ "${DAQIRI_ENABLE_OTEL_METRICS}" = "ON" ]; then \
      git clone --depth 1 --branch "${OPENTELEMETRY_CPP_VERSION}" \
        https://github.com/open-telemetry/opentelemetry-cpp.git /tmp/opentelemetry-cpp \
      && cmake -S /tmp/opentelemetry-cpp -B /tmp/opentelemetry-cpp-build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_TESTING=OFF \
        -DWITH_EXAMPLES=OFF \
        -DWITH_OTLP=OFF \
        -DWITH_PROMETHEUS=ON \
        -DWITH_ZIPKIN=OFF \
        -DWITH_ABSEIL=OFF \
        -DWITH_STL=CXX17 \
      && cmake --build /tmp/opentelemetry-cpp-build --target install -j "$(nproc)" \
      && ldconfig \
      && rm -rf /tmp/opentelemetry-cpp /tmp/opentelemetry-cpp-build; \
    fi

# ==============================================================
# rdma: Named target for consistent per-manager container builds.
# Identical to dpdk (which already includes RDMA/ibverbs deps).
# ==============================================================
FROM dpdk AS rdma

# ==============================
# Rivermax Target
# This stage is only built when --target rivermax is specified. It installs and configures Rivermax SDK.
# Note: Rivermax does not require DPDK - extends from base-deps directly.
# ==============================
FROM base-deps AS rivermax

# Define Rivermax-specific build arguments and environment variables
ARG RIVERMAX_VERSION=1.70.32
ARG RIVERMAX_SDK_ZIP_PATH=./rivermax_ubuntu2404_${RIVERMAX_VERSION}.tar.gz
ARG MAXPROC=8


# RMAX_TEGRA controls whether the build targets NVIDIA's Tegra platform (default is OFF).
# Affects Rivermax sample apps (via build args).
# Set to ON/TRUE to target Tegra devices like Jetson.
ARG RMAX_TEGRA=OFF
ENV RMAX_TEGRA=${RMAX_TEGRA}

# Install additional dependencies required for Rivermax
# Update package cache and install additional dependencies required for Rivermax
RUN apt-get update && apt-get install -y --no-install-recommends \
    iproute2 \
    libcap-dev \
    gdb \
    ethtool \
    iputils-ping \
    net-tools \
    libfmt-dev \
    libnl-3-dev \
    libnl-genl-3-dev \
    libglu1-mesa-dev \
    freeglut3-dev \
    mesa-common-dev \
    libglew-dev \
    cuda-nvml-dev-12-6 \
    cuda-nvtx-12-6 \
    && rm -rf /var/lib/apt/lists/*

# Copy and extract the Rivermax SDK
COPY ${RIVERMAX_SDK_ZIP_PATH} /tmp/rivermax_sdk.tar.gz
RUN if [ -f "/tmp/rivermax_sdk.tar.gz" ]; then \
      echo "Extracting Rivermax SDK..." && \
      tar -xzf /tmp/rivermax_sdk.tar.gz && \
      mv /opt/${RIVERMAX_VERSION} /opt/rivermax_sdk && \
      rm -v /tmp/rivermax_sdk.tar.gz; \
    else \
      echo "Error: Rivermax SDK tar.gz not found in /tmp"; exit 1; \
    fi

WORKDIR /opt/rivermax_sdk

# Find and install the Rivermax .deb package based on the build architecture
RUN DEB_FILE=$(find . -name "rivermax_${RIVERMAX_VERSION}_${TARGETARCH}.deb" -type f) && \
    if [ -f "$DEB_FILE" ]; then \
        echo "Installing Rivermax core from $DEB_FILE..." && \
        dpkg -i "$DEB_FILE"; \
    else \
        echo "Error: Rivermax ${TARGETARCH}.deb package not found"; exit 1; \
    fi

# Build Rivermax test and sample applications
RUN cd apps && \
    cmake -B build -DRMAX_CUDA=ON -DRMAX_TEGRA=${RMAX_TEGRA} -DRMAX_BUILD_VIEWER=ON && \
    cmake --build build -j $(nproc)

# Build rmax_apps_lib sample applications
RUN cd rivermax-dev-kit && \
    cmake -B build -DRMAX_CUDA=ON -DRMAX_TEGRA=${RMAX_TEGRA} -DRMAX_BUILD_VIEWER=ON && \
    cmake --build build -j $(nproc)

# ==============================
# daqiri-build: standalone configure/build/install from this repository
# ==============================
FROM ${DAQIRI_BASE_TARGET} AS daqiri-build

ARG DAQIRI_MGR
ARG DAQIRI_BUILD_PYTHON
ARG DAQIRI_ENABLE_S3
ARG BUILD_SHARED_LIBS
ARG DAQIRI_ENABLE_OTEL_METRICS

WORKDIR /workspace/daqiri
COPY . .
RUN rm -rf build

RUN cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/opt/daqiri \
      -DCMAKE_CUDA_ARCHITECTURES=all-major \
      -DBUILD_SHARED_LIBS=${BUILD_SHARED_LIBS} \
      -DDAQIRI_BUILD_PYTHON=${DAQIRI_BUILD_PYTHON} \
      -DDAQIRI_ENABLE_OTEL_METRICS=${DAQIRI_ENABLE_OTEL_METRICS} \
      -DDAQIRI_ENABLE_S3=${DAQIRI_ENABLE_S3} \
      -DDAQIRI_MGR="${DAQIRI_MGR}" \
    && cmake --build build -j "$(nproc)" \
    && cmake --install build

# ==============================
# runtime: carries DAQIRI install artifacts only
# ==============================
FROM ${DAQIRI_BASE_TARGET} AS runtime

COPY --from=daqiri-build /opt/daqiri /opt/daqiri
ENV CMAKE_PREFIX_PATH=/opt/daqiri
ENV LD_LIBRARY_PATH=/opt/daqiri/lib
EXPOSE 9464
WORKDIR /opt/daqiri
