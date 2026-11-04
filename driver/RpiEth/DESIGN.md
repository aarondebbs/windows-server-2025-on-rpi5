# RpiEthNic design notes

`RpiEthNic.c` is a NetAdapterCx (2.4) / KMDF (1.33) driver for the Raspberry Pi 5's RP1 Ethernet: a Cadence GEM MAC and a Broadcom BCM54213PE PHY (MDIO address 1). It binds to `ACPI\RPIE0001`, which the firmware patch in `firmware/rp1-eth-acpi.patch` creates.

**Status:** link, receive, transmit and DHCP work on a real Pi 5 (test-signed build). It is still a bring-up driver: the diagnostics are still in the code, and there is no multicast filter, no offload and no 10/100 support yet.

## How it works

- **Resources.** Five ACPI memory windows are mapped: GEM registers, config, GPIO bank + pads, Ethernet pads, clocks. One interrupt.
- **D0 entry.** Resets the GEM, then programs it the way Linux `macb_init_hw()` does: RGMII in `USRIO`, `NCFGR` (including the data bus width), `GEM_AMP` (AXI pipeline), `DMACFG` (64-bit descriptors, 16-beat bursts), MAC address. Then PHY bring-up: clear the GPIO 32 pad output-disable bit, pulse the PHY reset, check the Broadcom ID, enable RGMII delays, restart auto-negotiation.
- **Link.** A one-second timer reads the PHY, sets the MAC speed and duplex, and publishes link state to Windows.
- **Data path: a copy model.** The driver owns its descriptor rings and packet buffers (uncached, contiguous, below 4 GB). Packets are copied between the OS fragments and those buffers through `NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS`. TX packets are completed to the OS as soon as they are copied. The framework's DMA mapping of OS buffers is not used (see "What we learned").
- **Rings.** 128 descriptors each way, 16-byte (64-bit address) descriptors, 2048-byte buffers.
- **Interrupts.** Needed for receive: without them nothing wakes the driver. The ISR masks the GEM interrupt and the DPC notifies the queues. Status is cleared by writing 1 to it (`DCFG1.IRQCOR` = 0).

## What we learned

These cost the most time, so they are written down:

1. **The GEM must be told the AXI bus width.** `DCFG1.DBWDEF` reads 4, meaning a 128-bit bus. Linux copies that into `NCFGR.DBW` (bits 22:21, value 2 for 128-bit). With the field left at 0 the DMA behaves as if the bus were 32 bits wide. Symptom: link up and receive working, but the transmit engine never completed a descriptor and its queue pointer ran away through memory regardless of ring contents.
2. **RP1 needs the AXI pipeline settings** that the Linux device tree gives it (`cdns,aw2w-max-pipe` = 8, `cdns,ar2r-max-pipe` = 8, `cdns,use-aw2b-fill`), written to `GEM_AMP` (offset 0x54).
3. Both fixes went into the same build, so it is **not known which one is strictly required**. Set `NoDbw` or `NoAmp` to 1 to switch one off and find out.
4. **Do not let the framework map OS buffers for this MAC.** The first data-path tests froze the whole machine; the copy model above removed the freeze.
5. **RX needs interrupts.** Polling-only left the receive ring unserviced.
6. Clocks and Ethernet pads were already enabled by the Pi boot code. The `clk_eth_tsu` timestamp clock is not, so the driver enables it (`TsuClk`, default on); whether that is needed is also unproven.
7. Descriptor memory kept below 4 GB, uncached, and identity-mapped worked; an upper address offset (`DmaAddrOffsetHigh`) made no difference.

## Registry options

Device key: `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RPIE0001\0\Device Parameters` (DWORD unless noted). The defaults work; none are required.

| Value | Default | Meaning |
|---|---|---|
| `DataPath` | 3 | Bit 0 = TX engine, bit 1 = RX engine, 0 = link only |
| `EnableIrq` | 1 | 0 = do not create the interrupt object |
| `PollSec` | 1 | Link poll interval in seconds |
| `OnlyGigabit` | 1 | Advertise 1000BASE-T only (10/100 TX clock divider is unverified) |
| `MacAddress` | generated | REG_BINARY, 6 bytes; kept if the GEM has none |
| `TsuClk` | 1 | 1 = enable `clk_eth_tsu`, 2 = also select the 50 MHz crystal source, 0 = leave alone |
| `NoDbw` | 0 | 1 = do not set `NCFGR.DBW` (diagnostic) |
| `NoAmp` | 0 | 1 = do not write `GEM_AMP` (diagnostic) |
| `Dma32` | 1 | Keep driver DMA buffers below 4 GB |
| `DmaAddrOffsetHigh` | 0 | Added (shifted left 32) to every DMA address; had no effect |
| `NoDma`, `Bisect`, `TxScan`, `TxLate`, `Force32` | 0 | Bring-up experiments, to be removed |

Driver trace values (counters, GEM registers, PHY state) are published under `HKLM\SYSTEM\CurrentControlSet\Services\RpiEthNic\Parameters`. Old values persist; delete `Cnt*` and `LinkUp` before a measurement run.

## Install and recovery notes

- Always unplug the cable before restarting the driver.
- Replace a driver: `pnputil /delete-driver oemNN.inf /uninstall /force`, then `pnputil /add-driver RpiEthNic.inf /install`, then `pnputil /restart-device "ACPI\RPIE0001\0"`.
- If a driver build hangs the machine at boot, boot the installer USB, press Shift+F10, `reg load HKLM\OFFSYS C:\Windows\System32\config\SYSTEM`, set `ControlSet001\Services\RpiEthNic\Start` to 4 (disabled), `reg unload HKLM\OFFSYS`, and reboot.
- Error `0xC0000428` means Windows rejected the signature: test signing is off.

## Open work

1. Remove the bring-up diagnostics and experiment knobs.
2. Receive filter and multicast hash (the GEM currently copies all frames).
3. 10/100 Mbit/s: the RGMII TX clock divider (guess: 125 MHz / 5 and / 50) is unverified.
4. Offloads (checksum, TSO), flow control, power management and wake-on-LAN.
5. Bisect `NoDbw` / `NoAmp` / `TsuClk` to find out which of the three are strictly required.
6. Throughput and long-run stability testing.
