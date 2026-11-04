# Graphics driver plan (Raspberry Pi 5, Windows Server ARM64)

**Status: research done, first probe drafted, nothing run on hardware yet.**
Everything below comes from reading the Raspberry Pi Linux tree (`rpi-6.12.y`:
`drivers/gpu/drm/{v3d,vc4}`, `drivers/pmdomain/bcm`, the BCM2712 device trees) and the
Pi 5 UEFI firmware source (`worproject/edk2-platforms`, branch `rpi5-dev`).

## 1. What "port the Linux driver" means here

The Ethernet driver worked because the hardware is one small block (a MAC and a PHY) and
the Linux `macb` driver is mostly "write these registers in this order". The graphics
hardware is different in two ways:

- **There are several blocks, not one.** Display output (HVS compositor, pixel valves,
  HDMI controllers and PHYs, IOMMU) and the 3D GPU (V3D 7.1) are separate hardware with
  separate Linux drivers (`vc4`, `v3d`), plus clocks, power and resets that Linux gets
  from the VideoCore firmware and the PM block.
- **The Linux drivers are written against the Linux DRM stack** (KMS, GEM, GPU scheduler,
  IOMMU, clock and power-domain frameworks). Windows has none of that, so the code
  can't be moved over. What carries over is the *hardware knowledge*: register maps, power
  and init sequences, command formats. We rewrite against the Windows display model (WDDM).

On top of that, a Windows GPU driver has two halves: a kernel-mode miniport and, for 3D, a
user-mode driver (Direct3D or Vulkan). Linux's equivalent of the user half is Mesa.

## 2. The hardware (from the Pi 5 device tree)

