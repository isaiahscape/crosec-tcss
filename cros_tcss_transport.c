/*
 * cros_tcss_transport.c — LPC Transport & ACPI Parsers
 *
 * Implements:
 *   1. Direct I/O port LPC v3 protocol for ChromeOS EC host commands
 *   2. Intel PMC IPC MMIO command submission
 *   3. ACPI _DSD evaluation for USB port-mapping discovery
 *   4. SOP' cable VDO parsing and PMC mode_data bitpacking
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <acpiioct.h>
#include <initguid.h>
#include "cros_tcss_hw.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, CrosTcss_EcSendCommand)
#pragma alloc_text(PAGE, CrosTcss_PmcSendIpcCommand)
#pragma alloc_text(PAGE, CrosTcss_ParseAcpiPortMappings)
#pragma alloc_text(PAGE, CrosTcss_BuildModeData)
#endif

/* =========================================================================
 * Internal Helpers
 * ========================================================================= */

/*
 * CrosTcss_ComputeChecksum - Calculate 8-bit checksum for EC protocol v3
 *
 * The checksum is computed such that the sum of all bytes in the
 * header + data region equals zero (mod 256).
 */
static UINT8
CrosTcss_ComputeChecksum(
    _In_reads_bytes_(Length) const UINT8* Data,
    _In_ UINT32 Length
    )
{
    UINT8 sum = 0;
    for (UINT32 i = 0; i < Length; i++) {
        sum += Data[i];
    }
    return (UINT8)(0 - sum);
}

/*
 * CrosTcss_WaitEcReady - Poll EC status register until not busy
 */
static NTSTATUS
CrosTcss_WaitEcReady(VOID)
{
    LARGE_INTEGER delay;
    UINT32 elapsed = 0;

    while (elapsed < EC_CMD_TIMEOUT_US) {
        UINT8 status = READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)EC_LPC_ADDR_HOST_CMD);
        if (!(status & EC_LPC_STATUS_PROCESSING)) {
            return STATUS_SUCCESS;
        }
        /* Stall for the poll interval */
        delay.QuadPart = -((LONGLONG)EC_CMD_POLL_INTERVAL_US * 10);  /* 100ns units */
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        elapsed += EC_CMD_POLL_INTERVAL_US;
    }

    return STATUS_IO_TIMEOUT;
}

