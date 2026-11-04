# RpiEth data driver (UNTESTED, never compiled against the WDK)

`RpiEthNic.c` is a NetAdapterCx KMDF driver for the RP1 GEM + BCM54213PE.
It was written from the Raspberry Pi Linux `macb` driver and Microsoft's
NetAdapterCx docs/sample, and has not been built or run.

## What it does
- Maps the five ACPI memory windows (GEM, cfg, GPIO bank1+pads, ETH pads, clocks).
- D0 entry: configures the GEM (RGMII, 64-bit descriptors, MDC divider),
  clears the GPIO 32 pad output-disable bit, resets the PHY, verifies the
  Broadcom ID, enables RGMII-ID delays, restarts auto-negotiation.
- 1 s timer reads the PHY link and publishes link state/speed.
- TX/RX packet queues over 128-entry rings in WDF common buffers; one fragment
  per TX packet; ISR masks interrupts and the DPC notifies the queues.

## Registry knobs (device key, Device Parameters)
- `DmaAddrOffsetHigh` (DWORD): value added (shifted left 32) to every DMA
  address given to the GEM. Default 0. If the RP1 sees host RAM at a PCIe
  offset (Linux device tree suggests 0x10_0000_0000), set it to 0x10.
- `OnlyGigabit` (DWORD, default 1): advertise 1000BASE-T only so the RGMII TX
  clock stays at 125 MHz. Set 0 to allow 10/100 (clock divider unverified).
- `MacAddress` (REG_BINARY 6): generated on first start if the GEM has none.

- `DataPath` (DWORD, default 0): bit 0 enables the TX engine, bit 1 the RX engine
  (1 = TX only, 2 = RX only, 3 = both, 0 = link only).

## Open problems
1. DMA addressing and cache coherency through the RP1 PCIe bridge.
2. 10/100 TX clock divider (guess: 125 MHz / 5 or / 50).
3. Receive filter: the GEM currently copies all frames.
4. Offloads (checksum, TSO), multicast hash, flow control, power management.
