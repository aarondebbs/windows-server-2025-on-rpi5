/*
 * RpiEthProbe - EXPERIMENTAL KMDF probe driver for the Raspberry Pi 5 RP1
 * Ethernet MAC (Cadence GEM), matched as ACPI\RPIE0001.
 *
 * Version 4 (diagnostics + pads/clock). v3 steps below, plus: maps the ETH pad
 * block and RP1 clock block, dumps them, enables clk_eth if it is off, enables
 * the pad inputs, and rescans MDIO after each.
 * Version 3 (diagnostics). What it does:
 *   1. Maps the memory windows the firmware ACPI device exposes
 *      (resource 0 = GEM registers, 1 = ETH config block,
 *       2 = RP1 GPIO bank 1, present with the GPIO-window firmware patch).
 *   2. Reads the GEM module ID register (offset 0xFC).
 *   3. Reads register 2 of every MDIO address (0-31) in several states and
 *      stores the raw values: before touching anything, with the PHY reset
 *      pin driven high, driven low (reset asserted), and high again.
 *   4. Reads back RP1 GPIO 32 STATUS/CTRL in each state, to see whether the
 *      pin really moved.
 *   5. Writes everything to the device's registry key (regedit, no debugger).
 *
 * What it does NOT do: bring up the Ethernet clocks or any data path.
 *
 * Results so far (Pi 5): GEM module ID 0x00070163 (registers work), MDIO
 * transactions complete, GPIO window mapped and reset pulsed, but no PHY
 * answers at any address.
 *
 * Build: Visual Studio + WDK, platform ARM64, KMDF driver.
 */

#include <ntddk.h>
#include <wdf.h>

#define RPIE_TAG 'EIPR'

/* GEM register offsets (from Linux drivers/net/ethernet/cadence/macb.h) */
#define GEM_NCR      0x0000   /* Network control */
#define GEM_NCFGR    0x0004   /* Network config  */
#define GEM_NSR      0x0008   /* Network status  */
#define GEM_MAN      0x0034   /* PHY maintenance */
#define GEM_MID      0x00FC   /* Module ID       */

#define NCR_MPE      (1UL << 4)   /* Management port enable */
#define NSR_IDLE     (1UL << 2)   /* PHY management logic idle */

#define NCFGR_CLK_SHIFT 18        /* MDC clock divider, 3 bits (GEM) */
#define NCFGR_CLK_MASK  (7UL << NCFGR_CLK_SHIFT)
#define GEM_CLK_DIV224  7UL       /* slowest MDC, safest without knowing pclk */

#define MAN_SOF_CL22    (1UL << 30)
#define MAN_RW_READ     (2UL << 28)
#define MAN_CODE        (2UL << 16)
#define MAN_PHYA(a)     (((ULONG)(a) & 0x1F) << 23)
#define MAN_REGA(r)     (((ULONG)(r) & 0x1F) << 18)

#define PHY_ADDR_DT     1         /* Pi 5 device tree: ethernet-phy@1 */
#define PHY_NOT_FOUND   0xFFFFFFFFUL

#define RAW_TIMEOUT     0xDEAD    /* sentinel stored when an MDIO read timed out */

/*
 * RP1 GPIO (from Raspberry Pi Linux pinctrl-rp1.c). GPIO 32 is pin 4 of
 * bank 1 (GPIO 28-33). Per pin: STATUS at +8*n, CTRL at +8*n+4.
 * CTRL: FUNCSEL bits 4:0, OUTOVER bits 13:12 (2 = low, 3 = high),
 *       OEOVER bits 15:14 (3 = output enabled).
 */
#define GPIO_PHY_RESET_PIN_IN_BANK  4
#define GPIO_STATUS_OFFSET(n)       ((n) * 8)
#define GPIO_CTRL_OFFSET(n)         ((n) * 8 + 4)
#define GPIO_FUNCSEL_NULL           0x1FUL
#define GPIO_OUTOVER_LOW            (2UL << 12)
#define GPIO_OUTOVER_HIGH           (3UL << 12)
#define GPIO_OEOVER_ENABLE          (3UL << 14)

