/*
 * CrosTcssMux.c — Driver Lifecycle & Power Management
 *
 * Implements:
 *   1. DriverEntry and driver-level initialization
 *   2. EvtDriverDeviceAdd — device object creation with work items and timers
 *   3. EvtDevicePrepareHardware — PMC MMIO mapping, ACPI parsing, notification binding
 *   4. EvtDeviceReleaseHardware — cleanup and MMIO unmapping
 *   5. EvtDeviceD0Entry / EvtDeviceD0Exit — power state transitions
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#include <ntddk.h>
#include <wdf.h>
#include <acpiioct.h>
#include <ntstrsafe.h>
#include "cros_tcss_hw.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, CrosTcss_EvtDeviceAdd)
#pragma alloc_text(PAGE, CrosTcss_EvtPrepareHardware)
#pragma alloc_text(PAGE, CrosTcss_EvtReleaseHardware)
#pragma alloc_text(PAGE, CrosTcss_EvtD0Entry)
#pragma alloc_text(PAGE, CrosTcss_EvtD0Exit)
#endif

/* External work item / timer callbacks declared in cros_tcss_irq.c */
EVT_WDF_WORKITEM CrosTcss_EvtEventWorkItem;
EVT_WDF_TIMER CrosTcss_EvtWatchdogTimer;

/* =========================================================================
 * Internal: Platform Detection
 *
 * Detect the Intel platform generation by checking PCI Device IDs of the
 * PMC controller. This determines the PMC bit layout to use.
 * ========================================================================= */

static TCSS_PLATFORM_GEN
CrosTcss_DetectPlatform(VOID)
{
    /*
     * The PMC PCI device ID indicates the platform:
     *   Tiger Lake:   0xA0A1
     *   Alder Lake-P: 0x51A1
     *   Alder Lake-S: 0x7A01 (not typical for Chromebooks)
     *   Raptor Lake:  0xA801
     *   Meteor Lake:  0x7E01 / 0x7D01
     *
     * We use a CPUID-based heuristic as a simpler approach,
     * checking the model/family from CPUID leaf 1.
     */
    int cpuInfo[4] = {0};
    UINT32 family, model, extModel;

    __cpuid(cpuInfo, 1);

    family = (cpuInfo[0] >> 8) & 0xF;
    model = (cpuInfo[0] >> 4) & 0xF;
    extModel = (cpuInfo[0] >> 16) & 0xF;

    if (family == 0x6) {
        model |= (extModel << 4);

        /* Tiger Lake: model 0x8C (TGL-U/Y) or 0x8D (TGL-H) */
        if (model == 0x8C || model == 0x8D) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "CrosTcssMux: Detected Tiger Lake (model=0x%02X)\n", model));
            return TCSS_GEN_TIGER_LAKE;
        }

        /* Alder Lake: model 0x9A (ADL-P/M) or 0x97 (ADL-S) */
        if (model == 0x9A || model == 0x97) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "CrosTcssMux: Detected Alder Lake (model=0x%02X)\n", model));
            return TCSS_GEN_ALDER_LAKE;
        }

        /* Raptor Lake: model 0xB7 (RPL-S) or 0xBA (RPL-P) or 0xBF */
        if (model == 0xB7 || model == 0xBA || model == 0xBF) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "CrosTcssMux: Detected Raptor Lake (model=0x%02X)\n", model));
            return TCSS_GEN_RAPTOR_LAKE;
        }

        /* Meteor Lake: model 0xAA (MTL-P) or 0xAC */
        if (model == 0xAA || model == 0xAC) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "CrosTcssMux: Detected Meteor Lake (model=0x%02X)\n", model));
            return TCSS_GEN_METEOR_LAKE;
        }
    }

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
        "CrosTcssMux: Unknown platform (family=0x%X model=0x%02X), "
        "defaulting to Alder Lake layout\n", family, model));
    return TCSS_GEN_ALDER_LAKE;
}

/* =========================================================================
 * DriverEntry — KMDF driver initialization
 * ========================================================================= */

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: DriverEntry\n"));

    WDF_DRIVER_CONFIG_INIT(&config, CrosTcss_EvtDeviceAdd);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        &attributes,
        &config,
        WDF_NO_HANDLE);

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "CrosTcssMux: WdfDriverCreate failed (0x%08X)\n", status));
        return status;
    }

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: Driver created successfully\n"));

    return STATUS_SUCCESS;
}

