#pragma once

#include <Windows.h>

#define USB_PROTECTION_SERVICE_NAME L"UsbProtectionService"

void WINAPI UsbProtectionServiceMain(DWORD argc, LPWSTR* argv);
DWORD WINAPI UsbProtectionServiceHandler(DWORD control, DWORD eventType, LPVOID eventData, LPVOID context);
