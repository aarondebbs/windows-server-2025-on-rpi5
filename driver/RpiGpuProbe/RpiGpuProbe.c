/*
 * RpiGpuProbe - EXPERIMENTAL KMDF probe driver for the Raspberry Pi 5
 * VideoCore side (firmware mailbox, PM block, V3D GPU), matched as
 * ACPI\RPIG0001 (see firmware/gpu-acpi.patch).
 *
 * STATUS: never compiled, never run. Written from the Raspberry Pi Linux
 * tree (bcm2835-mailbox, raspberrypi-firmware, bcm2835-power, v3d) and the
 * Pi 5 UEFI RpiFirmwareDxe driver.
 *
 * Purpose: find out, with data from real hardware, what Windows can reach
 * before any display or 3D driver is written. Results go into the device's
 * registry key (Device Parameters), like RpiEthProbe.
 *
 * Probe levels (registry value ProbeLevel, DWORD, default 0):
 *   0  Read-only. Talks to the VideoCore firmware over the mailbox with GET
 *      requests only: firmware revision, board revision, current framebuffer
 *      geometry, EDID block 0, state/rate of the display and V3D clocks.
 *      Also reads the PM block register that holds the V3D reset bit.
 *   1  Level 0, then powers up V3D the way Linux does on BCM2712: enable
 *      and set the V3D clock through the firmware, then release V3DRSTN in
 *      PM_GRAFX_2712.
 *   2  Level 1, then reads the V3D hub/core identification registers.
 *      RISK: if the V3D bus bridge is not up, this read can hang the CPU or
 *      bugcheck. The registry value "Stage" is written BEFORE each risky
 *      step so the last value tells you where it stopped.
 *
 * Other registry values: BusOffset (default 0xC0000000, VideoCore bus alias
 * added to the mailbox buffer address, from the UEFI DMA setup), V3dRate
 * (Hz, 0 = firmware maximum).
 *
 * Build: Visual Studio + WDK, platform ARM64, KMDF driver (needs ntstrsafe).
 */

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>

#define GPUP_TAG 'PUGR'

/* Resource order from the firmware ACPI device (VCR0). */
#define RES_MAILBOX   0
#define RES_PM        1
#define RES_V3D_HUB   2
#define RES_V3D_CORE  3
#define RES_V3D_SMS   4

/* bcm2835 mailbox (Linux bcm2835-mailbox.c) */
#define MBX_RD        0x00        /* mailbox 0 read (VC -> ARM) */
#define MBX_RX_STA    0x18        /* mailbox 0 status */
#define MBX_WRT       0x20        /* mailbox 1 write (ARM -> VC) */
#define MBX_TX_STA    0x38        /* mailbox 1 status */
#define MBX_FULL      0x80000000UL
#define MBX_EMPTY     0x40000000UL
#define MBX_CH_PROP   8           /* property tags, ARM -> VC */

/* Firmware property tags (Linux raspberrypi-firmware.h) */
#define TAG_GET_REVISION        0x00000001UL
#define TAG_GET_BOARD_REVISION  0x00010002UL
#define TAG_GET_CLOCK_STATE     0x00030001UL
#define TAG_GET_CLOCK_RATE      0x00030002UL
#define TAG_GET_MAX_CLOCK_RATE  0x00030004UL
#define TAG_GET_MIN_CLOCK_RATE  0x00030007UL
#define TAG_GET_EDID_BLOCK      0x00030020UL
#define TAG_SET_CLOCK_STATE     0x00038001UL
#define TAG_SET_CLOCK_RATE      0x00038002UL
#define TAG_FB_GET_PHYS_WH      0x00040003UL
#define TAG_FB_GET_DEPTH        0x00040005UL
#define TAG_FB_GET_PITCH        0x00040008UL

/* Firmware clock IDs (Linux enum raspberrypi_firmware_clk_id) */
#define CLK_ARM    3
#define CLK_CORE   4
#define CLK_V3D    5
#define CLK_PIXEL  9
#define CLK_M2MC   13
#define CLK_BVB    14
#define CLK_DISP   16

/* PM block (Linux bcm2835-power.c): password in the top byte on writes. */
#define PM_GRAFX_2712   0x304
#define PM_PASSWORD     0x5A000000UL
#define PM_V3DRSTN      (1UL << 6)

/* V3D registers (Linux v3d_regs.h) */
#define V3D_HUB_AXICFG  0x00
#define V3D_HUB_IDENT0  0x08
#define V3D_HUB_IDENT1  0x0C
#define V3D_HUB_IDENT2  0x10
#define V3D_HUB_IDENT3  0x14
#define V3D_CTL_IDENT0  0x00
#define V3D_CTL_IDENT1  0x04
#define V3D_CTL_IDENT2  0x08