/* =========================================================================
 * CrosTcss_EcSendCommand — Send a host command to the ChromeOS EC via LPC
 *
 * Uses EC host command protocol v3 over LPC I/O ports:
 *   - Request header + data written to ports 0x800..0x8FF
 *   - Command triggered by writing 0x00 to port 0x204
 *   - Response header + data read back from 0x800..0x8FF
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
CrosTcss_EcSendCommand(
    UINT16 Command,
    UINT8 CommandVersion,
    const VOID* RequestData,
    UINT16 RequestSize,
    VOID* ResponseData,
    UINT16 ResponseSize,
    UINT16* ActualResponseSize
    )
{
    NTSTATUS status;
    EC_HOST_REQUEST request;
    EC_HOST_RESPONSE response;
    UINT8 checksum;

    PAGED_CODE();

    /* Validate sizes fit in the LPC parameter window */
    if ((sizeof(EC_HOST_REQUEST) + RequestSize) > EC_LPC_HOST_PARAM_SIZE ||
        (sizeof(EC_HOST_RESPONSE) + ResponseSize) > EC_LPC_HOST_PARAM_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Wait for EC to be ready before sending */
    status = CrosTcss_WaitEcReady();
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "CrosTcssMux: EC not ready (timeout waiting for idle)\n"));
        return status;
    }

    /* Build the v3 request header */
    RtlZeroMemory(&request, sizeof(request));
    request.StructVersion = EC_HOST_REQUEST_VERSION;
    request.Command = Command;
    request.CommandVersion = CommandVersion;
    request.DataLen = RequestSize;
    request.Checksum = 0;

    /* Compute checksum: header checksum byte is initially 0,
     * set it so that (sum of header bytes + sum of data bytes) == 0 mod 256. */
    checksum = 0;
    {
        const UINT8* hdrBytes = (const UINT8*)&request;
        for (UINT32 i = 0; i < sizeof(EC_HOST_REQUEST); i++) {
            checksum += hdrBytes[i];
        }
        if (RequestData != NULL && RequestSize > 0) {
            const UINT8* dataBytes = (const UINT8*)RequestData;
            for (UINT32 i = 0; i < RequestSize; i++) {
                checksum += dataBytes[i];
            }
        }
    }
    request.Checksum = (UINT8)(0 - checksum);

    /* Write request header to LPC memory window at 0x800 */
    {
        const UINT8* hdrBytes = (const UINT8*)&request;
        for (UINT32 i = 0; i < sizeof(EC_HOST_REQUEST); i++) {
            WRITE_PORT_UCHAR(
                (PUCHAR)(ULONG_PTR)(EC_LPC_ADDR_HOST_PARAM + i),
                hdrBytes[i]);
        }
    }

    /* Write request data immediately after header */
    if (RequestData != NULL && RequestSize > 0) {
        const UINT8* dataBytes = (const UINT8*)RequestData;
        for (UINT32 i = 0; i < RequestSize; i++) {
            WRITE_PORT_UCHAR(
                (PUCHAR)(ULONG_PTR)(EC_LPC_ADDR_HOST_PARAM + sizeof(EC_HOST_REQUEST) + i),
                dataBytes[i]);
        }
    }

    /* Trigger the command by writing 0x00 to the host command port */
    WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)EC_LPC_ADDR_HOST_CMD, 0x00);

    /* Wait for EC to process the command */
    status = CrosTcss_WaitEcReady();
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "CrosTcssMux: EC command 0x%04X timed out\n", Command));
        return status;
    }

    /* Read response header from LPC memory window */
    {
        UINT8* respBytes = (UINT8*)&response;
        for (UINT32 i = 0; i < sizeof(EC_HOST_RESPONSE); i++) {
            respBytes[i] = READ_PORT_UCHAR(
                (PUCHAR)(ULONG_PTR)(EC_LPC_ADDR_HOST_PARAM + i));
        }
    }

    /* Validate response */
    if (response.StructVersion != EC_HOST_RESPONSE_VERSION) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "CrosTcssMux: Bad response version %d for cmd 0x%04X\n",
            response.StructVersion, Command));
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    if (response.ResultCode != EC_RES_SUCCESS) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "CrosTcssMux: EC cmd 0x%04X failed with result %d\n",
            Command, response.ResultCode));
        return STATUS_UNSUCCESSFUL;
    }

    /* Validate response checksum */
    {
        UINT8 respChecksum = 0;
        const UINT8* respBytes = (const UINT8*)&response;
        for (UINT32 i = 0; i < sizeof(EC_HOST_RESPONSE); i++) {
            respChecksum += respBytes[i];
        }
        /* Read and checksum the response data */
        if (response.DataLen > 0) {
            if (response.DataLen > (EC_LPC_HOST_PARAM_SIZE - sizeof(EC_HOST_RESPONSE))) {
                KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                    "CrosTcssMux: Response DataLen (%u) exceeds LPC param window\n",
                    response.DataLen));
                return STATUS_DEVICE_PROTOCOL_ERROR;
            }
            UINT16 copyLen = (response.DataLen < ResponseSize) ? response.DataLen : ResponseSize;
            UINT8* outBytes = (UINT8*)ResponseData;
            for (UINT32 i = 0; i < response.DataLen; i++) {
                UINT8 b = READ_PORT_UCHAR(
                    (PUCHAR)(ULONG_PTR)(EC_LPC_ADDR_HOST_PARAM + sizeof(EC_HOST_RESPONSE) + i));
                respChecksum += b;
                if (outBytes != NULL && i < copyLen) {
                    outBytes[i] = b;
                }
            }
        }
        if (respChecksum != 0) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                "CrosTcssMux: Response checksum mismatch for cmd 0x%04X\n", Command));
            return STATUS_CRC_ERROR;
        }
    }

    if (ActualResponseSize != NULL) {
        *ActualResponseSize = response.DataLen;
    }

    return STATUS_SUCCESS;
}

