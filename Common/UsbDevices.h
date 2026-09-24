#pragma once

#include <Windows.h>

#define USBP_MAX_CONNECTED_DEVICES 32

typedef struct _USBP_DEVICE_INFO {
    WCHAR Drives[32];
    WCHAR Name[128];
    WCHAR Serial[128];
    ULONGLONG DeviceHash;
    BOOL RemovableMedia;
} USBP_DEVICE_INFO, *PUSBP_DEVICE_INFO;

DWORD UsbDevicesEnumerate(PUSBP_DEVICE_INFO devices, DWORD capacity);