typedef struct _DEVICE_CONTEXT {
    PUCHAR  Mbx;   SIZE_T MbxLen;
    PUCHAR  Pm;    SIZE_T PmLen;
    PUCHAR  Hub;   SIZE_T HubLen;
    PUCHAR  Core;  SIZE_T CoreLen;
    PUCHAR  Sms;   SIZE_T SmsLen;
    PULONG  Msg;                 /* mailbox message buffer (uncached, < 1 GB) */
    PHYSICAL_ADDRESS MsgPa;
    ULONG   MsgBus;              /* address the VideoCore uses for it */
    ULONG   BusOffset;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD        GpuEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE  GpuEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE  GpuEvtReleaseHardware;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */
static ULONG Rd(PUCHAR Base, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Base + Off));
}

static VOID Wr(PUCHAR Base, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off), Val);
}

static VOID MsDelay(ULONG Ms)
{
    LARGE_INTEGER T;
    T.QuadPart = -((LONGLONG)Ms * 10000);
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &T);
}

static VOID SaveU(WDFKEY Key, PCWSTR Name, ULONG V)
{
    UNICODE_STRING U;
    if (Key == NULL) return;
    RtlInitUnicodeString(&U, Name);
    (VOID)WdfRegistryAssignULong(Key, &U, V);
}

static VOID SaveB(WDFKEY Key, PCWSTR Name, PVOID P, ULONG Len)
{
    UNICODE_STRING U;
    if (Key == NULL) return;
    RtlInitUnicodeString(&U, Name);
    (VOID)WdfRegistryAssignValue(Key, &U, REG_BINARY, Len, P);
}

/* Name + number, e.g. SaveIdx(Key, L"ClkRate", 5, v) -> "ClkRate_5" */
static VOID SaveIdx(WDFKEY Key, PCWSTR Base, ULONG Idx, ULONG V)
{
    WCHAR Name[40];
    if (Key == NULL) return;
    if (NT_SUCCESS(RtlStringCchPrintfW(Name, 40, L"%ws_%u", Base, Idx))) {
        SaveU(Key, Name, V);
    }
}

static ULONG LoadU(WDFKEY Key, PCWSTR Name, ULONG Default)
{
    UNICODE_STRING U;
    ULONG V = 0;
    if (Key == NULL) return Default;
    RtlInitUnicodeString(&U, Name);
    return NT_SUCCESS(WdfRegistryQueryULong(Key, &U, &V)) ? V : Default;
}

/* ------------------------------------------------------------------ */
/* Firmware mailbox: property interface, channel 8                     */
/* ------------------------------------------------------------------ */
static BOOLEAN MbxWait(PDEVICE_CONTEXT C, ULONG StatusOff, ULONG Mask)
{
    ULONG i;
    /* spin ~2 ms first, then sleep-poll up to ~2 s */
    for (i = 0; i < 200; i++) {
        if ((Rd(C->Mbx, StatusOff) & Mask) == 0) return TRUE;
        KeStallExecutionProcessor(10);
    }
    for (i = 0; i < 2000; i++) {
        if ((Rd(C->Mbx, StatusOff) & Mask) == 0) return TRUE;
        MsDelay(1);
    }
    return FALSE;
}

/*
 * One property request with a single tag.
 *   In/InWords:   request value words (may be NULL / 0)
 *   Out/OutWords: response value words copied back (may be NULL / 0)
 *   ValWords:     size of the tag's value buffer, in words (>= both)
 * Returns STATUS_SUCCESS when the firmware answered with success and the
 * tag's response bit is set.
 */
