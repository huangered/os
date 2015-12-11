/*++

Copyright (c) 2015 Minoca Corp. All Rights Reserved

Module Name:

    rtlw81hw.c

Abstract:

    This module implements device support for the Realtek RTL81xx family of
    wireless internet controllers.

Author:

    Chris Stevens 5-Oct-2015

Environment:

    Kernel

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <minoca/driver.h>
#include <minoca/net/netdrv.h>
#include <minoca/net/net80211.h>
#include <usb.h>
#include "rtlw81.h"

//
// --------------------------------------------------------------------- Macros
//

#define RTLW81_WRITE_REGISTER8(_Device, _Register, _Value) \
    Rtlw81pWriteRegister((_Device), (_Register), (_Value), sizeof(UCHAR))

#define RTLW81_WRITE_REGISTER16(_Device, _Register, _Value) \
    Rtlw81pWriteRegister((_Device), (_Register), (_Value), sizeof(USHORT))

#define RTLW81_WRITE_REGISTER32(_Device, _Register, _Value) \
    Rtlw81pWriteRegister((_Device), (_Register), (_Value), sizeof(ULONG))

#define RTLW81_READ_REGISTER8(_Device, _Register) \
    Rtlw81pReadRegister((_Device), (_Register), sizeof(UCHAR))

#define RTLW81_READ_REGISTER16(_Device, _Register) \
    Rtlw81pReadRegister((_Device), (_Register), sizeof(USHORT))

#define RTLW81_READ_REGISTER32(_Device, _Register) \
    Rtlw81pReadRegister((_Device), (_Register), sizeof(ULONG))

#define RTLW81_ARRAY_COUNT(_Array) (sizeof((_Array)) / sizeof((_Array)[0]))

//
// ---------------------------------------------------------------- Definitions
//

#define RTLW81_DEFAULT_CHANNEL 1

//
// Define the maximum number of bulk out transfers that are allowed to be
// submitted at the same time.
//

#define RTLW81_MAX_BULK_OUT_TRANSFER_COUNT 64

//
// ------------------------------------------------------ Data Type Definitions
//

/*++

Structure Description:

    This structure defines an RTLW81xx bulk out transfer. These transfers are
    allocated on demand and recycled when complete.

Members:

    ListEntry - Stores a pointer to the next and previous bulk out transfer
        on the devices free transfer list.

    Device - Stores a pointer to the RTLW81 device that owns the transfer.

    UsbTransfer - Stores a pointer to the USB transfer that belongs to this
        SM95 transfer for the duration of its existence.

    Packet - Stores a pointer to the network packet buffer whose data is being
        sent by the USB transfer.

    EndpointIndex - Stores the index into the device's out endpoint array for
        the endpoint to which the transfer belongs.

--*/

typedef struct _RTLW81_BULK_OUT_TRANSFER {
    LIST_ENTRY ListEntry;
    PRTLW81_DEVICE Device;
    PUSB_TRANSFER UsbTransfer;
    PNET_PACKET_BUFFER Packet;
    UCHAR EndpointIndex;
} RTLW81_BULK_OUT_TRANSFER, *PRTLW81_BULK_OUT_TRANSFER;

/*++

Structure Description:

    This structure defines device specific data used to initialize the device
    into the correct state.

Members:

    BbRegisters - Stores the BB registers to be programed.

    BbValues - Stores the BB values to program into the registers.

    BbCount - Stores the number of BB registers to program.

    AgcValues - Stores the AGC values to program into the device.

    AgcCount - Stores the number of AGC values to program.

    RfRegisters - Stores an array of RF registers to program for each chain.

    RfValues - Stores an array of RF values to set for each chain.

    RfCount - Stores the number of RF registers to set for each chain.

--*/

typedef struct _RTLW81_DEVICE_DATA {
    PUSHORT BbRegisters;
    PULONG BbValues;
    ULONG BbCount;
    PULONG AgcValues;
    ULONG AgcCount;
    PUCHAR RfRegisters[RTLW81_MAX_CHAIN_COUNT];
    PULONG RfValues[RTLW81_MAX_CHAIN_COUNT];
    ULONG RfCount[RTLW81_MAX_CHAIN_COUNT];
} RTLW81_DEVICE_DATA, *PRTLW81_DEVICE_DATA;

/*++

Structure Description:

    This structure defines the transmit power data for a default RTLW81xx
    device.

Members:

    GroupPower - Stores the power data for each group.

--*/

typedef struct _RTLW81_DEFAULT_TRANSMIT_POWER_DATA {
    UCHAR GroupPower[RTLW81_DEFAULT_GROUP_COUNT][RTLW81_POWER_STATE_COUNT];
} RTLW81_DEFAULT_TRANSMIT_POWER_DATA, *PRTLW81_DEFAULT_TRANSMIT_POWER_DATA;

/*++

Structure Description:

    This structure defines the transmit power data for an RTL8188EU device.

Members:

    GroupPower - Stores the power data for each group.

--*/

typedef struct _RTLW81_8188E_TRANSMIT_POWER_DATA {
    UCHAR GroupPower[RTLW81_8188E_GROUP_COUNT][RTLW81_POWER_STATE_COUNT];
} RTLW81_8188E_TRANSMIT_POWER_DATA, *PRTLW81_8188E_TRANSMIT_POWER_DATA;

//
// ----------------------------------------------- Internal Function Prototypes
//

KSTATUS
Rtlw81pReadRom (
    PRTLW81_DEVICE Device
    );

KSTATUS
Rtlw81pDefaultInitialize (
    PRTLW81_DEVICE Device
    );

KSTATUS
Rtlw81p8188eInitialize (
    PRTLW81_DEVICE Device
    );

KSTATUS
Rtlw81pInitializeDma (
    PRTLW81_DEVICE Device
    );

KSTATUS
Rtlw81pInitializeFirmware (
    PRTLW81_DEVICE Device,
    PIRP Irp
    );

VOID
Rtlw81pLoadFirmwareCompletionRoutine (
    PVOID Context,
    PLOADED_FILE File
    );

VOID
Rtlw81pLcCalibration (
    PRTLW81_DEVICE Device
    );

VOID
Rtlw81pSetChannel (
    PRTLW81_DEVICE Device,
    ULONG Channel
    );

VOID
Rtlw81pEnableChannelTransmitPower (
    PRTLW81_DEVICE Device,
    ULONG Chain,
    ULONG Channel
    );

KSTATUS
Rtlw81pWriteLlt (
    PRTLW81_DEVICE Device,
    ULONG Address,
    ULONG Data
    );

KSTATUS
Rtlw81pWriteData (
    PRTLW81_DEVICE Device,
    USHORT Address,
    PVOID Data,
    ULONG DataLength
    );

KSTATUS
Rtlw81pReadData (
    PRTLW81_DEVICE Device,
    USHORT Address,
    PVOID Data,
    ULONG DataLength
    );

VOID
Rtlw81pWriteRegister (
    PRTLW81_DEVICE Device,
    USHORT Register,
    ULONG Data,
    ULONG DataLength
    );

ULONG
Rtlw81pReadRegister (
    PRTLW81_DEVICE Device,
    USHORT Register,
    ULONG DataLength
    );

UCHAR
Rtlw81pEfuseRead8 (
    PRTLW81_DEVICE Device,
    USHORT Address
    );

VOID
Rtlw81pWriteRfRegister (
    PRTLW81_DEVICE Device,
    ULONG Chain,
    ULONG RfRegister,
    ULONG Data
    );

ULONG
Rtlw81pReadRfRegister (
    PRTLW81_DEVICE Device,
    ULONG Chain,
    ULONG RfRegister
    );

KSTATUS
Rtlw81pSendFirmwareCommand (
    PRTLW81_DEVICE Device,
    UCHAR CommandId,
    PVOID Message,
    ULONG MessageLength
    );

KSTATUS
Rtlw81pSubmitBulkInTransfers (
    PRTLW81_DEVICE Device
    );

VOID
Rtlw81pCancelBulkInTransfers (
    PRTLW81_DEVICE Device
    );

PRTLW81_BULK_OUT_TRANSFER
Rtlw81pAllocateBulkOutTransfer (
    PRTLW81_DEVICE Device,
    RTLW81_BULK_OUT_TYPE Type
    );

VOID
Rtlw81pFreeBulkOutTransfer (
    PRTLW81_BULK_OUT_TRANSFER Transfer
    );

VOID
Rtlw81pBulkOutTransferCompletion (
    PUSB_TRANSFER Transfer
    );

VOID
Rtlw81pSetLed (
    PRTLW81_DEVICE Device,
    BOOL Enable
    );

//
// -------------------------------------------------------------------- Globals
//

USHORT RtlwDefaultMacRegisters[] = {
    0x420, 0x423, 0x430, 0x431, 0x432, 0x433, 0x434, 0x435, 0x436, 0x437,
    0x438, 0x439, 0x43a, 0x43b, 0x43c, 0x43d, 0x43e, 0x43f, 0x440, 0x441,
    0x442, 0x444, 0x445, 0x446, 0x447, 0x458, 0x459, 0x45a, 0x45b, 0x460,
    0x461, 0x462, 0x463, 0x4c8, 0x4c9, 0x4cc, 0x4cd, 0x4ce, 0x500, 0x501,
    0x502, 0x503, 0x504, 0x505, 0x506, 0x507, 0x508, 0x509, 0x50a, 0x50b,
    0x50c, 0x50d, 0x50e, 0x50f, 0x512, 0x514, 0x515, 0x516, 0x517, 0x51a,
    0x524, 0x525, 0x546, 0x547, 0x550, 0x551, 0x559, 0x55a, 0x55d, 0x605,
    0x608, 0x609, 0x652, 0x63c, 0x63d, 0x63e, 0x63f, 0x66e, 0x700, 0x701,
    0x702, 0x703, 0x708, 0x709, 0x70a, 0x70b
};

UCHAR RtlwDefaultMacValues[] = {
    0x80, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x05, 0x06, 0x07,
    0x00, 0x00, 0x00, 0x01, 0x04, 0x05, 0x06, 0x07, 0x5d, 0x01,
    0x00, 0x15, 0xf0, 0x0f, 0x00, 0x41, 0xa8, 0x72, 0xb9, 0x66,
    0x66, 0x08, 0x03, 0xff, 0x08, 0xff, 0xff, 0x01, 0x26, 0xa2,
    0x2f, 0x00, 0x28, 0xa3, 0x5e, 0x00, 0x2b, 0xa4, 0x5e, 0x00,
    0x4f, 0xa4, 0x00, 0x00, 0x1c, 0x0a, 0x10, 0x0a, 0x10, 0x16,
    0x0f, 0x4f, 0x40, 0x00, 0x10, 0x10, 0x02, 0x02, 0xff, 0x30,
    0x0e, 0x2a, 0x20, 0x0a, 0x0e, 0x0a, 0x0e, 0x05, 0x21, 0x43,
    0x65, 0x87, 0x21, 0x43, 0x65, 0x87
};

USHORT Rtlw8188eMacRegisters[] = {
    0x026, 0x027, 0x040, 0x428, 0x429, 0x430, 0x431, 0x432, 0x433, 0x434,
    0x435, 0x436, 0x437, 0x438, 0x439, 0x43a, 0x43b, 0x43c, 0x43d, 0x43e,
    0x43f, 0x440, 0x441, 0x442, 0x444, 0x445, 0x446, 0x447, 0x458, 0x459,
    0x45a, 0x45b, 0x460, 0x461, 0x480, 0x4c8, 0x4c9, 0x4cc, 0x4cd, 0x4ce,
    0x4d3, 0x500, 0x501, 0x502, 0x503, 0x504, 0x505, 0x506, 0x507, 0x508,
    0x509, 0x50a, 0x50b, 0x50c, 0x50d, 0x50e, 0x50f, 0x512, 0x514, 0x516,
    0x525, 0x550, 0x551, 0x559, 0x55d, 0x605, 0x608, 0x609, 0x620, 0x621,
    0x622, 0x623, 0x624, 0x625, 0x626, 0x627, 0x652, 0x63c, 0x63d, 0x63e,
    0x63f, 0x640, 0x66e, 0x700, 0x701, 0x702, 0x703, 0x708, 0x709, 0x70a,
    0x70b
};

UCHAR Rtlw8188eMacValues[] = {
    0x41, 0x35, 0x00, 0x0a, 0x10, 0x00, 0x01, 0x02, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x00, 0x00, 0x01, 0x02, 0x04, 0x05, 0x06,
    0x07, 0x5d, 0x01, 0x00, 0x15, 0xf0, 0x0f, 0x00, 0x41, 0xa8,
    0x72, 0xb9, 0x66, 0x66, 0x08, 0xff, 0x08, 0xff, 0xff, 0x01,
    0x01, 0x26, 0xa2, 0x2f, 0x00, 0x28, 0xa3, 0x5e, 0x00, 0x2b,
    0xa4, 0x5e, 0x00, 0x4f, 0xa4, 0x00, 0x00, 0x1c, 0x0a, 0x0a,
    0x4f, 0x10, 0x10, 0x02, 0xff, 0x30, 0x0e, 0x2a, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x20, 0x0a, 0x0a, 0x0e,
    0x0e, 0x40, 0x05, 0x21, 0x43, 0x65, 0x87, 0x21, 0x43, 0x65,
    0x87
};

//
// Store the device specific arrays of BB initialization registers.
//

USHORT RtlwDefaultBbRegisters[] = {
    0x024, 0x028, 0x800, 0x804, 0x808, 0x80c, 0x810, 0x814, 0x818, 0x81c,
    0x820, 0x824, 0x828, 0x82c, 0x830, 0x834, 0x838, 0x83c, 0x840, 0x844,
    0x848, 0x84c, 0x850, 0x854, 0x858, 0x85c, 0x860, 0x864, 0x868, 0x86c,
    0x870, 0x874, 0x878, 0x87c, 0x880, 0x884, 0x888, 0x88c, 0x890, 0x894,
    0x898, 0x89c, 0x900, 0x904, 0x908, 0x90c, 0xa00, 0xa04, 0xa08, 0xa0c,
    0xa10, 0xa14, 0xa18, 0xa1c, 0xa20, 0xa24, 0xa28, 0xa2c, 0xa70, 0xa74,
    0xc00, 0xc04, 0xc08, 0xc0c, 0xc10, 0xc14, 0xc18, 0xc1c, 0xc20, 0xc24,
    0xc28, 0xc2c, 0xc30, 0xc34, 0xc38, 0xc3c, 0xc40, 0xc44, 0xc48, 0xc4c,
    0xc50, 0xc54, 0xc58, 0xc5c, 0xc60, 0xc64, 0xc68, 0xc6c, 0xc70, 0xc74,
    0xc78, 0xc7c, 0xc80, 0xc84, 0xc88, 0xc8c, 0xc90, 0xc94, 0xc98, 0xc9c,
    0xca0, 0xca4, 0xca8, 0xcac, 0xcb0, 0xcb4, 0xcb8, 0xcbc, 0xcc0, 0xcc4,
    0xcc8, 0xccc, 0xcd0, 0xcd4, 0xcd8, 0xcdc, 0xce0, 0xce4, 0xce8, 0xcec,
    0xd00, 0xd04, 0xd08, 0xd0c, 0xd10, 0xd14, 0xd18, 0xd2c, 0xd30, 0xd34,
    0xd38, 0xd3c, 0xd40, 0xd44, 0xd48, 0xd4c, 0xd50, 0xd54, 0xd58, 0xd5c,
    0xd60, 0xd64, 0xd68, 0xd6c, 0xd70, 0xd74, 0xd78, 0xe00, 0xe04, 0xe08,
    0xe10, 0xe14, 0xe18, 0xe1c, 0xe28, 0xe30, 0xe34, 0xe38, 0xe3c, 0xe40,
    0xe44, 0xe48, 0xe4c, 0xe50, 0xe54, 0xe58, 0xe5c, 0xe60, 0xe68, 0xe6c,
    0xe70, 0xe74, 0xe78, 0xe7c, 0xe80, 0xe84, 0xe88, 0xe8c, 0xed0, 0xed4,
    0xed8, 0xedc, 0xee0, 0xeec, 0xf14, 0xf4c, 0xf00
};

USHORT Rtlw8188euBbRegisters[] = {
    0x800, 0x804, 0x808, 0x80c, 0x810, 0x814, 0x818, 0x81c, 0x820, 0x824,
    0x828, 0x82c, 0x830, 0x834, 0x838, 0x83c, 0x840, 0x844, 0x848, 0x84c,
    0x850, 0x854, 0x858, 0x85c, 0x860, 0x864, 0x868, 0x86c, 0x870, 0x874,
    0x878, 0x87c, 0x880, 0x884, 0x888, 0x88c, 0x890, 0x894, 0x898, 0x89c,
    0x900, 0x904, 0x908, 0x90c, 0x910, 0x914, 0xa00, 0xa04, 0xa08, 0xa0c,
    0xa10, 0xa14, 0xa18, 0xa1c, 0xa20, 0xa24, 0xa28, 0xa2c, 0xa70, 0xa74,
    0xa78, 0xa7c, 0xa80, 0xb2c, 0xc00, 0xc04, 0xc08, 0xc0c, 0xc10, 0xc14,
    0xc18, 0xc1c, 0xc20, 0xc24, 0xc28, 0xc2c, 0xc30, 0xc34, 0xc38, 0xc3c,
    0xc40, 0xc44, 0xc48, 0xc4c, 0xc50, 0xc54, 0xc58, 0xc5c, 0xc60, 0xc64,
    0xc68, 0xc6c, 0xc70, 0xc74, 0xc78, 0xc7c, 0xc80, 0xc84, 0xc88, 0xc8c,
    0xc90, 0xc94, 0xc98, 0xc9c, 0xca0, 0xca4, 0xca8, 0xcac, 0xcb0, 0xcb4,
    0xcb8, 0xcbc, 0xcc0, 0xcc4, 0xcc8, 0xccc, 0xcd0, 0xcd4, 0xcd8, 0xcdc,
    0xce0, 0xce4, 0xce8, 0xcec, 0xd00, 0xd04, 0xd08, 0xd0c, 0xd10, 0xd14,
    0xd18, 0xd2c, 0xd30, 0xd34, 0xd38, 0xd3c, 0xd40, 0xd44, 0xd48, 0xd4c,
    0xd50, 0xd54, 0xd58, 0xd5c, 0xd60, 0xd64, 0xd68, 0xd6c, 0xd70, 0xd74,
    0xd78, 0xe00, 0xe04, 0xe08, 0xe10, 0xe14, 0xe18, 0xe1c, 0xe28, 0xe30,
    0xe34, 0xe38, 0xe3c, 0xe40, 0xe44, 0xe48, 0xe4c, 0xe50, 0xe54, 0xe58,
    0xe5c, 0xe60, 0xe68, 0xe6c, 0xe70, 0xe74, 0xe78, 0xe7c, 0xe80, 0xe84,
    0xe88, 0xe8c, 0xed0, 0xed4, 0xed8, 0xedc, 0xee0, 0xee8, 0xeec, 0xf14,
    0xf4c, 0xf00
};

USHORT Rtlw8188ruBbRegisters[] = {
    0x024, 0x028, 0x040, 0x800, 0x804, 0x808, 0x80c, 0x810, 0x814, 0x818,
    0x81c, 0x820, 0x824, 0x828, 0x82c, 0x830, 0x834, 0x838, 0x83c, 0x840,
    0x844, 0x848, 0x84c, 0x850, 0x854, 0x858, 0x85c, 0x860, 0x864, 0x868,
    0x86c, 0x870, 0x874, 0x878, 0x87c, 0x880, 0x884, 0x888, 0x88c, 0x890,
    0x894, 0x898, 0x89c, 0x900, 0x904, 0x908, 0x90c, 0xa00, 0xa04, 0xa08,
    0xa0c, 0xa10, 0xa14, 0xa18, 0xa1c, 0xa20, 0xa24, 0xa28, 0xa2c, 0xa70,
    0xa74, 0xc00, 0xc04, 0xc08, 0xc0c, 0xc10, 0xc14, 0xc18, 0xc1c, 0xc20,
    0xc24, 0xc28, 0xc2c, 0xc30, 0xc34, 0xc38, 0xc3c, 0xc40, 0xc44, 0xc48,
    0xc4c, 0xc50, 0xc54, 0xc58, 0xc5c, 0xc60, 0xc64, 0xc68, 0xc6c, 0xc70,
    0xc74, 0xc78, 0xc7c, 0xc80, 0xc84, 0xc88, 0xc8c, 0xc90, 0xc94, 0xc98,
    0xc9c, 0xca0, 0xca4, 0xca8, 0xcac, 0xcb0, 0xcb4, 0xcb8, 0xcbc, 0xcc0,
    0xcc4, 0xcc8, 0xccc, 0xcd0, 0xcd4, 0xcd8, 0xcdc, 0xce0, 0xce4, 0xce8,
    0xcec, 0xd00, 0xd04, 0xd08, 0xd0c, 0xd10, 0xd14, 0xd18, 0xd2c, 0xd30,
    0xd34, 0xd38, 0xd3c, 0xd40, 0xd44, 0xd48, 0xd4c, 0xd50, 0xd54, 0xd58,
    0xd5c, 0xd60, 0xd64, 0xd68, 0xd6c, 0xd70, 0xd74, 0xd78, 0xe00, 0xe04,
    0xe08, 0xe10, 0xe14, 0xe18, 0xe1c, 0xe28, 0xe30, 0xe34, 0xe38, 0xe3c,
    0xe40, 0xe44, 0xe48, 0xe4c, 0xe50, 0xe54, 0xe58, 0xe5c, 0xe60, 0xe68,
    0xe6c, 0xe70, 0xe74, 0xe78, 0xe7c, 0xe80, 0xe84, 0xe88, 0xe8c, 0xed0,
    0xed4, 0xed8, 0xedc, 0xee0, 0xeec, 0xee8, 0xf14, 0xf4c, 0xf00
};

//
// Store the device specific arrays of BB initializationvalues.
//

ULONG Rtlw8188euBbValues[] = {
    0x80040000, 0x00000003, 0x0000fc00, 0x0000000a, 0x10001331,
    0x020c3d10, 0x02200385, 0x00000000, 0x01000100, 0x00390204,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00010000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x569a11a9, 0x01000014, 0x66f60110,
    0x061f0649, 0x00000000, 0x27272700, 0x07000760, 0x25004000,
    0x00000808, 0x00000000, 0xb0000c1c, 0x00000001, 0x00000000,
    0xccc000c0, 0x00000800, 0xfffffffe, 0x40302010, 0x00706050,
    0x00000000, 0x00000023, 0x00000000, 0x81121111, 0x00000002,
    0x00000201, 0x00d047c8, 0x80ff000c, 0x8c838300, 0x2e7f120f,
    0x9500bb78, 0x1114d028, 0x00881117, 0x89140f00, 0x1a1b0000,
    0x090e1317, 0x00000204, 0x00d30000, 0x101fbf00, 0x00000007,
    0x00000900, 0x225b0606, 0x218075b1, 0x80000000, 0x48071d40,
    0x03a05611, 0x000000e4, 0x6c6c6c6c, 0x08800000, 0x40000100,
    0x08800000, 0x40000100, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x69e9ac47, 0x469652af, 0x49795994, 0x0a97971c,
    0x1f7c403f, 0x000100b7, 0xec020107, 0x007f037f, 0x69553420,
    0x43bc0094, 0x00013169, 0x00250492, 0x00000000, 0x7112848b,
    0x47c00bff, 0x00000036, 0x2c7f000d, 0x020610db, 0x0000001f,
    0x00b91612, 0x390000e4, 0x20f60000, 0x40000100, 0x20200000,
    0x00091521, 0x00000000, 0x00121820, 0x00007f7f, 0x00000000,
    0x000300a0, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x28000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x64b22427, 0x00766932,
    0x00222222, 0x00000000, 0x37644302, 0x2f97d40c, 0x00000740,
    0x00020401, 0x0000907f, 0x20010201, 0xa0633333, 0x3333bc43,
    0x7a8f5b6f, 0xcc979975, 0x00000000, 0x80608000, 0x00000000,
    0x00127353, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x6437140a, 0x00000000, 0x00000282, 0x30032064, 0x4653de68,
    0x04518a3c, 0x00002101, 0x2a201c16, 0x1812362e, 0x322c2220,
    0x000e3c24, 0x2d2d2d2d, 0x2d2d2d2d, 0x0390272d, 0x2d2d2d2d,
    0x2d2d2d2d, 0x2d2d2d2d, 0x2d2d2d2d, 0x00000000, 0x1000dc1f,
    0x10008c1f, 0x02140102, 0x681604c2, 0x01007c00, 0x01004800,
    0xfb000000, 0x000028d1, 0x1000dc1f, 0x10008c1f, 0x02140102,
    0x28160d05, 0x00000008, 0x001b25a4, 0x00c00014, 0x00c00014,
    0x01000014, 0x01000014, 0x01000014, 0x01000014, 0x00c00014,
    0x01000014, 0x00c00014, 0x00c00014, 0x00c00014, 0x00c00014,
    0x00000014, 0x00000014, 0x21555448, 0x01c00014, 0x00000003,
    0x00000000, 0x00000300
};