/* =========================================================================
 * CrosTcss_EvtDeviceAdd — Create the KMDF device object
 *
 * Sets up:
 *   - Device context with DEVICE_CONTEXT structure
 *   - PnP and power management callbacks
 *   - Work item for event processing
 *   - Watchdog timer (2-second period)
 *   - Spin lock for state synchronization
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
CrosTcss_EvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDFDEVICE device;
    PDEVICE_CONTEXT devCtx;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_WORKITEM_CONFIG workItemConfig;
    WDF_OBJECT_ATTRIBUTES workItemAttributes;
    WDF_TIMER_CONFIG timerConfig;
    WDF_OBJECT_ATTRIBUTES timerAttributes;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: EvtDeviceAdd\n"));

    /* Configure PnP power callbacks */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = CrosTcss_EvtPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = CrosTcss_EvtReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry = CrosTcss_EvtD0Entry;
    pnpCallbacks.EvtDeviceD0Exit = CrosTcss_EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    /* Create device with context */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    deviceAttributes.EvtCleanupCallback = NULL;

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "CrosTcssMux: WdfDeviceCreate failed (0x%08X)\n", status));
        return status;
    }

    devCtx = GetDeviceContext(device);
    RtlZeroMemory(devCtx, sizeof(DEVICE_CONTEXT));
    devCtx->Device = device;
    devCtx->D0Active = FALSE;

    /* Create spin lock for state protection */
    status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &devCtx->StateLock);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "CrosTcssMux: WdfSpinLockCreate failed (0x%08X)\n", status));
        return status;
    }

    /* Create work item for event processing (runs at PASSIVE_LEVEL) */
    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, CrosTcss_EvtEventWorkItem);
    workItemConfig.AutomaticSerialization = FALSE;

    WDF_OBJECT_ATTRIBUTES_INIT(&workItemAttributes);
    workItemAttributes.ParentObject = device;

    status = WdfWorkItemCreate(&workItemConfig, &workItemAttributes,
                               &devCtx->EventWorkItem);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "CrosTcssMux: WdfWorkItemCreate failed (0x%08X)\n", status));
        return status;
    }

    /* Create watchdog timer (250ms periodic for responsive hotplug detection) */
    WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, CrosTcss_EvtWatchdogTimer, 250);
    timerConfig.AutomaticSerialization = FALSE;
    timerConfig.TolerableDelay = 50;  /* 50ms tolerance for power efficiency */

    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = device;

    status = WdfTimerCreate(&timerConfig, &timerAttributes,
                            &devCtx->WatchdogTimer);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "CrosTcssMux: WdfTimerCreate failed (0x%08X)\n", status));
        return status;
    }

    /* Detect platform generation */
    devCtx->PlatformGen = CrosTcss_DetectPlatform();

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: Device created (platform=%d)\n", devCtx->PlatformGen));

    return STATUS_SUCCESS;
}

/* =========================================================================
 * CrosTcss_EvtPrepareHardware — Map MMIO, parse ACPI, bind notifications
 *
 * Called when the device enters the working state (D0) for the first time.
 * We:
 *   1. Map the Intel PMC IPC MMIO registers
 *   2. Parse ACPI _DSD for USB port mappings
 *   3. Register for ACPI EC notifications
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
CrosTcss_EvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    NTSTATUS status;
    PDEVICE_CONTEXT devCtx;
    PHYSICAL_ADDRESS pmcPhys;
    ULONG resourceCount;
    BOOLEAN mmioMapped = FALSE;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    PAGED_CODE();

    devCtx = GetDeviceContext(Device);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: EvtPrepareHardware\n"));

    /* ---- Step 1: Map PMC IPC MMIO ----
     *
     * First try to find a memory resource in the translated resource list.
     * If no memory resource is assigned (common for ACPI-enumerated filter
     * drivers), fall back to mapping the well-known physical address.
     */
    resourceCount = WdfCmResourceListGetCount(ResourcesTranslated);

    for (ULONG i = 0; i < resourceCount; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (desc->Type == CmResourceTypeMemory) {
            /* Use the first memory resource as PMC IPC base */
            devCtx->PmcIpcPhysAddr = desc->u.Memory.Start;
            devCtx->PmcIpcLength = desc->u.Memory.Length;

            devCtx->PmcIpcBase = MmMapIoSpaceEx(
                devCtx->PmcIpcPhysAddr,
                devCtx->PmcIpcLength,
                PAGE_READWRITE | PAGE_NOCACHE);

            if (devCtx->PmcIpcBase != NULL) {
                mmioMapped = TRUE;
                KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                    "CrosTcssMux: Mapped PMC IPC from resource: "
                    "phys=0x%llX len=0x%lX virt=%p\n",
                    devCtx->PmcIpcPhysAddr.QuadPart,
                    (ULONG)devCtx->PmcIpcLength,
                    devCtx->PmcIpcBase));
            }
            break;
        }
    }

    /* Fall back to well-known PMC IPC address if no resource was assigned */
    if (!mmioMapped) {
        pmcPhys.QuadPart = PMC_IPC_BASE_DEFAULT;
        devCtx->PmcIpcPhysAddr = pmcPhys;
        devCtx->PmcIpcLength = 0x100;  /* Map 256 bytes for IPC registers */

        devCtx->PmcIpcBase = MmMapIoSpaceEx(
            pmcPhys,
            devCtx->PmcIpcLength,
            PAGE_READWRITE | PAGE_NOCACHE);

        if (devCtx->PmcIpcBase == NULL) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                "CrosTcssMux: CRITICAL — Failed to map PMC IPC MMIO at "
                "0x%llX\n", pmcPhys.QuadPart));
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
            "CrosTcssMux: Mapped PMC IPC from default address: "
            "phys=0x%llX virt=%p\n",
            pmcPhys.QuadPart, devCtx->PmcIpcBase));
    }

    /* ---- Step 2: Parse ACPI port mappings ---- */
    status = CrosTcss_ParseAcpiPortMappings(devCtx, Device);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "CrosTcssMux: ACPI port mapping parse returned 0x%08X "
            "(using defaults)\n", status));
        /* Non-fatal; defaults were set */
    }

    /* ---- Step 3: Register ACPI notifications ---- */
    status = CrosTcss_RegisterAcpiNotify(devCtx);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "CrosTcssMux: ACPI notification registration returned 0x%08X "
            "(watchdog will be used)\n", status));
        /* Non-fatal; watchdog timer provides fallback */
    }

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: Hardware preparation complete (%d ports)\n",
        devCtx->PortCount));

    return STATUS_SUCCESS;
}

