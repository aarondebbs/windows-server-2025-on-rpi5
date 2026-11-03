/*
 * RpiEthNic - EXPERIMENTAL NetAdapterCx driver for the Raspberry Pi 5 RP1
 * Ethernet MAC (Cadence GEM + Broadcom BCM54213PE PHY), ACPI\RPIE0001.
 *
 * STATUS: written from the Raspberry Pi Linux macb driver and Microsoft's
 * NetAdapterCx documentation/sample. It has NEVER been compiled against the
 * WDK or run on hardware. Expect compile errors to fix first.
 *
 * Known open questions (see DESIGN.md):
 *  - DMA address the RP1 must be given (registry "DmaAddrOffset" is added to
 *    every address programmed into the GEM; default 0).
 *  - TX clock rate for 10/100 Mbit (default: advertise gigabit only).
 *  - Cache coherency of the RP1 DMA path.
 */

#include <ntddk.h>
#include <wdf.h>
#include <netadaptercx.h>

#define NIC_TAG 'ciNR'

/* ------------------------------------------------------------------ */
/* Hardware definitions (from Linux macb.h, drivers/net/phy/broadcom.c) */
/* ------------------------------------------------------------------ */
#define GEM_NCR         0x000
#define GEM_NCFGR       0x004
#define GEM_NSR         0x008
#define GEM_DMACFG      0x010
#define GEM_TSR         0x014
#define GEM_RBQP        0x018
#define GEM_TBQP        0x01C
#define GEM_RSR         0x020
#define GEM_ISR         0x024
#define GEM_IER         0x028
#define GEM_IDR         0x02C
#define GEM_MAN         0x034
#define GEM_SA1B        0x098
#define GEM_SA1T        0x09C
#define GEM_USRIO       0x0C0
#define GEM_MID         0x0FC
#define GEM_TBQPH       0x4C8
#define GEM_RBQPH       0x4D4

#define NCR_RE          (1UL << 2)
#define NCR_TE          (1UL << 3)
#define NCR_MPE         (1UL << 4)
#define NCR_CLRSTAT     (1UL << 5)
#define NCR_TSTART      (1UL << 9)

#define NCFGR_SPD       (1UL << 0)
#define NCFGR_FD        (1UL << 1)
#define NCFGR_CAF       (1UL << 4)
#define NCFGR_BIG       (1UL << 8)
#define NCFGR_GBE       (1UL << 10)
#define NCFGR_DRFCS     (1UL << 17)
#define NCFGR_CLK_DIV224 (7UL << 18)
#define NCFGR_CLK_MASK  (7UL << 18)

#define NSR_IDLE        (1UL << 2)

#define DMACFG_FBLDO16  (16UL << 0)
#define DMACFG_RXBMS    (3UL << 8)
#define DMACFG_TXPBMS   (1UL << 10)
#define DMACFG_RXBS(n)  (((ULONG)(n) & 0xFF) << 16)
#define DMACFG_ADDR64   (1UL << 30)

#define USRIO_RGMII     (1UL << 0)

#define INT_RCOMP       (1UL << 1)
#define INT_RXUBR       (1UL << 2)
#define INT_TCOMP       (1UL << 7)
#define INT_ALL_DATA    (INT_RCOMP | INT_RXUBR | INT_TCOMP)

#define RSR_BNA         (1UL << 0)

/* 64-bit descriptor: addr lo, ctrl, addr hi, reserved */
typedef struct _GEM_DESC {
    volatile ULONG Addr;
    volatile ULONG Ctrl;
    volatile ULONG AddrHi;
    volatile ULONG Rsvd;
} GEM_DESC;

#define RXD_USED        (1UL << 0)   /* set by hw when the frame is stored */
#define RXD_WRAP        (1UL << 1)
#define RXD_LEN(c)      ((c) & 0x1FFFUL)
#define RXD_SOF         (1UL << 14)
#define RXD_EOF         (1UL << 15)

#define TXD_LEN(n)      ((ULONG)(n) & 0x3FFFUL)
#define TXD_LAST        (1UL << 15)
#define TXD_WRAP        (1UL << 30)
#define TXD_USED        (1UL << 31)

#define RING_SIZE       128
#define RX_BUF_SIZE     2048
#define MAX_FRAME       1518

/* GPIO / pad / clock windows (ACPI resources 2..4, see Rp1.asi) */
#define GPIO_PHY_RESET_PIN      4
#define GPIO_CTRL(n)            ((n) * 8 + 4)
#define GPIO_FUNCSEL_NULL       0x1FUL
#define GPIO_OUTOVER_LOW        (2UL << 12)
#define GPIO_OUTOVER_HIGH       (3UL << 12)
#define GPIO_OEOVER_ENABLE      (3UL << 14)
#define GPIO32_PAD_OFFSET       (0x20000 + 0x04 + 4 * GPIO_PHY_RESET_PIN)
#define PAD_OD                  (1UL << 7)
#define PAD_IE                  (1UL << 6)
#define CLK_ETH_DIV             0x068

/* PHY */
#define PHY_ADDR        1
#define PHY_ID1_BCM     0x600D
#define PHY_ID2_MASK    0xFFF0
#define PHY_ID2_BCM     0x84A0

#ifndef NDIS_PACKET_TYPE_DIRECTED
#define NDIS_PACKET_TYPE_DIRECTED       0x00000001
#define NDIS_PACKET_TYPE_MULTICAST      0x00000002
#define NDIS_PACKET_TYPE_ALL_MULTICAST  0x00000004
#define NDIS_PACKET_TYPE_BROADCAST      0x00000008
#define NDIS_PACKET_TYPE_PROMISCUOUS    0x00000020
#endif

