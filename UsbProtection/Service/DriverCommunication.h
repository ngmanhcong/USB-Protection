#pragma once

#include <Windows.h>

bool UsbProtectionConnect(HANDLE* portHandle);
void UsbProtectionDisconnect(HANDLE portHandle);
bool UsbProtectionSendEnable(HANDLE portHandle);
bool UsbProtectionSendDisable(HANDLE portHandle);
bool UsbProtectionSendQueryStatus(HANDLE portHandle, bool* enabled);
