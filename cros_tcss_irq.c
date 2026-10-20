/*
 * cros_tcss_irq.c — Interrupt & Event Handling
 *
 * Implements:
 *   1. ACPI Notify registration via GUID_ACPI_INTERFACE_STANDARD2
 *   2. MKBP event FIFO draining (EC_CMD_GET_NEXT_EVENT)
 *   3. Host Event B fallback checking (EC_CMD_HOST_EVENT_GET_B)
 *   4. Port update orchestration (DISCONNECT -> SAFE_MODE -> CONNECT/ALT_MODE)
 *   5. Watchdog fallback timer for systems without ACPI notification
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#include <ntddk.h>
#include <wdf.h>
#include <acpiioct.h>
#include <initguid.h>
#include <wdmguid.h>
#include "cros_tcss_hw.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, CrosTcss_RegisterAcpiNotify)
#pragma alloc_text(PAGE, CrosTcss_UnregisterAcpiNotify)
#pragma alloc_text(PAGE, CrosTcss_ProcessPortUpdate)
#pragma alloc_text(PAGE, CrosTcss_ProcessAllPorts)
#endif

/* =========================================================================
 * Internal: ACPI Notification Callback
 *
 * Called by ACPI.sys when the EC ACPI device sends Notify(0x80).
 * We cannot do much at this IRQL, so queue a WDFWORKITEM.
 * ========================================================================= */

static VOID
CrosTcss_AcpiNotifyCallback(
    _In_ PVOID Context,
    _In_ ULONG NotifyValue
    )
{
    PDEVICE_CONTEXT devCtx = (PDEVICE_CONTEXT)Context;

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: ACPI Notify received (value=0x%02X)\n", NotifyValue));

    if (NotifyValue != 0x80) {
        /* Not our event; only EC mux changes use Notify 0x80 */
        return;
    }

    /* Atomically mark work item as queued to prevent flooding */
    if (InterlockedCompareExchange(&devCtx->EventWorkItemQueued, 1, 0) == 0) {
        WdfWorkItemEnqueue(devCtx->EventWorkItem);
    }
}

/* =========================================================================
 * Internal: Event Work Item Callback
 *
 * Runs at PASSIVE_LEVEL. Drains the EC's MKBP event FIFO, checks
 * Host Event B for USB-C events, and processes any port changes.
 * ========================================================================= */

EVT_WDF_WORKITEM CrosTcss_EvtEventWorkItem;

VOID
CrosTcss_EvtEventWorkItem(
    _In_ WDFWORKITEM WorkItem
    )
{
    PDEVICE_CONTEXT devCtx;
    NTSTATUS status;
    BOOLEAN anyPortChanged = FALSE;

    PAGED_CODE();

    devCtx = GetDeviceContext(WdfWorkItemGetParentObject(WorkItem));

    if (!devCtx->D0Active) {
        InterlockedExchange(&devCtx->EventWorkItemQueued, 0);
        return;
    }

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: Event work item running — draining EC events\n"));

    /* ---- Phase 1: Drain MKBP Event FIFO ---- */
    {
        UINT8 eventData[64];
        UINT16 actualLen = 0;
        UINT32 drainCount = 0;
        const UINT32 maxDrain = 32;  /* Safety limit */

        while (drainCount < maxDrain) {
            RtlZeroMemory(eventData, sizeof(eventData));
            status = CrosTcss_EcSendCommand(
                EC_CMD_GET_NEXT_EVENT,
                0,     /* version */
                NULL,  /* no request data */
                0,
                eventData,
                sizeof(eventData),
                &actualLen);

            if (!NT_SUCCESS(status)) {
                /* EC returned error — FIFO is empty or command failed */
                break;
            }

            if (actualLen == 0) {
                break;
            }

            /* First byte is event type */
            UINT8 eventType = eventData[0];

            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "CrosTcssMux: MKBP event type=0x%02X\n", eventType));

            if (eventType == EC_MKBP_EVENT_PD_MCU) {
                anyPortChanged = TRUE;
            } else if (eventType == EC_MKBP_EVENT_NONE) {
                break;
            }

            drainCount++;
        }

        if (drainCount >= maxDrain) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                "CrosTcssMux: MKBP drain hit safety limit (%d events)\n",
                maxDrain));
        }
    }

    /* ---- Phase 2: Check Host Event B for USB-C events ---- */
    {
        UINT32 hostEventB = 0;
        UINT16 actualLen = 0;

        status = CrosTcss_EcSendCommand(
            EC_CMD_HOST_EVENT_GET_B,
            0,
            NULL, 0,
            &hostEventB, sizeof(hostEventB),
            &actualLen);

        if (NT_SUCCESS(status) && actualLen >= sizeof(UINT32)) {
            if (hostEventB & (EC_HOST_EVENT_PD_MCU | EC_HOST_EVENT_USB_MUX)) {
                KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                    "CrosTcssMux: Host Event B indicates USB-C change "
                    "(events=0x%08X)\n", hostEventB));
                anyPortChanged = TRUE;
            }
        }
    }

    /* ---- Phase 3: Update all ports if any change detected ---- */
    if (anyPortChanged) {
        CrosTcss_ProcessAllPorts(devCtx);
    }

    /* Clear the queued flag so we can accept new events */
    InterlockedExchange(&devCtx->EventWorkItemQueued, 0);
}