/* =========================================================================
 * CrosTcss_PmcSendIpcCommand — Send an IPC command to the Intel PMC
 *
 * Uses memory-mapped registers at PWRMBASE + 0x2000.
 * Write buffers WBUF0..WBUF3, then write the command register to trigger.
 * Poll the status register until BUSY clears.
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
CrosTcss_PmcSendIpcCommand(
    PDEVICE_CONTEXT DevCtx,
    UINT8 SubCommand,
    UINT32 Wbuf0,
    UINT32 Wbuf1,
    UINT32 Wbuf2,
    UINT32 Wbuf3,
    UINT32* StatusData
    )
{
    UINT32 cmdVal;
    UINT32 stsVal;
    UINT32 elapsed = 0;
    LARGE_INTEGER delay;

    PAGED_CODE();

    if (DevCtx->PmcIpcBase == NULL) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "CrosTcssMux: PMC IPC base not mapped\n"));
        return STATUS_DEVICE_NOT_READY;
    }

    /* Wait for any previous IPC command to complete */
    elapsed = 0;
    while (elapsed < PMC_IPC_TIMEOUT_US) {
        stsVal = READ_REGISTER_ULONG(
            (PULONG)((PUCHAR)DevCtx->PmcIpcBase + PMC_IPC_STATUS_OFFSET));
        if (!(stsVal & PMC_IPC_STS_BUSY)) {
            break;
        }
        delay.QuadPart = -((LONGLONG)PMC_IPC_POLL_INTERVAL_US * 10);
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        elapsed += PMC_IPC_POLL_INTERVAL_US;
    }
    if (elapsed >= PMC_IPC_TIMEOUT_US) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
            "CrosTcssMux: PMC IPC busy timeout (pre-command)\n"));
        return STATUS_IO_TIMEOUT;
    }

    /* Write data buffers */
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)DevCtx->PmcIpcBase + PMC_IPC_WBUF0_OFFSET), Wbuf0);
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)DevCtx->PmcIpcBase + PMC_IPC_WBUF1_OFFSET), Wbuf1);
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)DevCtx->PmcIpcBase + PMC_IPC_WBUF2_OFFSET), Wbuf2);
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)DevCtx->PmcIpcBase + PMC_IPC_WBUF3_OFFSET), Wbuf3);

    /* Build and write the command register to trigger IPC */
    cmdVal = PMC_IPC_BUILD_CMD(PMC_IPC_USBC_CMD_ID, SubCommand, 4);
    WRITE_REGISTER_ULONG(
        (PULONG)((PUCHAR)DevCtx->PmcIpcBase + PMC_IPC_CMD_OFFSET), cmdVal);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: PMC IPC cmd=0x%08X wbuf={0x%08X, 0x%08X, 0x%08X, 0x%08X}\n",
        cmdVal, Wbuf0, Wbuf1, Wbuf2, Wbuf3));

    /* Poll for completion */
    elapsed = 0;
    while (elapsed < PMC_IPC_TIMEOUT_US) {
        stsVal = READ_REGISTER_ULONG(
            (PULONG)((PUCHAR)DevCtx->PmcIpcBase + PMC_IPC_STATUS_OFFSET));
        if (!(stsVal & PMC_IPC_STS_BUSY)) {
            /* Check for error */
            if (stsVal & PMC_IPC_STS_ERROR) {
                KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                    "CrosTcssMux: PMC IPC error, status=0x%08X\n", stsVal));
                return STATUS_DEVICE_HARDWARE_ERROR;
            }
            /* Read status data if requested */
            if (StatusData != NULL) {
                *StatusData = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)DevCtx->PmcIpcBase + PMC_IPC_STSDATA_OFFSET));
            }
            return STATUS_SUCCESS;
        }
        delay.QuadPart = -((LONGLONG)PMC_IPC_POLL_INTERVAL_US * 10);
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        elapsed += PMC_IPC_POLL_INTERVAL_US;
    }

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
        "CrosTcssMux: PMC IPC timeout (subcmd=0x%02X)\n", SubCommand));
    return STATUS_IO_TIMEOUT;
}

/* =========================================================================
 * ACPI _DSD Evaluation Helpers
 *
 * We evaluate _DSD on child device nodes (CON0, CON1, ...) under the
 * PMC PMCU ACPI path to discover physical USB2 and USB3 port numbers.
 * ========================================================================= */

/*
 * CrosTcss_EvalAcpiMethod - Evaluate a simple ACPI method and return the
 * output buffer. Caller must free the returned buffer.
 */