/* ------------------------------------------------------------------ */
/* Contexts                                                           */
/* ------------------------------------------------------------------ */
typedef struct _NIC_DEVICE {
    WDFDEVICE      Device;
    NETADAPTER     Adapter;

    PUCHAR Gem;  SIZE_T GemLen;
    PUCHAR Cfg;  SIZE_T CfgLen;
    PUCHAR Gpio; SIZE_T GpioLen;
    PUCHAR Pads; SIZE_T PadsLen;
    PUCHAR Clk;  SIZE_T ClkLen;

    WDFDMAENABLER   Dma;
    WDFCOMMONBUFFER RxRingBuf, TxRingBuf;
    GEM_DESC       *RxRing, *TxRing;
    UINT64          RxRingLa, TxRingLa;

    WDFINTERRUPT    Interrupt;
    WDFTIMER        LinkTimer;
    volatile ULONG  PendingStatus;

    NETPACKETQUEUE  RxQueue, TxQueue;
    ULONG TxPost, TxClean;       /* free-running counters */
    ULONG RxPost, RxClean;

    UINT64 DmaOffset;
    ULONG  OnlyGigabit;
    UCHAR  Mac[6];
    BOOLEAN PhyUp;
    BOOLEAN LinkUp;
    ULONG   LinkMbps;
} NIC_DEVICE, *PNIC_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NIC_DEVICE, GetNic)

typedef struct _ADAPTER_CTX { PNIC_DEVICE Nic; } ADAPTER_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(ADAPTER_CTX, GetAdapterCtx)

typedef struct _QUEUE_CTX {
    PNIC_DEVICE Nic;
    NET_RING_COLLECTION const *Rings;
    NET_EXTENSION LogAddr;
} QUEUE_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(QUEUE_CTX, GetQueueCtx)

/* ------------------------------------------------------------------ */
/* Register helpers                                                   */
/* ------------------------------------------------------------------ */
static ULONG Rd(PUCHAR B, ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(B + Off)); }
static VOID  Wr(PUCHAR B, ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(B + Off), V); }
static ULONG G(PNIC_DEVICE N, ULONG Off) { return Rd(N->Gem, Off); }
static VOID  P(PNIC_DEVICE N, ULONG Off, ULONG V) { Wr(N->Gem, Off, V); }

static VOID MsDelay(ULONG Ms)
{
    LARGE_INTEGER T;
    T.QuadPart = -((LONGLONG)Ms * 10000);
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &T);
}

/* ------------------------------------------------------------------ */
/* MDIO / PHY                                                         */
/* ------------------------------------------------------------------ */
static BOOLEAN MdioIdle(PNIC_DEVICE N)
{
    ULONG i;
    for (i = 0; i < 2000; i++) {
        if (G(N, GEM_NSR) & NSR_IDLE) return TRUE;
        KeStallExecutionProcessor(10);
    }
    return FALSE;
}

static BOOLEAN PhyRd(PNIC_DEVICE N, ULONG Reg, USHORT *V)
{
    if (!MdioIdle(N)) return FALSE;
    P(N, GEM_MAN, (1UL << 30) | (2UL << 28) | (2UL << 16) |
                  ((ULONG)PHY_ADDR << 23) | (Reg << 18));
    if (!MdioIdle(N)) return FALSE;
    *V = (USHORT)(G(N, GEM_MAN) & 0xFFFF);
    return TRUE;
}

static BOOLEAN PhyWr(PNIC_DEVICE N, ULONG Reg, USHORT V)
{
    if (!MdioIdle(N)) return FALSE;
    P(N, GEM_MAN, (1UL << 30) | (1UL << 28) | (2UL << 16) |
                  ((ULONG)PHY_ADDR << 23) | (Reg << 18) | V);
    return MdioIdle(N);
}

/* Broadcom helpers (bcm-phy-lib.c): aux control 0x18, shadow 0x1c */
static BOOLEAN BcmAuxRead(PNIC_DEVICE N, USHORT Shadow, USHORT *V)
{
    if (!PhyWr(N, 0x18, (USHORT)(0x0007 | (Shadow << 12)))) return FALSE;
    return PhyRd(N, 0x18, V);
}
static BOOLEAN BcmShdRead(PNIC_DEVICE N, USHORT Shadow, USHORT *V)
{
    USHORT R = 0;
    if (!PhyWr(N, 0x1C, (USHORT)((Shadow & 0x1F) << 10))) return FALSE;
    if (!PhyRd(N, 0x1C, &R)) return FALSE;
    *V = R & 0x3FF;
    return TRUE;
}

/* PHY reset line: RP1 GPIO 32 (active low). Sequence proven by the probe. */
static VOID PhyResetPulse(PNIC_DEVICE N)
{
    ULONG Pad = Rd(N->Gpio, GPIO32_PAD_OFFSET);
    Wr(N->Gpio, GPIO32_PAD_OFFSET, (Pad & ~PAD_OD) | PAD_IE);   /* the fix */
    Wr(N->Gpio, GPIO_CTRL(GPIO_PHY_RESET_PIN),
       GPIO_FUNCSEL_NULL | GPIO_OUTOVER_LOW | GPIO_OEOVER_ENABLE);
    MsDelay(50);
    Wr(N->Gpio, GPIO_CTRL(GPIO_PHY_RESET_PIN),
       GPIO_FUNCSEL_NULL | GPIO_OUTOVER_HIGH | GPIO_OEOVER_ENABLE);
    MsDelay(300);
}

