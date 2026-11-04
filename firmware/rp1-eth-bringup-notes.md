# RP1 Ethernet bring-up notes (Pi 5) — from Raspberry Pi Linux rpi-6.12.y

Source files: arch/arm64/boot/dts/broadcom/rp1.dtsi, bcm2712-rpi-5-b.dts,
drivers/clk/clk-rp1.c, drivers/net/ethernet/cadence/macb_main.c

## MAC
- Cadence GEM, compatible "raspberrypi,rp1-gem" / "cdns,macb"
- Registers: RP1 offset 0x100000, length 0x4000
- IRQ: RP1 interrupt 6 (RP1_INT_ETH), level high
- Caps (macb_main.c raspberrypi_rp1_config): gigabit, HW clock change, jumbo
  (max 10240), PTP, DMA burst length 16
- DT extras: aw2w-max-pipe 8, ar2r-max-pipe 8, use-aw2b-fill
- phy-mode: rgmii-id (PHY adds the RGMII delays)

## PHY (Pi 5 board file)
- MDIO address 1, Broadcom PHY
- Reset: RP1 GPIO 32, active low, hold 5 ms
- brcm,powerdown-enable; EEE broken for 1000T and 100TX; led-modes <0 8>

## Clocks (RP1 clock block at RP1 offset 0x18000)
- pclk and hclk: clk_sys
- tx_clk: clk_eth  (ctrl 0x64, div_int 0x68, sel 0x70), max 125 MHz,
  aux parents: pll_sys_sec, pll_sys, pll_video_sec, gp0..gp5
- tsu_clk: clk_eth_tsu (ctrl 0x134, div_int 0x138, sel 0x140), max 50 MHz,
  aux parents: xosc, pll_video_sec, gp0..gp5
- CLK_CTRL_ENABLE = bit 11, AUXSRC = bits 9:5
- Linux sets rates via assigned-clocks, so the defaults left by the
  bootloader cannot be assumed. NOT yet implemented in firmware.

## Findings on real hardware (2026-11-04)
The list of "missing firmware work" that stood here turned out mostly unnecessary:
- The Pi boot code had already enabled the Ethernet clocks (clk_eth ctrl read
  0x10000800, enabled, div_int 1) and pads. The firmware needs no clock code.
- The ETH block did not need a reset release for link and data to work.
- The PHY only answers after the RP1 pad for GPIO 32 has its output-disable bit
  cleared and the reset line is pulsed. The driver does this at D0 entry.
- clk_eth_tsu (0x134) is not enabled by the boot code. The driver enables it
  (registry `TsuClk`, default 1); whether it is strictly needed is not proven.
- The GEM needs `NCFGR.DBW` set from `DCFG1.DBWDEF` (128-bit bus here) and the
  AXI pipeline values in `GEM_AMP` (0x54). Without them transmit never worked;
  see driver/RpiEth/DESIGN.md.

## Still unknown
1. What RP1_ETH_CFG_BASE (0x104000) controls; Linux's macb code does not touch
   it directly.
2. The clk_eth rate needed for 10/100 Mbit/s (Linux changes it per speed:
   125 / 25 / 2.5 MHz); only 1 Gbit/s is verified.
