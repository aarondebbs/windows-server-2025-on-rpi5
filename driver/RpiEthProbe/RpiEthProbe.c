/*
 * RpiEthProbe - EXPERIMENTAL KMDF probe driver for the Raspberry Pi 5 RP1
 * Ethernet MAC (Cadence GEM), matched as ACPI\RPIE0001.
 *
 * Version 2. What it does:
 *   1. Maps the memory windows the firmware ACPI device exposes
 *      (resource 0 = GEM registers, 1 = ETH config block,
 *       2 = RP1 GPIO bank 1, optional: only present with the newer firmware).
 *   2. Reads the GEM module ID register (offset 0xFC).
 *   3. Scans all 32 MDIO addresses for a PHY (registers 2 and 3).
 *   4. If the GPIO window is present, pulses RP1 GPIO 32 (the Pi 5 PHY reset,
 *      active low) and scans again.
 *   5. Writes the results to the device's registry key so they can be read
 *      with regedit (no kernel debugger needed).
 *
 * What it does NOT do: bring up the Ethernet clocks or any data path. It is a
 * hardware reachability test only.
 *
 * Version 1 results on a Pi 5: GEM module ID 0x00070163 (register access
 * works), MDIO transactions complete, but no PHY answered at address 1
 * (all-ones). The likely cause is the PHY being held in reset, which is what
 * step 4 tries to fix.
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

/*
 * RP1 GPIO (from Raspberry Pi Linux pinctrl-rp1.c). GPIO 32 is pin 4 of
 * bank 1 (GPIO 28-33). Per pin: STATUS at +8*n, CTRL at +8*n+4.
 * CTRL: FUNCSEL bits 4:0, OUTOVER bits 13:12 (2 = low, 3 = high),
 *       OEOVER bits 15:14 (3 = output enabled).
 */
#define GPIO_PHY_RESET_PIN_IN_BANK  4
#define GPIO_CTRL_OFFSET(n)         ((n) * 8 + 4)
#define GPIO_FUNCSEL_NULL           0x1FUL
#define GPIO_OUTOVER_LOW            (2UL << 12)
#define GPIO_OUTOVER_HIGH           (3UL << 12)
#define GPIO_OEOVER_ENABLE          (3UL << 14)

typedef struct _DEVICE_CONTEXT {
    PUCHAR  Gem;
    SIZE_T  GemLen;
    PUCHAR  Cfg;
    SIZE_T  CfgLen;
    PUCHAR  Gpio;
    SIZE_T  GpioLen;
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

/*
 * Scan MDIO addresses 0-31 for a PHY. Returns the first address whose ID
 * register 2 is neither 0x0000 nor 0xFFFF (nothing driving the bus reads as
 * all ones), or PHY_NOT_FOUND. Id2/Id3 receive that PHY's ID words.
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

/* Pulse the PHY reset line (RP1 GPIO 32, active low) and wait for it to boot. */
static VOID PhyResetPulse(PDEVICE_CONTEXT C)
{
    volatile ULONG *Ctrl = (volatile ULONG *)
        (C->Gpio + GPIO_CTRL_OFFSET(GPIO_PHY_RESET_PIN_IN_BANK));

    WRITE_REGISTER_ULONG(Ctrl,
        GPIO_FUNCSEL_NULL | GPIO_OUTOVER_LOW | GPIO_OEOVER_ENABLE);
    MsDelay(20);                      /* Linux holds reset for 5 ms; be generous */
    WRITE_REGISTER_ULONG(Ctrl,
        GPIO_FUNCSEL_NULL | GPIO_OUTOVER_HIGH | GPIO_OEOVER_ENABLE);
    MsDelay(200);                     /* let the PHY come out of reset */
}

static VOID SaveUlong(WDFKEY Key, PCUNICODE_STRING Name, ULONG Value)
{
    (VOID)WdfRegistryAssignULong(Key, Name, Value);
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
    ULONG DtAddrId2 = 0xFFFF, DtAddrId3 = 0xFFFF;

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

    /* 3. Scan for a PHY before touching its reset line. */
    AddrBefore = PhyScan(Ctx, &Id2Before, &Id3Before);

    /* 4. Pulse the PHY reset if the firmware gave us the GPIO window. */
    if (Ctx->Gpio != NULL) {
        PhyResetPulse(Ctx);
        ResetDone = 1;
    }

    /* 5. Scan again, and also read the device-tree address (1) directly. */
    AddrAfter = PhyScan(Ctx, &Id2, &Id3);
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
    return STATUS_SUCCESS;
}