ULONG Rtlw8188ceBbValues[] = {
    0x0011800d, 0x00ffdb83, 0x80040000, 0x00000001, 0x0000fc00,
    0x0000000a, 0x10005388, 0x020c3d10, 0x02200385, 0x00000000,
    0x01000100, 0x00390004, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00010000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x569a569a,
    0x001b25a4, 0x66e60230, 0x061f0130, 0x00000000, 0x32323200,
    0x07000700, 0x22004000, 0x00000808, 0x00000000, 0xc0083070,
    0x000004d5, 0x00000000, 0xccc000c0, 0x00000800, 0xfffffffe,
    0x40302010, 0x00706050, 0x00000000, 0x00000023, 0x00000000,
    0x81121111, 0x00d047c8, 0x80ff000c, 0x8c838300, 0x2e68120f,
    0x9500bb78, 0x11144028, 0x00881117, 0x89140f00, 0x1a1b0000,
    0x090e1317, 0x00000204, 0x00d30000, 0x101fbf00, 0x00000007,
    0x48071d40, 0x03a05611, 0x000000e4, 0x6c6c6c6c, 0x08800000,
    0x40000100, 0x08800000, 0x40000100, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x69e9ac44, 0x469652cf, 0x49795994,
    0x0a97971c, 0x1f7c403f, 0x000100b7, 0xec020107, 0x007f037f,
    0x6954341e, 0x43bc0094, 0x6954341e, 0x433c0094, 0x00000000,
    0x5116848b, 0x47c00bff, 0x00000036, 0x2c7f000d, 0x018610db,
    0x0000001f, 0x00b91612, 0x40000100, 0x20f60000, 0x40000100,
    0x20200000, 0x00121820, 0x00000000, 0x00121820, 0x00007f7f,
    0x00000000, 0x00000080, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x28000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x64b22427,
    0x00766932, 0x00222222, 0x00000000, 0x37644302, 0x2f97d40c,
    0x00080740, 0x00020401, 0x0000907f, 0x20010201, 0xa0633333,
    0x3333bc43, 0x7a8f5b6b, 0xcc979975, 0x00000000, 0x80608000,
    0x00000000, 0x00027293, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x6437140a, 0x00000000, 0x00000000, 0x30032064,
    0x4653de68, 0x04518a3c, 0x00002101, 0x2a201c16, 0x1812362e,
    0x322c2220, 0x000e3c24, 0x2a2a2a2a, 0x2a2a2a2a, 0x03902a2a,
    0x2a2a2a2a, 0x2a2a2a2a, 0x2a2a2a2a, 0x2a2a2a2a, 0x00000000,
    0x1000dc1f, 0x10008c1f, 0x02140102, 0x681604c2, 0x01007c00,
    0x01004800, 0xfb000000, 0x000028d1, 0x1000dc1f, 0x10008c1f,
    0x02140102, 0x28160d05, 0x00000008, 0x001b25a4, 0x631b25a0,
    0x631b25a0, 0x081b25a0, 0x081b25a0, 0x081b25a0, 0x081b25a0,
    0x631b25a0, 0x081b25a0, 0x631b25a0, 0x631b25a0, 0x631b25a0,
    0x631b25a0, 0x001b25a0, 0x001b25a0, 0x6b1b25a0, 0x00000003,
    0x00000000, 0x00000300
};

ULONG Rtlw8188cuBbValues[] = {
    0x0011800d, 0x00ffdb83, 0x80040000, 0x00000001, 0x0000fc00,
    0x0000000a, 0x10005388, 0x020c3d10, 0x02200385, 0x00000000,
    0x01000100, 0x00390004, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00010000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x569a569a,
    0x001b25a4, 0x66e60230, 0x061f0130, 0x00000000, 0x32323200,
    0x07000700, 0x22004000, 0x00000808, 0x00000000, 0xc0083070,
    0x000004d5, 0x00000000, 0xccc000c0, 0x00000800, 0xfffffffe,
    0x40302010, 0x00706050, 0x00000000, 0x00000023, 0x00000000,
    0x81121111, 0x00d047c8, 0x80ff000c, 0x8c838300, 0x2e68120f,
    0x9500bb78, 0x11144028, 0x00881117, 0x89140f00, 0x1a1b0000,
    0x090e1317, 0x00000204, 0x00d30000, 0x101fbf00, 0x00000007,
    0x48071d40, 0x03a05611, 0x000000e4, 0x6c6c6c6c, 0x08800000,
    0x40000100, 0x08800000, 0x40000100, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x69e9ac44, 0x469652cf, 0x49795994,
    0x0a97971c, 0x1f7c403f, 0x000100b7, 0xec020107, 0x007f037f,
    0x6954341e, 0x43bc0094, 0x6954341e, 0x433c0094, 0x00000000,
    0x5116848b, 0x47c00bff, 0x00000036, 0x2c7f000d, 0x018610db,
    0x0000001f, 0x00b91612, 0x40000100, 0x20f60000, 0x40000100,
    0x20200000, 0x00121820, 0x00000000, 0x00121820, 0x00007f7f,
    0x00000000, 0x00000080, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x28000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x64b22427,
    0x00766932, 0x00222222, 0x00000000, 0x37644302, 0x2f97d40c,
    0x00080740, 0x00020401, 0x0000907f, 0x20010201, 0xa0633333,
    0x3333bc43, 0x7a8f5b6b, 0xcc979975, 0x00000000, 0x80608000,
    0x00000000, 0x00027293, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x6437140a, 0x00000000, 0x00000000, 0x30032064,
    0x4653de68, 0x04518a3c, 0x00002101, 0x2a201c16, 0x1812362e,
    0x322c2220, 0x000e3c24, 0x2a2a2a2a, 0x2a2a2a2a, 0x03902a2a,
    0x2a2a2a2a, 0x2a2a2a2a, 0x2a2a2a2a, 0x2a2a2a2a, 0x00000000,
    0x1000dc1f, 0x10008c1f, 0x02140102, 0x681604c2, 0x01007c00,
    0x01004800, 0xfb000000, 0x000028d1, 0x1000dc1f, 0x10008c1f,
    0x02140102, 0x28160d05, 0x00000008, 0x001b25a4, 0x631b25a0,
    0x631b25a0, 0x081b25a0, 0x081b25a0, 0x081b25a0, 0x081b25a0,
    0x631b25a0, 0x081b25a0, 0x631b25a0, 0x631b25a0, 0x631b25a0,
    0x631b25a0, 0x001b25a0, 0x001b25a0, 0x6b1b25a0, 0x00000003,
    0x00000000, 0x00000300
};

ULONG Rtlw8188ruBbValues[] = {
    0x0011800d, 0x00ffdb83, 0x000c0004, 0x80040000, 0x00000001,
    0x0000fc00, 0x0000000a, 0x10005388, 0x020c3d10, 0x02200385,
    0x00000000, 0x01000100, 0x00390204, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00010000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x569a569a, 0x001b25a4, 0x66e60230, 0x061f0130, 0x00000000,
    0x32323200, 0x03000300, 0x22004000, 0x00000808, 0x00ffc3f1,
    0xc0083070, 0x000004d5, 0x00000000, 0xccc000c0, 0x00000800,
    0xfffffffe, 0x40302010, 0x00706050, 0x00000000, 0x00000023,
    0x00000000, 0x81121111, 0x00d047c8, 0x80ff000c, 0x8c838300,
    0x2e68120f, 0x9500bb78, 0x11144028, 0x00881117, 0x89140f00,
    0x15160000, 0x070b0f12, 0x00000104, 0x00d30000, 0x101fbf00,
    0x00000007, 0x48071d40, 0x03a05611, 0x000000e4, 0x6c6c6c6c,
    0x08800000, 0x40000100, 0x08800000, 0x40000100, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x69e9ac44, 0x469652cf,
    0x49795994, 0x0a97971c, 0x1f7c403f, 0x000100b7, 0xec020107,
    0x007f037f, 0x6954342e, 0x43bc0094, 0x6954342f, 0x433c0094,
    0x00000000, 0x5116848b, 0x47c00bff, 0x00000036, 0x2c56000d,
    0x018610db, 0x0000001f, 0x00b91612, 0x24000090, 0x20f60000,
    0x24000090, 0x20200000, 0x00121820, 0x00000000, 0x00121820,
    0x00007f7f, 0x00000000, 0x00000080, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x28000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x64b22427, 0x00766932, 0x00222222, 0x00000000, 0x37644302,
    0x2f97d40c, 0x00080740, 0x00020401, 0x0000907f, 0x20010201,
    0xa0633333, 0x3333bc43, 0x7a8f5b6b, 0xcc979975, 0x00000000,
    0x80608000, 0x00000000, 0x00027293, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x6437140a, 0x00000000, 0x00000000,
    0x30032064, 0x4653de68, 0x04518a3c, 0x00002101, 0x2a201c16,
    0x1812362e, 0x322c2220, 0x000e3c24, 0x2a2a2a2a, 0x2a2a2a2a,
    0x03902a2a, 0x2a2a2a2a, 0x2a2a2a2a, 0x2a2a2a2a, 0x2a2a2a2a,
    0x00000000, 0x1000dc1f, 0x10008c1f, 0x02140102, 0x681604c2,
    0x01007c00, 0x01004800, 0xfb000000, 0x000028d1, 0x1000dc1f,
    0x10008c1f, 0x02140102, 0x28160d05, 0x00000010, 0x001b25a4,
    0x631b25a0, 0x631b25a0, 0x081b25a0, 0x081b25a0, 0x081b25a0,
    0x081b25a0, 0x631b25a0, 0x081b25a0, 0x631b25a0, 0x631b25a0,
    0x631b25a0, 0x631b25a0, 0x001b25a0, 0x001b25a0, 0x6b1b25a0,
    0x31555448, 0x00000003, 0x00000000, 0x00000300
};

ULONG Rtlw8192ceBbValues[] = {
    0x0011800d, 0x00ffdb83, 0x80040002, 0x00000003, 0x0000fc00,
    0x0000000a, 0x10005388, 0x020c3d10, 0x02200385, 0x00000000,
    0x01000100, 0x00390004, 0x01000100, 0x00390004, 0x27272727,
    0x27272727, 0x27272727, 0x27272727, 0x00010000, 0x00010000,
    0x27272727, 0x27272727, 0x00000000, 0x00000000, 0x569a569a,
    0x0c1b25a4, 0x66e60230, 0x061f0130, 0x27272727, 0x2b2b2b27,
    0x07000700, 0x22184000, 0x08080808, 0x00000000, 0xc0083070,
    0x000004d5, 0x00000000, 0xcc0000c0, 0x00000800, 0xfffffffe,
    0x40302010, 0x00706050, 0x00000000, 0x00000023, 0x00000000,
    0x81121313, 0x00d047c8, 0x80ff000c, 0x8c838300, 0x2e68120f,
    0x9500bb78, 0x11144028, 0x00881117, 0x89140f00, 0x1a1b0000,
    0x090e1317, 0x00000204, 0x00d30000, 0x101fbf00, 0x00000007,
    0x48071d40, 0x03a05633, 0x000000e4, 0x6c6c6c6c, 0x08800000,
    0x40000100, 0x08800000, 0x40000100, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x69e9ac44, 0x469652cf, 0x49795994,
    0x0a97971c, 0x1f7c403f, 0x000100b7, 0xec020107, 0x007f037f,
    0x6954341e, 0x43bc0094, 0x6954341e, 0x433c0094, 0x00000000,
    0x5116848b, 0x47c00bff, 0x00000036, 0x2c7f000d, 0x018610db,
    0x0000001f, 0x00b91612, 0x40000100, 0x20f60000, 0x40000100,
    0x20200000, 0x00121820, 0x00000000, 0x00121820, 0x00007f7f,
    0x00000000, 0x00000080, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x28000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x64b22427,
    0x00766932, 0x00222222, 0x00000000, 0x37644302, 0x2f97d40c,
    0x00080740, 0x00020403, 0x0000907f, 0x20010201, 0xa0633333,
    0x3333bc43, 0x7a8f5b6b, 0xcc979975, 0x00000000, 0x80608000,
    0x00000000, 0x00027293, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x6437140a, 0x00000000, 0x00000000, 0x30032064,
    0x4653de68, 0x04518a3c, 0x00002101, 0x2a201c16, 0x1812362e,
    0x322c2220, 0x000e3c24, 0x2a2a2a2a, 0x2a2a2a2a, 0x03902a2a,
    0x2a2a2a2a, 0x2a2a2a2a, 0x2a2a2a2a, 0x2a2a2a2a, 0x00000000,
    0x1000dc1f, 0x10008c1f, 0x02140102, 0x681604c2, 0x01007c00,
    0x01004800, 0xfb000000, 0x000028d1, 0x1000dc1f, 0x10008c1f,
    0x02140102, 0x28160d05, 0x00000010, 0x001b25a4, 0x63db25a4,
    0x63db25a4, 0x0c1b25a4, 0x0c1b25a4, 0x0c1b25a4, 0x0c1b25a4,
    0x63db25a4, 0x0c1b25a4, 0x63db25a4, 0x63db25a4, 0x63db25a4,
    0x63db25a4, 0x001b25a4, 0x001b25a4, 0x6fdb25a4, 0x00000003,
    0x00000000, 0x00000300
};

ULONG Rtlw8192cuBbValues[] = {
    0x0011800d, 0x00ffdb83, 0x80040002, 0x00000003, 0x0000fc00,
    0x0000000a, 0x10005388, 0x020c3d10, 0x02200385, 0x00000000,
    0x01000100, 0x00390004, 0x01000100, 0x00390004, 0x27272727,
    0x27272727, 0x27272727, 0x27272727, 0x00010000, 0x00010000,
    0x27272727, 0x27272727, 0x00000000, 0x00000000, 0x569a569a,
    0x0c1b25a4, 0x66e60230, 0x061f0130, 0x27272727, 0x2b2b2b27,
    0x07000700, 0x22184000, 0x08080808, 0x00000000, 0xc0083070,
    0x000004d5, 0x00000000, 0xcc0000c0, 0x00000800, 0xfffffffe,
    0x40302010, 0x00706050, 0x00000000, 0x00000023, 0x00000000,
    0x81121313, 0x00d047c8, 0x80ff000c, 0x8c838300, 0x2e68120f,
    0x9500bb78, 0x11144028, 0x00881117, 0x89140f00, 0x1a1b0000,
    0x090e1317, 0x00000204, 0x00d30000, 0x101fbf00, 0x00000007,
    0x48071d40, 0x03a05633, 0x000000e4, 0x6c6c6c6c, 0x08800000,
    0x40000100, 0x08800000, 0x40000100, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x69e9ac44, 0x469652cf, 0x49795994,
    0x0a97971c, 0x1f7c403f, 0x000100b7, 0xec020107, 0x007f037f,
    0x6954341e, 0x43bc0094, 0x6954341e, 0x433c0094, 0x00000000,
    0x5116848b, 0x47c00bff, 0x00000036, 0x2c7f000d, 0x0186115b,
    0x0000001f, 0x00b99612, 0x40000100, 0x20f60000, 0x40000100,
    0x20200000, 0x00121820, 0x00000000, 0x00121820, 0x00007f7f,
    0x00000000, 0x00000080, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x28000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x64b22427,
    0x00766932, 0x00222222, 0x00000000, 0x37644302, 0x2f97d40c,
    0x00080740, 0x00020403, 0x0000907f, 0x20010201, 0xa0633333,
    0x3333bc43, 0x7a8f5b6b, 0xcc979975, 0x00000000, 0x80608000,
    0x00000000, 0x00027293, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x6437140a, 0x00000000, 0x00000000, 0x30032064,
    0x4653de68, 0x04518a3c, 0x00002101, 0x2a201c16, 0x1812362e,
    0x322c2220, 0x000e3c24, 0x2a2a2a2a, 0x2a2a2a2a, 0x03902a2a,
    0x2a2a2a2a, 0x2a2a2a2a, 0x2a2a2a2a, 0x2a2a2a2a, 0x00000000,
    0x1000dc1f, 0x10008c1f, 0x02140102, 0x681604c2, 0x01007c00,
    0x01004800, 0xfb000000, 0x000028d1, 0x1000dc1f, 0x10008c1f,
    0x02140102, 0x28160d05, 0x00000010, 0x001b25a4, 0x63db25a4,
    0x63db25a4, 0x0c1b25a4, 0x0c1b25a4, 0x0c1b25a4, 0x0c1b25a4,
    0x63db25a4, 0x0c1b25a4, 0x63db25a4, 0x63db25a4, 0x63db25a4,
    0x63db25a4, 0x001b25a4, 0x001b25a4, 0x6fdb25a4, 0x00000003,
    0x00000000, 0x00000300
};

//
// Store the device specific arrays of AGC initialization values.
//

ULONG Rtlw8188euAgcValues[] = {
    0xfb000001, 0xfb010001, 0xfb020001, 0xfb030001, 0xfb040001, 0xfb050001,
    0xfa060001, 0xf9070001, 0xf8080001, 0xf7090001, 0xf60a0001, 0xf50b0001,
    0xf40c0001, 0xf30d0001, 0xf20e0001, 0xf10f0001, 0xf0100001, 0xef110001,
    0xee120001, 0xed130001, 0xec140001, 0xeb150001, 0xea160001, 0xe9170001,
    0xe8180001, 0xe7190001, 0xe61a0001, 0xe51b0001, 0xe41c0001, 0xe31d0001,
    0xe21e0001, 0xe11f0001, 0x8a200001, 0x89210001, 0x88220001, 0x87230001,
    0x86240001, 0x85250001, 0x84260001, 0x83270001, 0x82280001, 0x6b290001,
    0x6a2a0001, 0x692b0001, 0x682c0001, 0x672d0001, 0x662e0001, 0x652f0001,
    0x64300001, 0x63310001, 0x62320001, 0x61330001, 0x46340001, 0x45350001,
    0x44360001, 0x43370001, 0x42380001, 0x41390001, 0x403a0001, 0x403b0001,
    0x403c0001, 0x403d0001, 0x403e0001, 0x403f0001, 0xfb400001, 0xfb410001,
    0xfb420001, 0xfb430001, 0xfb440001, 0xfb450001, 0xfb460001, 0xfb470001,
    0xfb480001, 0xfa490001, 0xf94a0001, 0xf84B0001, 0xf74c0001, 0xf64d0001,
    0xf54e0001, 0xf44f0001, 0xf3500001, 0xf2510001, 0xf1520001, 0xf0530001,
    0xef540001, 0xee550001, 0xed560001, 0xec570001, 0xeb580001, 0xea590001,
    0xe95a0001, 0xe85b0001, 0xe75c0001, 0xe65d0001, 0xe55e0001, 0xe45f0001,
    0xe3600001, 0xe2610001, 0xc3620001, 0xc2630001, 0xc1640001, 0x8b650001,
    0x8a660001, 0x89670001, 0x88680001, 0x87690001, 0x866a0001, 0x856b0001,
    0x846c0001, 0x676d0001, 0x666e0001, 0x656f0001, 0x64700001, 0x63710001,
    0x62720001, 0x61730001, 0x60740001, 0x46750001, 0x45760001, 0x44770001,
    0x43780001, 0x42790001, 0x417a0001, 0x407b0001, 0x407c0001, 0x407d0001,
    0x407e0001, 0x407f0001
};

ULONG Rtlw8188ruAgcValues[] = {
    0x7b000001, 0x7b010001, 0x7b020001, 0x7b030001, 0x7b040001, 0x7b050001,
    0x7b060001, 0x7b070001, 0x7b080001, 0x7a090001, 0x790a0001, 0x780b0001,
    0x770c0001, 0x760d0001, 0x750e0001, 0x740f0001, 0x73100001, 0x72110001,
    0x71120001, 0x70130001, 0x6f140001, 0x6e150001, 0x6d160001, 0x6c170001,
    0x6b180001, 0x6a190001, 0x691a0001, 0x681b0001, 0x671c0001, 0x661d0001,
    0x651e0001, 0x641f0001, 0x63200001, 0x62210001, 0x61220001, 0x60230001,
    0x46240001, 0x45250001, 0x44260001, 0x43270001, 0x42280001, 0x41290001,
    0x402a0001, 0x262b0001, 0x252c0001, 0x242d0001, 0x232e0001, 0x222f0001,
    0x21300001, 0x20310001, 0x06320001, 0x05330001, 0x04340001, 0x03350001,
    0x02360001, 0x01370001, 0x00380001, 0x00390001, 0x003a0001, 0x003b0001,
    0x003c0001, 0x003d0001, 0x003e0001, 0x003f0001, 0x7b400001, 0x7b410001,
    0x7b420001, 0x7b430001, 0x7b440001, 0x7b450001, 0x7b460001, 0x7b470001,
    0x7b480001, 0x7a490001, 0x794a0001, 0x784b0001, 0x774c0001, 0x764d0001,
    0x754e0001, 0x744f0001, 0x73500001, 0x72510001, 0x71520001, 0x70530001,
    0x6f540001, 0x6e550001, 0x6d560001, 0x6c570001, 0x6b580001, 0x6a590001,
    0x695a0001, 0x685b0001, 0x675c0001, 0x665d0001, 0x655e0001, 0x645f0001,
    0x63600001, 0x62610001, 0x61620001, 0x60630001, 0x46640001, 0x45650001,
    0x44660001, 0x43670001, 0x42680001, 0x41690001, 0x406a0001, 0x266b0001,
    0x256c0001, 0x246d0001, 0x236e0001, 0x226f0001, 0x21700001, 0x20710001,
    0x06720001, 0x05730001, 0x04740001, 0x03750001, 0x02760001, 0x01770001,
    0x00780001, 0x00790001, 0x007a0001, 0x007b0001, 0x007c0001, 0x007d0001,
    0x007e0001, 0x007f0001, 0x3800001e, 0x3801001e, 0x3802001e, 0x3803001e,
    0x3804001e, 0x3805001e, 0x3806001e, 0x3807001e, 0x3808001e, 0x3c09001e,
    0x3e0a001e, 0x400b001e, 0x440c001e, 0x480d001e, 0x4c0e001e, 0x500f001e,
    0x5210001e, 0x5611001e, 0x5a12001e, 0x5e13001e, 0x6014001e, 0x6015001e,
    0x6016001e, 0x6217001e, 0x6218001e, 0x6219001e, 0x621a001e, 0x621b001e,
    0x621c001e, 0x621d001e, 0x621e001e, 0x621f001e
};

ULONG RtlwDefaultAgcValues[] = {
    0x7b000001, 0x7b010001, 0x7b020001, 0x7b030001, 0x7b040001, 0x7b050001,
    0x7a060001, 0x79070001, 0x78080001, 0x77090001, 0x760a0001, 0x750b0001,
    0x740c0001, 0x730d0001, 0x720e0001, 0x710f0001, 0x70100001, 0x6f110001,
    0x6e120001, 0x6d130001, 0x6c140001, 0x6b150001, 0x6a160001, 0x69170001,
    0x68180001, 0x67190001, 0x661a0001, 0x651b0001, 0x641c0001, 0x631d0001,
    0x621e0001, 0x611f0001, 0x60200001, 0x49210001, 0x48220001, 0x47230001,
    0x46240001, 0x45250001, 0x44260001, 0x43270001, 0x42280001, 0x41290001,
    0x402a0001, 0x262b0001, 0x252c0001, 0x242d0001, 0x232e0001, 0x222f0001,
    0x21300001, 0x20310001, 0x06320001, 0x05330001, 0x04340001, 0x03350001,
    0x02360001, 0x01370001, 0x00380001, 0x00390001, 0x003a0001, 0x003b0001,
    0x003c0001, 0x003d0001, 0x003e0001, 0x003f0001, 0x7b400001, 0x7b410001,
    0x7b420001, 0x7b430001, 0x7b440001, 0x7b450001, 0x7a460001, 0x79470001,
    0x78480001, 0x77490001, 0x764a0001, 0x754b0001, 0x744c0001, 0x734d0001,
    0x724e0001, 0x714f0001, 0x70500001, 0x6f510001, 0x6e520001, 0x6d530001,
    0x6c540001, 0x6b550001, 0x6a560001, 0x69570001, 0x68580001, 0x67590001,
    0x665a0001, 0x655b0001, 0x645c0001, 0x635d0001, 0x625e0001, 0x615f0001,
    0x60600001, 0x49610001, 0x48620001, 0x47630001, 0x46640001, 0x45650001,
    0x44660001, 0x43670001, 0x42680001, 0x41690001, 0x406a0001, 0x266b0001,
    0x256c0001, 0x246d0001, 0x236e0001, 0x226f0001, 0x21700001, 0x20710001,
    0x06720001, 0x05730001, 0x04740001, 0x03750001, 0x02760001, 0x01770001,
    0x00780001, 0x00790001, 0x007a0001, 0x007b0001, 0x007c0001, 0x007d0001,
    0x007e0001, 0x007f0001, 0x3800001e, 0x3801001e, 0x3802001e, 0x3803001e,
    0x3804001e, 0x3805001e, 0x3806001e, 0x3807001e, 0x3808001e, 0x3c09001e,
    0x3e0a001e, 0x400b001e, 0x440c001e, 0x480d001e, 0x4c0e001e, 0x500f001e,
    0x5210001e, 0x5611001e, 0x5a12001e, 0x5e13001e, 0x6014001e, 0x6015001e,
    0x6016001e, 0x6217001e, 0x6218001e, 0x6219001e, 0x621a001e, 0x621b001e,
    0x621c001e, 0x621d001e, 0x621e001e, 0x621f001e
};

//
// Store the RF chain 1 registers and values.
//

UCHAR RtlwDefaultRf1Registers[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x09, 0x0a, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0x19, 0x1a, 0x1b, 0x1c,
    0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24,
    0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2a,
    0x2b, 0x2a, 0x2b, 0x2b, 0x2c, 0x2a, 0x2b, 0x2b,
    0x2c, 0x2a, 0x2b, 0x2b, 0x2c, 0x2a, 0x2b, 0x2b,
    0x2c, 0x2a, 0x2b, 0x2b, 0x2c, 0x2a, 0x2b, 0x2b,
    0x2c, 0x2a, 0x2b, 0x2b, 0x2c, 0x2a, 0x2b, 0x2b,
    0x2c, 0x2a, 0x2b, 0x2b, 0x2c, 0x2a, 0x2b, 0x2b,
    0x2c, 0x2a, 0x2b, 0x2b, 0x2c, 0x2a, 0x2b, 0x2b,
    0x2c, 0x2a, 0x2b, 0x2b, 0x2c, 0x2a, 0x2b, 0x2b,
    0x2c, 0x2a, 0x10, 0x11, 0x10, 0x11, 0x10, 0x11,
    0x10, 0x11, 0x10, 0x11, 0x10, 0x11, 0x10, 0x11,
    0x12, 0x12, 0x12, 0x12, 0x13, 0x13, 0x13, 0x13,
    0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x14,
    0x14, 0x14, 0x14, 0x15, 0x15, 0x15, 0x15, 0x16,
    0x16, 0x16, 0x16, 0x00, 0x18, 0xfe, 0xfe, 0x1f,
    0xfe, 0xfe, 0x1e, 0x1f, 0x00
};

UCHAR Rtlw8188euRf1Registers[] = {
    0x00, 0x08, 0x18, 0x19, 0x1e, 0x1f, 0x2f, 0x3f,
    0x42, 0x57, 0x58, 0x67, 0x83, 0xb0, 0xb1, 0xb2,
    0xb4, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbf,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xdf, 0xef, 0x51, 0x52, 0x53, 0x56, 0x35,
    0x35, 0x35, 0x36, 0x36, 0x36, 0x36, 0xb6, 0x18,
    0x5a, 0x19, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34,
    0x34, 0x34, 0x34, 0x34, 0x34, 0x00, 0x84, 0x86,
    0x87, 0x8e, 0x8f, 0xef, 0x3b, 0x3b, 0x3b, 0x3b,
    0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b, 0x3b,
    0x3b, 0x3b, 0x3b, 0x3b, 0xef, 0x00, 0x18, 0xfe,
    0xfe, 0x1f, 0xfe, 0xfe, 0x1e, 0x1f, 0x00
};

