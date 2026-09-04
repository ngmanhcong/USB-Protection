#include <Windows.h>
#include <stdio.h>

#include "Service.h"

int wmain()
{
    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { const_cast<LPWSTR>(USB_PROTECTION_SERVICE_NAME), UsbProtectionServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        fwprintf(stderr,
                 L"[UsbProtectionService] StartServiceCtrlDispatcherW failed, error=%lu\n",
                 GetLastError());
        return 1;
    }

    return 0;
}
