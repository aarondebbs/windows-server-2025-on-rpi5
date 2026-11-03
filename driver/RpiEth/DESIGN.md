# RpiEth data driver (skeleton, UNTESTED, not yet buildable)

Status: the hardware layer (`gem_regs.h`, `gem_hw.[ch]`) compiles as plain C
(syntax-checked on Linux with stub register accessors). It has never touched
real hardware. The NetAdapterCx glue is NOT written yet.

## Blocked on
1. A PHY answering on MDIO (probe driver v5 tests the reset pad fix).
2. DMA: confirm which addresses the GEM can reach through the RP1 PCIe bridge
   (the skeleton assumes 32-bit buffer addresses, so rings and buffers must be
   below 4 GB) and whether memory is cache-coherent. Wrong guess = corruption.
3. RGMII TX clock: clk_eth must follow link speed (125/25/2.5 MHz). Needs the
   RP1 clock window (already in the ACPI device).

## Still to write
- KMDF/NetAdapterCx glue: NetAdapterCreate, link state from `GemPhyLink`,
  packet queues (TX/RX) over the rings, interrupt DPC (RP1 shared line, MSI-X
  already enabled by firmware), MAC address source (Pi 5 OTP/DT, or random).
- DMA allocation via WDF common buffers (32-bit mask) and RX buffer attach.
- 64-bit descriptors if buffers above 4 GB are ever needed.
- INF as a Net class driver, matching ACPI\RPIE0001.
