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

## Still missing in firmware (untested territory)
1. Program clk_eth to 125 MHz (gigabit; macb driver changes it per link speed:
   125 / 25 / 2.5 MHz) and clk_eth_tsu to 50 MHz, then enable both.
2. Release the Ethernet block from reset (RP1 resets block at offset 0x14000).
3. Pulse RP1 GPIO 32 low for >= 5 ms to reset the PHY (needs RP1 GPIO/pad setup).
4. Check what RP1_ETH_CFG_BASE (0x104000) controls; Linux's macb code does not
   touch it directly, so its role (RGMII/clock config) needs the RP1 datasheet.
