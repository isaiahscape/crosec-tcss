/*
 * cros_tcss_hw.h — Hardware Specifications & Structures
 *
 * Defines all constants, bitmasks, command IDs, and data structures used to
 * communicate with the ChromeOS Embedded Controller (EC) over LPC and with
 * the Intel Power Management Controller (PMC) North Mux-Agent over MMIO IPC.
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#pragma once

#include <ntddk.h>

/* =========================================================================
 * General Constants
 * ========================================================================= */

#define CROS_TCSS_MAX_PORTS          4   /* Most Chromebooks have 1-3 USB-C ports */
#define CROS_TCSS_POOL_TAG           'SSCT'

/* =========================================================================
 * ChromeOS EC LPC Transport Constants
 * ========================================================================= */

/* LPC I/O port addresses for EC host-command protocol v3 */
#define EC_LPC_ADDR_HOST_DATA        0x200
#define EC_LPC_ADDR_HOST_CMD         0x204
#define EC_LPC_ADDR_HOST_ARGS        0x800  /* Memory-mapped request/response window */
#define EC_LPC_ADDR_HOST_ARGS_END    0x8FF
#define EC_LPC_ADDR_HOST_PARAM       0x800
#define EC_LPC_HOST_PARAM_SIZE       0x100  /* 256 bytes available */

/* EC status register bit definitions */
#define EC_LPC_STATUS_PROCESSING     0x02   /* EC is processing a command */
#define EC_LPC_STATUS_LAST_CMD       0x04   /* Last command completed */
#define EC_LPC_STATUS_BUSY_MASK      0x01   /* Input buffer full (IBF) */
#define EC_LPC_STATUS_FROM_HOST      0x02   /* Data is from host */

/* Protocol v3 framing */
#define EC_HOST_REQUEST_VERSION      3
#define EC_HOST_RESPONSE_VERSION     3

/* Timeout for EC command completion (microseconds) */
#define EC_CMD_TIMEOUT_US            (5 * 1000 * 1000)  /* 5 seconds */
#define EC_CMD_POLL_INTERVAL_US      100

/* =========================================================================
 * ChromeOS EC Host Commands
 * ========================================================================= */

#define EC_CMD_GET_NEXT_EVENT        0x0067
#define EC_CMD_HOST_EVENT_GET_B      0x0087
#define EC_CMD_USB_PD_MUX_INFO      0x011A
#define EC_CMD_TYPEC_DISCOVERY       0x0133
#define EC_CMD_TYPEC_STATUS          0x0134

/* =========================================================================
 * EC_CMD_GET_NEXT_EVENT definitions
 * ========================================================================= */

/* Event types returned by EC_CMD_GET_NEXT_EVENT */
#define EC_MKBP_EVENT_PD_MCU         0x09   /* PD MCU event (mux changes) */
#define EC_MKBP_EVENT_NONE           0xFF   /* No more events */

/* Host event B bitmask for USB-C events */
#define EC_HOST_EVENT_PD_MCU         (1ULL << 22)  /* Bit 22: PD/Type-C event */
#define EC_HOST_EVENT_USB_MUX        (1ULL << 28)  /* Bit 28: USB mux change */

/* =========================================================================
 * EC_CMD_USB_PD_MUX_INFO — Query current mux state for a port
 * ========================================================================= */

#pragma pack(push, 1)

typedef struct _EC_USB_PD_MUX_INFO_REQUEST {
    UINT8  Port;  /* Zero-based USB-C port index */
} EC_USB_PD_MUX_INFO_REQUEST, *PEC_USB_PD_MUX_INFO_REQUEST;

typedef struct _EC_USB_PD_MUX_INFO_RESPONSE {
    UINT8  Flags;  /* Combination of USB_PD_MUX_* bitmasks */
} EC_USB_PD_MUX_INFO_RESPONSE, *PEC_USB_PD_MUX_INFO_RESPONSE;

#pragma pack(pop)

/* =========================================================================
 * USB PD Mux Bitmasks (from EC mux_state)
 * ========================================================================= */