static NTSTATUS PhyBringUp(PNIC_DEVICE N)
{
    USHORT I1 = 0, I2 = 0, V = 0;

    if (N->Gpio == NULL || N->GpioLen <= GPIO32_PAD_OFFSET + 4) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    PhyResetPulse(N);

    if (!PhyRd(N, 2, &I1) || !PhyRd(N, 3, &I2)) return STATUS_IO_TIMEOUT;
    if (I1 != PHY_ID1_BCM || (I2 & PHY_ID2_MASK) != PHY_ID2_BCM) {
        return STATUS_NO_SUCH_DEVICE;
    }

    /* RGMII-ID: enable RX skew (aux misc 0x07, bit 8, write-enable bit 15)
     * and the internal TX clock delay (shadow 0x03, bit 9). */
    if (BcmAuxRead(N, 0x07, &V)) {
        V |= 0x8000 | 0x0100;
        (VOID)PhyWr(N, 0x18, (USHORT)(0x07 | V));
    }
    if (BcmShdRead(N, 0x03, &V)) {
        V |= (1 << 9);
        (VOID)PhyWr(N, 0x1C, (USHORT)(0x8000 | (0x03 << 10) | V));
    }

    /* Advertisement. Gigabit-only keeps the TX clock at 125 MHz. */
    (VOID)PhyWr(N, 4, N->OnlyGigabit ? 0x0001 : 0x01E1);
    (VOID)PhyWr(N, 9, 0x0200);
    (VOID)PhyWr(N, 0, 0x1200);          /* autoneg enable + restart */
    N->PhyUp = TRUE;
    return STATUS_SUCCESS;
}

/* Returns link state, speed (Mbit) and duplex from standard registers. */
static VOID PhyReadLink(PNIC_DEVICE N, BOOLEAN *Up, ULONG *Mbps, BOOLEAN *Full)
{
    USHORT S = 0, St1000 = 0, Lpa = 0;
    *Up = FALSE; *Mbps = 0; *Full = FALSE;
    if (!PhyRd(N, 1, &S) || !PhyRd(N, 1, &S)) return;
    if (!(S & 0x0004)) return;
    (VOID)PhyRd(N, 10, &St1000);
    (VOID)PhyRd(N, 5, &Lpa);
    *Up = TRUE;
    if (St1000 & (1 << 11))      { *Mbps = 1000; *Full = TRUE; }
    else if (St1000 & (1 << 10)) { *Mbps = 1000; *Full = FALSE; }
    else if (Lpa & (1 << 8))     { *Mbps = 100;  *Full = TRUE; }
    else if (Lpa & (1 << 7))     { *Mbps = 100;  *Full = FALSE; }
    else if (Lpa & (1 << 6))     { *Mbps = 10;   *Full = TRUE; }
    else                         { *Mbps = 10;   *Full = FALSE; }
}

static VOID SetMacSpeed(PNIC_DEVICE N, ULONG Mbps, BOOLEAN Full)
{
    ULONG C = G(N, GEM_NCFGR) & ~(NCFGR_SPD | NCFGR_GBE | NCFGR_FD);
    if (Mbps == 100)  C |= NCFGR_SPD;
    if (Mbps == 1000) C |= NCFGR_GBE;
    if (Full)         C |= NCFGR_FD;
    P(N, GEM_NCFGR, C);

    /* RGMII TX clock must follow the speed. ASSUMPTION: clk_eth runs from a
     * 125 MHz source and DIV is an integer divider. NOT verified. */
    if (N->Clk != NULL && N->ClkLen > CLK_ETH_DIV + 4) {
        Wr(N->Clk, CLK_ETH_DIV, Mbps == 1000 ? 1 : (Mbps == 100 ? 5 : 50));
    }
}

static VOID PublishLink(PNIC_DEVICE N, BOOLEAN Up, ULONG Mbps, BOOLEAN Full)
{
    NET_ADAPTER_LINK_STATE LS;
    ULONGLONG Bps = (ULONGLONG)Mbps * 1000000ULL;

    NET_ADAPTER_LINK_STATE_INIT(
        &LS,
        Up ? Bps : 0,
        Up ? MediaConnectStateConnected : MediaConnectStateDisconnected,
        Up ? (Full ? MediaDuplexStateFull : MediaDuplexStateHalf) : MediaDuplexStateUnknown,
        NetAdapterPauseFunctionTypeUnsupported,
        NetAdapterAutoNegotiationFlagNone);
    NetAdapterSetLinkState(N->Adapter, &LS);
}

static VOID EvtLinkTimer(_In_ WDFTIMER Timer)
{
    PNIC_DEVICE N = GetNic(WdfTimerGetParentObject(Timer));
    BOOLEAN Up, Full;
    ULONG Mbps;

    if (!N->PhyUp) return;
    PhyReadLink(N, &Up, &Mbps, &Full);
    if (Up != N->LinkUp || (Up && Mbps != N->LinkMbps)) {
        if (Up) SetMacSpeed(N, Mbps, Full);
        N->LinkUp = Up;
        N->LinkMbps = Mbps;
        PublishLink(N, Up, Mbps, Full);
    }
}

