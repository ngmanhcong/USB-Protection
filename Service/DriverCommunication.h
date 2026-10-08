#pragma once

#include <Windows.h>

#include "SharedProtocol.h"

BOOL UsbProtectionConnect(HANDLE* portHandle);
void UsbProtectionDisconnect(HANDLE portHandle);
BOOL UsbProtectionSendEnable(HANDLE portHandle);
BOOL UsbProtectionSendDisable(HANDLE portHandle);
BOOL UsbProtectionSendQueryStatus(HANDLE portHandle, BOOL* enabled);
BOOL UsbProtectionSendSetExecutableBlocking(HANDLE portHandle, BOOL enabled);
BOOL UsbProtectionSendQueryPolicy(HANDLE portHandle, USB_PROTECTION_POLICY_REPLY* policy);
BOOL UsbProtectionSendHubFilterPolicy(HANDLE portHandle,
                                      BOOL approvedOnlyEnabled,
                                      DWORD approvedDeviceCount,
                                      const ULONGLONG* approvedDevices);
