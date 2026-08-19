# BlueField-3 PCIe-emulation harness

This harness turns a BlueField-3 Generic PCIe emulation function into the
reference DAQIRI third-party PCIe device. It is deliberately outside DAQIRI's
normal CMake build: the host component needs matching kernel headers and the
DPU component needs the DOCA SDK installed on the BF3.

## Components

- `host/daqiri_bf3_pcie.c` is a GPL-2.0-only Linux PCI driver. It creates the DAQIRI
  character node and sysfs discovery attribute, imports CUDA PCIe DMA-BUFs,
  allocates the coherent ownership rings, and conveys their DMA addresses to
  the emulated function through BAR0.
- `dpu/daqiri_bf3_controller.c` is the DOCA Generic PCI controller design. It
  owns the hot-plugged function and services the BAR doorbells; its data plane
  must use DOCA DMA against the host mappings established by the driver.

The implementation has one hard gate: CUDA's DMA-BUF must attach to the
emulated BF3 PCI device and its 64 KiB SG entries must describe one adjacent
BF3 DMA-address range. A failure here is a platform P2P/IOMMU limitation, not a
condition for a host-memory fallback.

## Bring-up

1. Install a supported DOCA release (2.7+) and BF3 firmware (32.41.1000+), put
   the BF3 in DPU mode, enable `PCI_SWITCH_EMULATION_ENABLE=1`, and reset it.
2. Enable the platform's documented IOMMU pass-through/hot-plug boot options;
   verify the BF3 and GPU share a peer-capable PCIe path.
3. Create and hot-plug the Generic PCI function with the DPU controller, then
   bind the host driver: `make -C host && sudo insmod host/daqiri_bf3_pcie.ko`.
4. Confirm `/sys/bus/pci/devices/<BDF>/daqiri_pcie/char` and the matching
   `/dev/daqiri-pcie-<BDF>` node exist. Run the BF3 YAML benchmark in `both`
   mode as documented in `docs/benchmarks/pcie_benchmarking.md`.

The device must complete an RX entry only after its GPU write is visible and a
TX entry only after all GPU reads have returned. The host driver refuses to
unmap GPU memory until STOP reports quiescence; RESET must synchronously stop
DMA.
