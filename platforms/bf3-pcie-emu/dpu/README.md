# BF3 controller integration

Base the controller on DOCA's `devemu_pci_device_dma` sample. Configure a
Generic PCI type with vendor ID `0x15b3`, device ID `0xda71`, and BAR0. On
CONFIGURE, acquire a DOCA mmap for the DMA addresses published by the host
driver; on START, service the queues whose BAR doorbells changed. Each queue
has its own work ring, completion ring, doorbell, and GPU region mapping. RX
batches DMA from preinitialized DPU memory directly into available GPU slots.
TX batches DMA from submitted GPU slots into independent DPU buffers. Each
queue waits once for its submitted DMA tasks before publishing its completion
batch and advancing its own consumer index.

This is not a network protocol. The NIC-like queue mechanics exist only for
parallel DMA and memory-region separation. Do not add Ethernet headers, flow
rules, TIRs, MPRQs, MAC/IP addresses, or packet steering.

The controller must not publish a completion until its DMA task has completed.
Handle FLR by stopping the context, dropping all mmap/buffer references, and
returning the device to the quiesced state before accepting a new epoch.