#define USB_PD_MUX_NONE              0x00
#define USB_PD_MUX_USB_ENABLED       0x01   /* USB 3.x data path active */
#define USB_PD_MUX_DP_ENABLED        0x02   /* DisplayPort Alt Mode active */
#define USB_PD_MUX_POLARITY_INVERTED 0x04   /* CC line is inverted (CC2) */
#define USB_PD_MUX_SAFE_MODE         0x08   /* Safe/Disconnect mode */
#define USB_PD_MUX_TBT_COMPAT_ENABLED 0x10  /* Thunderbolt 3 compatibility */
#define USB_PD_MUX_USB4_ENABLED      0x20   /* USB4/Thunderbolt 4 tunnel */
#define USB_PD_MUX_HPD_IRQ           0x40   /* DP HPD interrupt pending */
#define USB_PD_MUX_HPD_LVL           0x80   /* DP HPD level (high = connected) */

/* =========================================================================
 * EC_CMD_TYPEC_STATUS — Extended port status
 * ========================================================================= */

#pragma pack(push, 1)

typedef struct _EC_TYPEC_STATUS_REQUEST {
    UINT8  Port;
} EC_TYPEC_STATUS_REQUEST, *PEC_TYPEC_STATUS_REQUEST;

/*
 * Simplified version of the full response structure.
 * We only need the mux_state and a few key fields.
 */
typedef struct _EC_TYPEC_STATUS_RESPONSE {
    UINT8  PdEnabled;       /* 1 if PD negotiation active */
    UINT8  DevConnected;    /* Device connection state */
    UINT8  SinkCapCount;
    UINT8  SourceCapCount;
    UINT8  PowerRole;       /* 0 = sink, 1 = source */
    UINT8  DataRole;        /* 0 = UFP, 1 = DFP */
    UINT8  Polarity;        /* 0 = CC1, 1 = CC2 */
    UINT8  CC1;             /* CC1 voltage state */
    UINT8  CC2;             /* CC2 voltage state */
    UINT8  DpPinMode;       /* DP pin assignment (C/D/E) */
    UINT8  MuxState;        /* Current mux state flags (USB_PD_MUX_*) */
    UINT8  Reserved1;
    UINT32 Events;          /* Pending events bitmask */
    UINT32 SopPrimeVdo[6];  /* SOP' cable VDO data (up to 6 DWORDs) */
} EC_TYPEC_STATUS_RESPONSE, *PEC_TYPEC_STATUS_RESPONSE;

#pragma pack(pop)

/* =========================================================================
 * EC_CMD_TYPEC_DISCOVERY — Query SOP/SOP' discovery data
 * ========================================================================= */

/* Partner type selectors */
#define TYPEC_PARTNER_SOP            0   /* Port partner (SOP) */
#define TYPEC_PARTNER_SOP_PRIME      1   /* Cable plug (SOP') */

#pragma pack(push, 1)

typedef struct _EC_TYPEC_DISCOVERY_REQUEST {
    UINT8  Port;
    UINT8  PartnerType;  /* TYPEC_PARTNER_SOP or TYPEC_PARTNER_SOP_PRIME */
} EC_TYPEC_DISCOVERY_REQUEST, *PEC_TYPEC_DISCOVERY_REQUEST;

typedef struct _EC_TYPEC_DISCOVERY_RESPONSE {
    UINT8  IdentityCount;
    UINT8  SvidCount;
    UINT8  Reserved[2];
    /* Identity VDOs: ID Header, Cert Stat, Product, Product Type VDO1..3 */
    UINT32 IdentityVdo[7];
    /* SVID entries: each is {SVID[15:0], ModeCount[7:0], Reserved[7:0], ModeVDO[0..N]} */
    UINT32 SvidVdo[12];
} EC_TYPEC_DISCOVERY_RESPONSE, *PEC_TYPEC_DISCOVERY_RESPONSE;

#pragma pack(pop)