ULONG Rtlw8188ceRf1Values[] = {
    0x30159, 0x31284, 0x98000, 0x18c63, 0x210e7, 0x2044f, 0x1adb1, 0x54867,
    0x8992e, 0x0e52c, 0x39ce7, 0x00451, 0x00000, 0x10255, 0x60a00, 0xfc378,
    0xa1250, 0x4445f, 0x80001, 0x0b614, 0x6c000, 0x00000, 0x01558, 0x00060,
    0x00483, 0x4f200, 0xec7d9, 0x577c0, 0x04783, 0x00001, 0x21334, 0x00000,
    0x00054, 0x00001, 0x00808, 0x53333, 0x0000c, 0x00002, 0x00808, 0x5b333,
    0x0000d, 0x00003, 0x00808, 0x63333, 0x0000d, 0x00004, 0x00808, 0x6b333,
    0x0000d, 0x00005, 0x00808, 0x73333, 0x0000d, 0x00006, 0x00709, 0x5b333,
    0x0000d, 0x00007, 0x00709, 0x63333, 0x0000d, 0x00008, 0x0060a, 0x4b333,
    0x0000d, 0x00009, 0x0060a, 0x53333, 0x0000d, 0x0000a, 0x0060a, 0x5b333,
    0x0000d, 0x0000b, 0x0060a, 0x63333, 0x0000d, 0x0000c, 0x0060a, 0x6b333,
    0x0000d, 0x0000d, 0x0060a, 0x73333, 0x0000d, 0x0000e, 0x0050b, 0x66666,
    0x0001a, 0xe0000, 0x4000f, 0xe31fc, 0x6000f, 0xff9f8, 0x2000f, 0x203f9,
    0x3000f, 0xff500, 0x00000, 0x00000, 0x8000f, 0x3f100, 0x9000f, 0x23100,
    0x32000, 0x71000, 0xb0000, 0xfc000, 0x287b3, 0x244b7, 0x204ab, 0x1c49f,
    0x18493, 0x1429b, 0x10299, 0x0c29c, 0x081a0, 0x040ac, 0x00020, 0x1944c,
    0x59444, 0x9944c, 0xd9444, 0x0f424, 0x4f424, 0x8f424, 0xcf424, 0xe0330,
    0xa0330, 0x60330, 0x20330, 0x10159, 0x0f401, 0x00000, 0x00000, 0x80003,
    0x00000, 0x00000, 0x44457, 0x80000, 0x30159
};

ULONG Rtlw8188cuRf1Values[] = {
    0x30159, 0x31284, 0x98000, 0x18c63, 0x210e7, 0x2044f, 0x1adb1, 0x54867,
    0x8992e, 0x0e52c, 0x39ce7, 0x00451, 0x00000, 0x10255, 0x60a00, 0xfc378,
    0xa1250, 0x4445f, 0x80001, 0x0b614, 0x6c000, 0x00000, 0x01558, 0x00060,
    0x00483, 0x4f000, 0xec7d9, 0x577c0, 0x04783, 0x00001, 0x21334, 0x00000,
    0x00054, 0x00001, 0x00808, 0x53333, 0x0000c, 0x00002, 0x00808, 0x5b333,
    0x0000d, 0x00003, 0x00808, 0x63333, 0x0000d, 0x00004, 0x00808, 0x6b333,
    0x0000d, 0x00005, 0x00808, 0x73333, 0x0000d, 0x00006, 0x00709, 0x5b333,
    0x0000d, 0x00007, 0x00709, 0x63333, 0x0000d, 0x00008, 0x0060a, 0x4b333,
    0x0000d, 0x00009, 0x0060a, 0x53333, 0x0000d, 0x0000a, 0x0060a, 0x5b333,
    0x0000d, 0x0000b, 0x0060a, 0x63333, 0x0000d, 0x0000c, 0x0060a, 0x6b333,
    0x0000d, 0x0000d, 0x0060a, 0x73333, 0x0000d, 0x0000e, 0x0050b, 0x66666,
    0x0001a, 0xe0000, 0x4000f, 0xe31fc, 0x6000f, 0xff9f8, 0x2000f, 0x203f9,
    0x3000f, 0xff500, 0x00000, 0x00000, 0x8000f, 0x3f100, 0x9000f, 0x23100,
    0x32000, 0x71000, 0xb0000, 0xfc000, 0x287b3, 0x244b7, 0x204ab, 0x1c49f,
    0x18493, 0x1429b, 0x10299, 0x0c29c, 0x081a0, 0x040ac, 0x00020, 0x1944c,
    0x59444, 0x9944c, 0xd9444, 0x0f405, 0x4f405, 0x8f405, 0xcf405, 0xe0330,
    0xa0330, 0x60330, 0x20330, 0x10159, 0x0f401, 0x00000, 0x00000, 0x80003,
    0x00000, 0x00000, 0x44457, 0x80000, 0x30159
};

ULONG Rtlw8188euRf1Values[] = {
    0x30000, 0x84000, 0x00407, 0x00012, 0x80009, 0x00880, 0x1a060, 0x00000,
    0x060c0, 0xd0000, 0xbe180, 0x01552, 0x00000, 0xff8fc, 0x54400, 0xccc19,
    0x43003, 0x4953e, 0x1c718, 0x060ff, 0x80001, 0x40000, 0x00400, 0xc0000,
    0x02400, 0x00009, 0x40c91, 0x99999, 0x000a3, 0x88820, 0x76c06, 0x00000,
    0x80000, 0x00180, 0x001a0, 0x6b27d, 0x7e49d, 0x00073, 0x51ff3, 0x00086,
    0x00186, 0x00286, 0x01c25, 0x09c25, 0x11c25, 0x19c25, 0x48538, 0x00c07,
    0x4bd00, 0x739d0, 0x0adf3, 0x09df0, 0x08ded, 0x07dea, 0x06de7, 0x054ee,
    0x044eb, 0x034e8, 0x0246b, 0x01468, 0x0006d, 0x30159, 0x68200, 0x000ce,
    0x48a00, 0x65540, 0x88000, 0x020a0, 0xf02b0, 0xef7b0, 0xd4fb0, 0xcf060,
    0xb0090, 0xa0080, 0x90080, 0x8f780, 0x722b0, 0x6f7b0, 0x54fb0, 0x4f060,
    0x30090, 0x20080, 0x10080, 0x0f780, 0x000a0, 0x10159, 0x0f407, 0x00000,
    0x00000, 0x80003, 0x00000, 0x00000, 0x00001, 0x80000, 0x33e60
};

ULONG Rtlw8188ruRf1Values[] = {
    0x30159, 0x31284, 0x98000, 0x18c63, 0x210e7, 0x2044f, 0x1adb0, 0x54867,
    0x8992e, 0x0e529, 0x39ce7, 0x00451, 0x00000, 0x00255, 0x60a00, 0xfc378,
    0xa1250, 0x4445f, 0x80001, 0x0b614, 0x6c000, 0x0083c, 0x01558, 0x00060,
    0x00483, 0x4f000, 0xec7d9, 0x977c0, 0x04783, 0x00001, 0x21334, 0x00000,
    0x00054, 0x00001, 0x00808, 0x53333, 0x0000c, 0x00002, 0x00808, 0x5b333,
    0x0000d, 0x00003, 0x00808, 0x63333, 0x0000d, 0x00004, 0x00808, 0x6b333,
    0x0000d, 0x00005, 0x00808, 0x73333, 0x0000d, 0x00006, 0x00709, 0x5b333,
    0x0000d, 0x00007, 0x00709, 0x63333, 0x0000d, 0x00008, 0x0060a, 0x4b333,
    0x0000d, 0x00009, 0x0060a, 0x53333, 0x0000d, 0x0000a, 0x0060a, 0x5b333,
    0x0000d, 0x0000b, 0x0060a, 0x63333, 0x0000d, 0x0000c, 0x0060a, 0x6b333,
    0x0000d, 0x0000d, 0x0060a, 0x73333, 0x0000d, 0x0000e, 0x0050b, 0x66666,
    0x0001a, 0xe0000, 0x4000f, 0xe31fc, 0x6000f, 0xff9f8, 0x2000f, 0x203f9,
    0x3000f, 0xff500, 0x00000, 0x00000, 0x8000f, 0x3f100, 0x9000f, 0x23100,
    0xd8000, 0x90000, 0x51000, 0x12000, 0x28fb4, 0x24fa8, 0x207a4, 0x1c798,
    0x183a4, 0x14398, 0x101a4, 0x0c198, 0x080a4, 0x04098, 0x00014, 0x1944c,
    0x59444, 0x9944c, 0xd9444, 0x0f405, 0x4f405, 0x8f405, 0xcf405, 0xe0330,
    0xa0330, 0x60330, 0x20330, 0x10159, 0x0f401, 0x00000, 0x00000, 0x80003,
    0x00000, 0x00000, 0x44457, 0x80000, 0x30159
};

ULONG RtlwDefaultRf1Values[] = {
    0x30159, 0x31284, 0x98000, 0x18c63, 0x210e7, 0x2044f, 0x1adb1, 0x54867,
    0x8992e, 0x0e52c, 0x39ce7, 0x00451, 0x00000, 0x10255, 0x60a00, 0xfc378,
    0xa1250, 0x4445f, 0x80001, 0x0b614, 0x6c000, 0x00000, 0x01558, 0x00060,
    0x00483, 0x4f000, 0xec7d9, 0x577c0, 0x04783, 0x00001, 0x21334, 0x00000,
    0x00054, 0x00001, 0x00808, 0x53333, 0x0000c, 0x00002, 0x00808, 0x5b333,
    0x0000d, 0x00003, 0x00808, 0x63333, 0x0000d, 0x00004, 0x00808, 0x6b333,
    0x0000d, 0x00005, 0x00808, 0x73333, 0x0000d, 0x00006, 0x00709, 0x5b333,
    0x0000d, 0x00007, 0x00709, 0x63333, 0x0000d, 0x00008, 0x0060a, 0x4b333,
    0x0000d, 0x00009, 0x0060a, 0x53333, 0x0000d, 0x0000a, 0x0060a, 0x5b333,
    0x0000d, 0x0000b, 0x0060a, 0x63333, 0x0000d, 0x0000c, 0x0060a, 0x6b333,
    0x0000d, 0x0000d, 0x0060a, 0x73333, 0x0000d, 0x0000e, 0x0050b, 0x66666,
    0x0001a, 0xe0000, 0x4000f, 0xe31fc, 0x6000f, 0xff9f8, 0x2000f, 0x203f9,
    0x3000f, 0xff500, 0x00000, 0x00000, 0x8000f, 0x3f100, 0x9000f, 0x23100,
    0x32000, 0x71000, 0xb0000, 0xfc000, 0x287af, 0x244b7, 0x204ab, 0x1c49f,
    0x18493, 0x14297, 0x10295, 0x0c298, 0x0819c, 0x040a8, 0x0001c, 0x1944c,
    0x59444, 0x9944c, 0xd9444, 0x0f424, 0x4f424, 0x8f424, 0xcf424, 0xe0330,
    0xa0330, 0x60330, 0x20330, 0x10159, 0x0f401, 0x00000, 0x00000, 0x80003,
    0x00000, 0x00000, 0x44457, 0x80000, 0x30159
};

//
// Store the RF chain 2 registers and values.
//

UCHAR RtlwDefaultRf2Registers[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x09, 0x0a, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0x12, 0x12, 0x12, 0x12,
    0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13,
    0x13, 0x13, 0x13, 0x14, 0x14, 0x14, 0x14, 0x15,
    0x15, 0x15, 0x15, 0x16, 0x16, 0x16, 0x16
};

ULONG RtlwDefaultRf2Values[] = {
    0x30159, 0x31284, 0x98000, 0x18c63, 0x210e7, 0x2044f, 0x1adb1, 0x54867,
    0x8992e, 0x0e52c, 0x39ce7, 0x00451, 0x32000, 0x71000, 0xb0000, 0xfc000,
    0x287af, 0x244b7, 0x204ab, 0x1c49f, 0x18493, 0x14297, 0x10295, 0x0c298,
    0x0819c, 0x040a8, 0x0001c, 0x1944c, 0x59444, 0x9944c, 0xd9444, 0x0f424,
    0x4f424, 0x8f424, 0xcf424, 0xe0330, 0xa0330, 0x60330, 0x20330
};

RTLW81_DEVICE_DATA Rtlw8188euDeviceData = {
    Rtlw8188euBbRegisters,
    Rtlw8188euBbValues,
    RTLW81_ARRAY_COUNT(Rtlw8188euBbRegisters),
    Rtlw8188euAgcValues,
    RTLW81_ARRAY_COUNT(Rtlw8188euAgcValues),
    {
        Rtlw8188euRf1Registers,
        NULL
    },

    {
        Rtlw8188euRf1Values,
        NULL
    },

    {
        RTLW81_ARRAY_COUNT(Rtlw8188euRf1Registers),
        0
    }
};

RTLW81_DEVICE_DATA Rtlw8188ceDeviceData = {
    RtlwDefaultBbRegisters,
    Rtlw8188ceBbValues,
    RTLW81_ARRAY_COUNT(RtlwDefaultBbRegisters),
    RtlwDefaultAgcValues,
    RTLW81_ARRAY_COUNT(RtlwDefaultAgcValues),
    {
        RtlwDefaultRf1Registers,
        NULL
    },

    {
        Rtlw8188ceRf1Values,
        NULL
    },

    {
        RTLW81_ARRAY_COUNT(RtlwDefaultRf1Registers),
        0
    }
};

RTLW81_DEVICE_DATA Rtlw8188ruDeviceData = {
    Rtlw8188ruBbRegisters,
    Rtlw8188ruBbValues,
    RTLW81_ARRAY_COUNT(Rtlw8188ruBbRegisters),
    Rtlw8188ruAgcValues,
    RTLW81_ARRAY_COUNT(Rtlw8188ruAgcValues),
    {
        RtlwDefaultRf1Registers,
        NULL
    },

    {
        Rtlw8188ruRf1Values,
        NULL
    },

    {
        RTLW81_ARRAY_COUNT(RtlwDefaultRf1Registers),
        0
    }
};

RTLW81_DEVICE_DATA Rtlw8188cuDeviceData = {
    RtlwDefaultBbRegisters,
    Rtlw8188cuBbValues,
    RTLW81_ARRAY_COUNT(RtlwDefaultBbRegisters),
    RtlwDefaultAgcValues,
    RTLW81_ARRAY_COUNT(RtlwDefaultAgcValues),
    {
        RtlwDefaultRf1Registers,
        NULL
    },

    {
        Rtlw8188cuRf1Values,
        NULL
    },

    {
        RTLW81_ARRAY_COUNT(RtlwDefaultRf1Registers),
        0
    }
};

RTLW81_DEVICE_DATA Rtlw8192ceDeviceData = {
    RtlwDefaultBbRegisters,
    Rtlw8192ceBbValues,
    RTLW81_ARRAY_COUNT(RtlwDefaultBbRegisters),
    RtlwDefaultAgcValues,
    RTLW81_ARRAY_COUNT(RtlwDefaultAgcValues),
    {
        RtlwDefaultRf1Registers,
        RtlwDefaultRf2Registers
    },

    {
        RtlwDefaultRf1Values,
        RtlwDefaultRf2Values
    },

    {
        RTLW81_ARRAY_COUNT(RtlwDefaultRf1Registers),
        RTLW81_ARRAY_COUNT(RtlwDefaultRf2Registers),
    }
};

RTLW81_DEVICE_DATA Rtlw8192cuDeviceData = {
    RtlwDefaultBbRegisters,
    Rtlw8192cuBbValues,
    RTLW81_ARRAY_COUNT(RtlwDefaultBbRegisters),
    RtlwDefaultAgcValues,
    RTLW81_ARRAY_COUNT(RtlwDefaultAgcValues),
    {
        RtlwDefaultRf1Registers,
        RtlwDefaultRf2Registers
    },

    {
        RtlwDefaultRf1Values,
        RtlwDefaultRf2Values
    },

    {
        RTLW81_ARRAY_COUNT(RtlwDefaultRf1Registers),
        RTLW81_ARRAY_COUNT(RtlwDefaultRf2Registers),
    }
};

RTLW81_DEFAULT_TRANSMIT_POWER_DATA Rtlw8188ruTransmitPowerData[] = {
    {
        {
            {
                0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x08, 0x06, 0x06, 0x04,
                0x04, 0x00, 0x08, 0x06, 0x06, 0x04, 0x04, 0x02, 0x02, 0x00,
                0x08, 0x06, 0x06, 0x04, 0x04, 0x02, 0x02, 0x00
            },

            {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            },

            {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            }
        }
    }
};

RTLW81_DEFAULT_TRANSMIT_POWER_DATA RtlwDefaultTransmitPowerData[] = {
    {
        {
            {
                0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c, 0x0c, 0x0a, 0x08, 0x06,
                0x04, 0x02, 0x0e, 0x0d, 0x0c, 0x0a, 0x08, 0x06, 0x04, 0x02,
                0x0e, 0x0d, 0x0c, 0x0a, 0x08, 0x06, 0x04, 0x02
            },

            {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            },

            {
                0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x04, 0x04, 0x04, 0x02,
                0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            }
        }
    },

    {
        {
            {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            },

            {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            },

            {
                0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x04, 0x04, 0x04, 0x02,
                0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            }
        }
    }
};

RTLW81_8188E_TRANSMIT_POWER_DATA Rtlw8188eTransmitPowerData[] = {
    {
        {
            {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            },

            {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            },

            {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            },

            {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            },

            {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            },

            {
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            }
        }
    }
};

BOOL Rtlw81DisablePacketDropping = FALSE;

//
// ------------------------------------------------------------------ Functions
//

KSTATUS
Rtlw81Send (
    PVOID DriverContext,
    PNET_PACKET_LIST PacketList
    )

/*++

Routine Description:

    This routine sends data through the network.

Arguments:

    DriverContext - Supplies a pointer to the driver context associated with the
        link down which this data is to be sent.

    PacketList - Supplies a pointer to a list of network packets to send. Data
        in these packets may be modified by this routine, but must not be used
        once this routine returns.

Return Value:

    STATUS_SUCCESS if all packets were sent.

    STATUS_RESOURCE_IN_USE if some or all of the packets were dropped due to
    the hardware being backed up with too many packets to send.

    Other failure codes indicate that none of the packets were sent.

--*/

{

    RTLW81_BULK_OUT_TYPE BulkOutType;
    USHORT Checksum;
    ULONG DataRate;
    ULONG DataSize;
    PRTLW81_DEVICE Device;
    PRTLW81_TRANSMIT_HEADER Header;
    PUSHORT HeaderBuffer;
    ULONG Index;
    ULONG MacId;
    PNET80211_FRAME_HEADER Net80211Header;
    ULONG Net80211Type;
    PNET_PACKET_BUFFER Packet;
    ULONG QueueSelect;
    ULONG Raid;
    PRTLW81_BULK_OUT_TRANSFER Rtlw81Transfer;
    KSTATUS Status;
    PUSB_TRANSFER UsbTransfer;

    Device = (PRTLW81_DEVICE)DriverContext;

    //
    // If there are more bulk out transfers in transit that allowed, drop all
    // of these packets.
    //

    if ((Device->BulkOutTransferCount >= RTLW81_MAX_BULK_OUT_TRANSFER_COUNT) &&
        (Rtlw81DisablePacketDropping == FALSE)) {

        return STATUS_RESOURCE_IN_USE;
    }

    //
    // Otherwise submit all the packets. This may stretch over the maximum
    // number of bulk out transfers, but it's a flexible line.
    //

    while (NET_PACKET_LIST_EMPTY(PacketList) == FALSE) {
        Packet = LIST_VALUE(PacketList->Head.Next,
                            NET_PACKET_BUFFER,
                            ListEntry);

        NET_REMOVE_PACKET_FROM_LIST(Packet, PacketList);

        ASSERT(IS_ALIGNED(Packet->BufferSize, MmGetIoBufferAlignment()) !=
               FALSE);

        ASSERT(IS_ALIGNED((UINTN)Packet->Buffer,
                          MmGetIoBufferAlignment()) != FALSE);

        ASSERT(IS_ALIGNED((UINTN)Packet->BufferPhysicalAddress,
                          MmGetIoBufferAlignment()) != FALSE);

        //
        // There might be legitimate reasons for this assert to be spurious,
        // but most likely this assert fired because something in the
        // networking stack failed to properly allocate the required header
        // space. Go figure out who allocated this packet.
        //

        ASSERT(Packet->DataOffset == RTLW81_TRANSMIT_HEADER_SIZE);

        Net80211Header = Packet->Buffer + Packet->DataOffset;
        DataSize = Packet->FooterOffset - Packet->DataOffset;

        ASSERT(DataSize <= MAX_USHORT);

        Packet->DataOffset -= RTLW81_TRANSMIT_HEADER_SIZE;
        Header = Packet->Buffer;
        RtlZeroMemory(Header, RTLW81_TRANSMIT_HEADER_SIZE);
        Header->PacketLength = DataSize;
        Header->Offset = RTLW81_TRANSMIT_HEADER_SIZE;
        Header->TypeFlags = RTLW81_TRANSMIT_TYPE_FLAG_FIRST_SEGMENT |
                            RTLW81_TRANSMIT_TYPE_FLAG_LAST_SEGMENT |
                            RTLW81_TRANSMIT_TYPE_FLAG_OWN;

        //
        // Pick an endpoint based on the 802.11 frame type.
        //

        Net80211Type = NET80211_GET_FRAME_TYPE(Net80211Header);
        if ((Net80211Type == NET80211_FRAME_TYPE_CONTROL) ||
            (Net80211Type == NET80211_FRAME_TYPE_MANAGEMENT)) {

            BulkOutType = Rtlw81BulkOutVo;

        } else {
            BulkOutType = Rtlw81BulkOutBe;
        }

        //
        // Assume the default values for various fields in the header.
        //

        DataRate = RTLW81_TRANSMIT_DATA_RATE_INFORMATION_DATA_RATE_CCK1;
        MacId = RTLW81_TRANSMIT_IDENTIFICATION_MAC_ID_BSS;
        QueueSelect = RTLW81_TRANSMIT_IDENTIFICATION_QSEL_MGMT;
        Raid = RTLW81_TRANSMIT_IDENTIFICATION_RAID_11B;

        //
        // Handle non-multicast requests to send 802.11 data packets.
        //

        if ((NET80211_IS_MULTICAST_BROADCAST(Net80211Header) == FALSE) &&
            (Net80211Type == NET80211_FRAME_TYPE_DATA)) {

            //
            // TODO: Get the current IEEE802.11 mode.
            //

            Raid = RTLW81_TRANSMIT_IDENTIFICATION_RAID_11BG;
            QueueSelect = RTLW81_TRANSMIT_IDENTIFICATION_QSEL_BE;
            if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
                Header->AggBkFlag |= RTLW81_TRANSMIT_AGG_BK_FLAG;

            } else {
                Header->Identification |= RTLW81_TRANSMIT_IDENTIFICATION_AGG_BK;
            }

            //
            // TODO: Modify the rate information based on 802.11 protocol.
            //

            Header->RateInformation |=
                            (RTLW81_TRANSMIT_RATE_INFORMATION_RTSRATE_OFDM24 <<
                             RTLW81_TRANSMIT_RATE_INFORMATION_RTSRATE_SHIFT) &
                            RTLW81_TRANSMIT_RATE_INFORMATION_RTSRATE_MASK;

            Header->DataRateInformation |=
                                  RTLW81_TRANSMIT_DATA_RATE_INFORMATION_OFDM24;

            DataRate = RTLW81_TRANSMIT_DATA_RATE_INFORMATION_DATA_RATE_OFDM54;

        //
        // Handle multicast packets.
        //

        } else if (NET80211_IS_MULTICAST_BROADCAST(Net80211Header) != FALSE) {
            Header->TypeFlags |= RTLW81_TRANSMIT_TYPE_FLAG_MULTICAST_BROADCAST;
            MacId = RTLW81_TRANSMIT_IDENTIFICATION_MAC_ID_BROADCAST;
        }

        Header->Identification |=
                       (MacId << RTLW81_TRANSMIT_IDENTIFICATION_MAC_ID_SHIFT) &
                       RTLW81_TRANSMIT_IDENTIFICATION_MAC_ID_MASK;

        Header->Identification |=
                   (QueueSelect << RTLW81_TRANSMIT_IDENTIFICATION_QSEL_SHIFT) &
                   RTLW81_TRANSMIT_IDENTIFICATION_QSEL_MASK;

        Header->Identification |=
                          (Raid << RTLW81_TRANSMIT_IDENTIFICATION_RAID_SHIFT) &
                          RTLW81_TRANSMIT_IDENTIFICATION_RAID_MASK;

        Header->DataRateInformation |=
                      (DataRate <<
                       RTLW81_TRANSMIT_DATA_RATE_INFORMATION_DATA_RATE_SHIFT) &
                      RTLW81_TRANSMIT_DATA_RATE_INFORMATION_DATA_RATE_MASK;

        if (DataRate == RTLW81_TRANSMIT_DATA_RATE_INFORMATION_DATA_RATE_CCK1) {
            Header->RateInformation |= RTLW81_TRANSMIT_RATE_INFORMATION_DRVRATE;
        }

        //
        // Unless it is a QOS Data packet, use hardware sequence numbering.
        //

        if ((Net80211Type != NET80211_FRAME_TYPE_DATA) ||
            (NET80211_GET_FRAME_SUBTYPE(Net80211Header) !=
             NET80211_DATA_FRAME_SUBTYPE_QOS_DATA)) {

            Header->RateInformation |= RTLW81_TRANSMIT_RATE_INFORMATION_HWSEQ;
            Header->Sequence |= RTLW81_TRANSMIT_SEQUENCE_PACKET_ID;

        } else {
            Header->Sequence = NET80211_GET_SEQUENCE_NUMBER(Net80211Header);
        }

        //
        // Compute the 16-bit XOR checksum of the header.
        //

        Checksum = 0;
        HeaderBuffer = (PUSHORT)Header;
        Header->HeaderChecksum = 0;
        for (Index = 0;
             Index < (RTLW81_TRANSMIT_HEADER_SIZE / sizeof(USHORT));
             Index += 1) {

            Checksum ^= HeaderBuffer[Index];
        }

        Header->HeaderChecksum = Checksum;

        //
        // Allocate a transfer for this packet. All packets need to be dealt
        // with, so if the allocation or submission fails then free the buffer.
        //

        Rtlw81Transfer = Rtlw81pAllocateBulkOutTransfer(Device, BulkOutType);
        if (Rtlw81Transfer == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            RtlDebugPrint("RTLW81: Failed to allocate transfer.\n");
            NetFreeBuffer(Packet);
            break;
        }

        Rtlw81Transfer->Packet = Packet;
        UsbTransfer = Rtlw81Transfer->UsbTransfer;
        UsbTransfer->Length = Packet->FooterOffset;
        UsbTransfer->BufferActualLength = Packet->BufferSize;
        UsbTransfer->Buffer = Header;
        UsbTransfer->BufferPhysicalAddress = Packet->BufferPhysicalAddress;
        RtlAtomicAdd32(&(Device->BulkOutTransferCount), 1);
        Status = UsbSubmitTransfer(UsbTransfer);
        if (!KSUCCESS(Status)) {
            RtlDebugPrint("RTLW81: Failed to submit transmit packet: %x\n",
                          Status);

            Rtlw81pFreeBulkOutTransfer(Rtlw81Transfer);
            NetFreeBuffer(Packet);
            RtlAtomicAdd32(&(Device->BulkOutTransferCount), -1);
            break;
        }
    }

    return Status;
}

