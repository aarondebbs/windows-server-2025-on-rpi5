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

| Value                | Meaning                                                      |
|----------------------|--------------------------------------------------------------|
| MemResourcesMapped   | should be 2 (GEM window + config window)                     |
| GemModuleId          | 0x00000000 or 0xFFFFFFFF = block not clocked/not reachable   |
|                      | anything else = a live GEM (IDNUM in bits 27:16)             |
| PhyReadStatus        | 0 = MDIO worked, 0xC00000B5 = timeout                        |
| PhyId1Reg2/PhyId2Reg3| PHY identifier words; 0 or 0xFFFF = no PHY answering         |

## Risks
- Reading an unclocked RP1 block may hang the system. If the Pi freezes at boot
  after installing, boot the 32GB EFI card or Safe Mode and remove the driver
  (`pnputil /delete-driver oemNN.inf /uninstall /force`).
- Nothing here brings up clocks, reset or the PHY. The firmware still has to do
  that (see rp1-eth-bringup-notes.md).