/* =========================================================================
 * CrosTcss_EvtReleaseHardware — Cleanup on device removal or power down
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
CrosTcss_EvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PDEVICE_CONTEXT devCtx;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PAGED_CODE();

    devCtx = GetDeviceContext(Device);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: EvtReleaseHardware\n"));

    /* Stop the watchdog timer */
    WdfTimerStop(devCtx->WatchdogTimer, TRUE /* wait */);

    /* Unregister ACPI notifications */
    CrosTcss_UnregisterAcpiNotify(devCtx);

    /* Flush any pending work items */
    WdfWorkItemFlush(devCtx->EventWorkItem);

    /* Unmap PMC IPC MMIO */
    if (devCtx->PmcIpcBase != NULL) {
        MmUnmapIoSpace(devCtx->PmcIpcBase, devCtx->PmcIpcLength);
        devCtx->PmcIpcBase = NULL;
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
            "CrosTcssMux: PMC IPC MMIO unmapped\n"));
    }

    return STATUS_SUCCESS;
}

/* =========================================================================
 * CrosTcss_EvtD0Entry — Device entering D0 (working) state
 *
 * Called on:
 *   - Initial power-up
 *   - Resume from Modern Standby (S0ix)
 *   - Resume from Hibernate (S4)
 *
 * We invalidate all cached port states and replay the full mux programming
 * sequence since the PMC crossbar state is lost across power transitions.
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
CrosTcss_EvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PDEVICE_CONTEXT devCtx;
    LARGE_INTEGER delay;

    PAGED_CODE();

    devCtx = GetDeviceContext(Device);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: EvtD0Entry (previous state = %d)\n", PreviousState));

    devCtx->D0Active = TRUE;

    /* Invalidate all cached port states.
     * The PMC crossbar loses its configuration across power transitions,
     * so we must reprogram everything from scratch. */
    for (UINT8 i = 0; i < devCtx->PortCount; i++) {
        devCtx->Ports[i].PmcConfigured = FALSE;
        devCtx->Ports[i].MuxState = 0xFF;  /* Force fresh evaluation against EC */
        devCtx->Ports[i].LastModeData = 0;
        devCtx->Ports[i].LastPortMap = 0;

        if (PreviousState != WdfPowerDeviceD3Final) {
            /* Keep Connected flag as-is for resume; clear CableVDO to re-query */
            devCtx->Ports[i].CableVdo = 0;
            devCtx->Ports[i].TbtCableVdo = 0;
        }
    }

    /* Brief delay to let the EC stabilize after boot / system resume */
    delay.QuadPart = -500000LL;  /* 50ms */
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    /* Process all ports to reprogram the PMC crossbar */
    CrosTcss_ProcessAllPorts(devCtx);

    /* Start the watchdog timer (250ms interval) */
    WdfTimerStart(devCtx->WatchdogTimer, WDF_REL_TIMEOUT_IN_MS(250));

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: D0 entry complete — watchdog started\n"));

    return STATUS_SUCCESS;
}

/* =========================================================================
 * CrosTcss_EvtD0Exit — Device leaving D0 state (going to sleep/hibernate)
 *
 * Stop the watchdog timer and flush pending work items to ensure clean
 * teardown before the platform enters a low-power state.
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
CrosTcss_EvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PDEVICE_CONTEXT devCtx;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(TargetState);

    devCtx = GetDeviceContext(Device);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: EvtD0Exit (target state = %d)\n", TargetState));

    /* Mark device as leaving D0 to prevent new work from being queued */
    devCtx->D0Active = FALSE;

    /* Stop the watchdog timer synchronously */
    WdfTimerStop(devCtx->WatchdogTimer, TRUE /* wait for completion */);

    /* Flush any in-flight work items */
    WdfWorkItemFlush(devCtx->EventWorkItem);

    /* Reset the work item queued flag */
    InterlockedExchange(&devCtx->EventWorkItemQueued, 0);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: D0 exit complete — timer stopped, work flushed\n"));

    return STATUS_SUCCESS;
}