/* RP1 main clock block: clk_eth and clk_eth_tsu. CTRL bit 11 = enable. */
#define CLK_ETH_CTRL        0x064
#define CLK_ETH_DIV         0x068
#define CLK_ETH_SEL         0x070
#define CLK_ETH_TSU_CTRL    0x134
#define CLK_ETH_TSU_DIV     0x138
#define CLK_ETH_TSU_SEL     0x140
#define CLK_CTRL_ENABLE     (1UL << 11)

/* ETH pad block: dword 0 is the voltage select, dwords 1.. are per-pad. */
#define PADS_DWORDS         16
#define PAD_IE              (1UL << 6)   /* input enable   */
#define PAD_OD              (1UL << 7)   /* output disable */

typedef struct _DEVICE_CONTEXT {
    PUCHAR  Gem;
    SIZE_T  GemLen;
    PUCHAR  Cfg;
    SIZE_T  CfgLen;
    PUCHAR  Gpio;
    SIZE_T  GpioLen;
    PUCHAR  Pads;
    SIZE_T  PadsLen;
    PUCHAR  Clk;
    SIZE_T  ClkLen;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD        RpiEthEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE  RpiEthEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE  RpiEthEvtReleaseHardware;

static ULONG GemRead(PDEVICE_CONTEXT C, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(C->Gem + Off));
}

static VOID GemWrite(PDEVICE_CONTEXT C, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(C->Gem + Off), Val);
}

static ULONG RegRd(PUCHAR Base, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Base + Off));
}

static VOID RegWr(PUCHAR Base, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off), Val);
}

static VOID DumpPads(PDEVICE_CONTEXT C, ULONG Out[PADS_DWORDS])
{
    ULONG i;
    for (i = 0; i < PADS_DWORDS; i++) {
        Out[i] = RegRd(C->Pads, i * 4);
    }
}

static ULONG GpioStatusRead(PDEVICE_CONTEXT C)
{
    return READ_REGISTER_ULONG((volatile ULONG *)
        (C->Gpio + GPIO_STATUS_OFFSET(GPIO_PHY_RESET_PIN_IN_BANK)));
}

static ULONG GpioCtrlRead(PDEVICE_CONTEXT C)
{
    return READ_REGISTER_ULONG((volatile ULONG *)
        (C->Gpio + GPIO_CTRL_OFFSET(GPIO_PHY_RESET_PIN_IN_BANK)));
}

/* Drive GPIO 32 (PHY reset, active low) high = released, low = asserted. */
static VOID GpioDrive(PDEVICE_CONTEXT C, BOOLEAN High)
{
    WRITE_REGISTER_ULONG(
        (volatile ULONG *)(C->Gpio + GPIO_CTRL_OFFSET(GPIO_PHY_RESET_PIN_IN_BANK)),
        GPIO_FUNCSEL_NULL | (High ? GPIO_OUTOVER_HIGH : GPIO_OUTOVER_LOW) |
        GPIO_OEOVER_ENABLE);
}

static VOID MsDelay(ULONG Ms)
{
    LARGE_INTEGER T;
    T.QuadPart = -((LONGLONG)Ms * 10000);   /* relative, 100 ns units */
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &T);
}

/* Wait for the MDIO engine to be idle. Returns FALSE on timeout. */
static BOOLEAN MdioWaitIdle(PDEVICE_CONTEXT C)
{
    ULONG i;
    for (i = 0; i < 2000; i++) {
        if (GemRead(C, GEM_NSR) & NSR_IDLE) {
            return TRUE;
        }
        KeStallExecutionProcessor(10);
    }
    return FALSE;
}

/* Clause 22 PHY register read. */
static NTSTATUS PhyRead(PDEVICE_CONTEXT C, ULONG Phy, ULONG Reg, PUSHORT Value)
{
    if (!MdioWaitIdle(C)) {
        return STATUS_IO_TIMEOUT;
    }
    GemWrite(C, GEM_MAN,
             MAN_SOF_CL22 | MAN_RW_READ | MAN_CODE | MAN_PHYA(Phy) | MAN_REGA(Reg));
    if (!MdioWaitIdle(C)) {
        return STATUS_IO_TIMEOUT;
    }
    *Value = (USHORT)(GemRead(C, GEM_MAN) & 0xFFFF);
    return STATUS_SUCCESS;
}