static NTSTATUS
CrosTcss_EvalAcpiMethod(
    _In_ WDFDEVICE Device,
    _In_ PCSTR MethodPath,
    _Out_ PACPI_EVAL_OUTPUT_BUFFER* OutputBuffer,
    _Out_ ULONG* OutputSize
    )
{
    NTSTATUS status;
    WDFIOTARGET ioTarget;
    WDF_MEMORY_DESCRIPTOR inputDesc;
    WDF_MEMORY_DESCRIPTOR outputDesc;
    ACPI_EVAL_INPUT_BUFFER_EX inputBuffer;
    ULONG outBufSize = 4096;
    PACPI_EVAL_OUTPUT_BUFFER outBuf;

    ioTarget = WdfDeviceGetIoTarget(Device);

    /* Build the input for IOCTL_ACPI_EVAL_METHOD_EX */
    RtlZeroMemory(&inputBuffer, sizeof(inputBuffer));
    inputBuffer.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE_EX;
    status = RtlStringCbCopyA(
        inputBuffer.MethodName,
        sizeof(inputBuffer.MethodName),
        MethodPath);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Allocate output buffer */
    outBuf = (PACPI_EVAL_OUTPUT_BUFFER)ExAllocatePool2(
        POOL_FLAG_PAGED, outBufSize, CROS_TCSS_POOL_TAG);
    if (outBuf == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDesc, &inputBuffer, sizeof(inputBuffer));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputDesc, outBuf, outBufSize);

    status = WdfIoTargetSendIoctlSynchronously(
        ioTarget,
        NULL,
        IOCTL_ACPI_EVAL_METHOD_EX,
        &inputDesc,
        &outputDesc,
        NULL,
        NULL);

    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(outBuf, CROS_TCSS_POOL_TAG);
        return status;
    }

    *OutputBuffer = outBuf;
    *OutputSize = outBufSize;
    return STATUS_SUCCESS;
}

/*
 * CrosTcss_FindDsdIntegerProperty - Search an ACPI _DSD output for a named
 * integer property. Returns STATUS_SUCCESS if found and writes the value.
 *
 * _DSD returns a package of { UUID, Package { { "name", value }, ... } }
 * We walk the ACPI_METHOD_ARGUMENT list looking for string/integer pairs.
 */
static NTSTATUS
CrosTcss_FindDsdIntegerProperty(
    _In_ PACPI_EVAL_OUTPUT_BUFFER DsdOutput,
    _In_ PCSTR PropertyName,
    _Out_ UINT32* Value
    )
{
    PACPI_METHOD_ARGUMENT arg;
    ULONG remaining;
    BOOLEAN uuidMatched = FALSE;

    if (DsdOutput->Count < 2) {
        return STATUS_NOT_FOUND;
    }

    /* Walk arguments: first should be a buffer (UUID), second a package */
    arg = &DsdOutput->Argument[0];
    remaining = DsdOutput->Count;

    while (remaining > 0) {
        /* Look for UUID buffer argument */
        if (arg->Type == ACPI_METHOD_ARGUMENT_BUFFER &&
            arg->DataLength >= sizeof(GUID)) {
            GUID* guid = (GUID*)arg->Data;
            if (IsEqualGUID(guid, &ACPI_DSD_DEVICE_PROPERTIES_UUID)) {
                uuidMatched = TRUE;
            }
        }

        /* If UUID matched, look for packages containing name-value pairs */
        if (uuidMatched && arg->Type == ACPI_METHOD_ARGUMENT_PACKAGE) {
            /* This is the properties package — iterate its children */
            PACPI_METHOD_ARGUMENT inner = (PACPI_METHOD_ARGUMENT)arg->Data;
            ULONG innerRemaining = arg->DataLength;

            while (innerRemaining > sizeof(ACPI_METHOD_ARGUMENT)) {
                /* Each property is a sub-package { String, Integer } */
                if (inner->Type == ACPI_METHOD_ARGUMENT_PACKAGE &&
                    inner->DataLength >= sizeof(ACPI_METHOD_ARGUMENT)) {
                    PACPI_METHOD_ARGUMENT nameArg =
                        (PACPI_METHOD_ARGUMENT)inner->Data;
                    ULONG nameArgLen = ACPI_METHOD_ARGUMENT_LENGTH(nameArg->DataLength);
                    if (nameArg->Type == ACPI_METHOD_ARGUMENT_STRING &&
                        nameArgLen < inner->DataLength) {
                        size_t propLen = strlen(PropertyName);
                        /* Compare the property name safely within its length */
                        if (nameArg->DataLength >= propLen &&
                            _strnicmp((PCSTR)nameArg->Data, PropertyName, propLen) == 0 &&
                            (nameArg->DataLength == propLen || nameArg->Data[propLen] == '\0')) {
                            /* Next sibling should be the integer value */
                            if (nameArgLen + sizeof(ACPI_METHOD_ARGUMENT) <= inner->DataLength) {
                                PACPI_METHOD_ARGUMENT valArg =
                                    ACPI_METHOD_NEXT_ARGUMENT(nameArg);
                                if (valArg->Type == ACPI_METHOD_ARGUMENT_INTEGER) {
                                    *Value = valArg->Argument;
                                    return STATUS_SUCCESS;
                                }
                            }
                        }
                    }
                }

                /* Advance to next sibling argument */
                ULONG argSize = ACPI_METHOD_ARGUMENT_LENGTH(inner->DataLength);
                if (argSize == 0 || argSize > innerRemaining) {
                    break;
                }
                innerRemaining -= argSize;
                inner = (PACPI_METHOD_ARGUMENT)((PUCHAR)inner + argSize);
            }
        }

        /* Advance to next top-level argument */
        ULONG argSize = ACPI_METHOD_ARGUMENT_LENGTH(arg->DataLength);
        if (argSize == 0 || argSize > (ULONG)((PUCHAR)DsdOutput +
            DsdOutput->Length - (PUCHAR)arg)) {
            break;
        }
        remaining--;
        arg = (PACPI_METHOD_ARGUMENT)((PUCHAR)arg + argSize);
    }

    return STATUS_NOT_FOUND;
}

