# Platform tests

This directory is reserved for correctness and qualification tests that require a
provisioned GPU, NIC, privileged container, hugepages, or specific host topology. Keep
test logic capability-oriented; dedicated CI/CD jobs select it according to each
runner's declared platform profile. See the parent [testing guide](../README.md) for
the marker, preflight, and local-reproduction policy.