KSTATUS
Rtlw81GetSetInformation (
    PVOID DriverContext,
    NET_LINK_INFORMATION_TYPE InformationType,
    PVOID Data,
    PUINTN DataSize,
    BOOL Set
    )

/*++

Routine Description:

    This routine gets or sets the network device layer's link information.

Arguments:

    DriverContext - Supplies a pointer to the driver context associated with the
        link for which information is being set or queried.

    InformationType - Supplies the type of information being queried or set.

    Data - Supplies a pointer to the data buffer where the data is either
        returned for a get operation or given for a set operation.

    DataSize - Supplies a pointer that on input contains the size of the data
        buffer. On output, contains the required size of the data buffer.

    Set - Supplies a boolean indicating if this is a get operation (FALSE) or a
        set operation (TRUE).

Return Value:

    Status code.

--*/

{

    PULONG Flags;
    KSTATUS Status;

    switch (InformationType) {
    case NetLinkInformationChecksumOffload:
        if (*DataSize != sizeof(ULONG)) {
            return STATUS_INVALID_PARAMETER;
        }

        if (Set != FALSE) {
            return STATUS_NOT_SUPPORTED;
        }

        Flags = (PULONG)Data;
        *Flags = 0;
        break;

    default:
        Status = STATUS_NOT_SUPPORTED;
        break;
    }

    return Status;
}

KSTATUS
Rtlw81SetChannel (
    PVOID DriverContext,
    ULONG Channel
    )

/*++

Routine Description:

    This routine sets the 802.11 link's channel to the given value.

Arguments:

    DriverContext - Supplies a pointer to the driver context associated with
        the 802.11 link whose channel is to be set.

    Channel - Supplies the channel to which the device should be set.

Return Value:

    Status code.

--*/

{

    PRTLW81_DEVICE Device;

    Device = (PRTLW81_DEVICE)DriverContext;
    Rtlw81pSetChannel(Device, Channel);
    return STATUS_SUCCESS;
}

KSTATUS
Rtlw81SetState (
    PVOID DriverContext,
    NET80211_STATE State,
    PNET80211_BSS_INFORMATION BssInformation
    )

/*++

Routine Description:

    This routine sets the 802.11 link to the given state. State information is
    provided to communicate the details of the 802.11 core's current state.

Arguments:

    DriverContext - Supplies a pointer to the driver context associated with
        the 802.11 link whose state is to be set.

    State - Supplies the state to which the link is being set.

    BssInformation - Supplies a pointer to the BSS information collected by the
        802.11 core.

Return Value:

    Status code.

--*/

{

    ULONG BasicRates;
    ULONG BeaconInterval;
    ULONG BssIndex;
    UCHAR BssRate;
    PRTLW81_DEVICE Device;
    ULONG LocalIndex;
    UCHAR LocalRate;
    RTLW81_MAC_ID_CONFIG_COMMAND MacIdCommand;
    ULONG MaxBasicRateIndex;
    ULONG MaxRateIndex;
    ULONG Rates;
    USHORT Register;
    KSTATUS Status;
    ULONGLONG Timestamp;
    ULONG Value;

    Device = DriverContext;
    Status = STATUS_SUCCESS;
    switch (State) {
    case Net80211StateProbing:

        //
        // Receive frames from all BSSIDs during the probing state.
        //

        Value = RTLW81_READ_REGISTER32(Device,
                                       Rtlw81RegisterReceiveConfiguration);

        Value &= ~(RTLW81_RECEIVE_CONFIGURATION_CBSSID_DATA |
                   RTLW81_RECEIVE_CONFIGURATION_CBSSID_BCN);

        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterReceiveConfiguration,
                                Value);

        //
        // Set the gain used in the probing state.
        //

        Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterOfdm0AgcCore1);
        Value &= ~RTLW81_OFDM0_AGC_CORE1_GAIN_MASK;
        Value |= RTLW81_OFDM0_AGC_CORE1_GAIN_PROBE_VALUE;
        RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterOfdm0AgcCore1, Value);
        if ((Device->Flags & RTLW81_FLAG_8188E) == 0) {
            Register = Rtlw81RegisterOfdm0AgcCore1 + 8;
            Value = RTLW81_READ_REGISTER32(Device, Register);
            Value &= ~RTLW81_OFDM0_AGC_CORE1_GAIN_MASK;
            Value |= RTLW81_OFDM0_AGC_CORE1_GAIN_PROBE_VALUE;
            RTLW81_WRITE_REGISTER32(Device, Register, Value);
        }

        break;

    case Net80211StateAuthenticating:

        //
        // Set the gain used in the authenticating state.
        //

        Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterOfdm0AgcCore1);
        Value &= ~RTLW81_OFDM0_AGC_CORE1_GAIN_MASK;
        Value |= RTLW81_OFDM0_AGC_CORE1_GAIN_AUTHENTICATE_VALUE;
        RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterOfdm0AgcCore1, Value);
        if ((Device->Flags & RTLW81_FLAG_8188E) == 0) {
            Register = Rtlw81RegisterOfdm0AgcCore1 + 8;
            Value = RTLW81_READ_REGISTER32(Device, Register);
            Value &= ~RTLW81_OFDM0_AGC_CORE1_GAIN_MASK;
            Value |= RTLW81_OFDM0_AGC_CORE1_GAIN_AUTHENTICATE_VALUE;
            RTLW81_WRITE_REGISTER32(Device, Register, Value);
        }

        break;

    case Net80211StateAssociated:

        //
        // Set the network type to associated.
        //

        Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterConfiguration);
        Value &= ~RTLW81_CONFIGURATION_NETWORK_TYPE_MASK;
        Value |= (RTLW81_CONFIGURATION_NETWORK_TYPE_INFRA <<
                  RTLW81_CONFIGURATION_NETWORK_TYPE_SHIFT);

        RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterConfiguration, Value);

        //
        // Filter out traffic that is not coming from the BSSID.
        //

        Value = *((PULONG)&(BssInformation->Bssid[0]));
        RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterBssid0, Value);
        Value = *((PUSHORT)&(BssInformation->Bssid[4]));
        RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterBssid1, Value);

        //
        // Set the rate for 11b/g.
        //

        RTLW81_WRITE_REGISTER8(Device,
                               Rtlw81RegisterIniRtsRateSelect,
                               RTLW81_INI_RTS_RATE_SELECT_11BG);

        //
        // Accept all data frames.
        //

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterReceiveDataFilter,
                                0xFFFF);

        //
        // Enable transmit.
        //

        RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterTransmitPause, 0);

        //
        // Set the beacon interval.
        //

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterBeaconInterval,
                                BssInformation->BeaconInterval);

        //
        // Enable filtering based on the BSSID.
        //

        Value = RTLW81_READ_REGISTER32(Device,
                                       Rtlw81RegisterReceiveConfiguration);

        Value |= RTLW81_RECEIVE_CONFIGURATION_CBSSID_BCN |
                 RTLW81_RECEIVE_CONFIGURATION_CBSSID_DATA;

        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterReceiveConfiguration,
                                Value);

        //
        // Initialize TSF for the device. This keeps it in sync with the rest
        // of the BSS.
        //

        Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterBeaconControl);
        Value &= ~RTLW81_BEACON_CONTROL_DISABLE_TSF_UDT0;
        RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterBeaconControl, Value);
        Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterBeaconControl);
        Value &= ~RTLW81_BEACON_CONTROL_ENABLE_BEACON;
        RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterBeaconControl, Value);
        Timestamp = BssInformation->Timestamp;
        BeaconInterval = BssInformation->BeaconInterval * NET80211_TIME_UNIT;
        Timestamp -= Timestamp % BeaconInterval;
        Timestamp -= NET80211_TIME_UNIT;
        RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterTsftr0, (ULONG)Timestamp);
        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterTsftr1,
                                (ULONG)(Timestamp >> 32));

        Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterBeaconControl);
        Value |= RTLW81_BEACON_CONTROL_ENABLE_BEACON;
        RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterBeaconControl, Value);

        //
        // Update the SIFS registers.
        //

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterSifsCck,
                                RTLW81_SIFS_CCK_ASSOCIATED);

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterSifsOfdm,
                                RTLW81_SIFS_OFDM_ASSOCIATED);

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterSpecSifs,
                                RTLW81_SPEC_SIFS_ASSOCIATED);

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterMacSpecSifs,
                                RTLW81_MAC_SPEC_SIFS_ASSOCIATED);

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterT2tSifs,
                                RTLW81_T2T_SIFS_ASSOCIATED);

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterR2tSifs,
                                RTLW81_R2T_SIFS_ASSOCIATED);

        //
        // Initialize rate adaptation.
        //

        if ((Device->Flags & RTLW81_FLAG_8188E) == 0) {

            //
            // Find the set of rates that are supported by both the local
            // device and the BSS.
            //

            Rates = 0;
            BasicRates = 0;
            MaxRateIndex = 0;
            MaxBasicRateIndex = 0;
            for (BssIndex = 0;
                 BssIndex < BssInformation->Rates->Count;
                 BssIndex += 1) {

                BssRate = BssInformation->Rates->Rates[BssIndex];
                BssRate &= NET80211_RATE_VALUE_MASK;
                for (LocalIndex = 0;
                     LocalIndex < RtlwDefaultRateInformation.Count;
                     LocalIndex += 1) {

                    LocalRate = RtlwDefaultRateInformation.Rates[LocalIndex];
                    LocalRate &= NET80211_RATE_VALUE_MASK;
                    if (LocalRate == BssRate) {
                        break;
                    }
                }

                if (LocalIndex == RtlwDefaultRateInformation.Count) {
                    continue;
                }

                Rates |= (1 << LocalIndex);
                if (LocalIndex > MaxRateIndex) {
                    MaxRateIndex = LocalIndex;
                }

                BssRate = BssInformation->Rates->Rates[BssIndex];
                if ((BssRate & NET80211_RATE_BASIC) != 0) {
                    BasicRates |= (1 << LocalIndex);
                    if (LocalIndex > MaxBasicRateIndex) {
                        MaxBasicRateIndex = LocalIndex;
                    }
                }
            }

            //
            // Set the basic rate information.
            //

            MacIdCommand.MacId = RTLW81_MAC_ID_CONFIG_COMMAND_ID_BROADCAST |
                                 RTLW81_MAC_ID_CONFIG_COMMAND_ID_VALID;

            MacIdCommand.Mask = (RTLW81_TRANSMIT_IDENTIFICATION_RAID_11BG <<
                                 RTLW81_MAC_ID_CONFIG_COMMAND_MASK_MODE_SHIFT) |
                                BasicRates;

            Status = Rtlw81pSendFirmwareCommand(
                                         Device,
                                         RTLW81_FIRMWARE_COMMAND_MAC_ID_CONFIG,
                                         &MacIdCommand,
                                         sizeof(RTLW81_MAC_ID_CONFIG_COMMAND));

            if (!KSUCCESS(Status)) {
                break;
            }

            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterIniDataRateSelectBroadcast,
                                   MaxBasicRateIndex);

            //
            // Set the overall rate information.
            //

            MacIdCommand.MacId = RTLW81_MAC_ID_CONFIG_COMMAND_ID_BSS |
                                 RTLW81_MAC_ID_CONFIG_COMMAND_ID_VALID;

            MacIdCommand.Mask = (RTLW81_TRANSMIT_IDENTIFICATION_RAID_11BG <<
                                 RTLW81_MAC_ID_CONFIG_COMMAND_MASK_MODE_SHIFT) |
                                Rates;

            Status = Rtlw81pSendFirmwareCommand(
                                         Device,
                                         RTLW81_FIRMWARE_COMMAND_MAC_ID_CONFIG,
                                         &MacIdCommand,
                                         sizeof(RTLW81_MAC_ID_CONFIG_COMMAND));

            if (!KSUCCESS(Status)) {
                break;
            }

            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterIniDataRateSelectBss,
                                   MaxRateIndex);
        }

        break;

    default:
        Status = STATUS_SUCCESS;
        break;
    }

    return Status;
}

VOID
Rtlw81BulkInTransferCompletion (
    PUSB_TRANSFER Transfer
    )

/*++

Routine Description:

    This routine is called when the bulk in transfer returns. It processes
    the notification from the device.

Arguments:

    Transfer - Supplies a pointer to the transfer that completed.

Return Value:

    None.

--*/

{

    PUCHAR Data;
    PRTLW81_DEVICE Device;
    PRTLW81_RECEIVE_HEADER Header;
    ULONG InfoSize;
    ULONG Length;
    NET_PACKET_BUFFER Packet;
    ULONG PacketCount;
    ULONG PacketLength;
    PHYSICAL_ADDRESS PhysicalAddress;
    KSTATUS Status;
    ULONG TotalLength;

    Device = Transfer->UserData;
    Status = STATUS_SUCCESS;

    //
    // If the transfer failed, don't bother with the data.
    //

    if (!KSUCCESS(Transfer->Status)) {

        //
        // If the transfer stalled, attempt to clear the HALT feature from the
        // endpoint.
        //

        if (Transfer->Error == UsbErrorTransferStalled) {
            Status = UsbClearFeature(Device->UsbCoreHandle,
                                     USB_SETUP_REQUEST_ENDPOINT_RECIPIENT,
                                     USB_FEATURE_ENDPOINT_HALT,
                                     Device->BulkInEndpoint);
        }

        goto BulkInTransferCompletionEnd;
    }

    Data = Transfer->Buffer;
    PhysicalAddress = Transfer->BufferPhysicalAddress;
    Length = Transfer->LengthTransferred;
    if (Length < sizeof(RTLW81_RECEIVE_HEADER)) {
        RtlDebugPrint("RTLW81: Received odd sized data (%d).\n", Length);
        goto BulkInTransferCompletionEnd;
    }

    Header = (PRTLW81_RECEIVE_HEADER)Data;
    PacketCount = Header->PacketCount;
    Packet.IoBuffer = NULL;
    Packet.Flags = 0;
    while (PacketCount != 0) {
        if (Length < sizeof(RTLW81_RECEIVE_HEADER)) {
            RtlDebugPrint("RTLW81: Received odd sized data (%d).\n", Length);
            break;
        }

        Header = (PRTLW81_RECEIVE_HEADER)Data;
        if ((Header->LengthAndErrorFlags & RTLW81_RECEIVE_ERROR_MASK) != 0) {
            RtlDebugPrint("RTLW81: Receive error 0x%x\n",
                          Header->LengthAndErrorFlags);

            break;
        }

        PacketLength = (Header->LengthAndErrorFlags &
                        RTLW81_RECEIVE_PACKET_LENGTH_MASK) >>
                       RTLW81_RECEIVE_PACKET_LENGTH_SHIFT;

        if (PacketLength == 0) {
            break;
        }

        InfoSize = ((Header->Status & RTLW81_RECEIVE_STATUS_INFO_SIZE_MASK) >>
                    RTLW81_RECEIVE_STATUS_INFO_SIZE_SHIFT) * 8;

        TotalLength = PacketLength + InfoSize + sizeof(RTLW81_RECEIVE_HEADER);
        if (TotalLength > Length) {
            RtlDebugPrint("RTLW81: Got packet purported to be size %d, but "
                          "only %d bytes remaining in the transfer.\n",
                          TotalLength,
                          Length);

            break;
        }

        Packet.Buffer = Data + sizeof(RTLW81_RECEIVE_HEADER) + InfoSize;
        Packet.BufferPhysicalAddress = PhysicalAddress +
                                       sizeof(RTLW81_RECEIVE_HEADER) +
                                       InfoSize;

        Packet.BufferSize = PacketLength;
        Packet.DataSize = Packet.BufferSize;
        Packet.DataOffset = 0;
        Packet.FooterOffset = Packet.DataSize;
        NetProcessReceivedPacket(Device->NetworkLink, &Packet);

        //
        // TODO: Get receive signal strength indicator (RSSI).
        //

        //
        // Advance to the next packet, adding an extra 4 and aligning the total
        // offset to 4.
        //

        TotalLength = ALIGN_RANGE_UP(TotalLength,
                                     RTLW81_BULK_IN_PACKET_ALIGNMENT);

        if (TotalLength >= Length) {
            break;
        }

        Length -= TotalLength;
        Data += TotalLength;
        PhysicalAddress += TotalLength;
    }

BulkInTransferCompletionEnd:

    //
    // TODO: Only resubmit the transfer if the link is still up.
    //

    Status = UsbSubmitTransfer(Transfer);
    if (!KSUCCESS(Status)) {
        RtlDebugPrint("RTLW81: Failed to resubmit bulk IN transfer.\n");
    }

    return;
}

KSTATUS
Rtlw81pInitialize (
    PRTLW81_DEVICE Device,
    PIRP Irp
    )

/*++

Routine Description:

    This routine initializes and enables the RTL81xx wireless device.

Arguments:

    Device - Supplies a pointer to the device.

    Irp - Supplies a pointer to the IRP that is driving the initialization.

Return Value:

    Status code.

--*/