/* =========================================================================
 * Internal: Watchdog Timer Callback
 *
 * Runs every 2000ms as a sanity check. Polls Host Event B to catch
 * USB-C changes that might be missed if ACPI notification routing is
 * absent or broken in the DSDT.
 * ========================================================================= */

EVT_WDF_TIMER CrosTcss_EvtWatchdogTimer;

VOID
CrosTcss_EvtWatchdogTimer(
    _In_ WDFTIMER Timer
    )
{
    PDEVICE_CONTEXT devCtx;

    devCtx = GetDeviceContext(WdfTimerGetParentObject(Timer));

    if (!devCtx->D0Active) {
        return;
    }

    /* Trigger the event work item to poll for changes */
    if (InterlockedCompareExchange(&devCtx->EventWorkItemQueued, 1, 0) == 0) {
        WdfWorkItemEnqueue(devCtx->EventWorkItem);
    }
}

/* =========================================================================
 * CrosTcss_ProcessPortUpdate — Handle a mux state change on a single port
 *
 * Implements the full PMC crossbar programming sequence:
 *   1. Query EC for current mux state
 *   2. If disconnected: send PMC_USB_DISCONNECT
 *   3. If safe mode: send PMC_USB_SAFE_MODE
 *   4. If connected with mode: CONNECT -> SAFE_MODE -> ALT_MODE/CONNECT
 *   5. Handle DP HPD separately if only HPD changed
 *
 * This function must be called at PASSIVE_LEVEL.
 * ========================================================================= */