static NTSTATUS FwCall(PDEVICE_CONTEXT C, ULONG Tag, ULONG ValWords,
                       const ULONG *In, ULONG InWords, ULONG *Out, ULONG OutWords)
{
    ULONG i, TotalWords, Ch;
    PULONG M = C->Msg;

    if (C->Msg == NULL || C->Mbx == NULL) return STATUS_DEVICE_NOT_READY;
    if (ValWords > 100) return STATUS_INVALID_PARAMETER;

    TotalWords = 5 + ValWords + 1;               /* header(2) + tag(3) + values + end */
    RtlZeroMemory(M, TotalWords * sizeof(ULONG) + 16);
    M[0] = TotalWords * sizeof(ULONG);           /* buffer size in bytes */
    M[1] = 0;                                    /* request */
    M[2] = Tag;
    M[3] = ValWords * sizeof(ULONG);             /* value buffer size */
    M[4] = 0;                                    /* request code */
    for (i = 0; i < InWords; i++) M[5 + i] = In[i];
    M[5 + ValWords] = 0;                         /* end tag */
    KeMemoryBarrier();

    /* drain stale replies */
    for (i = 0; i < 16 && (Rd(C->Mbx, MBX_RX_STA) & MBX_EMPTY) == 0; i++) {
        (VOID)Rd(C->Mbx, MBX_RD);
    }

    if (!MbxWait(C, MBX_TX_STA, MBX_FULL)) return STATUS_IO_TIMEOUT;
    Wr(C->Mbx, MBX_WRT, C->MsgBus | MBX_CH_PROP);
    KeMemoryBarrier();

    for (;;) {
        if (!MbxWait(C, MBX_RX_STA, MBX_EMPTY)) return STATUS_IO_TIMEOUT;
        Ch = Rd(C->Mbx, MBX_RD);
        if ((Ch & 0xF) == MBX_CH_PROP) break;    /* ignore replies for other channels */
    }
    KeMemoryBarrier();

    if (M[1] != 0x80000000UL) return STATUS_UNSUCCESSFUL;
    if ((M[4] & 0x80000000UL) == 0) return STATUS_NOT_SUPPORTED;   /* tag not answered */
    for (i = 0; i < OutWords; i++) Out[i] = M[5 + i];
    return STATUS_SUCCESS;
}

/* Convenience: request {id} -> response {id, value}. */
static NTSTATUS FwClockGet(PDEVICE_CONTEXT C, ULONG Tag, ULONG Id, PULONG Value)
{
    ULONG In[2], Out[2];
    NTSTATUS S;
    In[0] = Id; In[1] = 0;
    S = FwCall(C, Tag, 2, In, 2, Out, 2);
    if (NT_SUCCESS(S)) *Value = Out[1];
    return S;
}

/* ------------------------------------------------------------------ */
/* Probe steps                                                         */
/* ------------------------------------------------------------------ */
static VOID ProbeFirmware(PDEVICE_CONTEXT C, WDFKEY Key)
{
    ULONG W[2], V = 0, Id;
    NTSTATUS S;
    static const ULONG Clocks[] = { CLK_ARM, CLK_CORE, CLK_V3D, CLK_PIXEL, CLK_M2MC, CLK_BVB, CLK_DISP };
    UCHAR Edid[128];
    ULONG k;

    S = FwCall(C, TAG_GET_REVISION, 1, NULL, 0, &V, 1);
    SaveU(Key, L"FwRevStatus", (ULONG)S); if (NT_SUCCESS(S)) SaveU(Key, L"FwRevision", V);

    S = FwCall(C, TAG_GET_BOARD_REVISION, 1, NULL, 0, &V, 1);
    SaveU(Key, L"BoardRevStatus", (ULONG)S); if (NT_SUCCESS(S)) SaveU(Key, L"BoardRevision", V);

    /* current framebuffer, as set up by UEFI (GET only, nothing is allocated) */
    S = FwCall(C, TAG_FB_GET_PHYS_WH, 2, NULL, 0, W, 2);
    SaveU(Key, L"FbGeomStatus", (ULONG)S);
    if (NT_SUCCESS(S)) { SaveU(Key, L"FbWidth", W[0]); SaveU(Key, L"FbHeight", W[1]); }
    S = FwCall(C, TAG_FB_GET_DEPTH, 1, NULL, 0, &V, 1);
    if (NT_SUCCESS(S)) SaveU(Key, L"FbDepth", V);
    S = FwCall(C, TAG_FB_GET_PITCH, 1, NULL, 0, &V, 1);
    if (NT_SUCCESS(S)) SaveU(Key, L"FbPitch", V);

    /* EDID block 0 of the default display: request {block}, response {block, status, 128 bytes} */
    {
        ULONG In[1] = { 0 };
        ULONG Out[34];
        RtlZeroMemory(Out, sizeof(Out));
        S = FwCall(C, TAG_GET_EDID_BLOCK, 34, In, 1, Out, 34);
        SaveU(Key, L"EdidCallStatus", (ULONG)S);
        if (NT_SUCCESS(S)) {
            SaveU(Key, L"EdidFwStatus", Out[1]);
            RtlCopyMemory(Edid, &Out[2], 128);
            SaveB(Key, L"Edid0", Edid, 128);
        }
    }

    for (k = 0; k < RTL_NUMBER_OF(Clocks); k++) {
        Id = Clocks[k];
        S = FwClockGet(C, TAG_GET_CLOCK_STATE, Id, &V); if (NT_SUCCESS(S)) SaveIdx(Key, L"ClkState", Id, V);
        S = FwClockGet(C, TAG_GET_CLOCK_RATE, Id, &V);  if (NT_SUCCESS(S)) SaveIdx(Key, L"ClkRate", Id, V);
        S = FwClockGet(C, TAG_GET_MAX_CLOCK_RATE, Id, &V); if (NT_SUCCESS(S)) SaveIdx(Key, L"ClkMax", Id, V);
        S = FwClockGet(C, TAG_GET_MIN_CLOCK_RATE, Id, &V); if (NT_SUCCESS(S)) SaveIdx(Key, L"ClkMin", Id, V);
    }
}

