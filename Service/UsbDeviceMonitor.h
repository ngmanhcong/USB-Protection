#pragma once

#include <Windows.h>

BOOL UsbDeviceMonitorStart(void);
void UsbDeviceMonitorStop(void);
void UsbDeviceMonitorSetDriverPort(HANDLE driverPort);