/* Read register 2 of all 32 MDIO addresses. Timeouts are stored as 0xDEAD. */
static VOID ScanRaw(PDEVICE_CONTEXT C, USHORT Raw[32])
{
    ULONG Addr;
    for (Addr = 0; Addr < 32; Addr++) {
        USHORT R = 0;
        if (!NT_SUCCESS(PhyRead(C, Addr, 2, &R))) {
            R = RAW_TIMEOUT;
        }
        Raw[Addr] = R;
    }
}

/* Summarise a raw scan: how many addresses read all-ones, zero, or something else. */
static VOID CountRaw(const USHORT Raw[32], PULONG Ones, PULONG Zeros, PULONG Other)
{
    ULONG i;
    *Ones = *Zeros = *Other = 0;
    for (i = 0; i < 32; i++) {
        if (Raw[i] == 0xFFFF)      (*Ones)++;
        else if (Raw[i] == 0x0000) (*Zeros)++;
        else                       (*Other)++;
    }
}

/*
 * Find the first address whose ID register 2 is neither 0x0000 nor 0xFFFF
 * (nothing driving the bus reads as all ones). Returns the address or
 * PHY_NOT_FOUND; Id2/Id3 receive that PHY's ID words.
 */
static ULONG PhyScan(PDEVICE_CONTEXT C, PUSHORT Id2, PUSHORT Id3)
{
    ULONG Addr;

    for (Addr = 0; Addr < 32; Addr++) {
        USHORT R2 = 0, R3 = 0;

        if (!NT_SUCCESS(PhyRead(C, Addr, 2, &R2))) {
            return PHY_NOT_FOUND;
        }
        if (R2 == 0xFFFF || R2 == 0x0000) {
            continue;
        }
        (VOID)PhyRead(C, Addr, 3, &R3);
        *Id2 = R2;
        *Id3 = R3;
        return Addr;
    }
    return PHY_NOT_FOUND;
}

static VOID SaveUlong(WDFKEY Key, PCUNICODE_STRING Name, ULONG Value)
{
    (VOID)WdfRegistryAssignULong(Key, Name, Value);
}

/* Stores 32 USHORTs (64 bytes) as REG_BINARY; address 0 first, little endian. */
static VOID SaveRaw(WDFKEY Key, PCUNICODE_STRING Name, USHORT Raw[32])
{
    (VOID)WdfRegistryAssignValue(Key, Name, REG_BINARY, 32 * sizeof(USHORT), Raw);
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, RpiEthEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath,
                           WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}

NTSTATUS
RpiEthEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS Status;
    WDFDEVICE Device;
    WDF_OBJECT_ATTRIBUTES Attrs;
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = RpiEthEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = RpiEthEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attrs, DEVICE_CONTEXT);
    Status = WdfDeviceCreate(&DeviceInit, &Attrs, &Device);
    return Status;
}