/* =========================================================================
 * CrosTcss_ParseAcpiPortMappings — Discover USB port numbers from ACPI
 *
 * Walks child nodes CON0..CON3 under the PMC PMCU path, evaluating _DSD
 * to extract usb2-port-number and usb3-port-number for each connector.
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
CrosTcss_ParseAcpiPortMappings(
    PDEVICE_CONTEXT DevCtx,
    WDFDEVICE Device
    )
{
    NTSTATUS status;
    UINT8 portsFound = 0;
    CHAR connPath[128];

    PAGED_CODE();

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: Parsing ACPI _DSD port mappings\n"));

    for (UINT8 i = 0; i < CROS_TCSS_MAX_PORTS; i++) {
        PACPI_EVAL_OUTPUT_BUFFER dsdOutput = NULL;
        ULONG dsdSize = 0;
        UINT32 usb2Port = 0;
        UINT32 usb3Port = 0;
        BOOLEAN foundUsb2 = FALSE;
        BOOLEAN foundUsb3 = FALSE;

        /* Try primary path: \_SB.PCI0.PMC.PMCU.CONx._DSD */
        status = RtlStringCbPrintfA(connPath, sizeof(connPath),
            "%s.CON%d._DSD", ACPI_PMC_PATH_PRIMARY, i);
        if (!NT_SUCCESS(status)) {
            continue;
        }

        status = CrosTcss_EvalAcpiMethod(Device, connPath, &dsdOutput, &dsdSize);
        if (!NT_SUCCESS(status)) {
            /* Fallback path: \_SB.PC00.PMC.PMCU.CONx._DSD */
            status = RtlStringCbPrintfA(connPath, sizeof(connPath),
                "%s.CON%d._DSD", ACPI_PMC_PATH_FALLBACK, i);
            if (NT_SUCCESS(status)) {
                status = CrosTcss_EvalAcpiMethod(
                    Device, connPath, &dsdOutput, &dsdSize);
            }
        }

        if (!NT_SUCCESS(status) || dsdOutput == NULL) {
            /* No more connectors found */
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "CrosTcssMux: No _DSD for CON%d (status=0x%08X), stopping\n",
                i, status));
            break;
        }

        /* Extract usb2-port-number */
        if (NT_SUCCESS(CrosTcss_FindDsdIntegerProperty(
                dsdOutput, ACPI_DSD_USB2_PORT_NUMBER, &usb2Port))) {
            foundUsb2 = TRUE;
        }

        /* Extract usb3-port-number */
        if (NT_SUCCESS(CrosTcss_FindDsdIntegerProperty(
                dsdOutput, ACPI_DSD_USB3_PORT_NUMBER, &usb3Port))) {
            foundUsb3 = TRUE;
        }

        ExFreePoolWithTag(dsdOutput, CROS_TCSS_POOL_TAG);

        if (foundUsb2 || foundUsb3) {
            DevCtx->Ports[i].PortIndex = i;
            DevCtx->Ports[i].USB2PortNumber = foundUsb2 ? (UINT8)usb2Port : 0;
            DevCtx->Ports[i].USB3PortNumber = foundUsb3 ? (UINT8)usb3Port : 0;
            DevCtx->Ports[i].Connected = FALSE;
            DevCtx->Ports[i].PmcConfigured = FALSE;

            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "CrosTcssMux: Port %d: USB2=%d, USB3=%d\n",
                i, DevCtx->Ports[i].USB2PortNumber,
                DevCtx->Ports[i].USB3PortNumber));

            portsFound++;
        }
    }

    DevCtx->PortCount = portsFound;

    if (portsFound == 0) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "CrosTcssMux: WARNING — No USB-C port mappings found in ACPI!\n"
            "  Falling back to default 2-port mapping (USB2=1,2 USB3=1,2)\n"));
        /* Provide sensible defaults for a 2-port Chromebook */
        DevCtx->PortCount = 2;
        for (UINT8 i = 0; i < 2; i++) {
            DevCtx->Ports[i].PortIndex = i;
            DevCtx->Ports[i].USB2PortNumber = (UINT8)(i + 1);
            DevCtx->Ports[i].USB3PortNumber = (UINT8)(i + 1);
            DevCtx->Ports[i].Connected = FALSE;
            DevCtx->Ports[i].PmcConfigured = FALSE;
        }
    }

    return STATUS_SUCCESS;
}