/* =========================================================================
 * SOP' Cable VDO Parsing Flags
 *
 * USB PD Specification — Passive/Active Cable VDO (VDM Identity Response)
 * ========================================================================= */

/* VDO field: USB SuperSpeed signaling support (bits 2:0 of Product Type VDO1) */
#define CABLE_VDO_SS_USB20_ONLY      0x00
#define CABLE_VDO_SS_USB32_GEN1      0x01
#define CABLE_VDO_SS_USB32_GEN2      0x02  /* Also USB 3.2 Gen 2x1 */
#define CABLE_VDO_SS_USB40_GEN2      0x03
#define CABLE_VDO_SS_USB40_GEN3      0x04

/* VDO field: Cable type (bits 19:18 of Product Type VDO1) */
#define CABLE_VDO_TYPE_PASSIVE       0x00
#define CABLE_VDO_TYPE_ACTIVE_RE     0x01  /* Active redriver */
#define CABLE_VDO_TYPE_ACTIVE_RT     0x02  /* Active retimer */
#define CABLE_VDO_TYPE_OPTICAL       0x03

/* VDO field: Rounded frequency support (bit 4 of VDO1 for TBT3 VDO) */
#define CABLE_VDO_ROUNDED_NON        0x00
#define CABLE_VDO_ROUNDED_YES        0x01

/* VDO field: TBT cable speed (bits 18:16 of TBT3 Cable VDO) */
#define TBT_CABLE_SPEED_GEN3         0x01  /* TBT Gen 3 = ~20 Gbps per lane */

/* VDO field: Active/passive cable indicator (bit 24 of TBT VDO) */
#define TBT_CABLE_ACTIVE             0x01

/* VDO field: Cable retimer (bit 21 of TBT VDO) */
#define TBT_CABLE_RETIMER            0x01

/* Extract helpers for Product Type VDO1 */
#define CABLE_VDO1_SS_MASK           0x00000007  /* bits 2:0 */
#define CABLE_VDO1_SS_SHIFT          0
#define CABLE_VDO1_TYPE_MASK         0x000C0000  /* bits 19:18 */
#define CABLE_VDO1_TYPE_SHIFT        18

/* Extract helper for TBT3 Cable VDO */
#define TBT_VDO_SPEED_MASK           0x00070000  /* bits 18:16 */
#define TBT_VDO_SPEED_SHIFT          16
#define TBT_VDO_ROUNDED_MASK         0x00080000  /* bit 19 */
#define TBT_VDO_ROUNDED_SHIFT        19
#define TBT_VDO_RETIMER_BIT          21
#define TBT_VDO_ACTIVE_BIT           24

/* =========================================================================
 * Intel PMC IPC (Inter-Processor Communication) Constants
 *
 * The PMC's North Mux-Agent accepts commands via memory-mapped IPC registers
 * at PWRMBASE + 0x2000. PWRMBASE is typically 0xFE000000.
 * ========================================================================= */

/* Base addresses */
#define PMC_PWRMBASE_DEFAULT         0xFE000000ULL
#define PMC_IPC_OFFSET               0x2000
#define PMC_IPC_BASE_DEFAULT         (PMC_PWRMBASE_DEFAULT + PMC_IPC_OFFSET)

/* IPC register offsets relative to IPC base */
#define PMC_IPC_CMD_OFFSET           0x00  /* Command register (write) */
#define PMC_IPC_STATUS_OFFSET        0x04  /* Status register (read) */
#define PMC_IPC_STSDATA_OFFSET       0x08  /* Status data (read, 4 bytes) */
#define PMC_IPC_WBUF0_OFFSET         0x80  /* Write buffer DWORD 0 */
#define PMC_IPC_WBUF1_OFFSET         0x84  /* Write buffer DWORD 1 */
#define PMC_IPC_WBUF2_OFFSET         0x88  /* Write buffer DWORD 2 */
#define PMC_IPC_WBUF3_OFFSET         0x8C  /* Write buffer DWORD 3 */
#define PMC_IPC_RBUF0_OFFSET         0x90  /* Read buffer DWORD 0 */