/* ------------------------------------------------------------------ */
/* Interrupt                                                          */
/* ------------------------------------------------------------------ */
static BOOLEAN EvtIsr(_In_ WDFINTERRUPT Interrupt, _In_ ULONG MessageID)
{
    PNIC_DEVICE N = GetNic(WdfInterruptGetDevice(Interrupt));
    ULONG S;
    UNREFERENCED_PARAMETER(MessageID);

    if (N->Gem == NULL) return FALSE;
    S = G(N, GEM_ISR);                         /* clear-on-read */
    if (!(S & INT_ALL_DATA)) return FALSE;
    P(N, GEM_IDR, INT_ALL_DATA);               /* mask until the queues re-arm */
    InterlockedOr((volatile LONG *)&N->PendingStatus, (LONG)S);
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

static VOID EvtDpc(_In_ WDFINTERRUPT Interrupt, _In_ WDFOBJECT Obj)
{
    PNIC_DEVICE N = GetNic(WdfInterruptGetDevice(Interrupt));
    ULONG S = (ULONG)InterlockedExchange((volatile LONG *)&N->PendingStatus, 0);
    UNREFERENCED_PARAMETER(Obj);

    if ((S & (INT_RCOMP | INT_RXUBR)) && N->RxQueue)
        NetRxQueueNotifyMoreReceivedPacketsAvailable(N->RxQueue);
    if ((S & INT_TCOMP) && N->TxQueue)
        NetTxQueueNotifyMoreCompletedPacketsAvailable(N->TxQueue);
}

/* ------------------------------------------------------------------ */
/* Address helpers                                                    */
/* ------------------------------------------------------------------ */
static VOID SetDescAddr(PNIC_DEVICE N, GEM_DESC *D, UINT64 La, ULONG Flags)
{
    UINT64 A = La + N->DmaOffset;
    D->AddrHi = (ULONG)(A >> 32);
    D->Ctrl = 0;
    KeMemoryBarrier();
    D->Addr = (ULONG)(A & 0xFFFFFFFCUL) | Flags;
}

/* ------------------------------------------------------------------ */
/* RX queue                                                           */
/* ------------------------------------------------------------------ */
static VOID EvtRxStart(_In_ NETPACKETQUEUE Q)
{
    PNIC_DEVICE N = GetQueueCtx(Q)->Nic;
    ULONG i;
    UINT64 A = N->RxRingLa + N->DmaOffset;

    for (i = 0; i < RING_SIZE; i++) {
        N->RxRing[i].Ctrl = 0;
        N->RxRing[i].AddrHi = 0;
        N->RxRing[i].Addr = RXD_USED | (i == RING_SIZE - 1 ? RXD_WRAP : 0);  /* sw owned */
    }
    N->RxPost = N->RxClean = 0;
    P(N, GEM_RBQP, (ULONG)A);
    P(N, GEM_RBQPH, (ULONG)(A >> 32));
    P(N, GEM_NCR, G(N, GEM_NCR) | NCR_RE);
}

static VOID EvtRxStop(_In_ NETPACKETQUEUE Q)
{
    PNIC_DEVICE N = GetQueueCtx(Q)->Nic;
    P(N, GEM_NCR, G(N, GEM_NCR) & ~NCR_RE);
}

static VOID EvtRxAdvance(_In_ NETPACKETQUEUE Q)
{
    QUEUE_CTX *C = GetQueueCtx(Q);
    PNIC_DEVICE N = C->Nic;
    NET_RING *PR = NetRingCollectionGetPacketRing(C->Rings);
    NET_RING *FR = NetRingCollectionGetFragmentRing(C->Rings);
    UINT32 pi = PR->BeginIndex;
    UINT32 fi = FR->BeginIndex;
    UINT32 fEnd = FR->NextIndex;          /* fragments already posted to hw */

    /* Indicate received frames. */
    while (fi != fEnd) {
        GEM_DESC *D = &N->RxRing[N->RxClean % RING_SIZE];
        NET_FRAGMENT *F;
        NET_PACKET *Pk;
        ULONG Ctrl;

        if (!(D->Addr & RXD_USED)) break;           /* hw not done */
        KeMemoryBarrier();
        Ctrl = D->Ctrl;

        F = NetRingGetFragmentAtIndex(FR, fi);
        Pk = NetRingGetPacketAtIndex(PR, pi);
        F->Offset = 0;
        F->ValidLength = RXD_LEN(Ctrl);
        Pk->FragmentIndex = fi;
        Pk->FragmentCount = 1;
        RtlZeroMemory(&Pk->Layout, sizeof(Pk->Layout));
        if ((Ctrl & (RXD_SOF | RXD_EOF)) != (RXD_SOF | RXD_EOF) || F->ValidLength == 0) {
            Pk->Ignore = 1;                          /* split or empty frame: drop */
        }

        N->RxClean++;
        fi = NetRingIncrementIndex(FR, fi);
        pi = NetRingIncrementIndex(PR, pi);
    }
    FR->BeginIndex = fi;
    PR->BeginIndex = pi;

    /* Post new buffers. */
    fi = FR->NextIndex;
    fEnd = FR->EndIndex;
    while (fi != fEnd && (N->RxPost - N->RxClean) < RING_SIZE) {
        GEM_DESC *D = &N->RxRing[N->RxPost % RING_SIZE];
        NET_FRAGMENT *F = NetRingGetFragmentAtIndex(FR, fi);
        NET_FRAGMENT_LOGICAL_ADDRESS *LA =
            NetExtensionGetFragmentLogicalAddress(&C->LogAddr, fi);

        SetDescAddr(N, D, LA->LogicalAddress + F->Offset,
                    ((N->RxPost % RING_SIZE) == RING_SIZE - 1) ? RXD_WRAP : 0);
        N->RxPost++;
        fi = NetRingIncrementIndex(FR, fi);
    }
    FR->NextIndex = fi;
    P(N, GEM_RSR, RSR_BNA);                          /* resume if it had stalled */
}

static VOID EvtRxCancel(_In_ NETPACKETQUEUE Q)
{
    QUEUE_CTX *C = GetQueueCtx(Q);
    PNIC_DEVICE N = C->Nic;
    NET_RING *PR = NetRingCollectionGetPacketRing(C->Rings);
    NET_RING *FR = NetRingCollectionGetFragmentRing(C->Rings);
    UINT32 i;

    P(N, GEM_NCR, G(N, GEM_NCR) & ~NCR_RE);
    for (i = PR->BeginIndex; i != PR->EndIndex; i = NetRingIncrementIndex(PR, i)) {
        NetRingGetPacketAtIndex(PR, i)->Ignore = 1;
    }
    PR->BeginIndex = PR->EndIndex;
    FR->BeginIndex = FR->EndIndex;
}

static VOID EvtRxSetNotify(_In_ NETPACKETQUEUE Q, _In_ BOOLEAN Enable)
{
    PNIC_DEVICE N = GetQueueCtx(Q)->Nic;
    P(N, Enable ? GEM_IER : GEM_IDR, INT_RCOMP | INT_RXUBR);
}

/* ------------------------------------------------------------------ */
/* TX queue (one fragment per packet: MaximumNumberOfFragments = 1)   */
/* ------------------------------------------------------------------ */
static VOID EvtTxStart(_In_ NETPACKETQUEUE Q)
{
    PNIC_DEVICE N = GetQueueCtx(Q)->Nic;
    ULONG i;
    UINT64 A = N->TxRingLa + N->DmaOffset;

    for (i = 0; i < RING_SIZE; i++) {
        N->TxRing[i].Addr = 0;
        N->TxRing[i].AddrHi = 0;
        N->TxRing[i].Ctrl = TXD_USED | (i == RING_SIZE - 1 ? TXD_WRAP : 0);
    }
    N->TxPost = N->TxClean = 0;
    P(N, GEM_TBQP, (ULONG)A);
    P(N, GEM_TBQPH, (ULONG)(A >> 32));
    P(N, GEM_NCR, G(N, GEM_NCR) | NCR_TE);
}

static VOID EvtTxStop(_In_ NETPACKETQUEUE Q)
{
    PNIC_DEVICE N = GetQueueCtx(Q)->Nic;
    P(N, GEM_NCR, G(N, GEM_NCR) & ~NCR_TE);
}

static VOID EvtTxAdvance(_In_ NETPACKETQUEUE Q)
{
    QUEUE_CTX *C = GetQueueCtx(Q);
    PNIC_DEVICE N = C->Nic;
    NET_RING *PR = NetRingCollectionGetPacketRing(C->Rings);
    NET_RING *FR = NetRingCollectionGetFragmentRing(C->Rings);
    UINT32 idx;
    BOOLEAN Posted = FALSE;

    /* Post. */
    idx = PR->NextIndex;
    while (idx != PR->EndIndex) {
        NET_PACKET *Pk = NetRingGetPacketAtIndex(PR, idx);
        if (!Pk->Ignore) {
            GEM_DESC *D = &N->TxRing[N->TxPost % RING_SIZE];
            NET_FRAGMENT *F;
            NET_FRAGMENT_LOGICAL_ADDRESS *LA;
            UINT64 A;

            if (!(D->Ctrl & TXD_USED)) break;           /* ring full */
            F = NetRingGetFragmentAtIndex(FR, Pk->FragmentIndex);
            LA = NetExtensionGetFragmentLogicalAddress(&C->LogAddr, Pk->FragmentIndex);
            A = LA->LogicalAddress + F->Offset + N->DmaOffset;

            D->Addr = (ULONG)A;
            D->AddrHi = (ULONG)(A >> 32);
            KeMemoryBarrier();
            D->Ctrl = TXD_LEN(F->ValidLength) | TXD_LAST |
                      (((N->TxPost % RING_SIZE) == RING_SIZE - 1) ? TXD_WRAP : 0);
            N->TxPost++;
            FR->NextIndex = NetRingIncrementIndex(FR, Pk->FragmentIndex);
            Posted = TRUE;
        }
        idx = NetRingIncrementIndex(PR, idx);
    }
    PR->NextIndex = idx;
    if (Posted) {
        KeMemoryBarrier();
        P(N, GEM_NCR, G(N, GEM_NCR) | NCR_TSTART);
    }

    /* Drain completed. */
    idx = PR->BeginIndex;
    while (idx != PR->NextIndex) {
        NET_PACKET *Pk = NetRingGetPacketAtIndex(PR, idx);
        UINT32 k, f;

        if (!Pk->Ignore) {
            if (N->TxClean == N->TxPost) break;
            if (!(N->TxRing[N->TxClean % RING_SIZE].Ctrl & TXD_USED)) break;
            N->TxClean++;
        }
        f = Pk->FragmentIndex;
        for (k = 0; k < Pk->FragmentCount; k++) f = NetRingIncrementIndex(FR, f);
        FR->BeginIndex = f;
        idx = NetRingIncrementIndex(PR, idx);
    }
    PR->BeginIndex = idx;
}

static VOID EvtTxCancel(_In_ NETPACKETQUEUE Q)
{
    QUEUE_CTX *C = GetQueueCtx(Q);
    NET_RING *PR = NetRingCollectionGetPacketRing(C->Rings);
    UINT32 i;
    for (i = PR->BeginIndex; i != PR->EndIndex; i = NetRingIncrementIndex(PR, i)) {
        NetRingGetPacketAtIndex(PR, i)->Scratch = 1;
    }
    PR->BeginIndex = PR->EndIndex;
}

static VOID EvtTxSetNotify(_In_ NETPACKETQUEUE Q, _In_ BOOLEAN Enable)
{
    PNIC_DEVICE N = GetQueueCtx(Q)->Nic;
    P(N, Enable ? GEM_IER : GEM_IDR, INT_TCOMP);
}

/* ------------------------------------------------------------------ */
/* Queue creation                                                     */
/* ------------------------------------------------------------------ */
static NTSTATUS EvtCreateRxQueue(_In_ NETADAPTER Adapter, _Inout_ NETRXQUEUE_INIT *Init)
{
    WDF_OBJECT_ATTRIBUTES A;
    NET_PACKET_QUEUE_CONFIG Cfg;
    NETPACKETQUEUE Q;
    NET_EXTENSION_QUERY Ext;
    QUEUE_CTX *C;
    NTSTATUS S;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A, QUEUE_CTX);
    NET_PACKET_QUEUE_CONFIG_INIT(&Cfg, EvtRxAdvance, EvtRxSetNotify, EvtRxCancel);
    Cfg.EvtStart = EvtRxStart;
    Cfg.EvtStop = EvtRxStop;
    S = NetRxQueueCreate(Init, &A, &Cfg, &Q);
    if (!NT_SUCCESS(S)) return S;

    C = GetQueueCtx(Q);
    C->Nic = GetAdapterCtx(Adapter)->Nic;
    C->Rings = NetRxQueueGetRingCollection(Q);
    NET_EXTENSION_QUERY_INIT(&Ext, NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_VERSION_1,
                             NetExtensionTypeFragment);
    NetRxQueueGetExtension(Q, &Ext, &C->LogAddr);
    C->Nic->RxQueue = Q;
    return STATUS_SUCCESS;
}