/* =========================================================================
 * CrosTcss_QueryCableVdo — Fetch SOP' cable VDO via TYPEC_DISCOVERY
 *
 * Queries the EC for cable plug discovery data (SOP') and extracts the
 * Product Type VDO1 and any TBT3 mode VDO.
 * ========================================================================= */

static NTSTATUS
CrosTcss_QueryCableVdo(
    _In_ UINT8 PortIndex,
    _Out_ UINT32* CableVdo,
    _Out_ UINT32* TbtCableVdo
    )
{
    NTSTATUS status;
    EC_TYPEC_DISCOVERY_REQUEST req;
    EC_TYPEC_DISCOVERY_RESPONSE resp;

    PAGED_CODE();

    *CableVdo = 0;
    *TbtCableVdo = 0;

    req.Port = PortIndex;
    req.PartnerType = TYPEC_PARTNER_SOP_PRIME;

    status = CrosTcss_EcSendCommand(
        EC_CMD_TYPEC_DISCOVERY,
        0,  /* version */
        &req, sizeof(req),
        &resp, sizeof(resp),
        NULL);

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
            "CrosTcssMux: SOP' discovery failed for port %d (0x%08X)\n",
            PortIndex, status));
        return status;
    }

    /* Identity VDOs layout:
     *   [0] = ID Header VDO
     *   [1] = Cert Stat VDO
     *   [2] = Product VDO
     *   [3] = Product Type VDO1 (Cable VDO)
     *   [4] = Product Type VDO2 (if present)
     *   [5] = Product Type VDO3 (if present)
     */
    if (resp.IdentityCount >= 4) {
        *CableVdo = resp.IdentityVdo[3];
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
            "CrosTcssMux: Port %d cable VDO = 0x%08X\n",
            PortIndex, *CableVdo));
    }

    /* Search SVID entries for Intel TBT SVID (0x8087) */
    if (resp.SvidCount >= 2) {
        /* SVID entry VDO: header at SvidVdo[0], mode VDO at SvidVdo[1] */
        UINT32 svidHeader = resp.SvidVdo[0];
        UINT16 svid = (UINT16)(svidHeader & 0xFFFF);
        if (svid == 0x8087) {
            /* Intel TBT3 VDO */
            *TbtCableVdo = resp.SvidVdo[1];
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                "CrosTcssMux: Port %d TBT cable VDO = 0x%08X\n",
                PortIndex, *TbtCableVdo));
        }
    }

    return STATUS_SUCCESS;
}

/* =========================================================================
 * CrosTcss_QueryTypecStatus — Get full port status from EC
 * ========================================================================= */

static NTSTATUS
CrosTcss_QueryTypecStatus(
    _In_ UINT8 PortIndex,
    _Out_ EC_TYPEC_STATUS_RESPONSE* StatusResp
    )
{
    EC_TYPEC_STATUS_REQUEST req;

    PAGED_CODE();

    req.Port = PortIndex;

    return CrosTcss_EcSendCommand(
        EC_CMD_TYPEC_STATUS,
        0,
        &req, sizeof(req),
        StatusResp, sizeof(*StatusResp),
        NULL);
}

