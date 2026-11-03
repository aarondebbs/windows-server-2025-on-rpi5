# RpiEthProbe - build and test notes (UNTESTED)

Probe driver for the Pi 5 RP1 Ethernet MAC. It only maps the registers, reads
the GEM module ID and the PHY ID, and stores them in the registry.

## Build (on a Windows PC)
1. Install Visual Studio 2022 with the "Desktop development with C++" workload,
   the ARM64 build tools, the matching Windows SDK and the WDK.
2. New project -> "Kernel Mode Driver, Empty (KMDF)". Name it RpiEthProbe.
3. Delete the template's INF. Add RpiEthProbe.c and RpiEthProbe.inf from this folder
   (Add -> Existing Item). If Visual Studio wants an .inx, rename the file to
   RpiEthProbe.inx and keep $KMDFVERSION$.
4. Set the platform to ARM64 and the configuration to Release, then Build.
   Output: RpiEthProbe.sys, RpiEthProbe.inf, RpiEthProbe.cat.
   If the build complains about warnings treated as errors, tell me the message.

## Install on the Pi (Windows Server)
Windows will not load an unsigned kernel driver on ARM64.
1. Elevated cmd: `bcdedit /set testsigning on`, then reboot.
   (A "Test Mode" watermark will appear. Undo with `bcdedit /set testsigning off`.)
2. Import the test certificate that Visual Studio created for the build
   (the .cer next to the .sys) into "Trusted Root Certification Authorities" and
   "Trusted Publishers" on the Pi (certmgr.msc, or `certutil -addstore`).
3. Copy the three output files to the Pi, then:
   `pnputil /add-driver RpiEthProbe.inf /install`
   (or Device Manager -> the unknown ACPI\RPIE0001 device -> Update driver).

## Read the result
regedit -> HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RPIE0001\<instance>\Device Parameters

Driver v2 values (v1 only had MemResourcesMapped, GemModuleId, PhyReadStatus and the PhyId pair):

| Value                | Meaning                                                      |
|----------------------|--------------------------------------------------------------|
| MemResourcesMapped   | 2 with the first firmware patch, 3 with the GPIO-window patch |
| GpioMapped           | 1 if the RP1 GPIO bank window was mapped, else 0             |
| PhyResetPulsed       | 1 if the driver pulsed RP1 GPIO 32 (PHY reset)               |
| GemModuleId          | 0x00000000 or 0xFFFFFFFF = block not reachable;              |
|                      | anything else = a live GEM (IDNUM in bits 27:16, e.g. 0x00070163) |
| PhyAddrBeforeReset   | MDIO address where a PHY answered before the reset pulse, or 0xFFFFFFFF (4294967295) for none |
| PhyId1BeforeReset / PhyId2BeforeReset | that PHY's ID words (0 if none)             |
| PhyAddrAfterReset    | MDIO address where a PHY answered after the reset pulse, or 0xFFFFFFFF for none |
| PhyId1Reg2 / PhyId2Reg3 | ID words of the PHY found after reset (0 if none)         |
| PhyAddr1Reg2 / PhyAddr1Reg3 | raw reads at address 1 (the device-tree address); 65535 (0xFFFF) = nothing driving the bus |

First results (v1, Pi 5, firmware with the ETH0 ACPI device): MemResourcesMapped 2,
GemModuleId 0x00070163 (live GEM), MDIO completed, PHY ID words 0xFFFF at address 1,
so no PHY was answering. v2 adds the reset pulse and an all-address scan to find out why.

## Risks
- Reading an unclocked RP1 block may hang the system. If the Pi freezes at boot
  after installing, boot the 32GB EFI card or Safe Mode and remove the driver
  (`pnputil /delete-driver oemNN.inf /uninstall /force`).
- v2 drives RP1 GPIO 32 (output, low then high). That is the PHY reset line on the Pi 5
  according to the Raspberry Pi device tree, but this is untested on hardware.
- The Ethernet clocks (clk_eth, clk_eth_tsu) are still not enabled by anything, so the
  data path cannot work yet (see firmware/rp1-eth-bringup-notes.md).