static NTSTATUS EvtCreateTxQueue(_In_ NETADAPTER Adapter, _Inout_ NETTXQUEUE_INIT *Init)
{
    WDF_OBJECT_ATTRIBUTES A;
    NET_PACKET_QUEUE_CONFIG Cfg;
    NETPACKETQUEUE Q;
    NET_EXTENSION_QUERY Ext;
    QUEUE_CTX *C;
    NTSTATUS S;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A, QUEUE_CTX);
    NET_PACKET_QUEUE_CONFIG_INIT(&Cfg, EvtTxAdvance, EvtTxSetNotify, EvtTxCancel);
    Cfg.EvtStart = EvtTxStart;
    Cfg.EvtStop = EvtTxStop;
    S = NetTxQueueCreate(Init, &A, &Cfg, &Q);
    if (!NT_SUCCESS(S)) return S;

    C = GetQueueCtx(Q);
    C->Nic = GetAdapterCtx(Adapter)->Nic;
    C->Rings = NetTxQueueGetRingCollection(Q);
    NET_EXTENSION_QUERY_INIT(&Ext, NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_VERSION_1,
                             NetExtensionTypeFragment);
    NetTxQueueGetExtension(Q, &Ext, &C->LogAddr);
    C->Nic->TxQueue = Q;
    return STATUS_SUCCESS;
}

