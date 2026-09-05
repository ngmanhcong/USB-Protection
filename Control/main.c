#include <Windows.h>
#include <fltuser.h>
#include <stdio.h>
#include <wchar.h>

#include "SharedProtocol.h"

static void PrintUsage()
{
    wprintf(L"Usage: UsbProtectionCtl.exe enable|disable|status\n");
}

static BOOL Connect(HANDLE* portHandle)
{
    HRESULT hr = FilterConnectCommunicationPort(USB_PROTECTION_PORT_NAME,
                                                0,
                                                NULL,
                                                0,
                                                NULL,
                                                portHandle);
    if (FAILED(hr)) {
        fwprintf(stderr, L"FilterConnectCommunicationPort failed, hr=0x%08X\n", hr);
        return FALSE;
    }

    return TRUE;
}

static BOOL SendCommand(HANDLE portHandle, USB_PROTECTION_COMMAND command)
{
    USB_PROTECTION_MESSAGE message;
    DWORD bytesReturned = 0;
    HRESULT hr;

    ZeroMemory(&message, sizeof(message));
    message.Command = (USBP_UINT32)command;

    hr = FilterSendMessage(portHandle,
                           &message,
                           sizeof(message),
                           NULL,
                           0,
                           &bytesReturned);
    if (FAILED(hr)) {
        fwprintf(stderr, L"FilterSendMessage failed, hr=0x%08X\n", hr);
        return FALSE;
    }

    return TRUE;
}

static BOOL QueryStatus(HANDLE portHandle)
{
    USB_PROTECTION_MESSAGE message;
    USB_PROTECTION_REPLY reply;
    DWORD bytesReturned = 0;
    HRESULT hr;

    ZeroMemory(&message, sizeof(message));
    ZeroMemory(&reply, sizeof(reply));

    message.Command = (USBP_UINT32)UsbProtectionQueryStatus;

    hr = FilterSendMessage(portHandle,
                           &message,
                           sizeof(message),
                           &reply,
                           sizeof(reply),
                           &bytesReturned);
    if (FAILED(hr)) {
        fwprintf(stderr, L"FilterSendMessage(status) failed, hr=0x%08X\n", hr);
        return FALSE;
    }

    if (bytesReturned < sizeof(reply)) {
        fwprintf(stderr, L"Invalid status reply from driver\n");
        return FALSE;
    }

    wprintf(L"Protection: %s\n", reply.ProtectionEnabled ? L"ON" : L"OFF");
    return TRUE;
}

int wmain(int argc, wchar_t** argv)
{
    HANDLE portHandle = INVALID_HANDLE_VALUE;
    BOOL ok = FALSE;

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
        ok = FALSE;
    }

    CloseHandle(portHandle);
    return ok ? 0 : 1;
}
