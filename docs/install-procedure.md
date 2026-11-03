# Install procedure (what worked)

Hardware: Raspberry Pi 5, a USB SSD (fixed disk) as the install target, a USB stick with the Server ARM64 installer, a second card/stick holding the stock rpi5-uefi release files, HDMI, USB keyboard.

1. **Firmware.** Format a drive's first partition FAT32 and extract the rpi5-uefi release zip to its root. Keep it as a fallback.
2. **Installer media.** Build an ARM64 Windows Server ISO with UUP dump, write it to a USB stick.
3. **Boot.** Power on, press Esc at the Pi logo, Boot Manager, choose the installer stick. Setup did not list the built-in SD slot's disk reliably, and Windows refuses to partition removable media ("cannot apply on removable media"), so use a USB SSD.
4. **Partition the SSD** (Shift+F10 in Setup; check the disk number by size first, this wipes it):
   ```
   diskpart
   select disk <n>
   clean
   convert gpt
   create partition efi size=512
   format quick fs=fat32 label="System"
   assign letter=S
   create partition msr size=16
   create partition primary
   format quick fs=ntfs label="Windows"
   assign letter=W
   exit
   ```
   If `S:` is not found, the System partition may have no usable letter or file system: select it, `assign letter=`, and `format quick fs=fat32`.
5. **Pick the right image index** with `dism /Get-WimInfo /WimFile:<installer>:\sources\install.wim`. On Server media index 1 is typically Server Core; Desktop Experience is a later index.
6. **Apply the image:**
   ```
   mkdir W:\scratch
   dism /Apply-Image /ImageFile:<installer>:\sources\install.wim /Index:<n> /ApplyDir:W: /ScratchDir:W:\scratch
   ```
7. **Copy the firmware to the SSD's EFI partition, then add boot files:**
   ```
   robocopy <firmware drive>:\ S:\ /E /XD "System Volume Information"
   bcdboot W:\Windows /s S: /f UEFI
   ```
8. **Reboot** with only the SSD attached. First boot shows a black screen for a while, then "Getting devices ready"; give it time.

Expect: no onboard network (use a USB Ethernet adapter), Basic Display Adapter, and no fan control.
