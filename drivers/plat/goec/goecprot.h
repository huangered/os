/*++

Copyright (c) 2015 Minoca Corp. All Rights Reserved

Module Name:

    goecprot.h

Abstract:

    This header contains definitions for the Google Embedded Controller
    communication protocol.

Author:

    Evan Green 25-Aug-2015

--*/

//
// ------------------------------------------------------------------- Includes
//

//
// ---------------------------------------------------------------- Definitions
//

//
// Define the current supported version of EC commands.
//

#define GOEC_COMMAND_HEADER_VERSION 3
#define GOEC_RESPONSE_HEADER_VERSION GOEC_COMMAND_HEADER_VERSION

#define GOEC_MESSAGE_HEADER_BYTES 3
#define GOEC_MESSAGE_TRAILER_BYTES 2
#define GOEC_MESSAGE_PROTOCOL_BYTES \
    (GOEC_MESSAGE_HEADER_BYTES + GOEC_MESSAGE_TRAILER_BYTES)

#define GOEC_PROTO2_MAX_PARAM_SIZE 0xFC
#define GOEC_PROTO3_MAX_PACKET_SIZE 268
#define GOEC_MAX_DATA (GOEC_PROTO2_MAX_PARAM_SIZE + GOEC_MESSAGE_PROTOCOL_BYTES)

//
// ------------------------------------------------------ Data Type Definitions
//

typedef enum _GOEC_COMMAND_CODE {
    GoecCommandHello = 0x01,
    GoecCommandGetVersion = 0x02,
    GoecCommandReadTest = 0x03,
    GoecCommandGetBuildInfo = 0x04,
    GoecCommandGetChipInfo = 0x05,
    GoecCommandGetBoardVersion = 0x06,
    GoecCommandReadMemoryMap = 0x07,
    GoecCommandGetCommandVersions = 0x08,
    GoecCommandGetCommsStatus = 0x09,
    GoecCommandTestProtocol = 0x0A,
    GoecCommandGetProtocolInfo = 0x0B,
    GoecCommandFlashInfo = 0x10,
    GoecCommandFlashRead = 0x11,
    GoecCommandFlashWrite = 0x12,
    GoecCommandFlashErase = 0x13,
    GoecCommandFlashProtect = 0x15,
    GoecCommandFlashRegionInfo = 0x16,
    GoecCommandVbNvContext = 0x17,
    GoecCommandPwmGetFanTargetRpm = 0x20,
    GoecCommandPwmSetFanTargetRpm = 0x21,
    GoecCommandPwmGetKeyboardBacklight = 0x22,
    GoecCommandPwmSetKeyboardBacklight = 0x23,
    GoecCommandPwmSetFanDuty = 0x24,
    GoecCommandLightBar = 0x28,
    GoecCommandLedControl = 0x29,
    GoecCommandVBootHash = 0x2A,
    GoecCommandMotionSense = 0x2B,
    GoecCommandUsbChargeSetMode = 0x30,
    GoecCommandPstoreRead = 0x41,
    GoecCommandPstoreWrite = 0x42,
    GoecCommandRtcGetValue = 0x44,
    GoecCommandRtcGetAlarm = 0x45,
    GoecCommandRtcSetValue = 0x46,
    GoecCommandRtcSetAlarm = 0x47,
    GoecCommandPort80Read = 0x48,
    GoecCommandThermalSetThreshold = 0x50,
    GoecCommandThermalGetThreshold = 0x51,
    GoecCommandThermalAutoFanControl = 0x52,
    GoecCommandTmp006GetCalibration = 0x53,
    GoecCommandTmp006SetCalibration = 0x54,
    GoecCommandTmp006GetRaw = 0x55,
    GoecCommandKeyboardState = 0x60,
    GoecCommandKeyboardInfo = 0x61,
    GoecCommandKeyboardSimulateKey = 0x62,
    GoecCommandKeyboardSetConfig = 0x64,
    GoecCommandKeyboardGetConfig = 0x65,
    GoecCommandKeyscanSequenceControl = 0x66,
    GoecCommandTempSensorGetInfo = 0x70,
    GoecCommandAcpiRead = 0x80,
    GoecCommandAcpiWrite = 0x81,
    GoecCommandAcpiQueryEvent = 0x84,
    GoecCommandHostGetEventB = 0x87,
    GoecCommandHostGetSmiMask = 0x88,
    GoecCommandHostGetSciMask = 0x89,
    GoecCommandHostGetWakeMask = 0x8D,
    GoecCommandHostSetSmiMask = 0x8A,
    GoecCommandHostSetSciMask = 0x8B,
    GoecCommandHostEventClear = 0x8C,
    GoecCommandHostSetWakeMask = 0x8E,
    GoecCommandHostClearB = 0x8F,
    GoecCommandSwitchEnableBacklight = 0x90,
    GoecCommandSwitchEnableWireless = 0x91,
    GoecCommandGpioSet = 0x92,
    GoecCommandGpioGet = 0x93,
    GoecCommandI2cRead = 0x94,
    GoecCommandI2cWrite = 0x95,
    GoecCommandChargeControl = 0x96,
    GoecCommandConsoleSnapshot = 0x97,
    GoecCommandConsoleRead = 0x98,
    GoecCommandBatteryCutoff = 0x99,
    GoecCommandUsbMux = 0x9A,
    GoecCommandLdoSet = 0x9B,
    GoecCommandLdoGet = 0x9C,
    GoecCommandPowerInfo = 0x9D,
    GoecCommandI2cPassthrough = 0x9E,
    GoecCommandHangDetect = 0x9F,
    GoecCommandChargeState = 0xA0,
    GoecCommandChargeCurrentLimit = 0xA1,
    GoecCommandExtPowerCurrentLimit = 0xA2,
    GoecCommandBatteryReadWord = 0xB0,
    GoecCommandBatteryWriteWord = 0xB1,
    GoecCommandBatteryReadBlock = 0xB2,
    GoecCommandBatteryWriteBlock = 0xB3,
    GoecCommandBatteryVendorParameter = 0xB4,
    GoecCommandFirmwareUpdate = 0xB5,
    GoecCommandEnteringMode = 0xB6,
    GoecCommandRebootEc = 0xD2,
    GoecCommandGetPanicInfo = 0xD3,
    GoecCommandReboot = 0xD1,
    GoecCommandResendResponse = 0xD2,
    GoecCommandVersion0 = 0xDC,
    GoecCommandPdExchangeStatus = 0x100,
    GoecCommandUsePdControl = 0x101,
    GoecCommandUsbPdFirmwareUpdate = 0x110,
    GoecCommandUsbPdRwHashEntry = 0x111,
    GoecCommandUsbPdDevInfo = 0x112
} GOEC_COMMAND_CODE, *PGOEC_COMMAND_CODE;