/* IPC status bits */
#define PMC_IPC_STS_BUSY             0x01  /* Command in progress */
#define PMC_IPC_STS_ERROR            0x02  /* Command error */
#define PMC_IPC_STS_CMD_ID_MASK      0x00FF0000  /* Echo of command ID */

/* IPC command construction */
#define PMC_IPC_USBC_CMD_ID          0xA7  /* USB Type-C mux command ID */
#define PMC_IPC_SUBCMD_SHIFT         12
#define PMC_IPC_CMDID_SHIFT          0
#define PMC_IPC_SIZE_SHIFT           16

/*
 * PMC_IPC_CMD encoding:
 *   bits [7:0]   = Command ID (0xA7)
 *   bits [11:8]  = Reserved
 *   bits [15:12] = Sub-command
 *   bits [23:16] = Write data size (in DWORDs)
 *   bits [31:24] = Reserved
 */
#define PMC_IPC_BUILD_CMD(cmd_id, subcmd, size_dwords) \
    ((UINT32)(cmd_id) | \
     ((UINT32)(subcmd) << PMC_IPC_SUBCMD_SHIFT) | \
     ((UINT32)(size_dwords) << PMC_IPC_SIZE_SHIFT))

/* PMC IPC Sub-commands for USB Type-C */
#define PMC_USB_CONNECT              0x00  /* Connect a USB-C port */
#define PMC_USB_DISCONNECT           0x01  /* Disconnect a port */
#define PMC_USB_SAFE_MODE            0x02  /* Transition to safe/idle state */
#define PMC_USB_ALT_MODE             0x03  /* Enter alternate mode (DP/TBT/USB4) */
#define PMC_USB_DP_HPD               0x04  /* DP hotplug detect signaling */

/* IPC command timeout */
#define PMC_IPC_TIMEOUT_US           (1 * 1000 * 1000)  /* 1 second */
#define PMC_IPC_POLL_INTERVAL_US     50

/* =========================================================================
 * PMC Mux-Agent Mode Data Bitfield Layouts
 *
 * The exact bit positions differ between:
 *   - Pre-MTL: Tiger Lake, Alder Lake, Raptor Lake (11th-13th Gen)
 *   - MTL+: Meteor Lake and newer (14th Gen+)
 * ========================================================================= */

typedef enum _TCSS_PLATFORM_GEN {
    TCSS_GEN_TIGER_LAKE  = 0,  /* 11th Gen (TGL) */
    TCSS_GEN_ALDER_LAKE  = 1,  /* 12th Gen (ADL) */
    TCSS_GEN_RAPTOR_LAKE = 2,  /* 13th Gen (RPL) */
    TCSS_GEN_METEOR_LAKE = 3,  /* 14th Gen (MTL+) */
} TCSS_PLATFORM_GEN;

/* --- Pre-MTL (TGL/ADL/RPL) PMC connect command WBUF layout ---
 *
 * WBUF0 (mode_data):
 *   bits [1:0]   = USB-C port number (0-based)
 *   bit  [2]     = Orientation (0 = normal CC1, 1 = flipped CC2)
 *   bit  [3]     = UFP/DFP (0 = DFP, 1 = UFP)
 *   bit  [4]     = Debug accessory mode
 *   bits [7:5]   = Cable speed (from cable VDO)
 *   bit  [8]     = Cable type (passive/active)
 *   bits [11:9]  = Reserved / cable generation
 *   bits [15:12] = Reserved
 *   bit  [16]    = TBT link training (0 = bi-directional, 1 = uni-directional)
 *   bits [18:17] = TBT cable speed
 *   bit  [19]    = TBT cable generation (rounded)
 *   bits [22:20] = DP pin assignment
 *   bit  [23]    = DP HPD level
 *   bit  [24]    = DP HPD IRQ
 *   bits [26:25] = USB-C mode type (00=USB, 01=DP, 10=TBT, 11=USB4)
 *   bits [31:27] = Reserved
 *
 * WBUF1 (port_map):
 *   bits [7:0]   = USB2 port number
 *   bits [15:8]  = USB3 port number
 */