{

    ULONG Chain;
    PRTLW81_DEVICE_DATA DeviceData;
    ULONG Index;
    ULONG MacRegisterCount;
    UCHAR PaSetting;
    RTLW81_REGISTER Register;
    UCHAR RfRegister;
    ULONG Shift;
    KSTATUS Status;
    ULONG Type;
    ULONG Value;

    Status = STATUS_SUCCESS;

    //
    // Start phase 0 initialization. This goes up until the asynchronous load
    // of the firmware.
    //

    if (Device->InitializationPhase == 0) {
        Device->InitializationStatus = STATUS_SUCCESS;

        //
        // Figure out the device type and set the appropriate flags.
        //

        if ((Device->Flags & RTLW81_FLAG_8188E) == 0) {
            Value = RTLW81_READ_REGISTER32(Device,
                                           Rtlw81RegisterSysConfiguration);

            if ((Value & RTLW81_SYS_CONFIGURATION_TRP_VAUX_ENABLE) != 0) {
                Status = STATUS_INVALID_CONFIGURATION;
                goto InitializeEnd;
            }

            if ((Value & RTLW81_SYS_CONFIGURATION_VENDOR_UMC) != 0) {
                Device->Flags |= RTLW81_FLAG_UMC;
                if ((Value & RTLW81_SYS_CONFIGURATION_VERSION_MASK) == 0) {
                    Device->Flags |= RTLW81_FLAG_UMC_A_CUT;
                }
            }

            if ((Value & RTLW81_SYS_CONFIGURATION_TYPE_8192C) != 0) {
                Device->Flags |= RTLW81_FLAG_8192C;
                Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterHponFsm);
                Value &= RTLW81_HPON_FSM_CHIP_BONDING_ID_MASK;
                Value >>= RTLW81_HPON_FSM_CHIP_BONDING_ID_SHIFT;
                if (Value == RTLW81_HPON_FSM_CHIP_BONDING_ID_8192C_1T2R) {
                    Device->Flags |= RTLW81_FLAG_8192C_1T2R;
                }
            }
        }

        //
        // Record the number of transmit and receive chains.
        //

        if ((Device->Flags & RTLW81_FLAG_8192C) != 0) {
            if ((Device->Flags & RTLW81_FLAG_8192C_1T2R) != 0) {
                Device->TransmitChainCount =
                                        RTLW81_8192C_1T2R_TRANSMIT_CHAIN_COUNT;

            } else {
                Device->TransmitChainCount = RTLW81_8192C_TRANSMIT_CHAIN_COUNT;
            }

            Device->ReceiveChainCount = RTLW81_8192C_RECEIVE_CHAIN_COUNT;

        } else {
            Device->TransmitChainCount = RTLW81_DEFAULT_TRANSMIT_CHAIN_COUNT;
            Device->ReceiveChainCount = RTLW81_DEFAULT_RECEIVE_CHAIN_COUNT;
        }

        //
        // Read the device ROM. This caches information needed later, like the
        // MAC address, in the device structure.
        //

        Status = Rtlw81pReadRom(Device);
        if (!KSUCCESS(Status)) {
            goto InitializeEnd;
        }

        //
        // Perform device specific initialization steps to power on the device
        // and enable transmit and receive.
        //

        if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
            Status = Rtlw81p8188eInitialize(Device);
            if (!KSUCCESS(Status)) {
                goto InitializeEnd;
            }

        } else {
            Status = Rtlw81pDefaultInitialize(Device);
            if (!KSUCCESS(Status)) {
                goto InitializeEnd;
            }
        }

        //
        // Initialize the device's DMA queues.
        //

        Status = Rtlw81pInitializeDma(Device);
        if (!KSUCCESS(Status)) {
            RtlDebugPrint("RTWL: DMA init failed: 0x%08x\n", Status);
            goto InitializeEnd;
        }

        //
        // Set the driver information size.
        //

        RTLW81_WRITE_REGISTER8(Device,
                               Rtlw81RegisterReceiveDriverInformationSize,
                               RTLW81_DRIVER_INFORMATION_SIZE_DEFAULT);

        //
        // Turn on the interrupts.
        //

        if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81Register8188eInterruptStatus,
                                    0xFFFFFFFF);

            Value = (RTLW81_8188E_INTERRUPT_MASK_CPWM |
                     RTLW81_8188E_INTERRUPT_MASK_CPWM2 |
                     RTLW81_8188E_INTERRUPT_MASK_TBDER |
                     RTLW81_8188E_INTERRUPT_MASK_PS_TIMEOUT);

            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81Register8188eInterruptMask,
                                    Value);

            Value = (RTLW81_8188E_INTERRUPT_EXTRA_MASK_RECEIVE_FOVM |
                     RTLW81_8188E_INTERRUPT_EXTRA_MASK_TRANSMIT_FOVM |
                     RTLW81_8188E_INTERRUPT_EXTRA_MASK_RECEIVE_ERROR |
                     RTLW81_8188E_INTERRUPT_EXTRA_MASK_TRANSMIT_ERROR);

            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81Register8188eInterruptExtraMask,
                                    Value);

            Value = RTLW81_READ_REGISTER8(Device,
                                          Rtlw81RegisterUsbSpecialOption);

            Value |= RTLW81_USB_SPECIAL_OPTION_INT_BULK_SELECT;
            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterUsbSpecialOption,
                                   Value);

        } else {
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterDefaultInterruptStatus,
                                    0xFFFFFFFF);

            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterDefaultInterruptMask,
                                    0xFFFFFFFF);
        }

        //
        // Set the MAC address.
        //

        Rtlw81pWriteData(Device,
                         Rtlw81RegisterMacAddress,
                         Device->MacAddress,
                         NET80211_ADDRESS_SIZE);

        //
        // Create the core networking device.
        //

        Status = Rtlw81pCreateNetworkDevice(Device);
        if (!KSUCCESS(Status)) {
            goto InitializeEnd;
        }

        //
        // Set the network type.
        //

        Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterConfiguration);
        Value &= ~RTLW81_CONFIGURATION_NETWORK_TYPE_MASK;
        Value |= (RTLW81_CONFIGURATION_NETWORK_TYPE_NO_LINK <<
                  RTLW81_CONFIGURATION_NETWORK_TYPE_SHIFT);

        RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterConfiguration, Value);

        //
        // Initialize the receive filters.
        //

        Value = RTLW81_RECEIVE_CONFIGURATION_AAP |
                RTLW81_RECEIVE_CONFIGURATION_APM |
                RTLW81_RECEIVE_CONFIGURATION_AM |
                RTLW81_RECEIVE_CONFIGURATION_AB |
                RTLW81_RECEIVE_CONFIGURATION_APP_ICV |
                RTLW81_RECEIVE_CONFIGURATION_AMF |
                RTLW81_RECEIVE_CONFIGURATION_HTC_LOC_CTRL |
                RTLW81_RECEIVE_CONFIGURATION_APP_MIC |
                RTLW81_RECEIVE_CONFIGURATION_APP_PHYSTS;

        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterReceiveConfiguration,
                                Value);

        RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterMulticast1, 0xFFFFFFFF);
        RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterMulticast2, 0xFFFFFFFF);

        //
        // Accept all management frames.
        //

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterReceiveManagementFilter,
                                0xFFFF);

        //
        // Reject all control frames.
        //

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterReceiveControlFilter,
                                0x0000);

        //
        // Reject all data frames.
        //

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterReceiveDataFilter,
                                0x0000);

        //
        // Set the response rate.
        //

        Value = RTLW81_READ_REGISTER32(Device,
                                       Rtlw81RegisterReceiveResponseRate);

        Value &= ~RTLW81_RECEIVE_RESPONSE_RATE_BITMAP_MASK;
        Value |= (RTLW81_RECEIVE_RESPONSE_RATE_CCK_ONLY_1M <<
                  RTLW81_RECEIVE_RESPONSE_RATE_BITMAP_SHIFT);

        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterReceiveResponseRate,
                                Value);

        //
        // Set the retry limits.
        //

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterRetryLimit,
                                RTLW81_RETRY_LIMIT_DEFAULT);

        //
        // Disable the enhanced distributed channel access (EDCA) countdown to
        // reduce collisions.
        //

        Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterRdControl);
        Value |= RTLW81_RD_CONTROL_DISABLE_EDCA_COUNTDOWN;
        RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterRdControl, Value);

        //
        // Initialize the short interfame space (SIFS).
        //

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterSpecSifs,
                                RTLW81_SPEC_SIFS_DEFAULT);

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterMacSpecSifs,
                                RTLW81_MAC_SPEC_SIFS_DEFAULT);

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterSifsCck,
                                RTLW81_SIFS_CCK_DEFAULT);

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterSifsOfdm,
                                RTLW81_SIFS_OFDM_DEFAULT);

        //
        // Initialize the EDCA parameters for the four access categories.
        //

        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterEdcaBeParam,
                                RTLW81_EDCA_BE_PARAM_DEFAULT);

        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterEdcaBkParam,
                                RTLW81_EDCA_BK_PARAM_DEFAULT);

        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterEdcaViParam,
                                RTLW81_EDCA_VI_PARAM_DEFAULT);

        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterEdcaVoParam,
                                RTLW81_EDCA_VO_PARAM_DEFAULT);

        //
        // Setup rate fallback. This is not necessary on the RTL8188EU.
        //

        if ((Device->Flags & RTLW81_FLAG_8188E) == 0) {
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterDarfrc0,
                                    RTLW81_DARFRC0_DEFAULT);

            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterDarfrc1,
                                    RTLW81_DARFRC1_DEFAULT);

            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterRarfrc0,
                                    RTLW81_RARFRC0_DEFAULT);

            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterRarfrc1,
                                    RTLW81_RARFRC1_DEFAULT);
        }

        Register = Rtlw81RegisterFirmwareHardwareTransmitQueueControl;
        Value = RTLW81_READ_REGISTER8(Device, Register);
        Value |=
               RTLW81_FIRMWARE_HARDWARE_TRANSMIT_QUEUE_CONTROL_AMPDU_RETRY_NEW;

        RTLW81_WRITE_REGISTER8(Device, Register, Value);
        RTLW81_WRITE_REGISTER8(Device,
                               Rtlw81RegisterAckTimeout,
                               RTLW81_ACK_TIMEOUT_DEFAULT);

        //
        // Set up USB aggregation.
        //

        Value = RTLW81_READ_REGISTER32(
                                     Device,
                                     Rtlw81RegisterTransmitDescriptorControl0);

        Value &= ~RTLW81_TRANSMIT_DESCRIPTOR_CONTROL_BLOCK_COUNT_MASK;
        Value |= (RTLW81_TRANSMIT_DESCRIPTOR_CONTROL_BLOCK_COUNT_DEFAULT <<
                  RTLW81_TRANSMIT_DESCRIPTOR_CONTROL_BLOCK_COUNT_SHIFT);

        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterTransmitDescriptorControl0,
                                Value);

        Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterTransmitReceiveDma);
        Value |= RTLW81_TRANSMIT_RECEIVE_DMA_AGG_ENABLE;
        RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterTransmitReceiveDma, Value);
        RTLW81_WRITE_REGISTER8(Device,
                               Rtlw81RegisterReceiveDmaAggPgTh0,
                               RTLW81_RECEIVE_DMA_AGG_PG_TH0_DEFAULT);

        if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterReceiveDmaAggPgTh1,
                                   RTLW81_RECEIVE_DMA_AGG_PG_TH1_DEFAULT);

        } else {
            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterUsbDmaAggTo,
                                   RTLW81_USB_DMA_AGG_TO_DEFAULT);

            Value = RTLW81_READ_REGISTER8(Device,
                                          Rtlw81RegisterUsbSpecialOption);

            Value |= RTLW81_USB_SPECIAL_OPTION_AGG_ENABLE;
            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterUsbSpecialOption,
                                   Value);

            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterUsbAggTh,
                                   RTLW81_USB_AGG_TH_DEFAULT);

            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterUsbAggTo,
                                   RTLW81_USB_AGG_TO_DEFAULT);
        }

        //
        // Initialize the beacon parameters.
        //

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterBeaconControl,
                                RTLW81_BEACON_CONTROL_DEFAULT);

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterTbttProhibit,
                                RTLW81_TBTT_PROHIBIT_DEFAULT);

        RTLW81_WRITE_REGISTER8(Device,
                               Rtlw81RegisterDriverEarlyInt,
                               RTLW81_DRIVER_EARLY_INIT_DEFAULT);

        RTLW81_WRITE_REGISTER8(Device,
                               Rtlw81RegisterBeaconDmaTime,
                               RTLW81_BEACON_DMA_TIME_DEFAULT);

        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterBeaconTcfg,
                                RTLW81_BEACON_TCFG_DEFAULT);

        //
        // Initialize the AMPDU aggregation.
        //

        if ((Device->Flags & RTLW81_FLAG_8188E) == 0) {
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterAggregateLengthLimit,
                                    RTLW81_AGGREGATE_LENGTH_LIMIT_DEFAULT);

            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterAggregateBreakTime,
                                   RTLW81_AGGREGATE_BREAK_TIME_DEFAULT);

            RTLW81_WRITE_REGISTER16(Device,
                                    Rtlw81RegisterMaxAggregationNumber,
                                    RTLW81_MAX_AGGREGATION_NUMBER_DEFAULT);

            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterBeaconMaxError,
                                   RTLW81_BEACON_MAX_ERROR_DEFAULT);
        }

        //
        // Load the device firmware.
        //

        Status = Rtlw81pInitializeFirmware(Device, Irp);
        if (!KSUCCESS(Status)) {
            goto InitializeEnd;
        }

        Device->InitializationPhase = 1;

    //
    // Phase 1 kicks off where phase 0 left off by finishing the firmware load.
    //

    } else {

        ASSERT(Device->InitializationPhase == 1);

        //
        // Finish loading the device firmware.
        //

        Status = Rtlw81pInitializeFirmware(Device, Irp);
        if (!KSUCCESS(Status)) {
            goto InitializeEnd;
        }

        //
        // Initialize the MAC.
        //

        if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
            MacRegisterCount = RTLW81_ARRAY_COUNT(Rtlw8188eMacRegisters);
            for (Index = 0; Index < MacRegisterCount; Index += 1) {
                RTLW81_WRITE_REGISTER8(Device,
                                       Rtlw8188eMacRegisters[Index],
                                       Rtlw8188eMacValues[Index]);
            }

            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterMaxAggregationNumber,
                                   RTLW81_MAX_AGGREGATION_NUMBER_8188E_DEFAULT);

        } else {
            MacRegisterCount = RTLW81_ARRAY_COUNT(RtlwDefaultMacRegisters);
            for (Index = 0; Index < MacRegisterCount; Index += 1) {
                RTLW81_WRITE_REGISTER8(Device,
                                       RtlwDefaultMacRegisters[Index],
                                       RtlwDefaultMacValues[Index]);
            }
        }

        //
        // Enable BB and RF.
        //

        Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterSysFunctionEnable);
        Value |= RTLW81_SYS_FUNCTION_ENABLE_BBRSTB |
                 RTLW81_SYS_FUNCTION_ENABLE_BB_GLB_RST |
                 RTLW81_SYS_FUNCTION_ENABLE_DIO_RF;

        RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterSysFunctionEnable, Value);
        if ((Device->Flags & RTLW81_FLAG_8188E) == 0) {
            RTLW81_WRITE_REGISTER16(Device,
                                    Rtlw81RegisterAfePllControl,
                                    RTLW81_AFE_PLL_CONTROL_DEFAULT);
        }

        Value = RTLW81_RF_CONTROL_ENABLE |
                RTLW81_RF_CONTROL_RSTB |
                RTLW81_RF_CONTROL_SDMRSTB;

        RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterRfControl, Value);
        Value = RTLW81_SYS_FUNCTION_ENABLE_BBRSTB |
                RTLW81_SYS_FUNCTION_ENABLE_BB_GLB_RST |
                RTLW81_SYS_FUNCTION_ENABLE_USBA |
                RTLW81_SYS_FUNCTION_ENABLE_USBD;

        RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterSysFunctionEnable, Value);
        if ((Device->Flags & RTLW81_FLAG_8188E) == 0) {
            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterLdohci12Control,
                                   RTLW81_LDOHCI_12_CONTROL_DEFAULT);

            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterAfeXtalControl1,
                                   RTLW81_AFE_XTAL_CONTROL1_DEFAULT);
        }

        //
        // Determine which values to use for BB and RF initialization,
        // overriding the defaults where necessary.
        //

        if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
            DeviceData = &Rtlw8188euDeviceData;

        } else if ((Device->Flags & RTLW81_FLAG_8192C) == 0) {
            if (Device->BoardType == RTLW81_ROM_RF_OPT1_BOARD_TYPE_MINICARD) {
                DeviceData = &Rtlw8188ceDeviceData;

            } else if (Device->BoardType ==
                       RTLW81_ROM_RF_OPT1_BOARD_TYPE_HIGHPA) {

                DeviceData = &Rtlw8188ruDeviceData;

            } else {
                DeviceData = &Rtlw8188cuDeviceData;
            }

        } else {
            if (Device->BoardType == RTLW81_ROM_RF_OPT1_BOARD_TYPE_MINICARD) {
                DeviceData = &Rtlw8192ceDeviceData;

            } else {
                DeviceData = &Rtlw8192cuDeviceData;
            }
        }

        //
        // Program the BB.
        //

        for (Index = 0; Index < DeviceData->BbCount; Index += 1) {
            RTLW81_WRITE_REGISTER32(Device,
                                    DeviceData->BbRegisters[Index],
                                    DeviceData->BbValues[Index]);

            HlBusySpin(100);
        }

        //
        // Handle special initialization for an 8192C chip that only has 1
        // transmit chain.
        //

        if ((Device->Flags & RTLW81_FLAG_8192C_1T2R) != 0) {
            Value = RTLW81_READ_REGISTER32(Device,
                                           Rtlw81RegisterFpga0TransmitInfo);

            Value &= ~RTLW81_FPGA0_TRANSMIT_INFO_INIT1_MASK;
            Value |= RTLW81_FPGA0_TRANSMIT_INFO_INIT1_VALUE;
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterFpga0TransmitInfo,
                                    Value);

            Value = RTLW81_READ_REGISTER32(Device,
                                           Rtlw81RegisterFpga1TransmitInfo);

            Value &= ~RTLW81_FPGA0_TRANSMIT_INFO_INIT2_MASK;
            Value |= RTLW81_FPGA0_TRANSMIT_INFO_INIT2_VALUE;
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterFpga1TransmitInfo,
                                    Value);

            Value = RTLW81_READ_REGISTER32(Device,
                                           Rtlw81RegisterCck0AfeSetting);

            Value &= ~RTLW81_CCK0_AFE_SETTING_INIT_MASK;
            Value |= RTLW81_CCK0_AFE_SETTING_INIT_VALUE;
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterCck0AfeSetting,
                                    Value);

            Value = RTLW81_READ_REGISTER32(
                                        Device,
                                        Rtlw81RegisterOfdm0TransmitPathEnable);

            Value &= ~RTLW81_OFDM0_TRANSMIT_PATH_ENABLE_INIT_MASK;
            Value |= RTLW81_OFDM0_TRANSMIT_PATH_ENABLE_INIT_VALUE;
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterOfdm0TransmitPathEnable,
                                    Value);

            Value = RTLW81_READ_REGISTER32(Device,
                                           Rtlw81RegisterOfdm0AgcParam1);

            Value &= ~RTLW81_OFDM0_AGC_PARAM1_INIT_MASK;
            Value |= RTLW81_OFDM0_AGC_PARAM1_INIT_VALUE;
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterOfdm0AgcParam1,
                                    Value);

            Value = RTLW81_READ_REGISTER32(Device,
                                           Rtlw81Register8192c1T2RInit0);

            Value &= ~RTLW81_8192C_1T2R_INIT_MASK;
            Value |= RTLW81_8192C_1T2R_INIT_VALUE;
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81Register8192c1T2RInit0,
                                    Value);

            Value = RTLW81_READ_REGISTER32(Device,
                                           Rtlw81Register8192c1T2RInit1);

            Value &= ~RTLW81_8192C_1T2R_INIT_MASK;
            Value |= RTLW81_8192C_1T2R_INIT_VALUE;
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81Register8192c1T2RInit1,
                                    Value);

            Value = RTLW81_READ_REGISTER32(Device,
                                           Rtlw81Register8192c1T2RInit2);

            Value &= ~RTLW81_8192C_1T2R_INIT_MASK;
            Value |= RTLW81_8192C_1T2R_INIT_VALUE;
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81Register8192c1T2RInit2,
                                    Value);

            Value = RTLW81_READ_REGISTER32(Device,
                                           Rtlw81Register8192c1T2RInit3);

            Value &= ~RTLW81_8192C_1T2R_INIT_MASK;
            Value |= RTLW81_8192C_1T2R_INIT_VALUE;
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81Register8192c1T2RInit3,
                                    Value);

            Value = RTLW81_READ_REGISTER32(Device,
                                           Rtlw81Register8192c1T2RInit5);

            Value &= ~RTLW81_8192C_1T2R_INIT_MASK;
            Value |= RTLW81_8192C_1T2R_INIT_VALUE;
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81Register8192c1T2RInit5,
                                    Value);
        }

        //
        // Set the AGC initialization values.
        //

        for (Index = 0; Index < DeviceData->AgcCount; Index += 1) {
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterOfdm0AgcrsstiTable,
                                    DeviceData->AgcValues[Index]);

            HlBusySpin(100);
        }

        if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterOfdm0AgcCore1,
                                    RTLW81_OFDM0_AGC_CORE1_INIT1);

            HlBusySpin(100);
            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterOfdm0AgcCore1,
                                    RTLW81_OFDM0_AGC_CORE1_INIT2);

            HlBusySpin(100);
            Value = RTLW81_READ_REGISTER32(Device,
                                           Rtlw81RegisterAfeXtalControl0);

            Value &= ~(RTLW81_AFE_XTAL_CONTROL_ADDRESS1_MASK |
                       RTLW81_AFE_XTAL_CONTROL_ADDRESS2_MASK);

            Value |= (Device->CrystalCapability <<
                      RTLW81_AFE_XTAL_CONTROL_ADDRESS1_SHIFT);

            Value |= (Device->CrystalCapability <<
                      RTLW81_AFE_XTAL_CONTROL_ADDRESS2_SHIFT);

            RTLW81_WRITE_REGISTER32(Device,
                                    Rtlw81RegisterAfeXtalControl0,
                                    Value);

        } else {
            Value = RTLW81_READ_REGISTER32(Device,
                                           Rtlw81RegisterHssiParameter2);

            if ((Value & RTLW81_HSSI_PARAMETER2_CCK_HIGH_POWER) != 0) {
                Device->Flags |= RTLW81_FLAG_CCK_HIGH_POWER;
            }
        }

        //
        // Program the RF.
        //

        for (Chain = 0; Chain < Device->ReceiveChainCount; Chain += 1) {

            //
            // Prepare the chain for the programming of the RF values.
            //

            Shift = (Chain % 2) * 16;
            Register = Rtlw81RegisterFpga0RfSoftwareInterface +
                       ((Chain / 2) * 4);

            Value = RTLW81_READ_REGISTER32(Device, Register);
            Type = (Value >> Shift) & RTLW81_FPGA0_RF_SOFTWARE_INTERFACE_TYPE;

            //
            // Enable the RF environment.
            //

            Register = Rtlw81RegisterFpga0RfOeInterface + (Chain * 4);
            Value = RTLW81_READ_REGISTER32(Device, Register);
            Value |= RTLW81_FPGA0_RF_OE_INTERFACE_ENABLE;
            RTLW81_WRITE_REGISTER32(Device, Register, Value);
            HlBusySpin(100);
            Value = RTLW81_READ_REGISTER32(Device, Register);
            Value |= RTLW81_FPGA0_RF_OE_INTERFACE_HIGH_OUTPUT;
            RTLW81_WRITE_REGISTER32(Device, Register, Value);
            HlBusySpin(100);

            //
            // Set the RF register address and data lengths.
            //

            Register = Rtlw81RegisterHssiParameter2 + (Chain * 8);
            Value = RTLW81_READ_REGISTER32(Device, Register);
            Value &= ~RTLW81_HSSI_PARAMETER2_ADDRESS_LENGTH;
            RTLW81_WRITE_REGISTER32(Device, Register, Value);
            HlBusySpin(100);
            Value = RTLW81_READ_REGISTER32(Device, Register);
            Value &= ~RTLW81_HSSI_PARAMETER2_DATA_LENGTH;
            RTLW81_WRITE_REGISTER32(Device, Register, Value);
            HlBusySpin(100);

            //
            // Program the RF values to this chain.
            //

            for (Index = 0; Index < DeviceData->RfCount[Chain]; Index += 1) {
                RfRegister = DeviceData->RfRegisters[Chain][Index];
                if ((RfRegister >= RTLW81_RF_REGISTER_DELAY_VALUE_MIN) &&
                    (RfRegister <= RTLW81_RF_REGISTER_DELAY_VALUE_MAX)) {

                    KeDelayExecution(FALSE, FALSE, 50);
                    continue;
                }

                Rtlw81pWriteRfRegister(Device,
                                       Chain,
                                       RfRegister,
                                       DeviceData->RfValues[Chain][Index]);

                HlBusySpin(100);
            }

            Register = Rtlw81RegisterFpga0RfSoftwareInterface +
                       ((Chain / 2) * 4);

            Value = RTLW81_READ_REGISTER32(Device, Register);
            Value &= ~(RTLW81_FPGA0_RF_SOFTWARE_INTERFACE_TYPE << Shift);
            Value |= Type << Shift;
            RTLW81_WRITE_REGISTER32(Device, Register, Value);
        }

        //
        // Program RF receive state on 8188 UMC-A chips.
        //

        if ((Device->Flags & (RTLW81_FLAG_8192C | RTLW81_FLAG_UMC_A_CUT)) ==
            RTLW81_FLAG_UMC_A_CUT) {

            Rtlw81pWriteRfRegister(Device,
                                   0,
                                   Rtlw81RfRegisterReceiveG1,
                                   RTLW81_RF_RECEIVE_G1_DEFAULT);

            Rtlw81pWriteRfRegister(Device,
                                   0,
                                   Rtlw81RfRegisterReceiveG1,
                                   RTLW81_RF_RECEIVE_G2_DEFAULT);
        }

        //
        // Enabl MAC transmit and receive on RTL8188E devices.
        //

        if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
            Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterConfiguration);
            Value |= RTLW81_CONFIGURATION_MAC_TRANSMIT_ENABLE |
                     RTLW81_CONFIGURATION_MAC_RECEIVE_ENABLE;

            RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterConfiguration, Value);
        }

        //
        // Turn CCK and OFDM blocks on.
        //

        Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterFpga0Rfmod);
        Value |= RTLW81_RFMOD_CCK_ENABLE;
        RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterFpga0Rfmod, Value);
        Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterFpga0Rfmod);
        Value |= RTLW81_RFMOD_OFDM_ENABLE;
        RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterFpga0Rfmod, Value);

        //
        // Clear per-station keys table.
        //

        Value = RTLW81_CAM_COMMAND_CLEAR | RTLW81_CAM_COMMAND_POLLING;
        RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterCamCommand, Value);

        //
        // Enable hardware sequencing numbering.
        //

        RTLW81_WRITE_REGISTER8(Device,
                               Rtlw81RegisterHardwareSequencingControl,
                               RTW81_HARDWARE_SEQUENCING_CONTROL_DEFAULT);

        //
        // LC Calibration.
        //

        Rtlw81pLcCalibration(Device);

        //
        // Fix USB interface issues.
        //

        if ((Device->Flags & RTLW81_FLAG_8188E) == 0) {
            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterUsbInterference0,
                                   RTLW81_USB_INTERFERENCE0_DEFAULT);

            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterUsbInterference1,
                                   RLTW81_USB_INTERFERENCE1_DEFAULT);

            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterUsbInterference2,
                                   RTLW81_USB_INTERFERENCE2_DEFAULT);

            //
            // PA Bias init.
            //

            PaSetting = Rtlw81pEfuseRead8(Device, Rtlw81EfuseRegisterPaSetting);
            for (Index = 0; Index < Device->ReceiveChainCount; Index += 1) {
                if ((PaSetting & (1 << Index)) != 0) {
                    continue;
                }

                Rtlw81pWriteRfRegister(Device,
                                       Index,
                                       Rtlw81RfRegisterIpa,
                                       RTLW81_RF_IPA_INIT0);

                Rtlw81pWriteRfRegister(Device,
                                       Index,
                                       Rtlw81RfRegisterIpa,
                                       RTLW81_RF_IPA_INIT1);

                Rtlw81pWriteRfRegister(Device,
                                       Index,
                                       Rtlw81RfRegisterIpa,
                                       RTLW81_RF_IPA_INIT2);

                Rtlw81pWriteRfRegister(Device,
                                       Index,
                                       Rtlw81RfRegisterIpa,
                                       RTLW81_RF_IPA_INIT3);
            }

            if ((PaSetting & RTLW81_PA_SETTING_INIT_BIT) == 0) {
                Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterPaSetting);
                Value &= ~RTLW81_PA_SETTING_INIT_MASK;
                Value |= RTLW81_PA_SETTING_INIT_VALUE;
                RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterPaSetting, Value);
            }
        }

        //
        // Intialize GPIO settings.
        //

        Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterGpioMuxConfig);
        Value &= ~RTLW81_GPIO_MUX_CONFIG_ENABLE_BT;
        RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterGpioMuxConfig, Value);

        //
        // Fix for lower temperature.
        //

        if ((Device->Flags & RTLW81_FLAG_8188E) == 0) {
            RTLW81_WRITE_REGISTER8(Device,
                                   Rtlw81RegisterTempatureControl,
                                   RTLW81_TEMPERATURE_CONTROL_DEFAULT);
        }

        //
        // Set the default channel to start.
        //

        Rtlw81pSetChannel(Device, RTLW81_DEFAULT_CHANNEL);

        //
        // Start the Bulk Receive USB transfers.
        //

        if (KSUCCESS(Device->InitializationStatus)) {
            Rtlw81pSetLed(Device, TRUE);
            Status = Net80211StartLink(Device->NetworkLink);
            if (!KSUCCESS(Status)) {
                goto InitializeEnd;
            }

            Status = Rtlw81pSubmitBulkInTransfers(Device);
            if (!KSUCCESS(Status)) {
                goto InitializeEnd;
            }
        }
    }

InitializeEnd:
    if (KSUCCESS(Status)) {
        Status = Device->InitializationStatus;
    }

    return Status;
}

VOID
Rtlw81pDestroyBulkOutTransfers (
    PRTLW81_DEVICE Device
    )

/*++

Routine Description:

    This routine destroys the RTLW815xx device's bulk out tranfers.

Arguments:

    Device - Supplies a pointer to the device.

Return Value:

    None.

--*/

{

    PLIST_ENTRY FreeList;
    ULONG Index;
    PRTLW81_BULK_OUT_TRANSFER Rtlw81Transfer;

    for (Index = 0; Index < RTLW81_MAX_BULK_OUT_ENDPOINT_COUNT; Index += 1) {
        FreeList = &(Device->BulkOutFreeTransferList[Index]);
        while (LIST_EMPTY(FreeList) == FALSE) {
            Rtlw81Transfer = LIST_VALUE(FreeList->Next,
                                        RTLW81_BULK_OUT_TRANSFER,
                                        ListEntry);

            ASSERT(Rtlw81Transfer->Packet == NULL);

            LIST_REMOVE(&(Rtlw81Transfer->ListEntry));
            UsbDestroyTransfer(Rtlw81Transfer->UsbTransfer);
            MmFreePagedPool(Rtlw81Transfer);
        }
    }

    return;
}

//
// --------------------------------------------------------- Internal Functions
//

KSTATUS
Rtlw81pReadRom (
    PRTLW81_DEVICE Device
    )

/*++

Routine Description:

    This routine reads and saves the RTLW81xx device's ROM.

Arguments:

    Device - Supplies a pointer to the RTLW81 device.

Return Value:

    Status code.

--*/