_Use_decl_annotations_
VOID
CrosTcss_ProcessPortUpdate(
    PDEVICE_CONTEXT DevCtx,
    UINT8 PortIndex
    )
{
    NTSTATUS status;
    PTCSS_PORT_STATE port;
    EC_USB_PD_MUX_INFO_REQUEST muxReq;
    EC_USB_PD_MUX_INFO_RESPONSE muxResp;
    UINT8 newMuxState;
    UINT8 oldMuxState;
    UINT32 modeData = 0;
    UINT32 portMap = 0;
    LARGE_INTEGER delay;

    PAGED_CODE();

    if (PortIndex >= DevCtx->PortCount) {
        return;
    }

    port = &DevCtx->Ports[PortIndex];

    /* Query current mux state from EC */
    muxReq.Port = PortIndex;
    status = CrosTcss_EcSendCommand(
        EC_CMD_USB_PD_MUX_INFO,
        0,
        &muxReq, sizeof(muxReq),
        &muxResp, sizeof(muxResp),
        NULL);

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "CrosTcssMux: Failed to query mux info for port %d (0x%08X)\n",
            PortIndex, status));
        return;
    }

    newMuxState = muxResp.Flags;
    oldMuxState = port->MuxState;

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: Port %d mux: old=0x%02X new=0x%02X\n",
        PortIndex, oldMuxState, newMuxState));

    /* Short-circuit if nothing changed (including HPD bits) */
    if (newMuxState == oldMuxState && port->PmcConfigured) {
        return;
    }

    /* Update cached state */
    port->MuxState = newMuxState;

    /* ---- CASE 1: Complete disconnect (all flags cleared) ---- */
    if (newMuxState == USB_PD_MUX_NONE) {
        if (port->Connected || port->PmcConfigured) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "CrosTcssMux: Port %d disconnected — sending PMC DISCONNECT\n",
                PortIndex));

            status = CrosTcss_BuildModeData(DevCtx, PortIndex, 0,
                                            &modeData, &portMap);
            if (NT_SUCCESS(status)) {
                CrosTcss_PmcSendIpcCommand(DevCtx, PMC_USB_DISCONNECT,
                                           modeData, portMap, 0, 0, NULL);
            }

            port->Connected = FALSE;
            port->PmcConfigured = FALSE;
            port->CableVdo = 0;
            port->TbtCableVdo = 0;
            port->LastModeData = 0;
            port->LastPortMap = 0;
        }
        return;
    }

    /* ---- CASE 2: Safe mode only ---- */
    if (newMuxState == USB_PD_MUX_SAFE_MODE) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
            "CrosTcssMux: Port %d entering safe mode\n", PortIndex));

        status = CrosTcss_BuildModeData(DevCtx, PortIndex,
                                        USB_PD_MUX_SAFE_MODE,
                                        &modeData, &portMap);
        if (NT_SUCCESS(status)) {
            CrosTcss_PmcSendIpcCommand(DevCtx, PMC_USB_SAFE_MODE,
                                       modeData, portMap, 0, 0, NULL);
        }

        port->Connected = TRUE;
        port->PmcConfigured = TRUE;
        return;
    }

    /* ---- CASE 3: HPD-only change (DP mode already established) ---- */
    if ((oldMuxState & ~(USB_PD_MUX_HPD_IRQ | USB_PD_MUX_HPD_LVL)) ==
        (newMuxState & ~(USB_PD_MUX_HPD_IRQ | USB_PD_MUX_HPD_LVL)) &&
        port->PmcConfigured &&
        (newMuxState & USB_PD_MUX_DP_ENABLED)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
            "CrosTcssMux: Port %d HPD-only update\n", PortIndex));

        status = CrosTcss_BuildModeData(DevCtx, PortIndex, newMuxState,
                                        &modeData, &portMap);
        if (NT_SUCCESS(status)) {
            CrosTcss_PmcSendIpcCommand(DevCtx, PMC_USB_DP_HPD,
                                       modeData, portMap, 0, 0, NULL);
        }
        return;
    }

    /* ---- CASE 4: Full connection with mode (USB/DP/TBT/USB4) ----
     *
     * Programming sequence:
     *   1. CONNECT  — Establish the physical link
     *   2. SAFE_MODE — Transition crossbar to a known state
     *   3. ALT_MODE  — Program the final mode (or just CONNECT for USB-only)
     *
     * A short delay between steps ensures the PMC has time to
     * reconfigure the crossbar.
     */

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: Port %d programming full sequence (mux=0x%02X)\n",
        PortIndex, newMuxState));

    /* Step 1: CONNECT */
    status = CrosTcss_BuildModeData(DevCtx, PortIndex,
                                    USB_PD_MUX_USB_ENABLED,
                                    &modeData, &portMap);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "CrosTcssMux: Failed to build CONNECT data for port %d\n",
            PortIndex));
        return;
    }

    status = CrosTcss_PmcSendIpcCommand(DevCtx, PMC_USB_CONNECT,
                                        modeData, portMap, 0, 0, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "CrosTcssMux: PMC CONNECT failed for port %d (0x%08X)\n",
            PortIndex, status));
        return;
    }

    port->Connected = TRUE;

    /* Brief delay for crossbar stabilization */
    delay.QuadPart = -10000LL;  /* 1ms in 100ns units */
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    /* Step 2: SAFE_MODE */
    status = CrosTcss_BuildModeData(DevCtx, PortIndex,
                                    USB_PD_MUX_SAFE_MODE,
                                    &modeData, &portMap);
    if (NT_SUCCESS(status)) {
        CrosTcss_PmcSendIpcCommand(DevCtx, PMC_USB_SAFE_MODE,
                                   modeData, portMap, 0, 0, NULL);
    }

    delay.QuadPart = -10000LL;  /* 1ms */
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    /* Step 3: Final mode */
    status = CrosTcss_BuildModeData(DevCtx, PortIndex, newMuxState,
                                    &modeData, &portMap);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "CrosTcssMux: Failed to build ALT_MODE data for port %d\n",
            PortIndex));
        return;
    }

    /* Determine the correct PMC sub-command for the final mode */
    {
        UINT8 subCmd;
        UINT8 muxCore = newMuxState &
            (USB_PD_MUX_USB_ENABLED | USB_PD_MUX_DP_ENABLED |
             USB_PD_MUX_TBT_COMPAT_ENABLED | USB_PD_MUX_USB4_ENABLED);

        if (muxCore & (USB_PD_MUX_DP_ENABLED | USB_PD_MUX_TBT_COMPAT_ENABLED |
                       USB_PD_MUX_USB4_ENABLED)) {
            subCmd = PMC_USB_ALT_MODE;
        } else {
            subCmd = PMC_USB_CONNECT;
        }

        status = CrosTcss_PmcSendIpcCommand(DevCtx, subCmd,
                                            modeData, portMap, 0, 0, NULL);
        if (NT_SUCCESS(status)) {
            port->PmcConfigured = TRUE;
            port->LastModeData = modeData;
            port->LastPortMap = portMap;
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "CrosTcssMux: Port %d configured successfully\n", PortIndex));
        } else {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                "CrosTcssMux: PMC mode command failed for port %d (0x%08X)\n",
                PortIndex, status));
        }
    }

    /* Step 4: If DP mode with HPD, send the HPD signal separately */
    if ((newMuxState & USB_PD_MUX_DP_ENABLED) &&
        (newMuxState & (USB_PD_MUX_HPD_LVL | USB_PD_MUX_HPD_IRQ))) {
        delay.QuadPart = -10000LL;  /* 1ms */
        KeDelayExecutionThread(KernelMode, FALSE, &delay);

        CrosTcss_PmcSendIpcCommand(DevCtx, PMC_USB_DP_HPD,
                                   modeData, portMap, 0, 0, NULL);
    }
}