/* Pre-MTL bit positions (WBUF0) */
#define PRE_MTL_PORT_SHIFT           0
#define PRE_MTL_PORT_MASK            0x03
#define PRE_MTL_ORIENT_BIT           2
#define PRE_MTL_UFP_BIT              3
#define PRE_MTL_DBG_ACC_BIT          4
#define PRE_MTL_CABLE_SPEED_SHIFT    5
#define PRE_MTL_CABLE_SPEED_MASK     0x07
#define PRE_MTL_CABLE_TYPE_BIT       8
#define PRE_MTL_TBT_LINK_TRAIN_BIT   16
#define PRE_MTL_TBT_CABLE_SPEED_SHIFT 17
#define PRE_MTL_TBT_CABLE_SPEED_MASK  0x03
#define PRE_MTL_TBT_ROUNDED_BIT      19
#define PRE_MTL_DP_PIN_SHIFT         20
#define PRE_MTL_DP_PIN_MASK          0x07
#define PRE_MTL_DP_HPD_LVL_BIT      23
#define PRE_MTL_DP_HPD_IRQ_BIT      24
#define PRE_MTL_MODE_TYPE_SHIFT      25
#define PRE_MTL_MODE_TYPE_MASK       0x03

/* Mode type encoding (pre-MTL) */
#define PMC_MODE_TYPE_USB            0x00
#define PMC_MODE_TYPE_DP             0x01
#define PMC_MODE_TYPE_TBT            0x02
#define PMC_MODE_TYPE_USB4           0x03

/* Port-map WBUF1 encoding */
#define PMC_PORTMAP_USB2_SHIFT       0
#define PMC_PORTMAP_USB2_MASK        0xFF
#define PMC_PORTMAP_USB3_SHIFT       8
#define PMC_PORTMAP_USB3_MASK        0xFF

/* --- Meteor Lake+ (MTL) PMC bit layout ---
 *
 * Bit positions shifted to accommodate wider port numbering.
 * Key differences:
 *   bits [2:0]   = USB-C port number (now 3 bits for >4 ports)
 *   bit  [3]     = Orientation
 *   bit  [4]     = UFP/DFP
 *   bits [7:5]   = Cable speed
 *   bit  [8]     = Cable type
 *   bits [26:25] = Mode type (same encoding)
 *
 * WBUF1 layout unchanged.
 */

#define MTL_PORT_SHIFT               0
#define MTL_PORT_MASK                0x07  /* 3-bit port for MTL+ */
#define MTL_ORIENT_BIT               3
#define MTL_UFP_BIT                  4
#define MTL_CABLE_SPEED_SHIFT        5
#define MTL_CABLE_SPEED_MASK         0x07
#define MTL_CABLE_TYPE_BIT           8
#define MTL_TBT_LINK_TRAIN_BIT      16
#define MTL_TBT_CABLE_SPEED_SHIFT   17
#define MTL_TBT_CABLE_SPEED_MASK    0x03
#define MTL_TBT_ROUNDED_BIT         19
#define MTL_DP_PIN_SHIFT             20
#define MTL_DP_PIN_MASK              0x07
#define MTL_DP_HPD_LVL_BIT          23
#define MTL_DP_HPD_IRQ_BIT          24
#define MTL_MODE_TYPE_SHIFT          25
#define MTL_MODE_TYPE_MASK           0x03

/* =========================================================================
 * DP Pin Assignment Encodings
 * ========================================================================= */

#define DP_PIN_ASSIGN_C              0x04  /* 4 lanes DP */
#define DP_PIN_ASSIGN_D              0x08  /* 2 lanes DP + 2 lanes USB */
#define DP_PIN_ASSIGN_E              0x10  /* 4 lanes DP (alt pinout) */