{

    ULONG Address;
    PRTLW81_POWER_DEFAULT DefaultPower;
    UCHAR Diff;
    UCHAR EfuseValue;
    ULONG Index;
    ULONG Mask;
    ULONG Offset;
    PUCHAR Rom;
    ULONG RomSize;
    PRTLW81_POWER_8188E Rtlw8188ePower;
    ULONG Value;

    //
    // Allocate a buffer to hold the ROM data.
    //

    RomSize = RTLW81_DEFAULT_ROM_SIZE;
    if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
        RomSize = RTLW81_8188E_ROM_SIZE;
    }

    Rom = MmAllocatePagedPool(RTLW81_DEFAULT_ROM_SIZE, RTLW81_ALLOCATION_TAG);
    if (Rom == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Enable EFUSE access.
    //

    RTLW81_WRITE_REGISTER8(Device,
                           Rtlw81RegisterEfuseAccess,
                           RTLW81_EFUSE_ACCESS_ON);

    Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterSysIsoControl);
    if ((Value & RTLW81_SYS_ISO_CONTROL_PWC_EV12V) == 0) {
        Value |= RTLW81_SYS_ISO_CONTROL_PWC_EV12V;
        RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterSysIsoControl, Value);
    }

    Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterSysFunctionEnable);
    if ((Value & RTLW81_SYS_FUNCTION_ENABLE_ELDR) == 0) {
        Value |= RTLW81_SYS_FUNCTION_ENABLE_ELDR;
        RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterSysFunctionEnable, Value);
    }

    Mask = RTLW81_SYS_CLOCK_LOADER_ENABLE | RTLW81_SYS_CLOCK_ANA8M;
    Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterSysClock);
    if ((Value & Mask) != Mask) {
        Value |= Mask;
        RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterSysClock, Value);
    }

    //
    // Read the entire ROM.
    //

    Address = 0;
    RtlSetMemory(Rom, RTLW81_EFUSE_INVALID, RomSize);
    while (Address < RTLW81_EFUSE_MAX_ADDRESS) {
        EfuseValue = Rtlw81pEfuseRead8(Device, Address);
        if (EfuseValue == RTLW81_EFUSE_INVALID) {
            break;
        }

        Address += 1;
        if ((EfuseValue & RTLW81_EFUSE_ENCODING_MASK) ==
            RTLW81_EFUSE_ENCODING_EXTENDED) {

            Offset = (EfuseValue & RTLW81_EFUSE_EXTENDED_FIRST_OFFSET_MASK) >>
                     RTLW81_EFUSE_EXTENDED_FIRST_OFFSET_SHIFT;

            EfuseValue = Rtlw81pEfuseRead8(Device, Address);
            if ((EfuseValue & RTLW81_EFUSE_EXTENDED_ENCODING_MASK) !=
                RTLW81_EFUSE_EXTENDED_ENCODING_NO_OFFSET) {

                Offset |= (EfuseValue &
                           RTLW81_EFUSE_EXTENDED_SECOND_OFFSET_MASK) >>
                          RTLW81_EFUSE_EXTENDED_SECOND_OFFSET_SHIFT;
            }

            Address += 1;

        } else {
            Offset = (EfuseValue & RTLW81_EFUSE_DEFAULT_OFFSET_MASK) >>
                     RTLW81_EFUSE_DEFAULT_OFFSET_SHIFT;
        }

        Mask = EfuseValue & RTLW81_EFUSE_VALID_MASK;
        for (Index = 0; Index < 4; Index += 1) {
            if ((Mask & 0x1) == 0) {
                EfuseValue = Rtlw81pEfuseRead8(Device, Address);
                Rom[Offset * 8 + Index * 2] = EfuseValue;
                Address += 1;
                EfuseValue = Rtlw81pEfuseRead8(Device, Address);
                Rom[Offset * 8 + Index * 2 + 1] = EfuseValue;
                Address += 1;
            }

            Mask >>= 1;
        }
    }

    //
    // Cache any values based on the device type as the ROMs are formatted a
    // little differently.
    //

    if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
        Rtlw8188ePower = &(Device->Power.Rtlw8188e);
        RtlCopyMemory(Rtlw8188ePower->CckTransmitPower,
                      &(Rom[RTLW81_8188E_ROM_CCK_TRANSMIT_POWER_OFFSET]),
                      sizeof(Rtlw8188ePower->CckTransmitPower));

        RtlCopyMemory(Rtlw8188ePower->Ht40TransmitPower,
                      &(Rom[RTLW81_8188E_ROM_HT_40_TRANSMIT_POWER_OFFSET]),
                      sizeof(Rtlw8188ePower->Ht40TransmitPower));

        Diff = Rom[RTLW81_8188E_ROM_POWER_OPTION_OFFSET];
        Diff &= RTLW81_8188E_ROM_POWER_OPTION_BW_20_MASK;
        Diff >>= RTLW81_8188E_ROM_POWER_OPTION_BW_20_SHIFT;
        if ((Diff & RTLW81_8188E_ROM_POWER_OPTION_HIGH_BITS_SET) != 0) {
            Diff |= RTLW81_8188E_ROM_POWER_OPTION_HIGH_BITS;
        }

        Rtlw8188ePower->Bw20TransmitPowerDiff = Diff;
        Diff = Rom[RTLW81_8188E_ROM_POWER_OPTION_OFFSET];
        Diff &= RTLW81_8188E_ROM_POWER_OPTION_OFDM_MASK;
        Diff >>= RTLW81_8188E_ROM_POWER_OPTION_OFDM_SHIFT;
        if ((Diff & RTLW81_8188E_ROM_POWER_OPTION_HIGH_BITS_SET) != 0) {
            Diff |= RTLW81_8188E_ROM_POWER_OPTION_HIGH_BITS;
        }

        Rtlw8188ePower->OfdmTransmitPowerDiff = Diff;
        Device->BoardType = Rom[RTLW81_8188E_ROM_RF_OPT1_OFFSET];
        Device->Regulatory = Rom[RTLW81_8188E_ROM_RF_OPT1_OFFSET];
        Device->CrystalCapability =
                               Rom[RTLW81_8188E_ROM_CRYSTAL_CAPABILITY_OFFSET];

        if (Device->CrystalCapability ==
            RTLW81_8188E_ROM_CRYSTAL_CAPABILITY_INVALID) {

            Device->CrystalCapability =
                                   RTLW81_8188E_ROM_CRYSTAL_CAPABILITY_DEFAULT;
        }

        Device->CrystalCapability &= RTLW81_8188E_ROM_CRYSTAL_CAPABILITY_MASK;
        RtlCopyMemory(Device->MacAddress,
                      &(Rom[RTLW81_8188E_ROM_MAC_ADDRESS_OFFSET]),
                      sizeof(Device->MacAddress));

    } else {
        DefaultPower = &(Device->Power.Default);
        RtlCopyMemory(DefaultPower->CckTransmitPower,
                      &(Rom[RTLW81_DEFAULT_ROM_CCK_TRANSMIT_POWER_OFFSET]),
                      sizeof(DefaultPower->CckTransmitPower));

        RtlCopyMemory(DefaultPower->Ht40TransmitPower,
                      &(Rom[RTLW81_DEFAULT_ROM_HT_40_TRANSMIT_POWER_OFFSET]),
                      sizeof(DefaultPower->Ht40TransmitPower));

        RtlCopyMemory(
                   DefaultPower->Ht40TransmitPowerDiff,
                   &(Rom[RTLW81_DEFAULT_ROM_HT_40_TRANSMIT_POWER_DIFF_OFFSET]),
                   sizeof(DefaultPower->Ht40TransmitPowerDiff));

        RtlCopyMemory(DefaultPower->Ht40MaxPower,
                      &(Rom[RTLW81_DEFAULT_ROM_HT_40_MAX_POWER_OFFSET]),
                      sizeof(DefaultPower->Ht40MaxPower));

        RtlCopyMemory(
                   DefaultPower->Ht20TransmitPowerDiff,
                   &(Rom[RTLW81_DEFAULT_ROM_HT_20_TRANSMIT_POWER_DIFF_OFFSET]),
                   sizeof(DefaultPower->Ht20TransmitPowerDiff));

        RtlCopyMemory(DefaultPower->Ht20MaxPower,
                      &(Rom[RTLW81_DEFAULT_ROM_HT_20_MAX_POWER_OFFSET]),
                      sizeof(DefaultPower->Ht20MaxPower));

        RtlCopyMemory(
                    DefaultPower->OfdmTransmitPowerDiff,
                    &(Rom[RTLW81_DEFAULT_ROM_OFDM_TRANSMIT_POWER_DIFF_OFFSET]),
                    sizeof(DefaultPower->OfdmTransmitPowerDiff));

        RtlCopyMemory(Device->MacAddress,
                      &(Rom[RTLW81_DEFAULT_ROM_MAC_ADDRESS_OFFSET]),
                      sizeof(Device->MacAddress));

        Device->BoardType = Rom[RTLW81_DEFAULT_ROM_RF_OPT1_OFFSET];
        Device->Regulatory= Rom[RTLW81_DEFAULT_ROM_RF_OPT1_OFFSET];
    }

    Device->BoardType &= RTLW81_ROM_RF_OPT1_BOARD_TYPE_MASK;
    Device->BoardType >>= RTLW81_ROM_RF_OPT1_BOARD_TYPE_SHIFT;
    Device->Regulatory &= RTLW81_ROM_RF_OPT1_REGULATORY_MASK;
    Device->Regulatory >>= RTLW81_ROM_RF_OPT1_REGULATORY_SHIFT;

    //
    // Disable EFUSE access.
    //

    RTLW81_WRITE_REGISTER8(Device,
                           Rtlw81RegisterEfuseAccess,
                           RTLW81_EFUSE_ACCESS_OFF);

    MmFreePagedPool(Rom);
    return STATUS_SUCCESS;
}

KSTATUS
Rtlw81pDefaultInitialize (
    PRTLW81_DEVICE Device
    )

/*++

Routine Description:

    This routine initializes and enables a default RTL81xx wireless device.

Arguments:

    Device - Supplies a pointer to the device.

Return Value:

    Status code.

--*/

{

    ULONGLONG CurrentTime;
    KSTATUS Status;
    ULONGLONG Timeout;
    ULONGLONG TimeoutTicks;
    ULONG Value;

    TimeoutTicks = HlQueryTimeCounterFrequency() * RTLW81_DEVICE_TIMEOUT;

    //
    // Wait for the autoload done bit to be set.
    //

    CurrentTime = KeGetRecentTimeCounter();
    Timeout = CurrentTime + TimeoutTicks;
    do {
        Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterApsFsmco);
        if ((Value & RTLW81_APS_FSMCO_PFM_AUTOLOAD_DONE) != 0) {
            break;
        }

        CurrentTime = KeGetRecentTimeCounter();

    } while (CurrentTime <= Timeout);

    if (CurrentTime > Timeout) {
        Status = STATUS_TIMEOUT;
        goto DefaultInitializeEnd;
    }

    //
    // Unlock the ISO, Power, and Clock control register.
    //

    RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterRsvControl, 0);

    //
    // Move SPS into PWM mode.
    //

    RTLW81_WRITE_REGISTER8(Device,
                           Rtlw81RegisterSps0Control,
                           RTLW81_SPS0_CONTROL_DEFAULT);

    HlBusySpin(100);

    //
    // Make sure LDV12 is enabled.
    //

    Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterLdov12dControl);
    if ((Value & RTLW81_LDOV12D_CONTROL_LDV12_ENABLE) == 0) {
        Value |= RTLW81_LDOV12D_CONTROL_LDV12_ENABLE;
        RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterLdov12dControl, Value);
        HlBusySpin(100);
        Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterSysIsoControl);
        Value &= ~RTLW81_SYS_ISO_CONTROL_MD2PP;
        RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterSysIsoControl, Value);
    }

    //
    // Auto-enable WLAN.
    //

    Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterApsFsmco);
    Value |= RTLW81_APS_FSMCO_APFM_ONMAC;
    RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterApsFsmco, Value);
    CurrentTime = KeGetRecentTimeCounter();
    Timeout = CurrentTime + TimeoutTicks;
    do {
        Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterApsFsmco);
        if ((Value & RTLW81_APS_FSMCO_APFM_ONMAC) == 0) {
            break;
        }

        CurrentTime = KeGetRecentTimeCounter();

    } while (CurrentTime <= Timeout);

    if (CurrentTime > Timeout) {
        Status = STATUS_TIMEOUT;
        goto DefaultInitializeEnd;
    }

    //
    // Enable radio, GPIO, and LED functions.
    //

    Value = RTLW81_APS_FSMCO_AFSM_HSUS |
            RTLW81_APS_FSMCO_PDN_EN |
            RTLW81_APS_FSMCO_PFM_AUTOLOAD_DONE;

    RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterApsFsmco, Value);

    //
    // Release RF digital isolation.
    //

    Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterSysIsoControl);
    Value &= ~RTLW81_SYS_ISO_CONTROL_DIOR;
    RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterSysIsoControl, Value);

    //
    // Initialize the MAC.
    //

    Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterApsdControl);
    Value &= ~RTLW81_APSD_CONTROL_OFF;
    RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterApsdControl, Value);
    CurrentTime = KeGetRecentTimeCounter();
    Timeout = CurrentTime + TimeoutTicks;
    do {
        Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterApsdControl);
        if ((Value & RTLW81_APSD_CONTROL_STATUS_OFF) == 0) {
            break;
        }

        CurrentTime = KeGetRecentTimeCounter();

    } while (CurrentTime <= Timeout);

    if (CurrentTime > Timeout) {
        Status = STATUS_TIMEOUT;
        goto DefaultInitializeEnd;
    }

    //
    // Enable MAC DMA/WMAC/Schedule/SEC blocks.
    //

    Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterConfiguration);
    Value |= RTLW81_CONFIGURATION_HCI_TRANSMIT_DMA_ENABLE |
             RTLW81_CONFIGURATION_HCI_RECEIVE_DMA_ENABLE |
             RTLW81_CONFIGURATION_TRANSMIT_DMA_ENABLE |
             RTLW81_CONFIGURATION_RECEIVE_DMA_ENABLE |
             RTLW81_CONFIGURATION_PROTOCOL_ENABLE |
             RTLW81_CONFIGURATION_SCHEDULE_ENABLE |
             RTLW81_CONFIGURATION_MAC_TRANSMIT_ENABLE |
             RTLW81_CONFIGURATION_MAC_RECEIVE_ENABLE |
             RTLW81_CONFIGURATION_SEC_ENABLE;

    RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterConfiguration, Value);

    //
    // This magic only shows up in FreeBSD. Not Linux.
    //

    RTLW81_WRITE_REGISTER8(Device,
                           Rtlw81RegisterUsbEnable,
                           RTLW81_USB_ENABLE_DEFAULT);

    Status = STATUS_SUCCESS;

DefaultInitializeEnd:
    return Status;
}

KSTATUS
Rtlw81p8188eInitialize (
    PRTLW81_DEVICE Device
    )

/*++

Routine Description:

    This routine initializes and enables an 8188E RTL81xx wireless device.

Arguments:

    Device - Supplies a pointer to the device.

Return Value:

    Status code.

--*/

{

    ULONGLONG CurrentTime;
    KSTATUS Status;
    ULONGLONG Timeout;
    ULONGLONG TimeoutTicks;
    ULONG Value;

    TimeoutTicks = HlQueryTimeCounterFrequency() * RTLW81_DEVICE_TIMEOUT;

    //
    // Wait for the autoload done bit to be set.
    //

    CurrentTime = KeGetRecentTimeCounter();
    Timeout = CurrentTime + TimeoutTicks;
    do {
        Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterApsFsmco);
        if ((Value & RTLW81_APS_FSMCO_SUS_HOST) != 0) {
            break;
        }

        CurrentTime = KeGetRecentTimeCounter();

    } while (CurrentTime <= Timeout);

    if (CurrentTime > Timeout) {
        Status = STATUS_TIMEOUT;
        goto DefaultInitializeEnd;
    }

    //
    // Reset the BB.
    //

    Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterSysFunctionEnable);
    Value &= ~(RTLW81_SYS_FUNCTION_ENABLE_BBRSTB |
               RTLW81_SYS_FUNCTION_ENABLE_BB_GLB_RST);

    RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterSysFunctionEnable, Value);
    Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterAfeXtalControl2);
    Value |= RTLW81_AFE_XTAL_CONTROL2_ENABLE;
    RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterAfeXtalControl2, Value);

    //
    // Disable hardware power down.
    //

    Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterApsFsmco);
    Value &= ~RTLW81_APS_FSMCO_APDM_HPDN;
    RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterApsFsmco, Value);

    //
    // Disable WLAN suspend.
    //

    Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterApsFsmco);
    Value &= ~(RTLW81_APS_FSMCO_AFSM_HSUS | RTLW81_APS_FSMCO_AFSM_PCIE);
    RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterApsFsmco, Value);

    //
    // Auto-enable WLAN.
    //

    Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterApsFsmco);
    Value |= RTLW81_APS_FSMCO_APFM_ONMAC;
    RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterApsFsmco, Value);
    CurrentTime = KeGetRecentTimeCounter();
    Timeout = CurrentTime + TimeoutTicks;
    do {
        Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterApsFsmco);
        if ((Value & RTLW81_APS_FSMCO_APFM_ONMAC) == 0) {
            break;
        }

        CurrentTime = KeGetRecentTimeCounter();

    } while (CurrentTime <= Timeout);

    if (CurrentTime > Timeout) {
        Status = STATUS_TIMEOUT;
        goto DefaultInitializeEnd;
    }

    //
    // Enable LDO in normal mode.
    //

    Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterLpldoControl);
    Value &= ~RTLW81_LPLDO_CONTROL_DISABLE;
    RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterLpldoControl, Value);

    //
    // Enable MAC DMA/WMAC/Schedule/SEC blocks.
    //

    RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterConfiguration, 0);
    Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterConfiguration);
    Value |= RTLW81_CONFIGURATION_HCI_TRANSMIT_DMA_ENABLE |
             RTLW81_CONFIGURATION_HCI_RECEIVE_DMA_ENABLE |
             RTLW81_CONFIGURATION_TRANSMIT_DMA_ENABLE |
             RTLW81_CONFIGURATION_RECEIVE_DMA_ENABLE |
             RTLW81_CONFIGURATION_PROTOCOL_ENABLE |
             RTLW81_CONFIGURATION_SCHEDULE_ENABLE |
             RTLW81_CONFIGURATION_SEC_ENABLE |
             RTLW81_CONFIGURATION_CALTMR_ENABLE;

    RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterConfiguration, Value);
    Status = STATUS_SUCCESS;

DefaultInitializeEnd:
    return Status;
}

KSTATUS
Rtlw81pInitializeDma (
    PRTLW81_DEVICE Device
    )

/*++

Routine Description:

    This routine initialize the DMA queues for the RTL81xx wireless device.

Arguments:

    Device - Supplies a pointer to the device.

Return Value:

    Status code.

--*/

{

    ULONG HighQueuePageCount;
    BOOL HighQueuePresent;
    ULONG Index;
    ULONG LowQueuePageCount;
    BOOL LowQueuePresent;
    ULONG NormalQueuePageCount;
    BOOL NormalQueuePresent;
    ULONG PageBoundary;
    ULONG PageCount;
    ULONG PagesPerQueue;
    ULONG PublicQueuePageCount;
    ULONG QueueCount;
    ULONG QueueMask;
    ULONG ReceiveBoundary2;
    ULONG RemainingPages;
    KSTATUS Status;
    ULONG Value;

    //
    // Initialize the LLT.
    //

    for (Index = 0; Index < RTLW81_DEFAULT_TRANSMIT_PAGE_COUNT; Index += 1) {
        Status = Rtlw81pWriteLlt(Device, Index, Index + 1);
        if (!KSUCCESS(Status)) {
            goto InitializeDmaEnd;
        }
    }

    Status = Rtlw81pWriteLlt(Device, Index, 0xff);
    if (!KSUCCESS(Status)) {
        goto InitializeDmaEnd;
    }

    for (Index = RTLW81_DEFAULT_TRANSMIT_PAGE_BOUNDARY;
         Index < (RTLW81_DEFAULT_TRANSMIT_PACKET_COUNT - 1);
         Index += 1) {

        Status = Rtlw81pWriteLlt(Device, Index, Index + 1);
        if (!KSUCCESS(Status)) {
            goto InitializeDmaEnd;
        }
    }

    Status = Rtlw81pWriteLlt(Device,
                             Index,
                             RTLW81_DEFAULT_TRANSMIT_PAGE_BOUNDARY);

    if (!KSUCCESS(Status)) {
        goto InitializeDmaEnd;
    }

    //
    // Figure out the initialization values based on the device type and
    // perform device specific DMA initialization steps.
    //

    HighQueuePresent = FALSE;
    NormalQueuePresent = FALSE;
    LowQueuePresent = FALSE;
    if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
        PageBoundary = RTLW81_8188E_TRANSMIT_PAGE_BOUNDARY;
        PublicQueuePageCount = RTLW81_8188E_PUBLIC_QUEUE_PAGE_COUNT;
        NormalQueuePageCount = RTLW81_8188E_NORMAL_QUEUE_PAGE_COUNT;
        LowQueuePageCount = RTLW81_8188E_LOW_QUEUE_PAGE_COUNT;
        HighQueuePageCount = RTLW81_8188E_HIGH_QUEUE_PAGE_COUNT;
        ReceiveBoundary2 = RTLW81_8188E_RECEIVE_BOUNDARY2;
        RTLW81_WRITE_REGISTER16(Device,
                                Rtlw81RegisterNormalQueuePageCount,
                                NormalQueuePageCount);

        QueueCount = Device->BulkOutEndpointCount;
        if (QueueCount == 1) {
            LowQueuePresent = TRUE;

        } else if (QueueCount == 2) {
            HighQueuePresent = TRUE;
            NormalQueuePresent = TRUE;

        } else {
            HighQueuePresent = TRUE;
            NormalQueuePresent = TRUE;
            LowQueuePresent = TRUE;
        }

    } else {
        PageBoundary = RTLW81_DEFAULT_TRANSMIT_PAGE_BOUNDARY;
        PublicQueuePageCount = RTLW81_DEFAULT_PUBLIC_QUEUE_PAGE_COUNT;
        ReceiveBoundary2 = RTLW81_DEFAULT_RECEIVE_BOUNDARY2;

        //
        // Set the number of pages per queue.
        //

        QueueMask = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterUsbEndpoint);
        QueueCount = 0;
        if ((QueueMask & RTLW81_USB_ENDPOINT_HQ_MASK) != 0) {
            HighQueuePresent = TRUE;
            QueueCount += 1;
        }

        if ((QueueMask & RTLW81_USB_ENDPOINT_NQ_MASK) != 0) {
            NormalQueuePresent = TRUE;
            QueueCount += 1;
        }

        if ((QueueMask & RTLW81_USB_ENDPOINT_LQ_MASK) != 0) {
            LowQueuePresent = TRUE;
            QueueCount += 1;
        }

        PageCount = RTLW81_DEFAULT_TRANSMIT_PAGE_COUNT -
                    RTLW81_DEFAULT_PUBLIC_QUEUE_PAGE_COUNT;

        PagesPerQueue = PageCount / QueueCount;
        RemainingPages = PageCount % QueueCount;
        NormalQueuePageCount = PagesPerQueue;
        if ((QueueMask & RTLW81_USB_ENDPOINT_NQ_MASK) == 0) {
            NormalQueuePageCount = 0;
        }

        RTLW81_WRITE_REGISTER8(Device,
                               Rtlw81RegisterNormalQueuePageCount,
                               NormalQueuePageCount);

        HighQueuePageCount = 0;
        if ((QueueMask & RTLW81_USB_ENDPOINT_HQ_MASK) != 0) {
            HighQueuePageCount = PagesPerQueue + RemainingPages;
        }

        LowQueuePageCount = 0;
        if ((QueueMask & RTLW81_USB_ENDPOINT_LQ_MASK) != 0) {
            LowQueuePageCount = PagesPerQueue;
        }
    }

    Value = (PublicQueuePageCount << RTLW81_QUEUE_PAGE_COUNT_PUBLIC_SHIFT) &
            RTLW81_QUEUE_PAGE_COUNT_PUBLIC_MASK;

    Value |= (HighQueuePageCount << RTLW81_QUEUE_PAGE_COUNT_HIGH_SHIFT) &
             RTLW81_QUEUE_PAGE_COUNT_HIGH_MASK;

    Value |= (LowQueuePageCount << RTLW81_QUEUE_PAGE_COUNT_LOW_SHIFT) &
             RTLW81_QUEUE_PAGE_COUNT_LOW_MASK;

    Value |= RTLW81_QUEUE_PAGE_COUNT_LOAD;
    RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterQueuePageCount, Value);

    //
    // Initialize the queue boundaries.
    //

    RTLW81_WRITE_REGISTER8(Device,
                           Rtlw81RegisterTransmitPacketNormalQueueBoundary,
                           PageBoundary);

    RTLW81_WRITE_REGISTER8(Device,
                           Rtlw81RegisterTransmitPacketQueueBoundary,
                           PageBoundary);

    RTLW81_WRITE_REGISTER8(Device,
                           Rtlw81RegisterTransmitPacketWmacLbkBfHd,
                           PageBoundary);

    RTLW81_WRITE_REGISTER8(Device,
                           Rtlw81RegisterTransmitReceiveBoundary0,
                           PageBoundary);

    RTLW81_WRITE_REGISTER8(Device,
                           Rtlw81RegisterTransmitDescriptorControl1,
                           PageBoundary);

    //
    // Set the queue to USB endpoint mappings.
    //

    Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterTransmitReceiveDma);
    Value &= ~RTLW81_TRANSMIT_RECEIVE_DMA_QMAP_MASK;
    if (QueueCount == 1) {
        if (HighQueuePresent != FALSE) {
            Value |= RTLW81_TRANSMIT_RECEIVE_DMA_QMAP_HIGH;

        } else if (NormalQueuePresent != FALSE) {
            Value |= RTLW81_TRANSMIT_RECEIVE_DMA_QMAP_NORMAL;

        } else {

            ASSERT(LowQueuePresent != FALSE);

            Value |= RTLW81_TRANSMIT_RECEIVE_DMA_QMAP_LOW;
        }

    } else if (QueueCount == 2) {
        if (HighQueuePresent == FALSE) {
            Status = STATUS_INVALID_CONFIGURATION;
            goto InitializeDmaEnd;
        }

        if (NormalQueuePresent != FALSE) {
            Value |= RTLW81_TRANSMIT_RECEIVE_DMA_QMAP_HIGH_NORMAL;

        } else {

            ASSERT(LowQueuePresent != FALSE);

            Value |= RTLW81_TRANSMIT_RECEIVE_DMA_QMAP_HIGH_LOW;
        }

    } else {
        Value |= RTLW81_TRANSMIT_RECEIVE_DMA_QMAP_HIGH_NORMAL_LOW;
    }

    RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterTransmitReceiveDma, Value);
    RTLW81_WRITE_REGISTER16(Device,
                            Rtlw81RegisterTransmitReceiveBoundary2,
                            ReceiveBoundary2);

    //
    // Set the transmit and receive page sizes.
    //

    Value = ((RTLW81_PAGE_CONFIGURATION_PAGE_SIZE_128 <<
              RTLW81_PAGE_CONFIGURATION_TRANSMIT_PAGE_SIZE_SHIFT) &
             RTLW81_PAGE_CONFIGURATION_TRANSMIT_PAGE_SIZE_MASK) |
            ((RTLW81_PAGE_CONFIGURATION_PAGE_SIZE_128 <<
              RTLW81_PAGE_CONFIGURATION_RECEIVE_PAGE_SIZE_SHIFT) &
             RTLW81_PAGE_CONFIGURATION_RECEIVE_PAGE_SIZE_MASK);

    RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterPageConfiguration, Value);

InitializeDmaEnd:
    return Status;
}

VOID
Rtlw81pFirmwareReset (
    PRTLW81_DEVICE Device
    )

/*++

Routine Description:

    This routine issues a firmware reset for the RTLW81xx device.

Arguments:

    Device - Supplies a pointer to the RTLW81xx device to reset.

Return Value:

    None.

--*/

