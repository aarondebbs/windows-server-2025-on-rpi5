# Windows Server 2025 on Raspberry Pi 5

Notes, patches and experimental code from an attempt to run **Windows Server (ARM64 Insider preview, 2025-era build)** natively on a **Raspberry Pi 5**, using the archived [worproject/rpi5-uefi](https://github.com/worproject/rpi5-uefi) firmware.

> **Status: experimental.** The server boots from an SSD. Built-in Ethernet and GPU acceleration do not work. Nothing in `firmware/` or `driver/` has been verified on hardware yet.

## What worked
- Windows Server ARM64 (Insider preview) installed by applying `install.wim` with DISM to an SSD, then booted on the Pi 5.
- The Pi UEFI firmware files copied into the SSD's EFI system partition, so the Pi boots from the SSD alone (no separate firmware card).

## What doesn't (yet)
| Area | State |
|---|---|
| Onboard Ethernet (RP1 GEM) | Not working: no ACPI device, no clocks/PHY init in firmware, no Windows driver |
| GPU | No driver; Microsoft Basic Display Adapter only |
| CPU frequency | Windows reports ~0.25 GHz; `config.txt` sets `force_turbo=1`, so likely a reporting issue (unconfirmed) |
| Install target | SD cards are "removable" to Windows, so Setup/DISM refuse them; use a USB SSD or NVMe |
| Licence/expiry | ARM64 Server previews have expired; Microsoft has announced no ARM64 Server release |

## Repository layout
- `docs/install-procedure.md` - the manual DISM install and firmware-on-ESP procedure
- `firmware/rp1-eth-acpi.patch` - patch for `worproject/edk2-platforms` that exposes the RP1 Ethernet MAC to Windows via ACPI (builds; untested on hardware)
- `firmware/rp1-eth-bringup-notes.md` - clocks, PHY, reset and interrupt facts taken from the Raspberry Pi Linux tree
- `driver/RpiEthProbe/` - stage-1 KMDF probe driver that reads the GEM module ID and PHY ID (uncompiled, untested)

## Known problems with the build
- Modern GCC (15+) fails in EDK2 BaseTools: build with `make -C edk2/BaseTools CC="gcc -std=gnu17"`.
- Some EDK2 submodules are gone upstream (`Zeex/subhook`); skip the unit-test submodules and create an empty `UnitTestFrameworkPkg/Library/CmockaLib/cmocka/include` if cmocka can't be fetched.
- Install `python-is-python3`.
- The main firmware volume is ~99% full; adding clock/PHY code will need space freed up.

## Licensing and sources
The patch modifies code under BSD-2-Clause-Patent (EDK2/edk2-platforms). No Microsoft binaries or ISOs are included; obtain Windows images yourself and respect the Insider Terms of Use.


## Status (2026-11-04)

- PHY bring-up works: the PHY (Broadcom BCM54213PE, ID 0x600d84a2, MDIO address 1)
  only answers after the RP1 pad for GPIO 32 has its output-disable bit cleared and the
  reset line is pulsed. Clocks and Ethernet pads were already enabled by the Pi boot code.
- `driver/RpiEth/RpiEthNic.c` is a NetAdapterCx driver (KMDF 1.33, NetAdapterCx 2.4).
  It installs, starts, brings up the PHY and reports link state to Windows.
- The data path (DMA) is OFF by default (`DataPath` registry value = 0). With it on, the
  first test froze the machine, probably because the DMA address the RP1 needs is not
  yet known (see `driver/RpiEth/DESIGN.md`, registry `DmaAddrOffsetHigh`).
- No internet yet: the packet engines are not enabled.

### Next steps
1. Work out the DMA address the GEM must be given (try `DmaAddrOffsetHigh` = 0x10).
2. Enable `DataPath` one engine at a time (TX first, then RX) and check for traffic.
3. Receive filtering, multicast, offloads, 10/100 TX clock divider.
