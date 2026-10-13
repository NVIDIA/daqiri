# C++ tests

This directory is reserved for build-backed C++ unit and integration tests. Register
them with CTest so configured builds have a common entry point for native, binding,
and platform tests. See the parent [testing guide](../README.md) for the test taxonomy.

With `DAQIRI_ENGINE="ibverbs"` and `DAQIRI_IBVERBS_DLOPEN=ON`, CTest also
checks real glue loading, search beside the executable with environment overrides
unset, concurrent cached loads, missing DSOs and entry
points, incompatible/incomplete tables, and Linux socket operation without RDMA
glue. These checks require RDMA userspace libraries but do not require a NIC.
