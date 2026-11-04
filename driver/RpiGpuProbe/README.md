# RpiGpuProbe - build and test notes (UNTESTED, never compiled)

First step of the graphics work: a probe driver that finds out, on real hardware,
what Windows can reach on the Pi 5's VideoCore side. See
[`docs/graphics-driver-plan.md`](../../docs/graphics-driver-plan.md) for why.

It does **not** draw anything. It talks to the VideoCore firmware over the mailbox
(the same way the Pi 5 UEFI firmware sets up the screen), reads a PM register, and
optionally powers up the V3D GPU and reads its ID registers. Results are written to
the device's registry key.

## What you need

1. **Firmware with the GPU ACPI device.** Apply [`firmware/gpu-acpi.patch`](../../firmware/gpu-acpi.patch)
   to `worproject/edk2-platforms` (branch `rpi5-dev`), rebuild, and copy the firmware to the
   EFI partition. It adds one ACPI device, `RPIG0001`, with the mailbox, PM and V3D windows.
   (Keep your existing `rp1-eth-acpi.patch` applied too.)
2. **Build** like the other drivers: Visual Studio + WDK, ARM64, KMDF. The code uses
   `ntstrsafe.h`. Output: `RpiGpuProbe.sys` / `.inf` / `.cat`.
3. **Test signing on** (see the main README).

## Install

```
pnputil /add-driver RpiGpuProbe.inf /install
```

Windows shows the device "Raspberry Pi 5 VideoCore / GPU (probe)". Results appear in:

`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RPIG0001\0\Device Parameters`

## Run it in three steps (do them in order)

Set `ProbeLevel` (DWORD) in the key above, then restart the device:
`pnputil /restart-device "ACPI\RPIG0001\0"`

| ProbeLevel | What happens | Risk |
|---|---|---|
| 0 (default) | Mailbox GET requests only, reads the PM register | Low |
| 1 | Also powers up V3D (firmware clock on + rate, releases `V3DRSTN`) | Medium |
| 2 | Also reads the V3D ID registers | **Can hang the machine** if the bus bridge is not up |

Start at 0. Only go to 1 if level 0 worked, and only to 2 if level 1 worked. The value
`Stage` is written before each risky step; if the Pi freezes, reboot and read `Stage`
(it tells us where it stopped).

## What to send back

A screenshot (or list) of the values in the key. The important ones:

| Value | Tells us |
|---|---|
| `FwRevision`, `BoardRevision`, `FwRevStatus` | The mailbox works from Windows at all (status 0 = success) |
| `MsgBusAddr`, `MsgPhysLo` | Buffer address used (if the mailbox times out, try a different `BusOffset`) |
| `FbWidth`, `FbHeight`, `FbDepth`, `FbPitch` | The framebuffer UEFI set up |
| `Edid0` (binary) | The monitor's EDID, so we can list real modes |
| `ClkRate_5`, `ClkMax_5`, `ClkState_5` | V3D clock (id 5) state/range; ids 9, 13, 14, 16 are display clocks |
| `PmGrafxInitial`, `PmGrafxBeforeReset`, `PmGrafxAfterReset` | Whether V3D is already out of reset (bit 6) |
| `V3dHubIdent0..3`, `V3dCoreIdent0..2` | V3D identification (level 2). In `V3dHubIdent1`, bits 3:0 are the tech version and 7:4 the revision (Pi 5 should be V3D 7.1); `V3dCoreIdent0` bits 31:24 are the core version. All-ones or all-zeros means the registers are not really answering |

If the mailbox calls return a timeout status (`0xC00000B5`), nothing else will work and
that is the first thing to fix.

## Registry options

| Value | Default | Meaning |
|---|---|---|
| `ProbeLevel` | 0 | See above |
| `BusOffset` | `0xC0000000` | Added to the mailbox buffer address (VideoCore bus alias, from the UEFI DMA setup) |
| `V3dRate` | 0 | V3D clock in Hz at level 1; 0 = firmware maximum |
