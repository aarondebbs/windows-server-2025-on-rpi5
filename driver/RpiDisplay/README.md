# RpiDisplay - Stage 1a: Microsoft's display-only sample on the Pi 5 (UNTESTED)

This is the first, cheapest display experiment. It uses **Microsoft's own KMDOD sample
driver unchanged** (apart from one line in its INF), so there is no new code of ours to debug.
Background and the plan are in [`docs/graphics-driver-plan.md`](../../docs/graphics-driver-plan.md).

## What it tests

1. Does Windows bind a display-only driver to our ACPI device and hand it the screen UEFI set up?
2. Does **a lower resolution make the desktop smoother?** The sample offers every standard mode
   up to the boot resolution (800x600 ... 1920x1200). A lower mode is drawn by Windows at the
   smaller size, so the CPU has fewer pixels to draw. It will appear **centred with black
   borders**, not full screen (full-screen modes are Stage 1b).

It is **not faster than Microsoft Basic Display Adapter at the same resolution.**

## What you need

- The firmware with [`firmware/gpu-acpi.patch`](../../firmware/gpu-acpi.patch) (adds the ACPI device
  `RPIG0001`). The same rebuild serves the GPU probe.
- A PC with Visual Studio 2022 + WDK + ARM64 build tools, and test signing on the Pi.

## Build

1. Get the sample: `git clone --depth 1 https://github.com/microsoft/Windows-driver-samples`
   and open `video\KMDOD\KMDOD.sln`.
2. Choose **Release | ARM64** (the solution and project already have ARM64 configurations) and build.
   Output: `SampleDisplay.sys`, `SampleDisplay.inf`, `SampleDisplay.cat`.
3. Edit `Sample\sampledisplay.inf` **in Visual Studio or Notepad** (the file is UTF-16; do not use
   a tool that rewrites its encoding). In the `[Standard.NTarm64]` section add one line:

   ```
   %SampleDeviceName% = KDODSamp_Inst, ACPI\RPIG0001
   ```

   (The existing `ACPI\CLS_0003&SUBCLS_0000` lines are Microsoft's generic ACPI-display IDs; our
   device matches by its own ID, so no ACPI class code is needed.)
4. Rebuild so the package picks up the INF, and test-sign it as you did for the Ethernet driver.

## Install on the Pi

If `RpiGpuProbe` is installed, remove it first (both match `RPIG0001`):

```
pnputil /enum-drivers
pnputil /delete-driver oemNN.inf /uninstall /force
pnputil /add-driver SampleDisplay.inf /install
```

Then reboot. In Device Manager a new display adapter "Kernel mode display only sample driver"
should appear (the Microsoft Basic Display Adapter may stay listed too).

## If the screen goes black or the driver fails

- The sample fails to start (Device Manager error 43) if Windows does not give it the POST display;
  the Basic Display Adapter keeps the screen, which is a safe outcome. Note the error code.
- If the console is unusable: boot the Windows installer USB, press Shift+F10, and disable the
  service the way you did for the Ethernet driver:
  `reg load HKLM\OFFSYS C:\Windows\System32\config\SYSTEM`, set
  `HKLM\OFFSYS\ControlSet001\Services\KDODSamp` value `Start` to 4, `reg unload HKLM\OFFSYS`.
- The sample does not support sleep (it bugchecks if the machine sleeps). A server should never sleep.

## What to report back

- Device Manager status for the adapter (working, or the error code).
- Display Settings: which resolutions are listed.
- At a lower resolution: does it look centred with borders, and **is the desktop smoother?**
  (Task Manager: CPU % while dragging a window, before and after.)

## Licence

Microsoft's sample is under the Microsoft Public License (MS-PL). When Stage 1b copies its
files into this repo, the MS-PL text and copyright notices stay with them; our own new files
keep whatever licence the repo ends up with.