/* Map ChromeOS EC dp_pin_mode (0-based index) to PMC pin assignment bits */
#define EC_DP_PIN_TO_PMC_PIN(pin) \
    ((pin) == 'C' ? 0x04 : \
     (pin) == 'D' ? 0x02 : \
     (pin) == 'E' ? 0x01 : 0x04)  /* Default to pin C */

/* =========================================================================
 * Per-Port Runtime State
 * ========================================================================= */

typedef struct _TCSS_PORT_STATE {
    UINT8  PortIndex;           /* EC port number (0-based) */
    UINT8  MuxState;            /* Last known USB_PD_MUX_* flags from EC */
    UINT8  Polarity;            /* 0 = CC1 normal, 1 = CC2 flipped */
    UINT8  DataRole;            /* 0 = UFP, 1 = DFP */
    UINT8  USB2PortNumber;      /* Physical USB2 port from ACPI _DSD */
    UINT8  USB3PortNumber;      /* Physical USB3 port from ACPI _DSD */
    UINT8  DpPinMode;           /* DP pin assignment letter */
    BOOLEAN Connected;          /* TRUE if a device is currently connected */
    BOOLEAN PmcConfigured;      /* TRUE if PMC crossbar has been programmed */
    UINT32 CableVdo;            /* SOP' cable Product Type VDO1 */
    UINT32 TbtCableVdo;         /* TBT3 cable mode VDO (if any) */
    UINT32 LastModeData;        /* Last WBUF0 sent to PMC (for dedup) */
    UINT32 LastPortMap;         /* Last WBUF1 sent to PMC (for dedup) */
} TCSS_PORT_STATE, *PTCSS_PORT_STATE;

/* =========================================================================
 * Device Context
 * ========================================================================= */

