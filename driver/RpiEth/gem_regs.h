/*
 * Cadence GEM register and descriptor definitions for the RP1 Ethernet MAC.
 * Values are taken from the Raspberry Pi Linux macb driver (macb.h) and have
 * NOT been verified on hardware. UNTESTED.
 */
#ifndef GEM_REGS_H
#define GEM_REGS_H

#include <stdint.h>

#define GEM_NCR     0x000
#define GEM_NCFGR   0x004
#define GEM_NSR     0x008
#define GEM_DMACFG  0x010
#define GEM_TSR     0x014
#define GEM_RBQP    0x018
#define GEM_TBQP    0x01C
#define GEM_RSR     0x020
#define GEM_ISR     0x024
#define GEM_IER     0x028
#define GEM_IDR     0x02C
#define GEM_IMR     0x030
#define GEM_MAN     0x034
#define GEM_SA1B    0x088
#define GEM_SA1T    0x08C
#define GEM_MID     0x0FC

/* NCR */
#define NCR_RE      (1u << 2)
#define NCR_TE      (1u << 3)
#define NCR_MPE     (1u << 4)
#define NCR_CLRSTAT (1u << 5)
#define NCR_TSTART  (1u << 9)

/* NCFGR */
#define NCFGR_SPD   (1u << 0)    /* 100 Mbit */
#define NCFGR_FD    (1u << 1)
#define NCFGR_CAF   (1u << 4)    /* copy all frames (promiscuous) */
#define NCFGR_NBC   (1u << 5)    /* no broadcast */
#define NCFGR_GBE   (1u << 10)   /* gigabit */
#define NCFGR_RXCOEN (1u << 24)  /* rx checksum offload */
#define NCFGR_DRFCS (1u << 17)   /* discard FCS from received frames */

/* DMACFG */
#define DMACFG_FBLDO_16     (16u << 0)
#define DMACFG_RXBMS_FULL   (3u << 8)
#define DMACFG_TXPBMS       (1u << 10)
#define DMACFG_RXBS(n64)    (((n64) & 0xFFu) << 16)  /* rx buf size / 64 */

/* Interrupts */
#define INT_RCOMP   (1u << 1)
#define INT_TCOMP   (1u << 7)
#define INT_RXUBR   (1u << 2)
#define INT_TXUBR   (1u << 3)
#define INT_HRESP   (1u << 11)

/* Descriptors, 32-bit addressing. 64-bit addressing needs the extended
 * descriptor format (DMACFG bit 30) and is NOT implemented here. */
typedef struct { uint32_t Addr; uint32_t Ctrl; } GEM_DESC;

#define RXD_ADDR_USED   (1u << 0)
#define RXD_ADDR_WRAP   (1u << 1)
#define RXD_CTRL_LEN(c) ((c) & 0x1FFFu)
#define RXD_CTRL_SOF    (1u << 14)
#define RXD_CTRL_EOF    (1u << 15)

#define TXD_CTRL_LEN(n) ((n) & 0x3FFFu)
#define TXD_CTRL_LAST   (1u << 15)
#define TXD_CTRL_WRAP   (1u << 30)
#define TXD_CTRL_USED   (1u << 31)

/* MDIO (MAN) */
#define MAN_SOF     (1u << 30)
#define MAN_READ    (2u << 28)
#define MAN_WRITE   (1u << 28)
#define MAN_CODE    (2u << 16)

#endif