/* Receive filter: the GEM is run with copy-all-frames for now. */
static VOID EvtSetReceiveFilter(_In_ NETADAPTER Adapter, _In_ NETRECEIVEFILTER Filter)
{
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Filter);
}

/* ------------------------------------------------------------------ */
/* Device bring-up                                                    */
/* ------------------------------------------------------------------ */
static VOID LoadSettings(PNIC_DEVICE N)
{
    WDFKEY Key;
    DECLARE_CONST_UNICODE_STRING(NDma,  L"DmaAddrOffset");
    DECLARE_CONST_UNICODE_STRING(NOnly, L"OnlyGigabit");
    DECLARE_CONST_UNICODE_STRING(NMac,  L"MacAddress");
    ULONG V = 0, Len = 0, Type = 0;
    UCHAR Mac[6];
    ULONG R;

    N->DmaOffset = 0;
    N->OnlyGigabit = 1;

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(N->Device, PLUGPLAY_REGKEY_DEVICE,
                                             KEY_ALL_ACCESS, WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
        Key = NULL;
    }
    if (Key) {
        ULONG Hi = 0;
        DECLARE_CONST_UNICODE_STRING(NDmaHi, L"DmaAddrOffsetHigh");
        if (NT_SUCCESS(WdfRegistryQueryULong(Key, &NDmaHi, &Hi))) {
            N->DmaOffset = (UINT64)Hi << 32;     /* e.g. 0x10 -> 0x10_0000_0000 */
        }
        if (NT_SUCCESS(WdfRegistryQueryULong(Key, &NOnly, &V))) N->OnlyGigabit = V;
    }
    UNREFERENCED_PARAMETER(NDma);

    /* MAC address: hardware if programmed, else saved, else generated. */
    R = G(N, GEM_SA1B);
    Mac[0] = (UCHAR)R; Mac[1] = (UCHAR)(R >> 8); Mac[2] = (UCHAR)(R >> 16); Mac[3] = (UCHAR)(R >> 24);
    R = G(N, GEM_SA1T);
    Mac[4] = (UCHAR)R; Mac[5] = (UCHAR)(R >> 8);
    if ((Mac[0] | Mac[1] | Mac[2] | Mac[3] | Mac[4] | Mac[5]) != 0 && !(Mac[0] & 1)) {
        RtlCopyMemory(N->Mac, Mac, 6);
    } else {
        BOOLEAN Have = FALSE;
        if (Key && NT_SUCCESS(WdfRegistryQueryValue(Key, &NMac, 6, Mac, &Len, &Type)) && Len == 6) {
            RtlCopyMemory(N->Mac, Mac, 6);
            Have = TRUE;
        }
        if (!Have) {
            LARGE_INTEGER T = KeQueryPerformanceCounter(NULL);
            N->Mac[0] = 0x02; N->Mac[1] = 0x50; N->Mac[2] = 0x49;
            N->Mac[3] = (UCHAR)(T.LowPart >> 16);
            N->Mac[4] = (UCHAR)(T.LowPart >> 8);
            N->Mac[5] = (UCHAR)(T.LowPart);
            if (Key) {
                (VOID)WdfRegistryAssignValue(Key, &NMac, REG_BINARY, 6, N->Mac);
            }
        }
    }
    if (Key) WdfRegistryClose(Key);
}