{

    ULONGLONG CurrentTime;
    ULONGLONG Timeout;
    ULONGLONG TimeoutTicks;
    ULONG Value;

    if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
        Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterSysFunctionEnable);
        Value &= ~RTLW81_SYS_FUNCTION_ENABLE_CPUEN;
        RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterSysFunctionEnable, Value);
        Value |= RTLW81_SYS_FUNCTION_ENABLE_CPUEN;
        RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterSysFunctionEnable, Value);

    } else {

        //
        // Issue a reset to the 8051.
        //

        RTLW81_WRITE_REGISTER8(Device,
                               Rtlw81RegisterHmetfr3,
                               RTLW81_HMENTFR3_RESET);

        //
        // Wait for the reset to clear itself.
        //

        CurrentTime = KeGetRecentTimeCounter();
        TimeoutTicks = HlQueryTimeCounterFrequency() * RTLW81_DEVICE_TIMEOUT;
        Timeout = CurrentTime + TimeoutTicks;
        do {
            Value = RTLW81_READ_REGISTER16(Device,
                                           Rtlw81RegisterSysFunctionEnable);

            if ((Value & RTLW81_SYS_FUNCTION_ENABLE_CPUEN) == 0) {
                goto FirmwareResetEnd;
            }

            CurrentTime = KeGetRecentTimeCounter();

        } while (CurrentTime <= Timeout);

        //
        // Just force the reset if it didn't clear above.
        //

        Value &= ~RTLW81_SYS_FUNCTION_ENABLE_CPUEN;
        RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterSysFunctionEnable, Value);
    }

FirmwareResetEnd:
    return;
}

KSTATUS
Rtlw81pInitializeFirmware (
    PRTLW81_DEVICE Device,
    PIRP Irp
    )

/*++

Routine Description:

    This routine initializes the device firmware. It loads the firmware binary
    file and write it into the device.

Arguments:

    Device - Supplies a pointer to the RTLW81xx device.

    Irp - Supplies a pointer to the IRP that is driving the initialization.

Return Value:

    Status code.

--*/

{

    ULONG BytesRemaining;
    ULONG BytesThisRound;
    ULONGLONG CurrentTime;
    USHORT DownloadAddress;
    PLOADED_FILE Firmware;
    PUCHAR FirmwareData;
    PRTLW81_FIRMWARE_HEADER FirmwareHeader;
    UINTN FirmwareLength;
    ULONG PageIndex;
    PSTR Path;
    ULONG PathLength;
    KSTATUS Status;
    ULONGLONG Timeout;
    ULONGLONG TimeoutTicks;
    ULONG Value;

    Firmware = NULL;
    if (Device->InitializationPhase == 0) {
        if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
            Path = RTLW81_8188E_FIRMWARE_PATH;

        } else if ((Device->Flags &
                    (RTLW81_FLAG_UMC_A_CUT | RTLW81_FLAG_8192C)) ==
                   RTLW81_FLAG_UMC_A_CUT) {

            Path = RTLW81_8188C_UMC_FIRMWARE_PATH;

        } else {
            Path = RTLW81_DEFAULT_FIRMWARE_PATH;
        }

        //
        // Pend the IRP before starting the asynchronous load of the firmware.
        //

        IoPendIrp(Rtlw81Driver, Irp);
        Device->InitializationIrp = Irp;
        PathLength = RtlStringLength(Path) + 1;
        Status = IoLoadFile(Path,
                            PathLength,
                            Rtlw81pLoadFirmwareCompletionRoutine,
                            Device);

        if (!KSUCCESS(Status)) {
            IoContinueIrp(Rtlw81Driver, Irp);
        }

        goto InitializeFirmwareEnd;
    }

    ASSERT(Device->InitializationPhase == 1);
    ASSERT(Device->InitializationIrp == Irp);
    ASSERT(Device->Firmware != NULL);

    TimeoutTicks = HlQueryTimeCounterFrequency() * RTLW81_DEVICE_TIMEOUT;
    Firmware = Device->Firmware;

    //
    // Make sure the I/O buffer is mapped contiguously.
    //

    Status = MmMapIoBuffer(Firmware->IoBuffer, FALSE, FALSE, TRUE);
    if (!KSUCCESS(Status)) {
        goto InitializeFirmwareEnd;
    }

    //
    // Check for a valid header and skip it.
    //

    FirmwareLength = Firmware->Length;
    if (FirmwareLength < sizeof(RTLW81_FIRMWARE_HEADER)) {
        Status = STATUS_INVALID_CONFIGURATION;
        goto InitializeFirmwareEnd;
    }

    FirmwareData = Firmware->IoBuffer->Fragment[0].VirtualAddress;
    FirmwareHeader = (PRTLW81_FIRMWARE_HEADER)FirmwareData;
    if (((FirmwareHeader->Signature >> 4) != RTLW81_88E_FIRMWARE_SIGNATURE) &&
        ((FirmwareHeader->Signature >> 4) != RTLW81_88C_FIRMWARE_SIGNATURE) &&
        ((FirmwareHeader->Signature >> 4) != RTLW81_92C_FIRMWARE_SIGNATURE)) {

        RtlDebugPrint("RTLW Unsupported FW signature 0x%04x\n",
                      FirmwareHeader->Signature);

        Status = STATUS_NOT_SUPPORTED;
        goto InitializeFirmwareEnd;
    }

    RtlDebugPrint("RTLW Firmware Version %d.%d %02d/%02d %02d:%02d\n",
                  FirmwareHeader->Version,
                  FirmwareHeader->Subversion,
                  FirmwareHeader->Month,
                  FirmwareHeader->MonthDay,
                  FirmwareHeader->Hour,
                  FirmwareHeader->Minute);

    FirmwareData += sizeof(RTLW81_FIRMWARE_HEADER);
    FirmwareLength -= sizeof(RTLW81_FIRMWARE_HEADER);

    //
    // Perform a firmware reset if necessary.
    //

    Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterMcuFirmwareDownload0);
    if ((Value & RTLW81_MCU_FIRMWARE_DOWNLOAD_RAM_DL_SELECT) != 0) {
        Rtlw81pFirmwareReset(Device);
        RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterMcuFirmwareDownload0, 0);
    }

    if ((Device->Flags & RTLW81_FLAG_8188E) == 0) {
        Value = RTLW81_READ_REGISTER16(Device, Rtlw81RegisterSysFunctionEnable);
        Value |= RTLW81_SYS_FUNCTION_ENABLE_CPUEN;
        RTLW81_WRITE_REGISTER16(Device, Rtlw81RegisterSysFunctionEnable, Value);
    }

    //
    // Enable firmware download.
    //

    Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterMcuFirmwareDownload0);
    Value |= RTLW81_MCU_FIRMWARE_DOWNLOAD_ENABLE;
    RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterMcuFirmwareDownload0, Value);
    Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterMcuFirmwareDownload2);
    Value &= ~(RTLW81_MCU_FIRMWARE_DOWNLOAD_CPRST >> 16);
    RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterMcuFirmwareDownload2, Value);

    //
    // Reset the checksum.
    //

    Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterMcuFirmwareDownload0);
    Value |= RTLW81_MCU_FIRMWARE_DOWNLOAD_CHECKSUM_REPORT;
    RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterMcuFirmwareDownload0, Value);

    //
    // Load the firmware into the chip one page at a time.
    //

    PageIndex = 0;
    while (FirmwareLength != 0) {
        BytesThisRound = FirmwareLength;
        if (BytesThisRound > RTLW81_FIRMWARE_PAGE_SIZE) {
            BytesThisRound = RTLW81_FIRMWARE_PAGE_SIZE;
        }

        FirmwareLength -= BytesThisRound;

        //
        // Set the current page.
        //

        Value = RTLW81_READ_REGISTER32(Device,
                                       Rtlw81RegisterMcuFirmwareDownload0);

        Value &= ~RTLW81_MCU_FIRMWARE_DOWNLOAD_PAGE_MASK;
        Value |= (PageIndex << RTLW81_MCU_FIRMWARE_DOWNLOAD_PAGE_SHIFT) &
                 RTLW81_MCU_FIRMWARE_DOWNLOAD_PAGE_MASK;

        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterMcuFirmwareDownload0,
                                Value);

        //
        // Write the bytes to the current page.
        //

        DownloadAddress = Rtlw81RegisterFirmwareDownload;
        BytesRemaining = BytesThisRound;
        while (BytesRemaining != 0) {
            if (BytesRemaining > RTLW81_MAX_FIRMWARE_WRITE_SIZE) {
                BytesThisRound = RTLW81_MAX_FIRMWARE_WRITE_SIZE;

            } else if (BytesRemaining > 4) {
                BytesThisRound = 4;

            } else {
                BytesThisRound = 1;
            }

            Rtlw81pWriteData(Device,
                             DownloadAddress,
                             FirmwareData,
                             BytesThisRound);

            DownloadAddress += BytesThisRound;
            FirmwareData += BytesThisRound;
            BytesRemaining -= BytesThisRound;
        }

        PageIndex += 1;
    }

    //
    // Disable firmware download.
    //

    Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterMcuFirmwareDownload0);
    Value &= ~RTLW81_MCU_FIRMWARE_DOWNLOAD_ENABLE;
    RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterMcuFirmwareDownload0, Value);
    RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterMcuFirmwareDownload1, 0);

    //
    // Wait for the checksum report.
    //

    CurrentTime = KeGetRecentTimeCounter();
    Timeout = CurrentTime + TimeoutTicks;
    do {
        Value = RTLW81_READ_REGISTER32(Device,
                                       Rtlw81RegisterMcuFirmwareDownload0);

        if ((Value & RTLW81_MCU_FIRMWARE_DOWNLOAD_CHECKSUM_REPORT) != 0) {
            break;
        }

        CurrentTime = KeGetRecentTimeCounter();

    } while (CurrentTime <= Timeout);

    if (CurrentTime > Timeout) {
        Status = STATUS_TIMEOUT;
        goto InitializeFirmwareEnd;
    }

    Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterMcuFirmwareDownload0);
    Value &= ~RTLW81_MCU_FIRMWARE_DOWNLOAD_WINTINI_READY;
    Value |= RTLW81_MCU_FIRMWARE_DOWNLOAD_READY;
    RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterMcuFirmwareDownload0, Value);

    //
    // Reset again for RTL8188E devices.
    //

    if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
        Rtlw81pFirmwareReset(Device);
    }

    //
    // Wait for the device to signal that the firmware is ready.
    //

    CurrentTime = KeGetRecentTimeCounter();
    Timeout = CurrentTime + TimeoutTicks;
    do {
        Value = RTLW81_READ_REGISTER32(Device,
                                       Rtlw81RegisterMcuFirmwareDownload0);

        if ((Value & RTLW81_MCU_FIRMWARE_DOWNLOAD_WINTINI_READY) != 0) {
            break;
        }

        CurrentTime = KeGetRecentTimeCounter();

    } while (CurrentTime <= Timeout);

    if (CurrentTime > Timeout) {
        Status = STATUS_TIMEOUT;
        goto InitializeFirmwareEnd;
    }

    Status = STATUS_SUCCESS;

InitializeFirmwareEnd:

    //
    // Unload the firmware.
    //

    if (Firmware != NULL) {

        ASSERT(Firmware == Device->Firmware);

        Device->Firmware = NULL;
        IoUnloadFile(Firmware);
    }

    if (!KSUCCESS(Status)) {
        RtlDebugPrint("RTLW: Initailize firmware failed 0x%08x\n", Status);
    }

    return Status;
}

VOID
Rtlw81pLoadFirmwareCompletionRoutine (
    PVOID Context,
    PLOADED_FILE File
    )

/*++

Routine Description:

    This routine is called when the asynchronous firmware load has completed.

Arguments:

    Context - Supplies the context supplied by the caller who initiation the
        file load.

    File - Supplies a pointer to the loaded file.

Return Value:

    None.

--*/

{

    PRTLW81_DEVICE Device;

    Device = (PRTLW81_DEVICE)Context;
    Device->Firmware = File;
    IoContinueIrp(Rtlw81Driver, Device->InitializationIrp);
    return;
}

VOID
Rtlw81pLcCalibration (
    PRTLW81_DEVICE Device
    )

/*++

Routine Description:

    This routine performcs LC calibration.

Arguments:

    Device - Supplies a pointer to the RTLW81xx device to calibrate.

Return Value:

    None.

--*/

{

    ULONG Index;
    ULONG RfAc[RTLW81_MAX_CHAIN_COUNT];
    UCHAR TransmitMode;
    ULONG Value;

    //
    // If the transmit mode is enabled, then disable all continuous transmits
    // and set the RF mode to standby.
    //

    TransmitMode = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterOfdm1Lstf3);
    if ((TransmitMode & RTLW81_OFDM1_LSTF3_TRANSMIT_ENABLED) != 0) {
        Value = TransmitMode & ~RTLW81_OFDM1_LSTF3_TRANSMIT_ENABLED;
        RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterOfdm1Lstf3, Value);
        for (Index = 0; Index < Device->ReceiveChainCount; Index += 1) {
            RfAc[Index] = Rtlw81pReadRfRegister(Device,
                                                Index,
                                                Rtlw81RfRegisterAc);

            Value = RfAc[Index];
            Value &= ~RTLW81_RF_AC_MODE_MASK;
            Value |= (RTLW81_RF_AC_MODE_STANDBY << RTLW81_RF_AC_MODE_SHIFT) &
                     RTLW81_RF_AC_MODE_MASK;

            Rtlw81pWriteRfRegister(Device, Index, Rtlw81RfRegisterAc, Value);
        }

    //
    // Otherwise block all transfer queues.
    //

    } else {
        RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterTransmitPause, 0xFF);
    }

    //
    // Start the calibration process.
    //

    Value = Rtlw81pReadRfRegister(Device, 0, Rtlw81RfRegisterChannelBandwidth);
    Value |= RTLW81_RF_CHANNEL_BANDWIDTH_LC_START;
    Rtlw81pWriteRfRegister(Device, 0, Rtlw81RfRegisterChannelBandwidth, Value);
    KeDelayExecution(FALSE, FALSE, 100 * MICROSECONDS_PER_MILLISECOND);

    //
    // Restore the mode.
    //

    if ((TransmitMode & RTLW81_OFDM1_LSTF3_TRANSMIT_ENABLED) != 0) {
        RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterOfdm1Lstf3, TransmitMode);
        for (Index = 0; Index < Device->ReceiveChainCount; Index += 1) {
            Value = RfAc[Index];
            Rtlw81pWriteRfRegister(Device, Index, Rtlw81RfRegisterAc, Value);
        }

    } else {
        RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterTransmitPause, 0x00);
    }

    return;
}

VOID
Rtlw81pSetChannel (
    PRTLW81_DEVICE Device,
    ULONG Channel
    )

/*++

Routine Description:

    This routine sets the given channel for the RTLW81xx device.

Arguments:

    Device - Supplies a pointer to the RTLW81xx device.

    Channel - Supplies the channel to set in the RTLW81xx device.

Return Value:

    None.

--*/

{

    ULONG BandwidthValue;
    ULONG Index;
    ULONG Value;

    //
    // Do nothing if the desired channel is already set.
    //

    if (Device->CurrentChannel == Channel) {
        return;
    }

    //
    // Enable transmit power on the channel.
    //

    for (Index = 0; Index < Device->TransmitChainCount; Index += 1) {
        Rtlw81pEnableChannelTransmitPower(Device, Index, Channel);
    }

    //
    // Enable the channel for receive.
    //

    for (Index = 0; Index < Device->ReceiveChainCount; Index += 1) {
        Value = Rtlw81pReadRfRegister(Device,
                                      Index,
                                      Rtlw81RfRegisterChannelBandwidth);

        Value &= ~RTLW81_RF_CHANNEL_BANDWIDTH_CHANNEL_MASK;
        Value |= (Channel << RTLW81_RF_CHANNEL_BANDWIDTH_CHANNEL_SHIFT) &
                 RTLW81_RF_CHANNEL_BANDWIDTH_CHANNEL_MASK;

        Rtlw81pWriteRfRegister(Device,
                               Index,
                               Rtlw81RfRegisterChannelBandwidth,
                               Value);
    }

    //
    // Set the bandwidth to 20MHz.
    //

    Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterBandwidthMode);
    Value |= RTLW81_BANDWIDTH_MODE_20MHZ;
    RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterBandwidthMode, Value);
    Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterFpga0Rfmod);
    Value &= ~RTLW81_RFMOD_40MHZ;
    RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterFpga0Rfmod, Value);
    Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterFpga1Rfmod);
    Value &= ~RTLW81_RFMOD_40MHZ;
    RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterFpga1Rfmod, Value);
    if ((Device->Flags & RTLW81_FLAG_8188E) == 0) {
        Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterFpga0AnaParam2);
        Value |= RTLW81_FPGA0_ANA_PARAM2_CBW20;
        RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterFpga0AnaParam2, Value);
    }

    BandwidthValue = RTLW81_RF_CHANNEL_BANDWIDTH_DEFAULT_20MHZ;
    if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
        BandwidthValue = RTLW81_RF_CHANNEL_BANDWIDTH_8188E_20MHZ;
    }

    for (Index = 0; Index < Device->ReceiveChainCount; Index += 1) {
        Value = Rtlw81pReadRfRegister(Device,
                                      Index,
                                      Rtlw81RfRegisterChannelBandwidth);

        Value &= ~RTLW81_RF_CHANNEL_BANDWIDTH_CHANNEL_MASK;
        Value |= (Channel << RTLW81_RF_CHANNEL_BANDWIDTH_CHANNEL_SHIFT) &
                 RTLW81_RF_CHANNEL_BANDWIDTH_CHANNEL_MASK;

        Value |= BandwidthValue;
        Rtlw81pWriteRfRegister(Device,
                               Index,
                               Rtlw81RfRegisterChannelBandwidth,
                               Value);
    }

    Device->CurrentChannel = Channel;
    return;
}

VOID
Rtlw81pEnableChannelTransmitPower (
    PRTLW81_DEVICE Device,
    ULONG Chain,
    ULONG Channel
    )

/*++

Routine Description:

    This routine enables the transmit power for the given channel on the
    supplied chain.

Arguments:

    Device - Supplies a pointer to the RTLW81xx device.

    Chain - Supplies the transmit chain to enable.

    Channel - Supplies the channel to set in the RTLW81xx device.

Return Value:

    None.

--*/

