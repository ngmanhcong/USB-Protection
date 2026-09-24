#pragma once

#include <Windows.h>

#include "SharedProtocol.h"

BOOL UsbProtectionConnect(HANDLE* portHandle);
void UsbProtectionDisconnect(HANDLE portHandle);
BOOL UsbProtectionSendEnable(HANDLE portHandle);
BOOL UsbProtectionSendDisable(HANDLE portHandle);
BOOL UsbProtectionSendQueryStatus(HANDLE portHandle, BOOL* enabled);
BOOL UsbProtectionSendSetExecutableBlocking(HANDLE portHandle, BOOL enabled);
BOOL UsbProtectionSendSetApprovedOnly(HANDLE portHandle, BOOL enabled);
BOOL UsbProtectionSendClearApprovedDevices(HANDLE portHandle);
BOOL UsbProtectionSendAddApprovedDevice(HANDLE portHandle, ULONGLONG deviceHash);
BOOL UsbProtectionSendRemoveApprovedDevice(HANDLE portHandle, ULONGLONG deviceHash);
BOOL UsbProtectionSendQueryPolicy(HANDLE portHandle, USB_PROTECTION_POLICY_REPLY* policy);
