#include "gem_hw.h"

int GemMdioWait(GEM *G)
{
    for (int i = 0; i < 2000; i++) {
        if (GemRd(G, GEM_NSR) & (1u << 2)) return 0;
        GemDelayUs(10);
    }
    return -1;
}

int GemMdioRead(GEM *G, int Phy, int Reg, uint16_t *V)
{
    if (GemMdioWait(G)) return -1;
    GemWr(G, GEM_MAN, MAN_SOF | MAN_READ | MAN_CODE | ((uint32_t)Phy << 23) | ((uint32_t)Reg << 18));
    if (GemMdioWait(G)) return -1;
    *V = (uint16_t)(GemRd(G, GEM_MAN) & 0xFFFF);
    return 0;
}

int GemMdioWrite(GEM *G, int Phy, int Reg, uint16_t V)
{
    if (GemMdioWait(G)) return -1;
    GemWr(G, GEM_MAN, MAN_SOF | MAN_WRITE | MAN_CODE | ((uint32_t)Phy << 23) | ((uint32_t)Reg << 18) | V);
    return GemMdioWait(G);
}

/* Reads BMSR (reg 1, latched, read twice) and the 1000BASE-T status (reg 10)
 * and link partner ability (reg 5). Works for the Broadcom PHY on the Pi 5. */
int GemPhyLink(GEM *G, int *Up, int *Mbps, int *Full)
{
    uint16_t Bmsr = 0, St1000 = 0, Lpa = 0;
    if (GemMdioRead(G, G->PhyAddr, 1, &Bmsr) || GemMdioRead(G, G->PhyAddr, 1, &Bmsr)) return -1;
    *Up = !!(Bmsr & (1u << 2));
    if (!*Up) { *Mbps = 0; *Full = 0; return 0; }
    GemMdioRead(G, G->PhyAddr, 10, &St1000);
    GemMdioRead(G, G->PhyAddr, 5, &Lpa);
    if (St1000 & (1u << 11))      { *Mbps = 1000; *Full = 1; }
    else if (St1000 & (1u << 10)) { *Mbps = 1000; *Full = 0; }
    else if (Lpa & (1u << 8))     { *Mbps = 100;  *Full = 1; }
    else if (Lpa & (1u << 7))     { *Mbps = 100;  *Full = 0; }
    else if (Lpa & (1u << 6))     { *Mbps = 10;   *Full = 1; }
    else                          { *Mbps = 10;   *Full = 0; }
    return 0;
}

void GemSetSpeed(GEM *G, int Mbps, int Full)
{
    uint32_t C = GemRd(G, GEM_NCFGR) & ~(NCFGR_SPD | NCFGR_GBE | NCFGR_FD);
    if (Mbps == 100)  C |= NCFGR_SPD;
    if (Mbps == 1000) C |= NCFGR_GBE;
    if (Full)         C |= NCFGR_FD;
    GemWr(G, GEM_NCFGR, C);
    /* TODO: RGMII needs the TX clock (clk_eth) set to 125/25/2.5 MHz to match
     * the speed. That is an RP1 clock-block write, done by the glue. */
}

/* Ring memory (G->Rx, G->Tx and their physical addresses) must already be
 * allocated, zeroed, and the RX buffers attached by the glue. */
void GemInit(GEM *G, const uint8_t Mac[6])
{
    GemWr(G, GEM_NCR, 0);
    GemWr(G, GEM_IDR, 0xFFFFFFFFu);
    GemWr(G, GEM_NCR, NCR_CLRSTAT);

    for (int i = 0; i < GEM_RING_SIZE; i++) {
        G->Tx[i].Ctrl = TXD_CTRL_USED | (i == GEM_RING_SIZE - 1 ? TXD_CTRL_WRAP : 0);
    }
    G->RxHead = G->TxHead = G->TxTail = 0;

    GemWr(G, GEM_RBQP, (uint32_t)G->RxPhys);
    GemWr(G, GEM_TBQP, (uint32_t)G->TxPhys);
    GemWr(G, GEM_DMACFG, DMACFG_FBLDO_16 | DMACFG_RXBMS_FULL | DMACFG_TXPBMS |
                         DMACFG_RXBS(GEM_RX_BUF_SIZE / 64));
    GemWr(G, GEM_NCFGR, GemRd(G, GEM_NCFGR) | NCFGR_DRFCS);

    GemWr(G, GEM_SA1B, Mac[0] | (Mac[1] << 8) | (Mac[2] << 16) | ((uint32_t)Mac[3] << 24));
    GemWr(G, GEM_SA1T, Mac[4] | (Mac[5] << 8));
}

void GemStart(GEM *G)
{
    GemWr(G, GEM_IER, INT_RCOMP | INT_TCOMP | INT_RXUBR | INT_HRESP);
    GemWr(G, GEM_NCR, GemRd(G, GEM_NCR) | NCR_RE | NCR_TE | NCR_MPE);
}

void GemStop(GEM *G)
{
    GemWr(G, GEM_NCR, GemRd(G, GEM_NCR) & ~(NCR_RE | NCR_TE));
    GemWr(G, GEM_IDR, 0xFFFFFFFFu);
}

int GemTxQueue(GEM *G, uint64_t BufPhys, uint32_t Len)
{
    uint32_t i = G->TxHead % GEM_RING_SIZE;
    if (!(G->Tx[i].Ctrl & TXD_CTRL_USED)) return -1;          /* ring full */
    G->Tx[i].Addr = (uint32_t)BufPhys;                        /* 32-bit only */
    G->Tx[i].Ctrl = TXD_CTRL_LEN(Len) | TXD_CTRL_LAST |
                    (i == GEM_RING_SIZE - 1 ? TXD_CTRL_WRAP : 0);
    G->TxHead++;
    GemWr(G, GEM_NCR, GemRd(G, GEM_NCR) | NCR_TSTART);
    return 0;
}