/* Linux bcm2835_asb_power_on() on BCM2712: clock on, then release V3DRSTN in PM_GRAFX_2712. */
static VOID PowerV3d(PDEVICE_CONTEXT C, WDFKEY Key, ULONG RateOverride)
{
    ULONG In[3], Out[3], Max = 0, Rate, V;
    NTSTATUS S;

    (VOID)FwClockGet(C, TAG_GET_MAX_CLOCK_RATE, CLK_V3D, &Max);
    Rate = RateOverride ? RateOverride : Max;

    In[0] = CLK_V3D; In[1] = 1;                          /* state: on */
    S = FwCall(C, TAG_SET_CLOCK_STATE, 2, In, 2, Out, 2);
    SaveU(Key, L"V3dClkOnStatus", (ULONG)S);

    if (Rate != 0) {
        In[0] = CLK_V3D; In[1] = Rate; In[2] = 0;         /* skip_setting_turbo = 0 */
        S = FwCall(C, TAG_SET_CLOCK_RATE, 3, In, 3, Out, 3);
        SaveU(Key, L"V3dClkRateStatus", (ULONG)S);
        if (NT_SUCCESS(S)) SaveU(Key, L"V3dClkRateSet", Out[1]);
    }
    S = FwClockGet(C, TAG_GET_CLOCK_RATE, CLK_V3D, &V);
    if (NT_SUCCESS(S)) SaveU(Key, L"V3dClkRateAfter", V);

    SaveU(Key, L"Stage", 5);
    MsDelay(1);
    V = Rd(C->Pm, PM_GRAFX_2712);
    SaveU(Key, L"PmGrafxBeforeReset", V);
    Wr(C->Pm, PM_GRAFX_2712, PM_PASSWORD | V | PM_V3DRSTN);
    MsDelay(1);
    SaveU(Key, L"PmGrafxAfterReset", Rd(C->Pm, PM_GRAFX_2712));
    SaveU(Key, L"Stage", 6);
}

static VOID ReadV3d(PDEVICE_CONTEXT C, WDFKEY Key)
{
    ULONG V;
    SaveU(Key, L"Stage", 7);                              /* about to touch V3D registers */
    V = Rd(C->Hub, V3D_HUB_IDENT0);
    SaveU(Key, L"V3dHubIdent0", V);
    SaveU(Key, L"Stage", 8);
    SaveU(Key, L"V3dHubIdent1", Rd(C->Hub, V3D_HUB_IDENT1));
    SaveU(Key, L"V3dHubIdent2", Rd(C->Hub, V3D_HUB_IDENT2));
    SaveU(Key, L"V3dHubIdent3", Rd(C->Hub, V3D_HUB_IDENT3));
    SaveU(Key, L"V3dHubAxiCfg",  Rd(C->Hub, V3D_HUB_AXICFG));
    SaveU(Key, L"Stage", 9);
    SaveU(Key, L"V3dCoreIdent0", Rd(C->Core, V3D_CTL_IDENT0));
    SaveU(Key, L"V3dCoreIdent1", Rd(C->Core, V3D_CTL_IDENT1));
    SaveU(Key, L"V3dCoreIdent2", Rd(C->Core, V3D_CTL_IDENT2));
    SaveU(Key, L"Stage", 10);
}

