# Installing Windows Server (ARM64) on a Raspberry Pi 5 with diskpart and DISM

This is the manual install used instead of the normal Setup GUI. It is written with placeholders so you can adapt it to your own drives. Drive letters inside the Setup command prompt are assigned on the fly and differ from machine to machine, so **always verify them before running a destructive command**.

## Placeholders used below

| Placeholder | Meaning | Example |
|---|---|---|
| `<N>` | diskpart disk number of the **target** drive | `1` |
| `<INST>` | drive letter of the installer USB stick (has a `sources` folder) | `E` |
| `<FW>` | drive letter of the drive holding the Pi UEFI firmware files (has `config.txt`) | `C` |
| `S` | letter you give the target's EFI System partition | `S` |
| `W` | letter you give the target's Windows partition | `W` |
| `<IDX>` | edition index inside `install.wim` | `2` |

`S` and `W` are suggestions. If a letter is already taken, use another and substitute it everywhere.

## What you need

- Raspberry Pi 5, HDMI display, USB keyboard
- **Target drive:** a USB SSD, USB hard disk or NVMe drive. An SD card or SD card reader will usually be reported as *removable media*, and Windows refuses to partition or apply an image to removable media.
- **Firmware drive:** an SD card or USB stick with the Pi 5 UEFI release files (`rpi5-uefi`) on a FAT32 first partition. Keep one untouched copy as a fallback.
- **Installer stick:** a USB stick with the ARM64 Windows Server installer. Insider ARM64 ISOs can be generated with UUP dump.

## 1. Boot the installer

1. Plug in the firmware drive, the installer stick and the target drive.
2. Power on. At the Pi logo press **Esc**, open **Boot Manager** and choose the installer stick.
3. When Windows Setup appears, press **Shift+F10** for a command prompt.

## 2. Identify your drives (do not skip)

```
diskpart
list disk
list volume
```

Match drives by **size**, not by order:

| Drive | How to recognise it |
|---|---|
| Target | the size of your SSD / NVMe |
| Installer stick | small, and its volume has a `sources` folder |
| Firmware drive | its volume has `config.txt` and the Pi firmware files |

Write down the target's disk number as `<N>`. Check any letter with `dir X:\` (for example `dir E:\` should show `sources` on the installer stick).

Leave diskpart with `exit`.

## 3. Partition the target drive

**This erases the whole target disk.** Double-check `<N>`.

```
diskpart
select disk <N>
detail disk
```

If `detail disk` shows the wrong size or model, stop. Otherwise:

```
clean
convert gpt
create partition efi size=512
format quick fs=fat32 label="System"
assign letter=S
create partition msr size=16
create partition primary
format quick fs=ntfs label="Windows"
assign letter=W
list volume
exit
```

Notes:
- The EFI partition is 512 MB rather than the usual 100-260 MB because the Pi firmware files will be copied into it later.
- If `S:` or `W:` is reported as not found afterwards, the partition has no letter or no file system. In diskpart: `list partition`, `select partition <number>`, `assign letter=S` (or any free letter), and if `dir` still fails, `format quick fs=fat32` (EFI partition) or `format quick fs=ntfs` (Windows partition). You can use whatever letter Windows gave you, as long as you use it consistently below.
- "Cannot apply on removable media" or a similar error means Windows sees the target as removable. Use a different drive type (USB SSD or NVMe).

## 4. Choose the image index

```
dir <INST>:\sources\install.*
dism /Get-WimInfo /WimFile:<INST>:\sources\install.wim
```

- If the file is `install.esd`, use that name instead.
- Note the **Index** of the edition you want. On Server media, the first entries are often **Server Core** (no desktop, text console only) and later ones **Desktop Experience**. Check the names in the output.

## 5. Apply the image

DISM can fail in the Setup environment because its default scratch space is a small RAM disk, so give it a scratch folder on the target:

```
mkdir W:\scratch
dism /Apply-Image /ImageFile:<INST>:\sources\install.wim /Index:<IDX> /ApplyDir:W: /ScratchDir:W:\scratch
```

Wait for 100%. This is slow on slow storage.

Common errors:
| Error | Likely cause |
|---|---|
| "The system cannot find the file specified" (error 2) | wrong installer letter or file name (`install.wim` vs `install.esd`); re-check with `dir` |
| "The directory is invalid" | the target letter or scratch folder is wrong, or the volume is not mounted; run `dir W:\`, and use `/ScratchDir` as above |
| "Cannot apply on removable media" | the target is removable media |

## 6. Copy the Pi firmware into the EFI partition

So the Pi can boot from the target alone, copy the firmware files into the target's EFI System partition **before** adding Windows boot files:

```
dir <FW>:\
robocopy <FW>:\ S:\ /E /XD "System Volume Information"
dir S:\
```

`S:\` should now contain `config.txt` and the firmware files. If you do not do this, keep the firmware drive plugged in on every boot.

## 7. Add the Windows boot files

```
bcdboot W:\Windows /s S: /f UEFI
```

You should see "Boot files successfully created."

## 8. Reboot

```
wpeutil reboot
```

- To test booting from the target alone, remove the installer stick and the firmware drive first. If it fails to boot, plug the firmware drive back in, press **Esc** at the Pi logo, open **Boot Manager**, and pick the target drive.
- The first boot can show a black screen for several minutes, then "Getting devices ready" and a couple of restarts. Give it 10-30 minutes before worrying.

## Expected limitations

- Onboard Ethernet needs the patched firmware and the `RpiEthNic` driver (test-signing mode); see the main README. Until it is installed, use a USB Ethernet adapter with an in-box Windows driver.
- No GPU driver: Microsoft Basic Display Adapter only.
- No fan control: use a heatsink and fan.
- The ARM64 Server previews are time-limited and expired builds may nag or restart.