typedef enum _GOEC_SPI_STATUS {
    GoecSpiFrameStart = 0xEC,
    GoecSpiPastEnd = 0xED,
    GoecSpiRxReady = 0xF8,
    GoecSpiReceiving = 0xF9,
    GoecSpiProcessing = 0xFA,
    GoecSpiRxBadData = 0xFB,
    GoecSpiNotReady = 0xFC,
    GoecSpiOldReady = 0xFD
} GOEC_SPI_STATUS, *PGOEC_SPI_STATUS;

/*++

Structure Description:

    This structure defines the software structure of a Google Embedded
    Controller command.

Members:

    Code - Stores the command code on input, and status on output.

    Version - Stores the command version.

    DataIn - Stores an optional pointer to the command data.

    DataOut - Stores an optional pointer to the response data.

    SizeIn - Stores the size of the command data.

    SizeOut - Stores the expected size of the command response on input.
        Returns the actual size of data received on output.

    DeviceIndex - Stores the device index for I2C passthrough.

--*/

typedef struct _GOEC_COMMAND {
    USHORT Code;
    UCHAR Version;
    PVOID DataIn;
    PVOID DataOut;
    USHORT SizeIn;
    USHORT SizeOut;
    INT DeviceIndex;
} GOEC_COMMAND, *PGOEC_COMMAND;

/*++

Structure Description:

    This structure defines the hardware structure of a Google Embedded
    Controller command header.

Members:

    Version - Stores the version of this structure. Set to
        GOEC_COMMAND_HEADER_VERSION.

    Checksum - Stores the checksum of the request and data. The checksum is
        defined such that the sum of all the bytes including the checksum
        should total zero.

    Command - Stores the command code.

    CommandVersion - Stores the version number of the command.

    Reserved - Stores a reserved byte that should always be zero.

    DataLength - Stores the length of the data following this header.

--*/

