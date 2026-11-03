/*
 * Hardware layer for the RP1 GEM: no WDF dependency, so it can be compiled and
 * unit-tested off target. UNTESTED on hardware.
 */
#ifndef GEM_HW_H
#define GEM_HW_H
#include <stdint.h>
#include "gem_regs.h"

#define GEM_RING_SIZE   64
#define GEM_RX_BUF_SIZE 2048

typedef struct {
    volatile uint8_t *Regs;
    GEM_DESC *Rx;  uint64_t RxPhys;  uint32_t RxHead;
    GEM_DESC *Tx;  uint64_t TxPhys;  uint32_t TxHead, TxTail;
    int       PhyAddr;
} GEM;

/* Platform hooks the glue code provides. */
uint32_t GemRd(GEM *G, uint32_t Off);
void     GemWr(GEM *G, uint32_t Off, uint32_t V);
void     GemDelayUs(uint32_t Us);

int  GemMdioRead (GEM *G, int Phy, int Reg, uint16_t *V);
int  GemMdioWrite(GEM *G, int Phy, int Reg, uint16_t V);
int  GemPhyLink(GEM *G, int *Up, int *Mbps, int *Full);   /* Clause 22 BMSR/1000T */
void GemInit(GEM *G, const uint8_t Mac[6]);
void GemSetSpeed(GEM *G, int Mbps, int Full);
void GemStart(GEM *G);
void GemStop(GEM *G);
int  GemTxQueue(GEM *G, uint64_t BufPhys, uint32_t Len);
#endif