/* ------------------------------------------------------------------ */
/* KMDF plumbing                                                       */
/* ------------------------------------------------------------------ */
NTSTATUS
GpuEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT C = GetDeviceContext(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), i, Idx = 0;
    WDFKEY Key = NULL;
    ULONG Level, V3dRate;
    PHYSICAL_ADDRESS Lo, Hi, Bnd;

    UNREFERENCED_PARAMETER(Raw);

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_ALL_ACCESS,
                                             WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
        Key = NULL;
    }
    SaveU(Key, L"Stage", 0);

    for (i = 0; i < Count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR D = WdfCmResourceListGetDescriptor(Translated, i);
        PVOID Va;
        if (D->Type != CmResourceTypeMemory) continue;
        Va = MmMapIoSpaceEx(D->u.Memory.Start, D->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE);
        if (Va == NULL) { SaveU(Key, L"MapFailedIndex", Idx); Idx++; continue; }
        switch (Idx) {
        case RES_MAILBOX:  C->Mbx  = Va; C->MbxLen  = D->u.Memory.Length; break;
        case RES_PM:       C->Pm   = Va; C->PmLen   = D->u.Memory.Length; break;
        case RES_V3D_HUB:  C->Hub  = Va; C->HubLen  = D->u.Memory.Length; break;
        case RES_V3D_CORE: C->Core = Va; C->CoreLen = D->u.Memory.Length; break;
        case RES_V3D_SMS:  C->Sms  = Va; C->SmsLen  = D->u.Memory.Length; break;
        default: MmUnmapIoSpace(Va, D->u.Memory.Length); break;
        }
        Idx++;
    }
    SaveU(Key, L"MemResources", Idx);
    SaveU(Key, L"Stage", 1);

    Level   = LoadU(Key, L"ProbeLevel", 0);
    V3dRate = LoadU(Key, L"V3dRate", 0);
    C->BusOffset = LoadU(Key, L"BusOffset", 0xC0000000UL);
    SaveU(Key, L"ProbeLevelSeen", Level);

    /* Mailbox buffer: uncached, physically contiguous, in the first GB (the VideoCore sees it at +BusOffset). */
    Lo.QuadPart = 0; Hi.QuadPart = 0x3FFFFFFF; Bnd.QuadPart = 0;
    C->Msg = (PULONG)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, Lo, Hi, Bnd, MmNonCached);
    if (C->Msg != NULL) {
        C->MsgPa = MmGetPhysicalAddress(C->Msg);
        C->MsgBus = (ULONG)(C->MsgPa.QuadPart + C->BusOffset);
        SaveU(Key, L"MsgPhysLo", (ULONG)C->MsgPa.QuadPart);
        SaveU(Key, L"MsgBusAddr", C->MsgBus);
    } else {
        SaveU(Key, L"MsgAllocFailed", 1);
    }
    SaveU(Key, L"Stage", 2);

    if (C->Mbx != NULL && C->Msg != NULL) {
        ProbeFirmware(C, Key);
    }
    SaveU(Key, L"Stage", 3);

    if (C->Pm != NULL) {
        SaveU(Key, L"PmGrafxInitial", Rd(C->Pm, PM_GRAFX_2712));
    }
    SaveU(Key, L"Stage", 4);

    if (Level >= 1 && C->Mbx != NULL && C->Msg != NULL && C->Pm != NULL) {
        PowerV3d(C, Key, V3dRate);
    }
    if (Level >= 2 && C->Hub != NULL && C->Core != NULL) {
        ReadV3d(C, Key);
    }

    if (Key != NULL) WdfRegistryClose(Key);
    return STATUS_SUCCESS;            /* always load: the registry values are the result */
}

NTSTATUS
GpuEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT C = GetDeviceContext(Device);
    UNREFERENCED_PARAMETER(Translated);

    if (C->Msg)  { MmFreeContiguousMemory(C->Msg); C->Msg = NULL; }
    if (C->Mbx)  { MmUnmapIoSpace(C->Mbx,  C->MbxLen);  C->Mbx = NULL; }
    if (C->Pm)   { MmUnmapIoSpace(C->Pm,   C->PmLen);   C->Pm = NULL; }
    if (C->Hub)  { MmUnmapIoSpace(C->Hub,  C->HubLen);  C->Hub = NULL; }
    if (C->Core) { MmUnmapIoSpace(C->Core, C->CoreLen); C->Core = NULL; }
    if (C->Sms)  { MmUnmapIoSpace(C->Sms,  C->SmsLen);  C->Sms = NULL; }
    return STATUS_SUCCESS;
}

NTSTATUS
GpuEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attr;
    WDFDEVICE Device;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = GpuEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = GpuEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, DEVICE_CONTEXT);
    return WdfDeviceCreate(&DeviceInit, &Attr, &Device);
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    WDF_DRIVER_CONFIG_INIT(&Config, GpuEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