CPU physical addresses (the device tree's `0x7cxxxxxx` SoC addresses are `0x10_7cxxxxxx`).
GIC interrupt number = SPI number + 32 for ACPI.

| Block | Address (size) | Interrupt | Linux driver | Notes |
|---|---|---|---|---|
| Firmware mailbox | `0x10_7c013880` (0x40) | none (polled) | `bcm2835-mailbox`, `raspberrypi-firmware` | Property channel 8. **UEFI already uses it to set the screen up.** |
| PM block | `0x10_7d200000` (0x308) | none | `bcm2835-power` | `PM_GRAFX_2712` (+0x304), bit 6 `V3DRSTN`, password `0x5A` in the top byte |
| V3D hub / core0 / sms | `0x10_02000000` (0x4000) / `0x10_02008000` (0x6000) / `0x10_02030800` (0x700) | SPI 250, 249 | `v3d` | Needs firmware clock id 5 and the PM reset; has its own MMU |
| HVS (compositor) | `0x10_7c580000` (0x1a000) | via display intc: ch0/1/2 end-of-frame | `vc4_hvs.c`, `vc4_plane.c` | Behind **IOMMU4** at `0x10_00005200` |
| Pixel valves 0 / 1 | `0x10_7c410000` / `0x10_7c411000` (0x100) | SPI 101 / 110 | `vc4_crtc.c` | Display timing generators |
| Display interrupt controller | `0x10_7c502000` (0x30) | SPI 97 | `brcm,l2-intc` | |
| HDMI0 | `0x10_7c701400` + PHY `0x10_7c701d00` + others | always-on intc (hotplug, CEC) | `vc4_hdmi.c`, `vc4_hdmi_phy.c` | DDC (EDID) on I2C `0x10_7d508200` |
| HDMI1 | `0x10_7c706400` + PHY `0x10_7c706d00` + others | always-on intc | same | DDC on `0x10_7d508280` |

What the Linux driver gets from outside the block (the part that makes it hard):

- **Clocks** come from the VideoCore firmware over the mailbox (V3D = id 5, pixel = 9,
  HDMI-related = 13, 14, display = 16), not from registers the ARM cores can program.
- **V3D power** on BCM2712 is just: firmware clock on, then release `V3DRSTN` in the PM block
  (no bus-bridge handshake: `bcm2835-power.c` skips it when the PM node has no ASB window, which is the Pi 5 case).
- **Memory** for the HVS goes through an IOMMU; V3D has its own MMU with page tables.

## 3. Where the firmware helps

The Pi 5 UEFI display driver (`Platform/RaspberryPi/Drivers/DisplayDxe`) does not touch the
HVS or HDMI at all. It asks the VideoCore firmware for a framebuffer through the mailbox
(`SET_FB_PGEOM` / `SET_FB_VGEOM` / `SET_FB_DEPTH` / `ALLOC_FB` / `GET_FB_LINELENGTH`) and the
firmware does the mode setting and scanout. The same UEFI driver can switch between several
resolutions this way. So **mode setting through the firmware is a known-working path on this
board**, and that is what makes a modest first stage possible.

## 4. The plan

Each stage is useful on its own, and each decision point depends on data from the previous one.

### Stage 0: probe (drafted: `driver/RpiGpuProbe`, `firmware/gpu-acpi.patch`)

Show whether Windows can reach the mailbox, the PM block and V3D, and collect the
firmware's view of the display (current framebuffer, EDID, clock states). Three levels, from
read-only to powering up V3D and reading its ID registers. Nothing is drawn.

**Done when:** the mailbox answers from Windows (firmware revision comes back), and we have
the framebuffer geometry, EDID and V3D clock/reset state from the real board.

### Stage 1: firmware-assisted display driver

A WDDM *display-only* miniport (the pattern of Microsoft's KMDOD sample) that replaces
Microsoft Basic Display Adapter:

- takes over the screen UEFI already set up,
- reads the monitor's EDID through the mailbox and offers real modes,
- changes resolution by asking the firmware for a new framebuffer,
- Windows still composites in software; the driver just presents the frames.

What you get: proper resolution choice and a real display adapter in Device Manager, with
no HVS/HDMI programming. What you don't get: acceleration, a second display, or independence
from the firmware.

**Open questions the probe answers:** does the mailbox work from Windows; can the firmware
still allocate a new framebuffer after the OS has taken over; is the new framebuffer memory
one Windows will let a driver map.

**Done when:** a mode change from Windows Display Settings changes what the monitor shows.

### Stage 2: native display pipeline

Port the mode-setting hardware sequence from `vc4`: HVS display lists, pixel valve timing,
HDMI controller and PHY, IOMMU mappings, hotplug. This removes the dependence on the
firmware's framebuffer, and is what second-display support, high refresh rates and
variable modes need. It is a lot of hardware: several thousand lines of Linux driver to
understand and re-express, with a real chance of blank screens along the way (Stage 1 keeps a
working fallback).

**Done when:** the driver brings up HDMI0 (then HDMI1) without the firmware's help.

### Stage 3: V3D 3D / compute

Port the job path from `v3d`: power and clocks (Stage 0 covers this), the V3D MMU, command-list
submission (binner, renderer, compute) and interrupts. A kernel driver alone doesn't give
Windows apps a GPU: it also needs a user-mode driver. The realistic route is a Vulkan driver
from Mesa's `v3dv`, ported to Windows on top of our kernel interface. That gives Vulkan to
applications, not a hardware-accelerated desktop: speeding up the Windows desktop itself needs
a full WDDM render driver with a Direct3D user-mode driver, which is a separate, much larger
project.

**Done when:** a Vulkan test program (a spinning triangle) runs on the GPU.

## 5. Risks

- **The probe can hang the machine** at level 2 (V3D register read with the bridge down).
  Each risky step writes a `Stage` marker first, and the levels are opt-in.
- **Code is not compiled or run.** Like the Ethernet driver, expect build errors and
  several hardware rounds; here each round costs more (a firmware rebuild is needed for the ACPI patch).
- **The firmware ACPI patch is unverified** (no ASL compiler available while writing it; it
  applies cleanly to `rpi5-dev` and copies the existing `URT0` resource pattern).
- **Stage 1 depends on firmware behaviour after boot** that we haven't observed yet.
- **Effort is very uneven:** Stage 1 is comparable to the Ethernet effort; Stage 2 is several
  times that; Stage 3 is a long project.
- **Licensing:** the Linux `vc4`/`v3d`/Mesa code is GPL-2.0 / MIT. We re-implement behaviour from
  register-level understanding rather than copying code, but the repo has no licence yet and that
  should be decided before any ported Mesa code goes in.

## 6. Next step

1. Rebuild the firmware with `firmware/gpu-acpi.patch` (keep the Ethernet patch).
2. Build and install `driver/RpiGpuProbe`, run level 0, send the registry values.
3. Based on that, start Stage 1 (or fix the mailbox path first).

## Sources (Linux, `rpi-6.12.y`)

`arch/arm64/boot/dts/broadcom/bcm2712.dtsi`, `bcm2712-ds.dtsi`, `bcm2712-rpi.dtsi`;
`drivers/gpu/drm/v3d/*`; `drivers/gpu/drm/vc4/*`; `drivers/pmdomain/bcm/bcm2835-power.c`,
`raspberrypi-power.c`; `include/soc/bcm2835/raspberrypi-firmware.h`.
UEFI: `worproject/edk2-platforms` (`rpi5-dev`): `Platform/RaspberryPi/Drivers/{DisplayDxe,RpiFirmwareDxe}`,
`Platform/RaspberryPi/RPi5/AcpiTables/Dsdt.asl`.