NTSTATUS
RpiEthEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT Ctx = GetDeviceContext(Device);
    ULONG Count, i, MemIndex = 0;
    NTSTATUS Status;
    WDFKEY Key = NULL;
    USHORT Id2 = 0, Id3 = 0;
    USHORT Id2Before = 0, Id3Before = 0;
    ULONG Mid;
    ULONG AddrBefore, AddrAfter;
    ULONG ResetDone = 0;
    ULONG GpioCtrlBefore = 0, GpioCtrlAfter = 0;
    ULONG GpioStatusBefore = 0, GpioStatusHigh1 = 0, GpioStatusLow = 0, GpioStatusHigh2 = 0;
    ULONG Ones, Zeros, Other;
    ULONG DtAddrId2 = 0xFFFF, DtAddrId3 = 0xFFFF;
    USHORT RawBefore[32] = {0};
    USHORT RawHigh1[32]  = {0};
    USHORT RawLow[32]    = {0};
    USHORT RawAfter[32]  = {0};
    USHORT RawClk[32]    = {0};
    USHORT RawPads[32]   = {0};
    ULONG  PadsBefore[PADS_DWORDS] = {0};
    ULONG  PadsAfter[PADS_DWORDS]  = {0};
    ULONG  ClkB[6] = {0};          /* eth ctrl,div,sel ; tsu ctrl,div,sel */
    ULONG  ClkA[6] = {0};
    ULONG  ClkMapped = 0, PadsMapped = 0, ClkEnabledByUs = 0;
    ULONG  ClkOnes = 0, ClkZeros = 0, ClkOther = 0;
    ULONG  PadOnes = 0, PadZeros = 0, PadOther = 0;
    ULONG  AddrClk = PHY_NOT_FOUND, AddrPads = PHY_NOT_FOUND;

    DECLARE_CONST_UNICODE_STRING(NameMid,        L"GemModuleId");
    DECLARE_CONST_UNICODE_STRING(NameNcr,        L"GemNcr");
    DECLARE_CONST_UNICODE_STRING(NameNsr,        L"GemNsr");
    DECLARE_CONST_UNICODE_STRING(NameMem,        L"MemResourcesMapped");
    DECLARE_CONST_UNICODE_STRING(NameGpio,       L"GpioMapped");
    DECLARE_CONST_UNICODE_STRING(NameResetDone,  L"PhyResetPulsed");
    DECLARE_CONST_UNICODE_STRING(NameAddrBefore, L"PhyAddrBeforeReset");
    DECLARE_CONST_UNICODE_STRING(NameBefore2,    L"PhyId1BeforeReset");
    DECLARE_CONST_UNICODE_STRING(NameBefore3,    L"PhyId2BeforeReset");
    DECLARE_CONST_UNICODE_STRING(NameAddrAfter,  L"PhyAddrAfterReset");
    DECLARE_CONST_UNICODE_STRING(NamePhy2,       L"PhyId1Reg2");
    DECLARE_CONST_UNICODE_STRING(NamePhy3,       L"PhyId2Reg3");
    DECLARE_CONST_UNICODE_STRING(NameDt2,        L"PhyAddr1Reg2");
    DECLARE_CONST_UNICODE_STRING(NameDt3,        L"PhyAddr1Reg3");
    DECLARE_CONST_UNICODE_STRING(NameGCtrlB,     L"GpioCtrlBefore");
    DECLARE_CONST_UNICODE_STRING(NameGCtrlA,     L"GpioCtrlAfter");
    DECLARE_CONST_UNICODE_STRING(NameGStB,       L"GpioStatusBefore");
    DECLARE_CONST_UNICODE_STRING(NameGStH1,      L"GpioStatusDrivenHigh");
    DECLARE_CONST_UNICODE_STRING(NameGStL,       L"GpioStatusDrivenLow");
    DECLARE_CONST_UNICODE_STRING(NameGStH2,      L"GpioStatusHighAgain");
    DECLARE_CONST_UNICODE_STRING(NameRawB,       L"RawScanBefore");
    DECLARE_CONST_UNICODE_STRING(NameRawH1,      L"RawScanDrivenHigh");
    DECLARE_CONST_UNICODE_STRING(NameRawL,       L"RawScanDrivenLow");
    DECLARE_CONST_UNICODE_STRING(NameRawA,       L"RawScanAfter");
    DECLARE_CONST_UNICODE_STRING(NamePadsM,      L"PadsMapped");
    DECLARE_CONST_UNICODE_STRING(NameClkM,       L"ClkMapped");
    DECLARE_CONST_UNICODE_STRING(NamePadsB,      L"PadsDumpBefore");
    DECLARE_CONST_UNICODE_STRING(NamePadsA,      L"PadsDumpAfter");
    DECLARE_CONST_UNICODE_STRING(NameClkBn,      L"ClkDumpBefore");
    DECLARE_CONST_UNICODE_STRING(NameClkAn,      L"ClkDumpAfter");
    DECLARE_CONST_UNICODE_STRING(NameClkEn,      L"ClkEthEnabledByDriver");
    DECLARE_CONST_UNICODE_STRING(NameRawClk,     L"RawScanAfterClk");
    DECLARE_CONST_UNICODE_STRING(NameRawPads,    L"RawScanAfterPads");
    DECLARE_CONST_UNICODE_STRING(NameAddrClk,    L"PhyAddrAfterClk");
    DECLARE_CONST_UNICODE_STRING(NameAddrPads,   L"PhyAddrAfterPads");
    DECLARE_CONST_UNICODE_STRING(NameClkOnes,    L"AfterClkScanAllOnes");
    DECLARE_CONST_UNICODE_STRING(NamePadOnes,    L"AfterPadsScanAllOnes");
    DECLARE_CONST_UNICODE_STRING(NameOnes,       L"AfterScanAllOnes");
    DECLARE_CONST_UNICODE_STRING(NameZeros,      L"AfterScanZeros");
    DECLARE_CONST_UNICODE_STRING(NameOther,      L"AfterScanOther");

    UNREFERENCED_PARAMETER(ResourcesRaw);

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < Count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR D =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (D->Type == CmResourceTypeMemory) {
            PVOID Va = MmMapIoSpaceEx(D->u.Memory.Start,
                                      D->u.Memory.Length,
                                      PAGE_READWRITE | PAGE_NOCACHE);
            if (Va == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            if (MemIndex == 0) {
                Ctx->Gem = (PUCHAR)Va;
                Ctx->GemLen = D->u.Memory.Length;
            } else if (MemIndex == 1) {
                Ctx->Cfg = (PUCHAR)Va;
                Ctx->CfgLen = D->u.Memory.Length;
            } else if (MemIndex == 2) {
                Ctx->Gpio = (PUCHAR)Va;
                Ctx->GpioLen = D->u.Memory.Length;
            } else if (MemIndex == 3) {
                Ctx->Pads = (PUCHAR)Va;
                Ctx->PadsLen = D->u.Memory.Length;
            } else if (MemIndex == 4) {
                Ctx->Clk = (PUCHAR)Va;
                Ctx->ClkLen = D->u.Memory.Length;
            } else {
                MmUnmapIoSpace(Va, D->u.Memory.Length);
            }
            MemIndex++;
        }
    }

    if (Ctx->Gem == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Status = WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE,
                                      KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &Key);
    if (!NT_SUCCESS(Status)) {
        /* Keep going: the probe still has value even if we can't record it. */
        Key = NULL;
    }

    /* 1. Module ID. A real GEM returns a non-trivial IDNUM in bits 27:16. */
    Mid = GemRead(Ctx, GEM_MID);

    /* 2. Enable the MDIO port with the slowest MDC divider. */
    {
        ULONG Ncfgr = GemRead(Ctx, GEM_NCFGR);
        Ncfgr = (Ncfgr & ~NCFGR_CLK_MASK) | (GEM_CLK_DIV224 << NCFGR_CLK_SHIFT);
        GemWrite(Ctx, GEM_NCFGR, Ncfgr);
        GemWrite(Ctx, GEM_NCR, GemRead(Ctx, GEM_NCR) | NCR_MPE);
    }

    /* 3. Baseline scan, before touching the PHY reset line. */
    ScanRaw(Ctx, RawBefore);
    AddrBefore = PhyScan(Ctx, &Id2Before, &Id3Before);

    /* 4. Walk the reset pin through high, low, high; scan in each state. */
    if (Ctx->Gpio != NULL) {
        GpioCtrlBefore   = GpioCtrlRead(Ctx);
        GpioStatusBefore = GpioStatusRead(Ctx);

        GpioDrive(Ctx, TRUE);                 /* reset released */
        MsDelay(300);
        GpioStatusHigh1 = GpioStatusRead(Ctx);
        ScanRaw(Ctx, RawHigh1);

        GpioDrive(Ctx, FALSE);                /* reset asserted */
        MsDelay(50);
        GpioStatusLow = GpioStatusRead(Ctx);
        ScanRaw(Ctx, RawLow);

        GpioDrive(Ctx, TRUE);                 /* reset released again */
        MsDelay(500);
        GpioStatusHigh2 = GpioStatusRead(Ctx);
        GpioCtrlAfter   = GpioCtrlRead(Ctx);
        ResetDone = 1;
    }

    /* 4b. Clock block: dump clk_eth / clk_eth_tsu; enable clk_eth if it is
     *     off and has a divider programmed (never write if DIV is 0). */
    if (Ctx->Clk != NULL) {
        ClkMapped = 1;
        ClkB[0] = RegRd(Ctx->Clk, CLK_ETH_CTRL);
        ClkB[1] = RegRd(Ctx->Clk, CLK_ETH_DIV);
        ClkB[2] = RegRd(Ctx->Clk, CLK_ETH_SEL);
        ClkB[3] = RegRd(Ctx->Clk, CLK_ETH_TSU_CTRL);
        ClkB[4] = RegRd(Ctx->Clk, CLK_ETH_TSU_DIV);
        ClkB[5] = RegRd(Ctx->Clk, CLK_ETH_TSU_SEL);

        if (!(ClkB[0] & CLK_CTRL_ENABLE) && ClkB[1] != 0) {
            RegWr(Ctx->Clk, CLK_ETH_CTRL, ClkB[0] | CLK_CTRL_ENABLE);
            ClkEnabledByUs = 1;
        }
        if (!(ClkB[3] & CLK_CTRL_ENABLE) && ClkB[4] != 0) {
            RegWr(Ctx->Clk, CLK_ETH_TSU_CTRL, ClkB[3] | CLK_CTRL_ENABLE);
            ClkEnabledByUs |= 2;
        }
        MsDelay(50);
        ClkA[0] = RegRd(Ctx->Clk, CLK_ETH_CTRL);
        ClkA[1] = RegRd(Ctx->Clk, CLK_ETH_DIV);
        ClkA[2] = RegRd(Ctx->Clk, CLK_ETH_SEL);
        ClkA[3] = RegRd(Ctx->Clk, CLK_ETH_TSU_CTRL);
        ClkA[4] = RegRd(Ctx->Clk, CLK_ETH_TSU_DIV);
        ClkA[5] = RegRd(Ctx->Clk, CLK_ETH_TSU_SEL);
    }
    ScanRaw(Ctx, RawClk);
    AddrClk = PhyScan(Ctx, &Id2, &Id3);
    CountRaw(RawClk, &ClkOnes, &ClkZeros, &ClkOther);

    /* 4c. Pad block: dump, then enable input and output on every pad
     *     (set IE, clear OD) so MDC/MDIO can reach the PHY. */
    if (Ctx->Pads != NULL) {
        ULONG p;
        PadsMapped = 1;
        DumpPads(Ctx, PadsBefore);
        for (p = 1; p < PADS_DWORDS; p++) {
            ULONG V = PadsBefore[p];
            RegWr(Ctx->Pads, p * 4, (V | PAD_IE) & ~PAD_OD);
        }
        MsDelay(50);
        DumpPads(Ctx, PadsAfter);
    }
    ScanRaw(Ctx, RawPads);
    AddrPads = PhyScan(Ctx, &Id2, &Id3);
    CountRaw(RawPads, &PadOnes, &PadZeros, &PadOther);

    /* 5. Final scan, and the device-tree address read directly. */
    ScanRaw(Ctx, RawAfter);
    AddrAfter = PhyScan(Ctx, &Id2, &Id3);
    CountRaw(RawAfter, &Ones, &Zeros, &Other);
    {
        USHORT R2 = 0xFFFF, R3 = 0xFFFF;
        (VOID)PhyRead(Ctx, PHY_ADDR_DT, 2, &R2);
        (VOID)PhyRead(Ctx, PHY_ADDR_DT, 3, &R3);
        DtAddrId2 = R2;
        DtAddrId3 = R3;
    }

    if (Key != NULL) {
        SaveUlong(Key, &NameMem,        MemIndex);
        SaveUlong(Key, &NameGpio,       Ctx->Gpio != NULL);
        SaveUlong(Key, &NameResetDone,  ResetDone);
        SaveUlong(Key, &NameMid,        Mid);
        SaveUlong(Key, &NameNcr,        GemRead(Ctx, GEM_NCR));
        SaveUlong(Key, &NameNsr,        GemRead(Ctx, GEM_NSR));
        SaveUlong(Key, &NameAddrBefore, AddrBefore);
        SaveUlong(Key, &NameBefore2,    Id2Before);
        SaveUlong(Key, &NameBefore3,    Id3Before);
        SaveUlong(Key, &NameAddrAfter,  AddrAfter);
        SaveUlong(Key, &NamePhy2,       Id2);
        SaveUlong(Key, &NamePhy3,       Id3);
        SaveUlong(Key, &NameDt2,        DtAddrId2);
        SaveUlong(Key, &NameDt3,        DtAddrId3);
        SaveUlong(Key, &NameGCtrlB,     GpioCtrlBefore);
        SaveUlong(Key, &NameGCtrlA,     GpioCtrlAfter);
        SaveUlong(Key, &NameGStB,       GpioStatusBefore);
        SaveUlong(Key, &NameGStH1,      GpioStatusHigh1);
        SaveUlong(Key, &NameGStL,       GpioStatusLow);
        SaveUlong(Key, &NameGStH2,      GpioStatusHigh2);
        SaveUlong(Key, &NameOnes,       Ones);
        SaveUlong(Key, &NameZeros,      Zeros);
        SaveUlong(Key, &NameOther,      Other);
        SaveUlong(Key, &NamePadsM, PadsMapped);
        SaveUlong(Key, &NameClkM,  ClkMapped);
        SaveUlong(Key, &NameClkEn, ClkEnabledByUs);
        SaveUlong(Key, &NameAddrClk,  AddrClk);
        SaveUlong(Key, &NameAddrPads, AddrPads);
        SaveUlong(Key, &NameClkOnes,  ClkOnes);
        SaveUlong(Key, &NamePadOnes,  PadOnes);
        (VOID)WdfRegistryAssignValue(Key, &NamePadsB, REG_BINARY, sizeof(PadsBefore), PadsBefore);
        (VOID)WdfRegistryAssignValue(Key, &NamePadsA, REG_BINARY, sizeof(PadsAfter), PadsAfter);
        (VOID)WdfRegistryAssignValue(Key, &NameClkBn, REG_BINARY, sizeof(ClkB), ClkB);
        (VOID)WdfRegistryAssignValue(Key, &NameClkAn, REG_BINARY, sizeof(ClkA), ClkA);
        SaveRaw(Key, &NameRawClk,  RawClk);
        SaveRaw(Key, &NameRawPads, RawPads);
        SaveRaw(Key, &NameRawB,  RawBefore);
        SaveRaw(Key, &NameRawH1, RawHigh1);
        SaveRaw(Key, &NameRawL,  RawLow);
        SaveRaw(Key, &NameRawA,  RawAfter);
        WdfRegistryClose(Key);
    }

    /* Succeed even if no PHY answered: the registry values tell us why. */
    return STATUS_SUCCESS;
}

NTSTATUS
RpiEthEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT Ctx = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (Ctx->Gem != NULL) {
        MmUnmapIoSpace(Ctx->Gem, Ctx->GemLen);
        Ctx->Gem = NULL;
    }
    if (Ctx->Cfg != NULL) {
        MmUnmapIoSpace(Ctx->Cfg, Ctx->CfgLen);
        Ctx->Cfg = NULL;
    }
    if (Ctx->Gpio != NULL) {
        MmUnmapIoSpace(Ctx->Gpio, Ctx->GpioLen);
        Ctx->Gpio = NULL;
    }
    if (Ctx->Pads != NULL) {
        MmUnmapIoSpace(Ctx->Pads, Ctx->PadsLen);
        Ctx->Pads = NULL;
    }
    if (Ctx->Clk != NULL) {
        MmUnmapIoSpace(Ctx->Clk, Ctx->ClkLen);
        Ctx->Clk = NULL;
    }
    return STATUS_SUCCESS;
}
