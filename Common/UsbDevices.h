#pragma once

#include <Windows.h>

#define USBP_MAX_CONNECTED_DEVICES 32

typedef struct _USBP_DEVICE_INFO {
    WCHAR Drives[32];
    WCHAR Name[128];
    WCHAR Serial[128];
    WCHAR InstanceId[512];
    ULONGLONG DeviceHash;
    BOOL RemovableMedia;
} USBP_DEVICE_INFO, *PUSBP_DEVICE_INFO;

ULONGLONG UsbDevicesHashStorageDescriptor(const BYTE* descriptorBuffer,
                                          DWORD descriptorLength);
ULONGLONG UsbDevicesHashIdentityStrings(const WCHAR* vendor,
                                        const WCHAR* product,
                                        const WCHAR* revision,
                                        const WCHAR* serial);
ULONGLONG UsbDevicesHashInstanceId(const WCHAR* instanceId);
DWORD UsbDevicesEnumerate(PUSBP_DEVICE_INFO devices, DWORD capacity);
BOOL UsbDevicesRestart(const WCHAR* instanceId);