{

    USHORT Bw20Power;
    USHORT CckPower;
    PRTLW81_DEFAULT_TRANSMIT_POWER_DATA DefaultPowerData;
    ULONG Diff;
    ULONG Group;
    USHORT HtPower;
    ULONG Index;
    UCHAR MaxPower;
    USHORT OfdmPower;
    USHORT PowerStates[RTLW81_POWER_STATE_COUNT];
    RTLW81_REGISTER Register;
    PRTLW81_8188E_TRANSMIT_POWER_DATA Rtl8188ePowerData;
    ULONG Value;

    RtlZeroMemory(PowerStates, RTLW81_POWER_STATE_COUNT * sizeof(USHORT));
    if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
        Rtl8188ePowerData = Rtlw8188eTransmitPowerData;
        if (Chain <= 2) {
            Group = 0;

        } else if (Chain <= 5) {
            Group = 1;

        } else if (Chain <= 8) {
            Group = 2;

        } else if (Chain <= 11) {
            Group = 3;

        } else if (Chain <= 13) {
            Group = 4;

        } else {
            Group = 5;
        }

        if (Device->Regulatory == 0) {
            for (Index = 0; Index <= 3; Index += 1) {
                PowerStates[Index] = Rtl8188ePowerData->GroupPower[0][Index];
            }
        }

        for (Index = 4; Index < RTLW81_POWER_STATE_COUNT; Index += 1) {
            if (Device->Regulatory == 3) {
                PowerStates[Index] = Rtl8188ePowerData->GroupPower[0][Index];

            } else if (Device->Regulatory == 1) {
                PowerStates[Index] =
                                   Rtl8188ePowerData->GroupPower[Group][Index];

            } else if (Device->Regulatory != 2) {
                PowerStates[Index] = Rtl8188ePowerData->GroupPower[0][Index];
            }
        }

        CckPower = Device->Power.Rtlw8188e.CckTransmitPower[Group];
        HtPower = Device->Power.Rtlw8188e.Ht40TransmitPower[Group];
        OfdmPower = HtPower + Device->Power.Rtlw8188e.OfdmTransmitPowerDiff;
        Bw20Power = HtPower + Device->Power.Rtlw8188e.Bw20TransmitPowerDiff;

    } else {
        if (((Device->Flags & RTLW81_FLAG_8192C) == 0) &&
            (Device->BoardType == RTLW81_ROM_RF_OPT1_BOARD_TYPE_HIGHPA)) {

            DefaultPowerData = &Rtlw8188ruTransmitPowerData[Chain];

        } else {
            DefaultPowerData = &RtlwDefaultTransmitPowerData[Chain];
        }

        if (Channel <= 3) {
            Group = 0;

        } else if (Channel <= 9) {
            Group = 1;

        } else {
            Group = 2;
        }

        if (Device->Regulatory == 0) {
            for (Index = 0; Index <= 3; Index += 1) {
                PowerStates[Index] = DefaultPowerData->GroupPower[0][Index];
            }
        }

        for (Index = 4; Index < RTLW81_POWER_STATE_COUNT; Index += 1) {
            if (Device->Regulatory == 3) {
                PowerStates[Index] = DefaultPowerData->GroupPower[Group][Index];
                MaxPower = Device->Power.Default.Ht20MaxPower[Group];
                MaxPower = (MaxPower >> (Chain * 4)) & 0xF;
                if (PowerStates[Index] > MaxPower) {
                    PowerStates[Index] = MaxPower;
                }

            } else if (Device->Regulatory == 1) {
                PowerStates[Index] = DefaultPowerData->GroupPower[Group][Index];

            } else if (Device->Regulatory != 2) {
                PowerStates[Index] = DefaultPowerData->GroupPower[0][Index];
            }
        }

        CckPower = Device->Power.Default.CckTransmitPower[Chain][Group];
        HtPower = Device->Power.Default.Ht40TransmitPower[Chain][Group];
        if (Device->TransmitChainCount > 1) {
            Diff = Device->Power.Default.Ht40TransmitPowerDiff[Group];
            Diff = (Diff >> (Chain * 4)) & 0xF;
            if (HtPower > Diff) {
                HtPower = HtPower - Diff;

            } else {
                HtPower = 0;
            }
        }

        Diff = Device->Power.Default.OfdmTransmitPowerDiff[Group];
        Diff = (Diff >> (Chain * 4)) & 0xF;
        OfdmPower = HtPower + Diff;
        Diff = Device->Power.Default.Ht20TransmitPowerDiff[Group];
        Diff = (Diff >> (Chain * 4)) & 0xF;
        Bw20Power = HtPower + Diff;
    }

    for (Index = 0; Index <= 3; Index += 1) {
        PowerStates[Index] += CckPower;
        if (PowerStates[Index] > RTLW81_MAX_TRANSMIT_POWER) {
            PowerStates[Index] = RTLW81_MAX_TRANSMIT_POWER;
        }
    }

    for (Index = 4; Index <= 11; Index += 1) {
        PowerStates[Index] += OfdmPower;
        if (PowerStates[Index] > RTLW81_MAX_TRANSMIT_POWER) {
            PowerStates[Index] = RTLW81_MAX_TRANSMIT_POWER;
        }
    }

    for (Index = 12; Index < RTLW81_POWER_STATE_COUNT; Index += 1) {
        PowerStates[Index] += Bw20Power;
        if (PowerStates[Index] > RTLW81_MAX_TRANSMIT_POWER) {
            PowerStates[Index] = RTLW81_MAX_TRANSMIT_POWER;
        }
    }

    //
    // Now set the power states in the hardware.
    //

    if (Chain == 0) {
        Value = RTLW81_READ_REGISTER32(Device,
                                       Rtlw81RegisterTransmitAgcACck1Mcs32);

        Value &= ~RLTW81_TRANSMIT_AGC_A_CCK1_MCS32_MASK;
        Value |= (PowerStates[0] << RLTW81_TRANSMIT_AGC_A_CCK1_MCS32_SHIFT) &
                 RLTW81_TRANSMIT_AGC_A_CCK1_MCS32_MASK;

        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterTransmitAgcACck1Mcs32,
                                Value);

        Value = RTLW81_READ_REGISTER32(Device,
                                       Rtlw81RegisterTransmitAgcBCck11ACck211);

        Value &= ~RTLW81_TRANSMIT_AGC_A_CCK2_MASK;
        Value |= (PowerStates[1] << RTLW81_TRANSMIT_AGC_A_CCK2_SHIFT) &
                 RTLW81_TRANSMIT_AGC_A_CCK2_MASK;

        Value &= ~RTLW81_TRANSMIT_AGC_A_CCK55_MASK;
        Value |= (PowerStates[2] << RTLW81_TRANSMIT_AGC_A_CCK55_SHIFT) &
                 RTLW81_TRANSMIT_AGC_A_CCK55_MASK;

        Value &= ~RTLW81_TRANSMIT_AGC_A_CCK11_MASK;
        Value |= (PowerStates[3] << RTLW81_TRANSMIT_AGC_A_CCK11_SHIFT) &
                 RTLW81_TRANSMIT_AGC_A_CCK11_MASK;

        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterTransmitAgcBCck11ACck211,
                                Value);

    } else {
        Value = RTLW81_READ_REGISTER32(Device,
                                       Rtlw81RegisterTransmitAgcBCck155Mcs32);

        Value &= ~RTLW81_TRANSMIT_AGC_B_CCK1_MASK;
        Value |= (PowerStates[0] << RTLW81_TRANSMIT_AGC_B_CCK1_SHIFT) &
                 RTLW81_TRANSMIT_AGC_B_CCK1_MASK;

        Value &= ~RTLW81_TRANSMIT_AGC_B_CCK2_MASK;
        Value |= (PowerStates[1] << RTLW81_TRANSMIT_AGC_B_CCK2_SHIFT) &
                 RTLW81_TRANSMIT_AGC_B_CCK2_MASK;

        Value &= ~RTLW81_TRANSMIT_AGC_B_CCK55_MASK;
        Value |= (PowerStates[2] << RTLW81_TRANSMIT_AGC_B_CCK55_SHIFT) &
                 RTLW81_TRANSMIT_AGC_B_CCK55_MASK;

        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterTransmitAgcBCck155Mcs32,
                                Value);

        Value = RTLW81_READ_REGISTER32(Device,
                                       Rtlw81RegisterTransmitAgcBCck11ACck211);

        Value &= ~RTLW81_TRANSMIT_AGC_B_CCK11_MASK;
        Value |= (PowerStates[3] << RTLW81_TRANSMIT_AGC_B_CCK11_SHIFT) &
                 RTLW81_TRANSMIT_AGC_B_CCK11_MASK;

        RTLW81_WRITE_REGISTER32(Device,
                                Rtlw81RegisterTransmitAgcBCck11ACck211,
                                Value);
    }

    Value = ((PowerStates[4] << RTLW81_TRANSMIT_AGC_RATE_06_SHIFT) &
             RTLW81_TRANSMIT_AGC_RATE_06_MASK) |
            ((PowerStates[5] << RTLW81_TRANSMIT_AGC_RATE_09_SHIFT) &
             RTLW81_TRANSMIT_AGC_RATE_09_MASK) |
            ((PowerStates[6] << RTLW81_TRANSMIT_AGC_RATE_12_SHIFT) &
             RTLW81_TRANSMIT_AGC_RATE_12_MASK) |
            ((PowerStates[7] << RTLW81_TRANSMIT_AGC_RATE_18_SHIFT) &
             RTLW81_TRANSMIT_AGC_RATE_18_MASK);

    Register = Rtlw81RegisterTransmitAgcRate1806Chain0;
    if (Chain == 1) {
        Register = Rtlw81RegisterTransmitAgcRate1806Chain1;
    }

    RTLW81_WRITE_REGISTER32(Device, Register, Value);
    Value = ((PowerStates[8] << RTLW81_TRANSMIT_AGC_RATE_24_SHIFT) &
             RTLW81_TRANSMIT_AGC_RATE_24_MASK) |
            ((PowerStates[9] << RTLW81_TRANSMIT_AGC_RATE_36_SHIFT) &
             RTLW81_TRANSMIT_AGC_RATE_36_MASK) |
            ((PowerStates[10] << RTLW81_TRANSMIT_AGC_RATE_48_SHIFT) &
             RTLW81_TRANSMIT_AGC_RATE_48_MASK) |
            ((PowerStates[11] << RTLW81_TRANSMIT_AGC_RATE_54_SHIFT) &
             RTLW81_TRANSMIT_AGC_RATE_54_MASK);

    Register = Rtlw81RegisterTransmitAgcRate5424Chain0;
    if (Chain == 1) {
        Register = Rtlw81RegisterTransmitAgcRate5424Chain1;
    }

    RTLW81_WRITE_REGISTER32(Device, Register, Value);
    Value = ((PowerStates[12] << RTLW81_TRANSMIT_AGC_MCS00_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS00_MASK) |
            ((PowerStates[13] << RTLW81_TRANSMIT_AGC_MCS01_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS01_MASK) |
            ((PowerStates[14] << RTLW81_TRANSMIT_AGC_MCS02_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS02_MASK) |
            ((PowerStates[15] << RTLW81_TRANSMIT_AGC_MCS03_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS03_MASK);

    Register = Rtlw81RegisterTransmitAgcMcs03Mcs00Chain0;
    if (Chain == 1) {
        Register = Rtlw81RegisterTransmitAgcMcs03Mcs00Chain1;
    }

    RTLW81_WRITE_REGISTER32(Device, Register, Value);
    Value = ((PowerStates[16] << RTLW81_TRANSMIT_AGC_MCS04_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS04_MASK) |
            ((PowerStates[17] << RTLW81_TRANSMIT_AGC_MCS05_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS05_MASK) |
            ((PowerStates[18] << RTLW81_TRANSMIT_AGC_MCS06_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS06_MASK) |
            ((PowerStates[19] << RTLW81_TRANSMIT_AGC_MCS07_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS07_MASK);

    Register = Rtlw81RegisterTransmitAgcMcs07Mcs04Chain0;
    if (Chain == 1) {
        Register = Rtlw81RegisterTransmitAgcMcs07Mcs04Chain1;
    }

    RTLW81_WRITE_REGISTER32(Device, Register, Value);
    Value = ((PowerStates[20] << RTLW81_TRANSMIT_AGC_MCS08_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS08_MASK) |
            ((PowerStates[21] << RTLW81_TRANSMIT_AGC_MCS09_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS09_MASK) |
            ((PowerStates[22] << RTLW81_TRANSMIT_AGC_MCS10_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS10_MASK) |
            ((PowerStates[23] << RTLW81_TRANSMIT_AGC_MCS11_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS11_MASK);

    Register = Rtlw81RegisterTransmitAgcMcs11Mcs08Chain0;
    if (Chain == 1) {
        Register = Rtlw81RegisterTransmitAgcMcs11Mcs08Chain1;
    }

    RTLW81_WRITE_REGISTER32(Device, Register, Value);
    Value = ((PowerStates[24] << RTLW81_TRANSMIT_AGC_MCS12_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS12_MASK) |
            ((PowerStates[25] << RTLW81_TRANSMIT_AGC_MCS13_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS13_MASK) |
            ((PowerStates[26] << RTLW81_TRANSMIT_AGC_MCS14_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS14_MASK) |
            ((PowerStates[27] << RTLW81_TRANSMIT_AGC_MCS15_SHIFT) &
             RTLW81_TRANSMIT_AGC_MCS15_MASK);

    Register = Rtlw81RegisterTransmitAgcMcs15Mcs12Chain0;
    if (Chain == 1) {
        Register = Rtlw81RegisterTransmitAgcMcs15Mcs12Chain1;
    }

    RTLW81_WRITE_REGISTER32(Device, Register, Value);
    return;
}

KSTATUS
Rtlw81pWriteLlt (
    PRTLW81_DEVICE Device,
    ULONG Address,
    ULONG Data
    )

/*++

Routine Description:

    This routine writes the given data to the LLT at the supplied address.

Arguments:

    Device - Supplies a pointer to the RTLW81 device.

    Address - Supplies the address of the LLT to write.

    Data - Supplies the data to write to the LLT.

Return Value:

    Status code.

--*/

{

    ULONGLONG CurrentTime;
    KSTATUS Status;
    ULONGLONG Timeout;
    ULONGLONG TimeoutTicks;
    ULONG Value;

    Value = (RTLW81_LLT_INIT_OP_WRITE << RTLW81_LLT_INIT_OP_SHIFT) |
            ((Data << RTLW81_LLT_INIT_DATA_SHIFT) & RTLW81_LLT_INIT_DATA_MASK) |
            ((Address << RTLW81_LLT_INIT_ADDRESS_SHIFT) &
             RTLW81_LLT_INIT_ADDRESS_MASK);

    RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterLltInit, Value);

    //
    // Wait for the write to complete.
    //

    CurrentTime = KeGetRecentTimeCounter();
    TimeoutTicks = HlQueryTimeCounterFrequency() * RTLW81_DEVICE_TIMEOUT;
    Timeout = CurrentTime + TimeoutTicks;
    do {
        Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterLltInit);
        Value = (Value & RTLW81_LLT_INIT_OP_MASK) >> RTLW81_LLT_INIT_OP_SHIFT;
        if (Value == RTLW81_LLT_INIT_OP_NO_ACTIVE) {
            break;
        }

        CurrentTime = KeGetRecentTimeCounter();

    } while (CurrentTime <= Timeout);

    if (CurrentTime > Timeout) {
        Status = STATUS_TIMEOUT;
        goto WriteLltEnd;
    }

    Status = STATUS_SUCCESS;

WriteLltEnd:
    return Status;
}

KSTATUS
Rtlw81pWriteData (
    PRTLW81_DEVICE Device,
    USHORT Address,
    PVOID Data,
    ULONG DataLength
    )

/*++

Routine Description:

    This routine performs a write to the wireless RTL81xx device at the given
    address.

Arguments:

    Device - Supplies a pointer to the device.

    Address - Supplies the address within the configuration space to write.

    Data - Supplies a pointer to the value to write.

    DataLength - Supplies the size of the data to write, in bytes.

Return Value:

    Status code.

--*/

{

    PUSB_TRANSFER ControlTransfer;
    PUSB_SETUP_PACKET Setup;
    KSTATUS Status;

    ControlTransfer = Device->ControlTransfer;
    Setup = ControlTransfer->Buffer;
    Setup->RequestType = USB_SETUP_REQUEST_TO_DEVICE |
                         USB_SETUP_REQUEST_VENDOR |
                         USB_SETUP_REQUEST_DEVICE_RECIPIENT;

    Setup->Request = RTLW81_VENDOR_REQUEST_REGISTER;
    Setup->Value = Address;
    Setup->Index = 0;
    Setup->Length = DataLength;
    RtlCopyMemory(Setup + 1, Data, DataLength);
    ControlTransfer->Direction = UsbTransferDirectionOut;
    ControlTransfer->Length = sizeof(USB_SETUP_PACKET) + DataLength;
    Status = UsbSubmitSynchronousTransfer(ControlTransfer);
    if (!KSUCCESS(Status) && KSUCCESS(Device->InitializationStatus)) {
        RtlDebugPrint("RTLW81: Write to address 0x%04x failed with status "
                      "0x%08x\n",
                      Address,
                      Status);

        Device->InitializationStatus = Status;
    }

    return Status;
}

KSTATUS
Rtlw81pReadData (
    PRTLW81_DEVICE Device,
    USHORT Address,
    PVOID Data,
    ULONG DataLength
    )

/*++

Routine Description:

    This routine performs a read from the wireless RTL81xx device at the given
    address.

Arguments:

    Device - Supplies a pointer to the device.

    Address - Supplies the address within the device configuration space to
        read.

    Data - Supplies a pointer to the data buffer to receive the read data.

    DataLength - Supplies the number of bytes to read.

Return Value:

    Status code.

--*/

{

    PUSB_TRANSFER ControlTransfer;
    PUSB_SETUP_PACKET Setup;
    KSTATUS Status;

    ControlTransfer = Device->ControlTransfer;
    Setup = ControlTransfer->Buffer;
    Setup->RequestType = USB_SETUP_REQUEST_TO_HOST |
                         USB_SETUP_REQUEST_VENDOR |
                         USB_SETUP_REQUEST_DEVICE_RECIPIENT;

    Setup->Request = RTLW81_VENDOR_REQUEST_REGISTER;
    Setup->Value = Address;
    Setup->Index = 0;
    Setup->Length = DataLength;
    ControlTransfer->Direction = UsbTransferDirectionIn;
    ControlTransfer->Length = sizeof(USB_SETUP_PACKET) + DataLength;
    Status = UsbSubmitSynchronousTransfer(ControlTransfer);
    if (KSUCCESS(Status)) {
        RtlCopyMemory(Data, Setup + 1, DataLength);

    } else if (KSUCCESS(Device->InitializationStatus)) {
        RtlDebugPrint("RTLW81: Read from address 0x%04x failed with status "
                      "0x%08x\n",
                      Address,
                      Status);

        Device->InitializationStatus = Status;
    }

    return Status;
}

VOID
Rtlw81pWriteRegister (
    PRTLW81_DEVICE Device,
    USHORT Register,
    ULONG Data,
    ULONG DataLength
    )

/*++

Routine Description:

    This routine performs a register write to the wireless RTL81xx device.

Arguments:

    Device - Supplies a pointer to the device.

    Register - Supplies the register number to write to.

    Data - Supplies a pointer to the value to write.

    DataLength - Supplies the size of the data to write, in bytes.

Return Value:

    Status code.

--*/

{

    Rtlw81pWriteData(Device, Register, &Data, DataLength);
    return;
}

ULONG
Rtlw81pReadRegister (
    PRTLW81_DEVICE Device,
    USHORT Register,
    ULONG DataLength
    )

/*++

Routine Description:

    This routine performs a register read from the wireless RTL81xx device.

Arguments:

    Device - Supplies a pointer to the device.

    Register - Supplies the register number to read from.

    DataLength - Supplies the number of bytes to read.

Return Value:

    Returns the value read from the register.

--*/

{

    ULONG Data;

    Data = 0;
    Rtlw81pReadData(Device, Register, &Data, DataLength);
    return Data;
}

UCHAR
Rtlw81pEfuseRead8 (
    PRTLW81_DEVICE Device,
    USHORT Address
    )

/*++

Routine Description:

    This routine reads a byte from the EFUSE region.

Arguments:

    Device - Supplies a pointer to the RTLW81 device.

    Address - Supplies the address of the EFUSE byte to read.

Return Value:

    Returns the value of the byte read from the EFUSE region..

--*/

{

    UCHAR Data;
    ULONG Index;
    ULONG Value;

    Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterEfuseControl);
    Value &= ~RTLW81_EFUSE_CONTROL_ADDRESS_MASK;
    Value |= (Address << RTLW81_EFUSE_CONTROL_ADDRESS_SHIFT) &
             RTLW81_EFUSE_CONTROL_ADDRESS_MASK;

    Value &= ~RTLW81_EFUSE_CONTROL_VALID;
    RTLW81_WRITE_REGISTER32(Device, Rtlw81RegisterEfuseControl, Value);

    //
    // Wait for the operation to complete.
    //

    for (Index = 0; Index < RTLW81_EFUSE_RETRY_COUNT; Index += 1) {
        Value = RTLW81_READ_REGISTER32(Device, Rtlw81RegisterEfuseControl);
        if ((Value & RTLW81_EFUSE_CONTROL_VALID) != 0) {
            break;
        }
    }

    Data = (Value & RTLW81_EFUSE_CONTROL_DATA_MASK) >>
           RTLW81_EFUSE_CONTROL_DATA_SHIFT;

    return Data;
}

VOID
Rtlw81pWriteRfRegister (
    PRTLW81_DEVICE Device,
    ULONG Chain,
    ULONG RfRegister,
    ULONG Data
    )

/*++

Routine Description:

    This routine writes an RF register for the given chain.

Arguments:

    Device - Supplies a pointer to the RTLW81 device.

    Chain - Supplies the chain whose RF register is to be written.

    RfRegister - Supplies the register to which the write will be performed.

    Data - Supplies the value to write to the RF register.

Return Value:

    None.

--*/

{

    ULONG Register;
    ULONG Value;

    Register = Rtlw81RegisterLssiParameter + (4 * Chain);
    if ((Device->Flags & RTLW81_FLAG_8188E) != 0) {
        Value = (RfRegister << RTLW81_LSSI_PARAMETER_8188E_ADDRESS_SHIFT) &
                RTLW81_LSSI_PARAMETER_8188E_ADDRESS_MASK;

    } else {
        Value = (RfRegister << RTLW81_LSSI_PARAMETER_DEFAULT_ADDRESS_SHIFT) &
                RTLW81_LSSI_PARAMETER_DEFAULT_ADDRESS_MASK;
    }

    Value |= (Data << RTLW81_LSSI_PARAMETER_DATA_SHIFT) &
             RTLW81_LSSI_PARAMETER_DATA_MASK;

    RTLW81_WRITE_REGISTER32(Device, Register, Value);
    return;
}

ULONG
Rtlw81pReadRfRegister (
    PRTLW81_DEVICE Device,
    ULONG Chain,
    ULONG RfRegister
    )

/*++

Routine Description:

    This routine read an RF register for the given chain.

Arguments:

    Device - Supplies a pointer to the RTLW81 device.

    Chain - Supplies the chain whose RF register is to be read.

    RfRegister - Supplies the RF register to read.

Return Value:

    Returns the value read from the RF register..

--*/

{

    ULONG ChainValues[RTLW81_MAX_CHAIN_COUNT];
    ULONG Register;
    ULONG Value;

    Register = Rtlw81RegisterHssiParameter2;
    ChainValues[0] = RTLW81_READ_REGISTER32(Device, Register);
    if (Chain != 0) {
        Register += (Chain * 8);
        ChainValues[Chain] = RTLW81_READ_REGISTER32(Device, Register);
    }

    //
    // Initiate the read from the RF register.
    //

    Value = ChainValues[0];
    Value &= ~RTLW81_HSSI_PARAMETER2_READ_EDGE;
    Register = Rtlw81RegisterHssiParameter2;
    RTLW81_WRITE_REGISTER32(Device, Register, Value);
    HlBusySpin(100);
    Value = ChainValues[Chain];
    Value &= ~RTLW81_HSSI_PARAMETER2_READ_ADDRESS_MASK;
    Value |= (RfRegister << RTLW81_HSSI_PARAMETER2_READ_ADDRESS_SHIFT) &
             RTLW81_HSSI_PARAMETER2_READ_ADDRESS_MASK;

    Value |= RTLW81_HSSI_PARAMETER2_READ_EDGE;
    Register += (Chain * 8);
    RTLW81_WRITE_REGISTER32(Device, Register, Value);
    HlBusySpin(100);
    Value = ChainValues[0];
    Value |= RTLW81_HSSI_PARAMETER2_READ_EDGE;
    Register = Rtlw81RegisterHssiParameter2;
    RTLW81_WRITE_REGISTER32(Device, Register, Value);
    HlBusySpin(100);

    //
    // Read the value back from the appropriate register.
    //

    Register = Rtlw81RegisterHssiParameter1 + (Chain * 8);
    Value = RTLW81_READ_REGISTER32(Device, Register);
    if ((Value & RTLW81_HSSI_PARAMETER1_PI) != 0) {
        Register = Rtlw81RegisterHspiReadback + (Chain * 4);

    } else {
        Register = Rtlw81RegisterLssiReadback + (Chain * 4);
    }

    Value = RTLW81_READ_REGISTER32(Device, Register);
    Value = (Value & RTLW81_LSSI_READBACK_DATA_MASK) >>
            RTLW81_LSSI_READBACK_DATA_SHIFT;

    return Value;
}

KSTATUS
Rtlw81pSendFirmwareCommand (
    PRTLW81_DEVICE Device,
    UCHAR CommandId,
    PVOID Message,
    ULONG MessageLength
    )

/*++

Routine Description:

    This routine sends a firmware command to the wireless RTL81xx device.

Arguments:

    Device - Supplies a pointer to the RTLW81xx device.

    CommandId - Supplies the firmware command ID to send to the device.

    Message - Supplies a pointer to the command message.

    MessageLength - Supplies the length of the command message to write.

Return Value:

    Status code.

--*/

{

    RTLW81_FIRMWARE_COMMAND Command;
    ULONGLONG CurrentTime;
    USHORT Register;
    KSTATUS Status;
    ULONGLONG Timeout;
    ULONGLONG TimeoutTicks;
    ULONG Value;

    //
    // Wait for the firmware box to be ready to receive the command.
    //

    CurrentTime = KeGetRecentTimeCounter();
    TimeoutTicks = HlQueryTimeCounterFrequency() * RTLW81_DEVICE_TIMEOUT;
    Timeout = CurrentTime + TimeoutTicks;
    do {
        Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterHmetfr0);
        if ((Value & (1 << Device->FirmwareBox)) == 0) {
            break;
        }

        CurrentTime = KeGetRecentTimeCounter();

    } while (CurrentTime > Timeout);

    if (CurrentTime > Timeout) {
        Status = STATUS_TIMEOUT;
        goto SendFirmwareCommandEnd;
    }

    //
    // Write the command to the current firmware box.
    //

    RtlZeroMemory(&Command, sizeof(RTLW81_FIRMWARE_COMMAND));
    Command.Id = CommandId;
    if (MessageLength > RTLW81_FIRMWARE_COMMAND_MAX_NO_EXTENSION_LENGTH) {
        Command.Id |= RTLW81_FIRMWARE_COMMAND_FLAG_EXTENSION;
    }

    ASSERT(MessageLength <= RTLW81_FIRMWARE_COMMAND_MAX_MESSAGE_LENGTH);

    RtlCopyMemory(Command.Message, Message, MessageLength);
    Register = Rtlw81RegisterHmeBoxExtension + (Device->FirmwareBox * 2);
    Status = Rtlw81pWriteData(Device, Register, &Command + 4, 2);
    if (!KSUCCESS(Status)) {
        goto SendFirmwareCommandEnd;
    }

    Register = Rtlw81RegisterHmeBox + (Device->FirmwareBox * 4);
    Status = Rtlw81pWriteData(Device, Register, &Command, 4);
    if (!KSUCCESS(Status)) {
        goto SendFirmwareCommandEnd;
    }

    //
    // Move to the next firmware box.
    //

    Device->FirmwareBox += 1;
    Device->FirmwareBox %= RTLW81_FIRMWARE_BOX_COUNT;

SendFirmwareCommandEnd:
    return Status;
}

KSTATUS
Rtlw81pSubmitBulkInTransfers (
    PRTLW81_DEVICE Device
    )

/*++

Routine Description:

    This routine submits all the bulk IN transfers allocated for the device.

Arguments:

    Device - Supplies a pointer to an SM95 device.

Return Value:

    Status code.

--*/

{

    ULONG Index;
    KSTATUS Status;

    for (Index = 0; Index < RTLW81_BULK_IN_TRANSFER_COUNT; Index += 1) {
        Status = UsbSubmitTransfer(Device->BulkInTransfer[Index]);
        if (!KSUCCESS(Status)) {
            break;
        }
    }

    return Status;
}

VOID
Rtlw81pCancelBulkInTransfers (
    PRTLW81_DEVICE Device
    )

/*++

Routine Description:

    This routine attempts to cancel all the bulk IN transfers for the device.

Arguments:

    Device - Supplies a pointer to an SM95 device.

Return Value:

    None.

--*/

{

    ULONG Index;

    for (Index = 0; Index < RTLW81_BULK_IN_TRANSFER_COUNT; Index += 1) {
        UsbCancelTransfer(Device->BulkInTransfer[Index], FALSE);
    }

    return;
}

PRTLW81_BULK_OUT_TRANSFER
Rtlw81pAllocateBulkOutTransfer (
    PRTLW81_DEVICE Device,
    RTLW81_BULK_OUT_TYPE Type
    )

/*++

Routine Description:

    This routine allocates an RTLW81 bulk OUT transfer. If there are no free
    bulk OUT transfers ready to go, it will create a new transfer.

Arguments:

    Device - Supplies a pointer to the RTLW81 device in need of a new transfer.

    Type - Supplies the type of bulk out transfer to allocate.

Return Value:

    Returns a pointer to the allocated RTLW81 bulk OUT transfer on success or
    NULL on failure.

--*/

{

    UCHAR Endpoint;
    UCHAR EndpointIndex;
    PLIST_ENTRY FreeList;
    PRTLW81_BULK_OUT_TRANSFER Rtlw81Transfer;
    PUSB_TRANSFER UsbTransfer;

    ASSERT(KeGetRunLevel() == RunLevelLow);

    EndpointIndex = Device->BulkOutTypeEndpointIndex[Type];
    Endpoint = Device->BulkOutEndpoint[EndpointIndex];
    FreeList = &(Device->BulkOutFreeTransferList[EndpointIndex]);

    //
    // Loop attempting to use the most recently released existing transfer, but
    // allocate a new transfer if none are available.
    //

    Rtlw81Transfer = NULL;
    while (Rtlw81Transfer == NULL) {
        if (LIST_EMPTY(FreeList) != FALSE) {
            Rtlw81Transfer = MmAllocatePagedPool(
                                              sizeof(RTLW81_BULK_OUT_TRANSFER),
                                              RTLW81_ALLOCATION_TAG);

            if (Rtlw81Transfer == NULL) {
                goto AllocateBulkOutTransferEnd;
            }

            UsbTransfer = UsbAllocateTransfer(Device->UsbCoreHandle,
                                              Endpoint,
                                              RTLW81_MAX_PACKET_SIZE);

            if (UsbTransfer == NULL) {
                MmFreePagedPool(Rtlw81Transfer);
                Rtlw81Transfer = NULL;
                goto AllocateBulkOutTransferEnd;
            }

            UsbTransfer->Direction = UsbTransferDirectionOut;
            UsbTransfer->CallbackRoutine = Rtlw81pBulkOutTransferCompletion;
            UsbTransfer->UserData = Rtlw81Transfer;
            Rtlw81Transfer->Device = Device;
            Rtlw81Transfer->UsbTransfer = UsbTransfer;
            Rtlw81Transfer->Packet = NULL;
            Rtlw81Transfer->EndpointIndex = EndpointIndex;

        } else {
            KeAcquireQueuedLock(Device->BulkOutListLock);
            if (LIST_EMPTY(FreeList) == FALSE) {
                Rtlw81Transfer = LIST_VALUE(FreeList->Next,
                                            RTLW81_BULK_OUT_TRANSFER,
                                            ListEntry);

                LIST_REMOVE(&(Rtlw81Transfer->ListEntry));
            }

            KeReleaseQueuedLock(Device->BulkOutListLock);
        }
    }

AllocateBulkOutTransferEnd:
    return Rtlw81Transfer;
}

VOID
Rtlw81pFreeBulkOutTransfer (
    PRTLW81_BULK_OUT_TRANSFER Transfer
    )

/*++

Routine Description:

    This routine releases an RTLW81 bulk OUT transfer for recycling.

Arguments:

    Transfer - Supplies a pointer to the RTLW81 transfer to be recycled.

Return Value:

    None.

--*/

{

    PRTLW81_DEVICE Device;
    PLIST_ENTRY FreeList;

    ASSERT(KeGetRunLevel() == RunLevelLow);

    //
    // Insert it onto the head of the list so it stays hot.
    //

    Device = Transfer->Device;
    FreeList = &(Device->BulkOutFreeTransferList[Transfer->EndpointIndex]);
    KeAcquireQueuedLock(Device->BulkOutListLock);
    INSERT_AFTER(&(Transfer->ListEntry), FreeList);
    KeReleaseQueuedLock(Device->BulkOutListLock);
    return;
}

VOID
Rtlw81pBulkOutTransferCompletion (
    PUSB_TRANSFER Transfer
    )

/*++

Routine Description:

    This routine is called when an asynchronous I/O request completes with
    success, failure, or is cancelled.

Arguments:

    Transfer - Supplies a pointer to the transfer that completed.

Return Value:

    None.

--*/

{

    PRTLW81_BULK_OUT_TRANSFER Rtlw81Transfer;

    Rtlw81Transfer = Transfer->UserData;
    RtlAtomicAdd32(&(Rtlw81Transfer->Device->BulkOutTransferCount), -1);
    NetFreeBuffer(Rtlw81Transfer->Packet);
    Rtlw81Transfer->Packet = NULL;
    Rtlw81pFreeBulkOutTransfer(Rtlw81Transfer);
    return;
}

VOID
Rtlw81pSetLed (
    PRTLW81_DEVICE Device,
    BOOL Enable
    )

/*++

Routine Description:

    This routine modified the RTL81xx wireless device's LED state.

Arguments:

    Device - Supplies a pointer to the RTLW81 device.

    Enable - Supplies a boolean indicating if the LED should be turned on
        (TRUE) or turned off (FALSE).

Return Value:

    None.

--*/

{

    UCHAR Value;

    Value = RTLW81_READ_REGISTER8(Device, Rtlw81RegisterLedConfig0);
    Value &= RTLW81_LED_SAVE_MASK;
    if (Enable == FALSE) {
        Value |= RTLW81_LED_DISABLE;
    }

    RTLW81_WRITE_REGISTER8(Device, Rtlw81RegisterLedConfig0, Value);
    return;
}