static NTSTATUS CreateRings(PNIC_DEVICE N)
{
    PHYSICAL_ADDRESS Pa;
    NTSTATUS S;
    SIZE_T Sz = RING_SIZE * sizeof(GEM_DESC);

    S = WdfCommonBufferCreate(N->Dma, Sz, WDF_NO_OBJECT_ATTRIBUTES, &N->RxRingBuf);
    if (!NT_SUCCESS(S)) return S;
    S = WdfCommonBufferCreate(N->Dma, Sz, WDF_NO_OBJECT_ATTRIBUTES, &N->TxRingBuf);
    if (!NT_SUCCESS(S)) return S;

    N->RxRing = (GEM_DESC *)WdfCommonBufferGetAlignedVirtualAddress(N->RxRingBuf);
    Pa = WdfCommonBufferGetAlignedLogicalAddress(N->RxRingBuf);
    N->RxRingLa = (UINT64)Pa.QuadPart;
    N->TxRing = (GEM_DESC *)WdfCommonBufferGetAlignedVirtualAddress(N->TxRingBuf);
    Pa = WdfCommonBufferGetAlignedLogicalAddress(N->TxRingBuf);
    N->TxRingLa = (UINT64)Pa.QuadPart;
    RtlZeroMemory(N->RxRing, Sz);
    RtlZeroMemory(N->TxRing, Sz);
    return STATUS_SUCCESS;
}

static NTSTATUS SetCapabilities(PNIC_DEVICE N)
{
    NET_ADAPTER_DMA_CAPABILITIES TxDma, RxDma;
    NET_ADAPTER_TX_CAPABILITIES Tx;
    NET_ADAPTER_RX_CAPABILITIES Rx;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES LL;
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES RF;
    NET_ADAPTER_LINK_LAYER_ADDRESS Addr;

    NET_ADAPTER_DMA_CAPABILITIES_INIT(&TxDma, N->Dma);
    NET_ADAPTER_TX_CAPABILITIES_INIT_FOR_DMA(&Tx, &TxDma, 1);
    Tx.MaximumNumberOfFragments = 1;
    Tx.FragmentRingNumberOfElementsHint = RING_SIZE;

    NET_ADAPTER_DMA_CAPABILITIES_INIT(&RxDma, N->Dma);
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED_DMA(&Rx, &RxDma, RX_BUF_SIZE, 1);
    Rx.FragmentBufferAlignment = 64;
    Rx.FragmentRingNumberOfElementsHint = RING_SIZE;

    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&LL, 1000000000ULL, 1000000000ULL);
    NetAdapterSetLinkLayerCapabilities(N->Adapter, &LL);
    NetAdapterSetLinkLayerMtuSize(N->Adapter, 1500);
    NetAdapterSetDataPathCapabilities(N->Adapter, &Tx, &Rx);

    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT(&RF, EvtSetReceiveFilter);
    RF.SupportedPacketFilters = NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST |
                                NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST |
                                NDIS_PACKET_TYPE_PROMISCUOUS;
    RF.MaximumMulticastAddresses = 32;
    NetAdapterSetReceiveFilterCapabilities(N->Adapter, &RF);

    NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&Addr, 6, N->Mac);
    NetAdapterSetPermanentLinkLayerAddress(N->Adapter, &Addr);
    NetAdapterSetCurrentLinkLayerAddress(N->Adapter, &Addr);
    return STATUS_SUCCESS;
}

static NTSTATUS EvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Tr)
{
    PNIC_DEVICE N = GetNic(Device);
    ULONG Count = WdfCmResourceListGetCount(Tr), i, Idx = 0;
    WDF_DMA_ENABLER_CONFIG DC;
    NTSTATUS S;
    UNREFERENCED_PARAMETER(Raw);

    for (i = 0; i < Count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR D = WdfCmResourceListGetDescriptor(Tr, i);
        PVOID Va;
        if (D->Type != CmResourceTypeMemory) continue;
        Va = MmMapIoSpaceEx(D->u.Memory.Start, D->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE);
        if (!Va) return STATUS_INSUFFICIENT_RESOURCES;
        switch (Idx) {
        case 0: N->Gem  = Va; N->GemLen  = D->u.Memory.Length; break;
        case 1: N->Cfg  = Va; N->CfgLen  = D->u.Memory.Length; break;
        case 2: N->Gpio = Va; N->GpioLen = D->u.Memory.Length; break;
        case 3: N->Pads = Va; N->PadsLen = D->u.Memory.Length; break;
        case 4: N->Clk  = Va; N->ClkLen  = D->u.Memory.Length; break;
        default: MmUnmapIoSpace(Va, D->u.Memory.Length); break;
        }
        Idx++;
    }
    if (!N->Gem || !N->Gpio) return STATUS_DEVICE_CONFIGURATION_ERROR;

    LoadSettings(N);

    WdfDeviceSetAlignmentRequirement(Device, FILE_64_BYTE_ALIGNMENT);
    WDF_DMA_ENABLER_CONFIG_INIT(&DC, WdfDmaProfileScatterGather64, 0x10000);
    DC.WdmDmaVersionOverride = 3;
    S = WdfDmaEnablerCreate(Device, &DC, WDF_NO_OBJECT_ATTRIBUTES, &N->Dma);
    if (!NT_SUCCESS(S)) return S;
    S = CreateRings(N);
    if (!NT_SUCCESS(S)) return S;

    SetCapabilities(N);
    return NetAdapterStart(N->Adapter);
}