/* =========================================================================
 * CrosTcss_BuildModeData — Synthesize PMC IPC WBUF0/WBUF1 payloads
 *
 * Translates the EC mux state + cable VDO data into the exact bitpacked
 * UINT32 values that the Intel PMC North Mux-Agent crossbar expects.
 *
 * The bit layout differs between pre-MTL and MTL+ platforms.
 * ========================================================================= */

_Use_decl_annotations_
NTSTATUS
CrosTcss_BuildModeData(
    PDEVICE_CONTEXT DevCtx,
    UINT8 PortIndex,
    UINT8 MuxState,
    UINT32* ModeData,
    UINT32* PortMap
    )
{
    PTCSS_PORT_STATE port;
    UINT32 data = 0;
    UINT32 map = 0;
    BOOLEAN isMtl;
    UINT8 cableSpeed = 0;
    UINT8 cableType = 0;
    UINT8 modeType = PMC_MODE_TYPE_USB;
    UINT8 dpPin = 0;
    NTSTATUS status;

    PAGED_CODE();

    if (PortIndex >= DevCtx->PortCount) {
        return STATUS_INVALID_PARAMETER;
    }

    port = &DevCtx->Ports[PortIndex];
    isMtl = (DevCtx->PlatformGen >= TCSS_GEN_METEOR_LAKE);

    /* Refresh cable VDO data if needed */
    if (port->CableVdo == 0 && (MuxState & ~USB_PD_MUX_SAFE_MODE)) {
        CrosTcss_QueryCableVdo(PortIndex, &port->CableVdo, &port->TbtCableVdo);
    }

    /* Also fetch full TYPEC_STATUS for extended fields */
    {
        EC_TYPEC_STATUS_RESPONSE typecStatus;
        status = CrosTcss_QueryTypecStatus(PortIndex, &typecStatus);
        if (NT_SUCCESS(status)) {
            port->Polarity = typecStatus.Polarity;
            port->DataRole = typecStatus.DataRole;
            port->DpPinMode = typecStatus.DpPinMode;
        }
    }

    /* Extract cable speed from Product Type VDO1 */
    cableSpeed = (UINT8)((port->CableVdo & CABLE_VDO1_SS_MASK) >> CABLE_VDO1_SS_SHIFT);
    cableType = (UINT8)((port->CableVdo & CABLE_VDO1_TYPE_MASK) >> CABLE_VDO1_TYPE_SHIFT);

    /* Determine mode type from mux state flags */
    if (MuxState & USB_PD_MUX_USB4_ENABLED) {
        modeType = PMC_MODE_TYPE_USB4;
    } else if (MuxState & USB_PD_MUX_TBT_COMPAT_ENABLED) {
        modeType = PMC_MODE_TYPE_TBT;
    } else if (MuxState & USB_PD_MUX_DP_ENABLED) {
        modeType = PMC_MODE_TYPE_DP;
    } else if (MuxState & USB_PD_MUX_USB_ENABLED) {
        modeType = PMC_MODE_TYPE_USB;
    }

    /* Determine DP pin assignment */
    switch (port->DpPinMode) {
    case 'C': dpPin = 0x04; break;
    case 'D': dpPin = 0x02; break;
    case 'E': dpPin = 0x01; break;
    default:  dpPin = 0x04; break;  /* Default to Pin C (4 lanes) */
    }

    /* ---- Build WBUF0 (mode_data) ---- */

    if (isMtl) {
        /* Meteor Lake+ layout */
        data |= ((UINT32)(PortIndex & MTL_PORT_MASK) << MTL_PORT_SHIFT);
        if (port->Polarity) {
            data |= (1U << MTL_ORIENT_BIT);
        }
        if (port->DataRole == 0) {  /* UFP */
            data |= (1U << MTL_UFP_BIT);
        }
        data |= ((UINT32)(cableSpeed & MTL_CABLE_SPEED_MASK) << MTL_CABLE_SPEED_SHIFT);
        if (cableType != CABLE_VDO_TYPE_PASSIVE) {
            data |= (1U << MTL_CABLE_TYPE_BIT);
        }

        /* Mode-specific bits */
        if (modeType == PMC_MODE_TYPE_TBT || modeType == PMC_MODE_TYPE_USB4) {
            UINT8 tbtSpeed = (UINT8)((port->TbtCableVdo & TBT_VDO_SPEED_MASK)
                                     >> TBT_VDO_SPEED_SHIFT);
            UINT8 tbtRounded = (UINT8)((port->TbtCableVdo & TBT_VDO_ROUNDED_MASK)
                                       >> TBT_VDO_ROUNDED_SHIFT);
            data |= ((UINT32)(tbtSpeed & MTL_TBT_CABLE_SPEED_MASK)
                     << MTL_TBT_CABLE_SPEED_SHIFT);
            if (tbtRounded) {
                data |= (1U << MTL_TBT_ROUNDED_BIT);
            }
        }

        if (modeType == PMC_MODE_TYPE_DP) {
            data |= ((UINT32)(dpPin & MTL_DP_PIN_MASK) << MTL_DP_PIN_SHIFT);
            if (MuxState & USB_PD_MUX_HPD_LVL) {
                data |= (1U << MTL_DP_HPD_LVL_BIT);
            }
            if (MuxState & USB_PD_MUX_HPD_IRQ) {
                data |= (1U << MTL_DP_HPD_IRQ_BIT);
            }
        }

        data |= ((UINT32)(modeType & MTL_MODE_TYPE_MASK) << MTL_MODE_TYPE_SHIFT);
    } else {
        /* Pre-MTL (TGL/ADL/RPL) layout */
        data |= ((UINT32)(PortIndex & PRE_MTL_PORT_MASK) << PRE_MTL_PORT_SHIFT);
        if (port->Polarity) {
            data |= (1U << PRE_MTL_ORIENT_BIT);
        }
        if (port->DataRole == 0) {  /* UFP */
            data |= (1U << PRE_MTL_UFP_BIT);
        }
        data |= ((UINT32)(cableSpeed & PRE_MTL_CABLE_SPEED_MASK)
                 << PRE_MTL_CABLE_SPEED_SHIFT);
        if (cableType != CABLE_VDO_TYPE_PASSIVE) {
            data |= (1U << PRE_MTL_CABLE_TYPE_BIT);
        }

        /* TBT/USB4 cable parameters */
        if (modeType == PMC_MODE_TYPE_TBT || modeType == PMC_MODE_TYPE_USB4) {
            UINT8 tbtSpeed = (UINT8)((port->TbtCableVdo & TBT_VDO_SPEED_MASK)
                                     >> TBT_VDO_SPEED_SHIFT);
            UINT8 tbtRounded = (UINT8)((port->TbtCableVdo & TBT_VDO_ROUNDED_MASK)
                                       >> TBT_VDO_ROUNDED_SHIFT);
            data |= ((UINT32)(tbtSpeed & PRE_MTL_TBT_CABLE_SPEED_MASK)
                     << PRE_MTL_TBT_CABLE_SPEED_SHIFT);
            if (tbtRounded) {
                data |= (1U << PRE_MTL_TBT_ROUNDED_BIT);
            }
        }

        /* DP pin assignment and HPD */
        if (modeType == PMC_MODE_TYPE_DP) {
            data |= ((UINT32)(dpPin & PRE_MTL_DP_PIN_MASK) << PRE_MTL_DP_PIN_SHIFT);
            if (MuxState & USB_PD_MUX_HPD_LVL) {
                data |= (1U << PRE_MTL_DP_HPD_LVL_BIT);
            }
            if (MuxState & USB_PD_MUX_HPD_IRQ) {
                data |= (1U << PRE_MTL_DP_HPD_IRQ_BIT);
            }
        }

        data |= ((UINT32)(modeType & PRE_MTL_MODE_TYPE_MASK)
                 << PRE_MTL_MODE_TYPE_SHIFT);
    }

    /* ---- Build WBUF1 (port_map) ---- */
    map |= ((UINT32)port->USB2PortNumber << PMC_PORTMAP_USB2_SHIFT);
    map |= ((UINT32)port->USB3PortNumber << PMC_PORTMAP_USB3_SHIFT);

    *ModeData = data;
    *PortMap = map;

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
        "CrosTcssMux: Port %d mode_data=0x%08X port_map=0x%08X "
        "(mux=0x%02X type=%d cable=0x%08X)\n",
        PortIndex, data, map, MuxState, modeType, port->CableVdo));

    return STATUS_SUCCESS;
}
