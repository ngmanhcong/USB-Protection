#pragma once

/*
 * Shared wire protocol for the Filter Manager communication port.
 * Keep this header plain C-compatible and fixed-width from the protocol
 * perspective so it can be included by both the kernel driver and user-mode
 * service/control utility.
 */

#if defined(_KERNEL_MODE) || defined(_NTDDK_) || defined(_FLT_KERNEL_)
#include <fltKernel.h>
typedef ULONG USBP_UINT32;
#else
#include <stdint.h>
typedef uint32_t USBP_UINT32;
#endif

#define USB_PROTECTION_PORT_NAME L"\\UsbProtectionPort"

typedef enum _USB_PROTECTION_COMMAND {
    UsbProtectionCommandInvalid = 0,
    UsbProtectionEnable = 1,
    UsbProtectionDisable = 2,
    UsbProtectionQueryStatus = 3,
    UsbProtectionSetExecutableBlocking = 4,
    UsbProtectionSetApprovedOnly = 5,
    UsbProtectionClearApprovedDevices = 6,
    UsbProtectionAddApprovedDevice = 7,
    UsbProtectionRemoveApprovedDevice = 8,
    UsbProtectionQueryPolicy = 9,
    UsbProtectionQueryApprovedDevice = 10
} USB_PROTECTION_COMMAND;

typedef struct _USB_PROTECTION_MESSAGE {
    USBP_UINT32 Command;
} USB_PROTECTION_MESSAGE, *PUSB_PROTECTION_MESSAGE;

typedef struct _USB_PROTECTION_REPLY {
    USBP_UINT32 ProtectionEnabled;
} USB_PROTECTION_REPLY, *PUSB_PROTECTION_REPLY;

/*
 * Extended messages are used by the service and the desktop management UI.
 * The original four-byte message/reply above remains unchanged so older
 * UsbProtectionCtl builds continue to work with enable/disable/status.
 */
#define USB_PROTECTION_MAX_APPROVED_DEVICES 64

typedef struct _USB_PROTECTION_POLICY_MESSAGE {
    USBP_UINT32 Command;
    USBP_UINT32 Value;
    unsigned long long DeviceHash;
} USB_PROTECTION_POLICY_MESSAGE, *PUSB_PROTECTION_POLICY_MESSAGE;

typedef struct _USB_PROTECTION_POLICY_REPLY {
    USBP_UINT32 DataLeakProtectionEnabled;
    USBP_UINT32 ExecutableBlockingEnabled;
    USBP_UINT32 ApprovedOnlyEnabled;
    USBP_UINT32 ApprovedDeviceCount;
    unsigned long long DeviceHash;
} USB_PROTECTION_POLICY_REPLY, *PUSB_PROTECTION_POLICY_REPLY;
