#pragma once

#include <fltKernel.h>
#include <dontuse.h>
#include <suppress.h>

#include "SharedProtocol.h"

#define USBP_TAG 'PbsU'

#define USBP_LOG(...) UsbProtectLog(__VA_ARGS__)

typedef struct _USBPROTECT_INSTANCE_CONTEXT {
    BOOLEAN IsUsb;
    BOOLEAN IsRemovable;
} USBPROTECT_INSTANCE_CONTEXT, *PUSBPROTECT_INSTANCE_CONTEXT;

extern PFLT_FILTER gUsbProtectFilter;
extern PFLT_PORT gUsbProtectServerPort;
extern volatile LONG gUsbProtectEnabled;

BOOLEAN
UsbProtectIsProtectionEnabled(
    VOID
    );

VOID
UsbProtectLog(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...
    );

VOID
UsbProtectSetProtectionEnabled(
    _In_ BOOLEAN Enabled
    );

DRIVER_INITIALIZE DriverEntry;

NTSTATUS
UsbProtectUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
    );

NTSTATUS
UsbProtectInstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
    _In_ DEVICE_TYPE VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType
    );

VOID
UsbProtectInstanceContextCleanup(
    _In_ PFLT_CONTEXT Context,
    _In_ FLT_CONTEXT_TYPE ContextType
    );
