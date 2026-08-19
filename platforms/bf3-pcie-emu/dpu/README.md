# BF3 controller integration

Base the controller on DOCA's `devemu_pci_device_dma` sample. Configure a
Generic PCI type with vendor ID `0x15b3`, device ID `0xda71`, BAR0, and a
doorbell. On CONFIGURE, acquire a DOCA mmap for the DMA addresses published by
the host driver; on START, poll the TX-submission and RX-available rings. For
each TX entry, DMA-read the GPU slot, DMA-write the bytes into one available RX
GPU slot, then publish the RX and TX completions with release ordering.

The controller must not publish a completion until its DMA task has completed.
Handle FLR by stopping the context, dropping all mmap/buffer references, and
returning the device to the quiesced state before accepting a new epoch.
