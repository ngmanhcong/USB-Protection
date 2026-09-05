#pragma once

#include <Windows.h>

BOOL UsbProtectionConnect(HANDLE* portHandle);
void UsbProtectionDisconnect(HANDLE portHandle);
BOOL UsbProtectionSendEnable(HANDLE portHandle);
BOOL UsbProtectionSendDisable(HANDLE portHandle);
BOOL UsbProtectionSendQueryStatus(HANDLE portHandle, BOOL* enabled);
