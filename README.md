# Windows Server on Raspberry Pi 5

Running **Windows Server (ARM64 Insider preview)** natively on a **Raspberry Pi 5**, on top of the archived [worproject/rpi5-uefi](https://github.com/worproject/rpi5-uefi) firmware, plus a from-scratch **onboard Gigabit Ethernet driver** for the Pi 5's RP1 chip.

![Platform](https://img.shields.io/badge/platform-Raspberry%20Pi%205-c51a4a)
![OS](https://img.shields.io/badge/OS-Windows%20Server%20ARM64%20(Insider)-0078d4)
![Boot](https://img.shields.io/badge/boots%20from%20SSD-working-brightgreen)
![Ethernet](https://img.shields.io/badge/onboard%20Ethernet-working-brightgreen)
![Signing](https://img.shields.io/badge/driver%20signing-test%20mode%20only-orange)
![GPU](https://img.shields.io/badge/graphics%20driver-upcoming-blue)
![Status](https://img.shields.io/badge/status-experimental-yellow)

> **Milestone (2026-11-04):** the onboard Ethernet port now gets a DHCP address on the LAN under Windows Server on the Pi 5, using the driver in [`driver/RpiEth`](driver/RpiEth).

---

## What's working

| | Legend |
|---|---|
| ✅ | Working, seen on real hardware |
| 🧪 | Works, but rough or only lightly tested |
| 🚧 | In progress |
| 📅 | Planned / upcoming |
| ❔ | Not tested yet |
| ❌ | Not working or blocked |

### System

| Component | Status | Notes |
|---|:---:|---|
| Pi 5 UEFI firmware ([rpi5-uefi](https://github.com/worproject/rpi5-uefi)) | ✅ | Firmware files live in the SSD's EFI partition, so no separate card is needed |
| Install Windows Server ARM64 (Insider) | ✅ | Manual DISM install, see [`docs/install-procedure.md`](docs/install-procedure.md) |
| Boot from USB SSD | ✅ | SD cards appear as "removable" to Windows, so they can't be an install target |
| USB keyboard | ✅ | Used throughout Setup and on the console |
| HDMI display (Basic Display Adapter) | 🧪 | Unaccelerated framebuffer from the UEFI firmware, see [Upcoming](#upcoming-graphics-driver) |
| CPU frequency | ❔ | Windows reports ~0.25 GHz while `config.txt` sets `force_turbo=1`; probably a reporting issue, unconfirmed |

### Onboard Ethernet (RP1 Cadence GEM + Broadcom BCM54213PE)

| Feature | Status | Notes |
|---|:---:|---|
| ACPI device for the MAC (`ACPI\RPIE0001`) | ✅ | [`firmware/rp1-eth-acpi.patch`](firmware/rp1-eth-acpi.patch) |
| PHY bring-up (reset, RGMII delays, auto-negotiation) | ✅ | PHY ID `0x600d84a2`, MDIO address 1 |
| Link detect and 1 Gbit/s full duplex | ✅ | Link state is published to Windows |
| Receive path (interrupt driven) | ✅ | |
| Transmit path | ✅ | |
| DHCP and basic networking | ✅ | Gets a LAN address |
| Throughput benchmark (iperf) | ❔ | Not measured yet |
| Long-run stability | ❔ | |
| Cable unplug / replug | 🧪 | Link is polled once per second |
| 10 / 100 Mbit/s | ❔ | Driver advertises 1000BASE-T only by default; TX clock divider for slower speeds is unverified |
| Receive filter and multicast | 🚧 | Currently receive-all in hardware |
| Checksum and segmentation offloads | 📅 | |
| Power management and wake-on-LAN | 📅 | |

### Drivers and security

| Item | Status | Notes |
|---|:---:|---|
| Ethernet driver loads in **test-signing mode** | ✅ | `bcdedit /set testsigning on` |
| Ethernet driver without test mode | ❌ | Windows Server only accepts Microsoft-signed (HLK-certified) kernel drivers, see [Running without test mode](#running-without-test-mode) |

### Everything else (untested)

| Component | Status |
|---|:---:|
| Wi-Fi and Bluetooth | ❔ |
| HDMI and USB audio | ❔ |
| PCIe / NVMe over the FFC connector | ❔ |
| GPIO, I²C, SPI, PWM fan header | ❔ |
| Real-time clock | ❔ |

---

## Roadmap

**Ethernet**
- [x] Boot Windows Server ARM64 on the Pi 5 from an SSD
- [x] Expose the RP1 Ethernet MAC to Windows through ACPI
- [x] PHY bring-up from Windows
- [x] Link up at 1 Gbit/s
- [x] Receive path
- [x] Transmit path
- [x] DHCP lease on the LAN
- [ ] Strip the bring-up diagnostics out of the driver
- [ ] Receive filter and multicast
- [ ] 10/100 Mbit/s support
- [ ] Offloads (checksum, TSO)
- [ ] Throughput and stability testing
- [ ] Run without test signing (needs a Microsoft-signed build)

**Graphics**
- [ ] 📅 Raspberry Pi graphics driver, see below

---

## Upcoming: graphics driver

> **Status: planned, not started.** Today the Pi shows the firmware's plain framebuffer through Microsoft Basic Display Adapter: one fixed mode, no acceleration.

The Pi 5 has a VideoCore VII GPU and a separate display pipeline (HVS, pixel valves, HDMI). The Linux `vc4` and `v3d` drivers are the reference for how those blocks work. The plan is to build up in stages, each one useful on its own:

| Stage | Goal | Status |
|---|---|:---:|
| 1. Research | Map the display and GPU blocks the ARM cores can reach from Windows, and what the UEFI firmware already sets up | 📅 |
| 2. Display-only driver | A WDDM display-only miniport: proper mode setting, EDID, resolutions beyond the firmware default, both HDMI ports | 📅 |
| 3. Accelerated 2D | Hardware cursor and basic acceleration | 📅 |
| 4. 3D | Command submission to the V3D core and a user-mode driver (long term; a very large project) | 📅 |

---

## Quick start: onboard Ethernet

You need the firmware patch, a test-signed driver build, and test signing enabled in Windows.

1. **Firmware.** Apply [`firmware/rp1-eth-acpi.patch`](firmware/rp1-eth-acpi.patch) to `worproject/edk2-platforms`, rebuild the Pi 5 UEFI firmware and copy it to the EFI partition (see [Known build problems](#known-build-problems)). Windows then shows an unknown device `ACPI\RPIE0001`.
2. **Test signing.** On the Pi, in an elevated prompt: `bcdedit /set testsigning on`, then reboot.
3. **Build the driver** on a PC: Visual Studio 2022 with the ARM64 tools, Windows SDK and WDK; KMDF 1.33, NetAdapterCx 2.4; test-sign the package.
4. **Install** (remove the old `RpiEthProbe` driver first if present):
   ```
   pnputil /add-driver RpiEthNic.inf /install
   ```
5. **Plug in the cable.** The defaults need no registry settings. Within a few seconds `ipconfig` should show an address from your router.

Registry options and how the driver works are in [`driver/RpiEth/DESIGN.md`](driver/RpiEth/DESIGN.md).

## Running without test mode

Test signing is the only switch that lets a self-signed kernel driver load, and it adds a "Test Mode" watermark. What was found:

- **Attestation signing does not cover Server.** [Microsoft's signing page](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings) says Windows Server 2016 and later doesn't accept attested driver submissions. Server drivers must pass the full Hardware Lab Kit (HLK) tests and be signed through Partner Center, which also needs an EV code-signing certificate.
- **Custom kernel signers** ([docs](https://learn.microsoft.com/en-in/windows/security/application-security/application-control/app-control-for-business/design/custom-kernel-signers)) need Secure Boot and a clean Windows 11 24H2 install, and are not available on this firmware.
- **Worth trying (unverified on this build):** `bcdedit /set testsigning off` followed by `bcdedit /set nointegritychecks on`. Reports on recent Windows builds are mixed. If the NIC then shows error 52, run `bcdedit /set testsigning on` and reboot.

## Repository layout

| Path | What it is |
|---|---|
| [`driver/RpiEth/`](driver/RpiEth) | `RpiEthNic`, the NetAdapterCx Ethernet driver, with its [design notes](driver/RpiEth/DESIGN.md) |
| [`driver/RpiEthProbe/`](driver/RpiEthProbe) | Early probe driver that read the GEM and PHY IDs (superseded by `RpiEthNic`) |
| [`firmware/rp1-eth-acpi.patch`](firmware/rp1-eth-acpi.patch) | edk2-platforms patch that exposes the RP1 MAC via ACPI |
| [`firmware/rp1-eth-bringup-notes.md`](firmware/rp1-eth-bringup-notes.md) | RP1 clock, PHY and reset facts, and what was found on real hardware |
| [`docs/install-procedure.md`](docs/install-procedure.md) | Manual DISM install and firmware-on-ESP procedure |

## Known build problems

- Modern GCC (15+) fails in EDK2 BaseTools: build with `make -C edk2/BaseTools CC="gcc -std=gnu17"`.
- Some EDK2 submodules are gone upstream (`Zeex/subhook`); skip the unit-test submodules and create an empty `UnitTestFrameworkPkg/Library/CmockaLib/cmocka/include` if cmocka can't be fetched.
- Install `python-is-python3`.
- The main firmware volume is ~99% full.

## Other limitations

- The ARM64 Server previews are time-limited; expired builds may nag or restart.
- No fan control: use a heatsink and fan.

## Licensing and credits

The firmware patch modifies code under BSD-2-Clause-Patent (EDK2/edk2-platforms). No Microsoft binaries or ISOs are included; obtain Windows images yourself and respect the Insider Terms of Use.

Thanks to the [worproject](https://github.com/worproject) team for the Pi 5 UEFI firmware. The Raspberry Pi Linux `macb` driver was the reference for the GEM and RP1 behaviour.
