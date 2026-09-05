#include <Windows.h>
#include <stdio.h>

#include "Service.h"

int wmain()
{
    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { (LPWSTR)USB_PROTECTION_SERVICE_NAME, UsbProtectionServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        fwprintf(stderr,
                 L"[UsbProtectionService] StartServiceCtrlDispatcherW failed, error=%lu\n",
                 GetLastError());
        return 1;
    }

    return 0;
}