typedef struct _DEVICE_CONTEXT {
    /* KMDF handles */
    WDFDEVICE            Device;
    WDFWORKITEM          EventWorkItem;
    WDFTIMER             WatchdogTimer;
    WDFSPINLOCK          StateLock;

    /* Platform identification */
    TCSS_PLATFORM_GEN    PlatformGen;

    /* PMC IPC MMIO */
    PVOID                PmcIpcBase;      /* Mapped virtual address of IPC regs */
    PHYSICAL_ADDRESS     PmcIpcPhysAddr;  /* Physical address */
    SIZE_T               PmcIpcLength;    /* Mapping length */

    /* ACPI notification state */
    PVOID                AcpiNotifyHandle; /* ACPI interface registration */
    BOOLEAN              AcpiNotifyRegistered;

    /* Port state tracking */
    UINT8                PortCount;       /* Number of USB-C ports discovered */
    TCSS_PORT_STATE      Ports[CROS_TCSS_MAX_PORTS];

    /* Event processing */
    volatile LONG        EventWorkItemQueued;  /* Interlocked flag */
    BOOLEAN              D0Active;             /* TRUE while in D0 */

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

/* =========================================================================
 * EC Host Command Protocol v3 Structures (LPC framing)
 * ========================================================================= */

#pragma pack(push, 1)

typedef struct _EC_HOST_REQUEST {
    UINT8  StructVersion;   /* Must be EC_HOST_REQUEST_VERSION (3) */
    UINT8  Checksum;        /* 8-bit checksum (sum of all bytes == 0) */
    UINT16 Command;         /* EC command ID */
    UINT8  CommandVersion;  /* Command version */
    UINT8  Reserved;
    UINT16 DataLen;         /* Length of data following this header */
} EC_HOST_REQUEST, *PEC_HOST_REQUEST;

typedef struct _EC_HOST_RESPONSE {
    UINT8  StructVersion;   /* Must be EC_HOST_RESPONSE_VERSION (3) */
    UINT8  Checksum;        /* 8-bit checksum */
    UINT16 ResultCode;      /* EC result (0 = success) */
    UINT16 DataLen;         /* Length of data following this header */
    UINT16 Reserved;
} EC_HOST_RESPONSE, *PEC_HOST_RESPONSE;

#pragma pack(pop)

/* EC result codes */
#define EC_RES_SUCCESS               0
#define EC_RES_INVALID_COMMAND       1
#define EC_RES_ERROR                 2
#define EC_RES_INVALID_PARAM         3
#define EC_RES_ACCESS_DENIED         4
#define EC_RES_INVALID_RESPONSE      5
#define EC_RES_INVALID_VERSION       6
#define EC_RES_INVALID_CHECKSUM      7
#define EC_RES_IN_PROGRESS           8
#define EC_RES_UNAVAILABLE           9
#define EC_RES_TIMEOUT               10
#define EC_RES_OVERFLOW              11
#define EC_RES_INVALID_HEADER        12
#define EC_RES_REQUEST_TRUNCATED     13
#define EC_RES_RESPONSE_TOO_BIG      14
#define EC_RES_BUS_ERROR             15
#define EC_RES_BUSY                  16

/* =========================================================================
 * ACPI GUIDs and Interface Structures
 * ========================================================================= */

/* Device Properties UUID for _DSD evaluation */
/* {DAFFD814-6EBA-4D8C-8A91-BC9BBF4AA301} */
DEFINE_GUID(ACPI_DSD_DEVICE_PROPERTIES_UUID,
    0xDAFFD814, 0x6EBA, 0x4D8C,
    0x8A, 0x91, 0xBC, 0x9B, 0xBF, 0x4A, 0xA3, 0x01);

/* ACPI interface GUID for notification registration */
/* Defined in acpiioct.h: GUID_ACPI_INTERFACE_STANDARD2 */

/* ACPI _DSD property names we search for */
#define ACPI_DSD_USB2_PORT_NUMBER    "usb2-port-number"
#define ACPI_DSD_USB3_PORT_NUMBER    "usb3-port-number"

/* ACPI node paths for PMC USB-C connector children */
#define ACPI_PMC_PATH_PRIMARY        "\\_SB.PCI0.PMC.PMCU"
#define ACPI_PMC_PATH_FALLBACK       "\\_SB.PC00.PMC.PMCU"
#define ACPI_CONNECTOR_PREFIX        "CON"  /* CON0, CON1, CON2, CON3 */

/* =========================================================================
 * Forward Declarations (implemented in transport/irq/main modules)
 * ========================================================================= */

/* cros_tcss_transport.c */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
CrosTcss_EcSendCommand(
    _In_ UINT16 Command,
    _In_ UINT8 CommandVersion,
    _In_reads_bytes_opt_(RequestSize) const VOID* RequestData,
    _In_ UINT16 RequestSize,
    _Out_writes_bytes_opt_(ResponseSize) VOID* ResponseData,
    _In_ UINT16 ResponseSize,
    _Out_opt_ UINT16* ActualResponseSize
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
CrosTcss_PmcSendIpcCommand(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ UINT8 SubCommand,
    _In_ UINT32 Wbuf0,
    _In_ UINT32 Wbuf1,
    _In_ UINT32 Wbuf2,
    _In_ UINT32 Wbuf3,
    _Out_opt_ UINT32* StatusData
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
CrosTcss_ParseAcpiPortMappings(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ WDFDEVICE Device
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
CrosTcss_BuildModeData(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ UINT8 PortIndex,
    _In_ UINT8 MuxState,
    _Out_ UINT32* ModeData,
    _Out_ UINT32* PortMap
    );

/* cros_tcss_irq.c */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
CrosTcss_RegisterAcpiNotify(
    _In_ PDEVICE_CONTEXT DevCtx
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
CrosTcss_UnregisterAcpiNotify(
    _In_ PDEVICE_CONTEXT DevCtx
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
CrosTcss_ProcessPortUpdate(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ UINT8 PortIndex
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
CrosTcss_ProcessAllPorts(
    _In_ PDEVICE_CONTEXT DevCtx
    );

/* Driver entry points (CrosTcssMux.c) */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD CrosTcss_EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE CrosTcss_EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE CrosTcss_EvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY CrosTcss_EvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT CrosTcss_EvtD0Exit;