/* =========================================================================
 * CrosTcss_ProcessAllPorts — Iterate all discovered ports and update each
 * ========================================================================= */

_Use_decl_annotations_
VOID
CrosTcss_ProcessAllPorts(
    PDEVICE_CONTEXT DevCtx
    )
{
    PAGED_CODE();

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: Processing all %d ports\n", DevCtx->PortCount));

    for (UINT8 i = 0; i < DevCtx->PortCount; i++) {
        CrosTcss_ProcessPortUpdate(DevCtx, i);
    }
}

/* =========================================================================
 * CrosTcss_RegisterAcpiNotify — Register for ACPI Notify(0x80) from EC
 *
 * Uses GUID_ACPI_INTERFACE_STANDARD2 to register our notification callback
 * on the underlying ACPI PDO. This allows us to receive events from the
 * ChromeOS EC without requiring an interrupt resource.
 * ========================================================================= */


_Use_decl_annotations_
NTSTATUS
CrosTcss_RegisterAcpiNotify(
    PDEVICE_CONTEXT DevCtx
    )
{
    NTSTATUS status;
    WDFDEVICE device;

    PAGED_CODE();

    device = DevCtx->Device;

    /*
     * Register for ACPI notifications via a WMI-style approach:
     * Send IOCTL_ACPI_REGISTER_NOTIFY to the ACPI PDO.
     *
     * Alternatively, we can use the ACPI interface directly. For maximum
     * compatibility with ChromeOS EC DSDT layouts, we use a simplified
     * approach that registers via the ACPI evaluation interface.
     *
     * The actual notification registration is done by evaluating a
     * RegisterForDeviceNotifications method if the ACPI interface
     * is available, otherwise we fall back to the watchdog timer.
     */

    /* First, try to get the ACPI notification interface */
    {
        ACPI_INTERFACE_STANDARD2 acpiInterface;

        RtlZeroMemory(&acpiInterface, sizeof(acpiInterface));

        /*
         * Query GUID_ACPI_INTERFACE_STANDARD2 from the underlying PDO.
         * This gives us RegisterForDeviceNotifications /
         * UnregisterForDeviceNotifications function pointers.
         */
        status = WdfFdoQueryForInterface(
            device,
            &GUID_ACPI_INTERFACE_STANDARD2,
            (PINTERFACE)&acpiInterface,
            sizeof(ACPI_INTERFACE_STANDARD2),
            1,   /* version */
            NULL  /* interface-specific data */
            );

        if (NT_SUCCESS(status) &&
            acpiInterface.RegisterForDeviceNotifications != NULL) {

            status = acpiInterface.RegisterForDeviceNotifications(
                acpiInterface.Context,
                CrosTcss_AcpiNotifyCallback,
                DevCtx);

            if (NT_SUCCESS(status)) {
                DevCtx->AcpiNotifyRegistered = TRUE;
                DevCtx->AcpiNotifyHandle = acpiInterface.Context;
                KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                    "CrosTcssMux: ACPI notification registered successfully\n"));
                return STATUS_SUCCESS;
            }

            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                "CrosTcssMux: ACPI RegisterForDeviceNotifications failed "
                "(0x%08X)\n", status));
        } else {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                "CrosTcssMux: ACPI interface not available (0x%08X) — "
                "relying on watchdog timer\n", status));
        }
    }

    /* Fall back to watchdog-only mode */
    DevCtx->AcpiNotifyRegistered = FALSE;
    return STATUS_SUCCESS;  /* Not fatal; watchdog will handle events */
}