typedef struct _GOEC_COMMAND_HEADER {
    UCHAR Version;
    UCHAR Checksum;
    USHORT Command;
    UCHAR CommandVersion;
    UCHAR Reserved;
    USHORT DataLength;
} PACKED GOEC_COMMAND_HEADER, *PGOEC_COMMAND_HEADER;

/*++

Structure Description:

    This structure defines the hardware structure of a Google Embedded
    Controller response header.

Members:

    Version - Stores the version of this structure. Set to
        GOEC_RESPONSE_HEADER_VERSION.

    Checksum - Stores the checksum of the request and data. The checksum is
        defined such that the sum of all the bytes including the checksum
        should total zero.

    Result - Stores the result code of the command.

    DataLength - Stores the length of the data following this header.

    Reserved - Stores a reserved value that should always be zero.

--*/

typedef struct _GOEC_RESPONSE_HEADER {
    UCHAR Version;
    UCHAR Checksum;
    USHORT Result;
    USHORT DataLength;
    USHORT Reserved;
} PACKED GOEC_RESPONSE_HEADER, *PGOEC_RESPONSE_HEADER;

/*++

Structure Description:

    This structure defines the hardware structure of a Google Embedded
    Controller command, version 3.

Members:

    Header - Stores the common packet header.

    Data - Stores the command-specific data.

--*/

typedef struct _GOEC_COMMAND_V3 {
    GOEC_COMMAND_HEADER Header;
    UCHAR Data[GOEC_MAX_DATA];
} GOEC_COMMAND_V3, *PGOEC_COMMAND_V3;

/*++

Structure Description:

    This structure defines the hardware structure of a Google Embedded
    Controller response, version 3.

Members:

    Header - Stores the common response header.

    Data - Stores the command-specific data.

--*/

typedef struct _GOEC_RESPONSE_V3 {
    GOEC_RESPONSE_HEADER Header;
    UCHAR Data[GOEC_MAX_DATA];
} GOEC_RESPONSE_V3, *PGOEC_RESPONSE_V3;

/*++

Structure Description:

    This structure defines the the parameters for the Hello command.

Members:

    InData - Stores any value.

--*/

typedef struct _GOEC_PARAMS_HELLO {
    ULONG InData;
} PACKED GOEC_PARAMS_HELLO, *PGOEC_PARAMS_HELLO;

/*++

Structure Description:

    This structure defines the the response for the Hello command.

Members:

    OutData - Stores the parameter InData plus 0x01020304.

--*/

typedef struct _GOEC_RESPONSE_HELLO {
    ULONG OutData;
} PACKED GOEC_RESPONSE_HELLO, *PGOEC_RESPONSE_HELLO;

typedef enum _GOEC_CURRENT_IMAGE {
    GoecImageUnknown = 0,
    GoecImageReadOnly = 1,
    GoecImageReadWrite = 2,
} GOEC_CURRENT_IMAGE, *PGOEC_CURRENT_IMAGE;

/*++

Structure Description:

    This structure defines the the response for the Get Version command.

Members:

    VersionStringRo - Stores the version string of the read-only firmware.

    VersionStringRw - Stores the version string of the read-write firmware.

    Reserved - Stores an unused string (that used to be the RW-B version).

    CurrentImage - Stores the current running image. See GOEC_CURRENT_IMAGE.

--*/

typedef struct _GOEC_RESPONSE_GET_VERSION {
    CHAR VersionStringRo[32];
    CHAR VersionStringRw[32];
    CHAR Reserved[32];
    ULONG CurrentImage;
} PACKED GOEC_RESPONSE_GET_VERSION, *PGOEC_RESPONSE_GET_VERSION;

/*++

Structure Description:

    This structure defines the the response for the Keyboard Information
    command.

Members:

    Rows - Stores the number of rows in the matrix keyboard.

    Columns - Stores the number of columns in the matrix keyboard.

    Switches - Stores the number of switches in the matrix keyboard.

--*/

typedef struct _GOEC_RESPONSE_KEYBOARD_INFO {
    ULONG Rows;
    ULONG Columns;
    UCHAR Switches;
} PACKED GOEC_RESPONSE_KEYBOARD_INFO, *PGOEC_RESPONSE_KEYBOARD_INFO;

//
// -------------------------------------------------------------------- Globals
//

//
// -------------------------------------------------------- Function Prototypes
//
