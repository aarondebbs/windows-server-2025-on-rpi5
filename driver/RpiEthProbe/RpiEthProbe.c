/*
 * RpiEthProbe - EXPERIMENTAL KMDF probe driver for the Raspberry Pi 5 RP1
 * Ethernet MAC (Cadence GEM), matched as ACPI\RPIE0001.
 *
 * What it does:
 *   1. Maps the two memory windows the firmware ACPI device exposes
 *      (resource 0 = GEM registers, resource 1 = ETH config block).
 *   2. Reads the GEM module ID register (offset 0xFC).
 *   3. Reads PHY ID registers 2 and 3 at MDIO address 1 (Pi 5 board file).
 *   4. Writes the results to the device's registry key so they can be read
 *      with regedit (no kernel debugger needed).
 *
 * What it does NOT do: bring up clocks, reset, PHY or any data path. It is a
 * hardware reachability test only.
 *
 * WARNING: if the RP1 Ethernet block is not clocked, reading its registers
 * may return garbage (0 or 0xFFFFFFFF) or even stall the PCIe bus and hang
 * the machine. Untested on real hardware.
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

#define PHY_ADDR        1         /* Pi 5 device tree: ethernet-phy@1 */

typedef struct _DEVICE_CONTEXT {
    PUCHAR  Gem;
    SIZE_T  GemLen;
    PUCHAR  Cfg;
    SIZE_T  CfgLen;
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
    ULONG Mid;
    NTSTATUS PhyStatus;

    DECLARE_CONST_UNICODE_STRING(NameMid,     L"GemModuleId");
    DECLARE_CONST_UNICODE_STRING(NameNcr,     L"GemNcr");
    DECLARE_CONST_UNICODE_STRING(NameNsr,     L"GemNsr");
    DECLARE_CONST_UNICODE_STRING(NamePhy2,    L"PhyId1Reg2");
    DECLARE_CONST_UNICODE_STRING(NamePhy3,    L"PhyId2Reg3");
    DECLARE_CONST_UNICODE_STRING(NamePhyStat, L"PhyReadStatus");
    DECLARE_CONST_UNICODE_STRING(NameMem,     L"MemResourcesMapped");

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
            } else {
                MmUnmapIoSpace(Va, D->u.Memory.Length);
            }
            MemIndex++;
        }
    }

    if (Ctx->Gem == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Status = WdfDeviceOpenRegKey(Device, PLUGPLAY_REGKEY_DEVICE,
                                 KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &Key);
    if (!NT_SUCCESS(Status)) {
        /* Keep going: the probe still has value even if we can't record it. */
        Key = NULL;
    }

    /* 1. Module ID. A real GEM returns a non-trivial IDNUM in bits 27:16. */
    Mid = GemRead(Ctx, GEM_MID);

    /* 2. Enable the MDIO port with the slowest MDC divider, then read PHY ID. */
    {
        ULONG Ncfgr = GemRead(Ctx, GEM_NCFGR);
        Ncfgr = (Ncfgr & ~NCFGR_CLK_MASK) | (GEM_CLK_DIV224 << NCFGR_CLK_SHIFT);
        GemWrite(Ctx, GEM_NCFGR, Ncfgr);
        GemWrite(Ctx, GEM_NCR, GemRead(Ctx, GEM_NCR) | NCR_MPE);
    }

    PhyStatus = PhyRead(Ctx, PHY_ADDR, 2, &Id2);
    if (NT_SUCCESS(PhyStatus)) {
        PhyStatus = PhyRead(Ctx, PHY_ADDR, 3, &Id3);
    }

    if (Key != NULL) {
        SaveUlong(Key, &NameMem,     MemIndex);
        SaveUlong(Key, &NameMid,     Mid);
        SaveUlong(Key, &NameNcr,     GemRead(Ctx, GEM_NCR));
        SaveUlong(Key, &NameNsr,     GemRead(Ctx, GEM_NSR));
        SaveUlong(Key, &NamePhy2,    Id2);
        SaveUlong(Key, &NamePhy3,    Id3);
        SaveUlong(Key, &NamePhyStat, (ULONG)PhyStatus);
        WdfRegistryClose(Key);
    }

    /* Succeed even if the PHY did not answer: the registry values tell us why. */
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
    return STATUS_SUCCESS;
}