/* =========================================================================
 * CrosTcss_UnregisterAcpiNotify — Remove the ACPI notification callback
 * ========================================================================= */

_Use_decl_annotations_
VOID
CrosTcss_UnregisterAcpiNotify(
    PDEVICE_CONTEXT DevCtx
    )
{
    NTSTATUS status;

    PAGED_CODE();

    if (!DevCtx->AcpiNotifyRegistered) {
        return;
    }

    /* Re-query the interface to get UnregisterForDeviceNotifications */
    {
        ACPI_INTERFACE_STANDARD2 acpiInterface;

        RtlZeroMemory(&acpiInterface, sizeof(acpiInterface));

        status = WdfFdoQueryForInterface(
            DevCtx->Device,
            &GUID_ACPI_INTERFACE_STANDARD2,
            (PINTERFACE)&acpiInterface,
            sizeof(ACPI_INTERFACE_STANDARD2),
            1,
            NULL);

        if (NT_SUCCESS(status) &&
            acpiInterface.UnregisterForDeviceNotifications != NULL) {

            acpiInterface.UnregisterForDeviceNotifications(
                acpiInterface.Context);

            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "CrosTcssMux: ACPI notification unregistered\n"));
        }
    }

    DevCtx->AcpiNotifyRegistered = FALSE;
    DevCtx->AcpiNotifyHandle = NULL;
}
