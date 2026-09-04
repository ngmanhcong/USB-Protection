#include <Windows.h>
#include <fltuser.h>
#include <stdio.h>
#include <wchar.h>

#include "SharedProtocol.h"

static void PrintUsage()
{
    wprintf(L"Usage: UsbProtectionCtl.exe enable|disable|status\n");
}

static bool Connect(HANDLE* portHandle)
{
    HRESULT hr = FilterConnectCommunicationPort(USB_PROTECTION_PORT_NAME,
                                                0,
                                                nullptr,
                                                0,
                                                nullptr,
                                                portHandle);
    if (FAILED(hr)) {
        fwprintf(stderr, L"FilterConnectCommunicationPort failed, hr=0x%08X\n", hr);
        return false;
    }

    return true;
}

static bool SendCommand(HANDLE portHandle, USB_PROTECTION_COMMAND command)
{
    USB_PROTECTION_MESSAGE message = {};
    DWORD bytesReturned = 0;
    HRESULT hr;

    message.Command = static_cast<USBP_UINT32>(command);

    hr = FilterSendMessage(portHandle,
                           &message,
                           sizeof(message),
                           nullptr,
                           0,
                           &bytesReturned);
    if (FAILED(hr)) {
        fwprintf(stderr, L"FilterSendMessage failed, hr=0x%08X\n", hr);
        return false;
    }

    return true;
}

static bool QueryStatus(HANDLE portHandle)
{
    USB_PROTECTION_MESSAGE message = {};
    USB_PROTECTION_REPLY reply = {};
    DWORD bytesReturned = 0;
    HRESULT hr;

    message.Command = static_cast<USBP_UINT32>(UsbProtectionQueryStatus);

    hr = FilterSendMessage(portHandle,
                           &message,
                           sizeof(message),
                           &reply,
                           sizeof(reply),
                           &bytesReturned);
    if (FAILED(hr)) {
        fwprintf(stderr, L"FilterSendMessage(status) failed, hr=0x%08X\n", hr);
        return false;
    }

    if (bytesReturned < sizeof(reply)) {
        fwprintf(stderr, L"Invalid status reply from driver\n");
        return false;
    }

    wprintf(L"Protection: %s\n", reply.ProtectionEnabled ? L"ON" : L"OFF");
    return true;
}

int wmain(int argc, wchar_t** argv)
{
    HANDLE portHandle = INVALID_HANDLE_VALUE;
    bool ok = false;

    if (argc != 2) {
        PrintUsage();
        return 2;
    }

    if (!Connect(&portHandle)) {
        return 1;
    }

    if (_wcsicmp(argv[1], L"enable") == 0) {
        ok = SendCommand(portHandle, UsbProtectionEnable);
        if (ok) {
            wprintf(L"Protection enabled\n");
        }
    } else if (_wcsicmp(argv[1], L"disable") == 0) {
        ok = SendCommand(portHandle, UsbProtectionDisable);
        if (ok) {
            wprintf(L"Protection disabled\n");
        }
    } else if (_wcsicmp(argv[1], L"status") == 0) {
        ok = QueryStatus(portHandle);
    } else {
        PrintUsage();
        ok = false;
    }

    CloseHandle(portHandle);
    return ok ? 0 : 1;
}
