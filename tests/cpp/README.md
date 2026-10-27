# C++ tests

This directory is reserved for build-backed C++ unit and integration tests. Register
them with CTest so configured builds have a common entry point for native, binding,
and platform tests. See the parent [testing guide](../README.md) for the test taxonomy.

With `DAQIRI_ENGINE="ibverbs"` and `DAQIRI_IBVERBS_DLOPEN=ON`, CTest also
checks real glue loading, search beside the executable with environment overrides
unset, including relative executable launches followed by a working-directory
change before initialization, concurrent cached loads, missing DSOs and entry
points, incompatible/incomplete tables, and Linux socket operation without RDMA
glue. These checks require RDMA userspace libraries but do not require a NIC.

The ibverbs TX test also covers shared segment pool exhaustion, reference-count
overflow, concurrent final release, private header isolation, delayed clones,
clone-of-clone, metadata recycling, arbitrary cancellation, cumulative completion,
send-credit exhaustion, and CQ-error retention for one through four segments.