static NTSTATUS EvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Tr)
{
    PNIC_DEVICE N = GetNic(Device);
    UNREFERENCED_PARAMETER(Tr);
    if (N->Gem)  { MmUnmapIoSpace(N->Gem,  N->GemLen);  N->Gem = NULL; }
    if (N->Cfg)  { MmUnmapIoSpace(N->Cfg,  N->CfgLen);  N->Cfg = NULL; }
    if (N->Gpio) { MmUnmapIoSpace(N->Gpio, N->GpioLen); N->Gpio = NULL; }
    if (N->Pads) { MmUnmapIoSpace(N->Pads, N->PadsLen); N->Pads = NULL; }
    if (N->Clk)  { MmUnmapIoSpace(N->Clk,  N->ClkLen);  N->Clk = NULL; }
    return STATUS_SUCCESS;
}

static NTSTATUS EvtD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE Prev)
{
    PNIC_DEVICE N = GetNic(Device);
    ULONG Mac4;
    UNREFERENCED_PARAMETER(Prev);

    P(N, GEM_IDR, 0xFFFFFFFF);
    P(N, GEM_NCR, 0);
    P(N, GEM_NCR, NCR_CLRSTAT);
    P(N, GEM_USRIO, USRIO_RGMII);
    P(N, GEM_NCFGR, NCFGR_CLK_DIV224 | NCFGR_DRFCS | NCFGR_BIG | NCFGR_CAF);
    P(N, GEM_DMACFG, DMACFG_FBLDO16 | DMACFG_RXBMS | DMACFG_TXPBMS |
                     DMACFG_RXBS(RX_BUF_SIZE / 64) | DMACFG_ADDR64);
    Mac4 = N->Mac[0] | (N->Mac[1] << 8) | (N->Mac[2] << 16) | ((ULONG)N->Mac[3] << 24);
    P(N, GEM_SA1B, Mac4);
    P(N, GEM_SA1T, N->Mac[4] | (N->Mac[5] << 8));
    P(N, GEM_NCR, NCR_MPE);

    if (!NT_SUCCESS(PhyBringUp(N))) {
        N->PhyUp = FALSE;                  /* adapter stays media-disconnected */
    }
    N->LinkUp = FALSE;
    PublishLink(N, FALSE, 0, FALSE);
    WdfTimerStart(N->LinkTimer, WDF_REL_TIMEOUT_IN_SEC(1));
    return STATUS_SUCCESS;
}

static NTSTATUS EvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE Target)
{
    PNIC_DEVICE N = GetNic(Device);
    UNREFERENCED_PARAMETER(Target);
    WdfTimerStop(N->LinkTimer, TRUE);
    if (N->Gem) {
        P(N, GEM_IDR, 0xFFFFFFFF);
        P(N, GEM_NCR, G(N, GEM_NCR) & ~(NCR_RE | NCR_TE));
    }
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Driver / device creation                                           */
/* ------------------------------------------------------------------ */
static NTSTATUS EvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS S;
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES A;
    WDFDEVICE Device;
    PNIC_DEVICE N;
    NETADAPTER_INIT *AI;
    NET_ADAPTER_DATAPATH_CALLBACKS DP;
    NETADAPTER Adapter;
    WDF_INTERRUPT_CONFIG IC;
    WDF_TIMER_CONFIG TC;
    WDF_OBJECT_ATTRIBUTES TA;
    UNREFERENCED_PARAMETER(Driver);

    S = NetDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(S)) return S;

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    Pnp.EvtDeviceD0Entry = EvtD0Entry;
    Pnp.EvtDeviceD0Exit = EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A, NIC_DEVICE);
    S = WdfDeviceCreate(&DeviceInit, &A, &Device);
    if (!NT_SUCCESS(S)) return S;
    N = GetNic(Device);
    N->Device = Device;

    WDF_INTERRUPT_CONFIG_INIT(&IC, EvtIsr, EvtDpc);
    IC.AutomaticSerialization = FALSE;
    S = WdfInterruptCreate(Device, &IC, WDF_NO_OBJECT_ATTRIBUTES, &N->Interrupt);
    if (!NT_SUCCESS(S)) return S;

    WDF_TIMER_CONFIG_INIT_PERIODIC(&TC, EvtLinkTimer, 1000);
    TC.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&TA);
    TA.ParentObject = Device;
    TA.ExecutionLevel = WdfExecutionLevelPassive;
    S = WdfTimerCreate(&TC, &TA, &N->LinkTimer);
    if (!NT_SUCCESS(S)) return S;

    AI = NetAdapterInitAllocate(Device);
    if (!AI) return STATUS_INSUFFICIENT_RESOURCES;
    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&DP, EvtCreateTxQueue, EvtCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(AI, &DP);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A, ADAPTER_CTX);
    S = NetAdapterCreate(AI, &A, &Adapter);
    if (!NT_SUCCESS(S)) { NetAdapterInitFree(AI); return S; }
    GetAdapterCtx(Adapter)->Nic = N;
    N->Adapter = Adapter;
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG C;
    WDF_DRIVER_CONFIG_INIT(&C, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &C, WDF_NO_HANDLE);
}
