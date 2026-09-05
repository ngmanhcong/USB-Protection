#include "DriverCommunication.h"

#include <fltuser.h>
#include <stdio.h>

#include "SharedProtocol.h"

static bool SendCommand(HANDLE portHandle, USB_PROTECTION_COMMAND command, USB_PROTECTION_REPLY* reply)
{
    USB_PROTECTION_MESSAGE message = {};
    DWORD bytesReturned = 0;
    HRESULT hr;

    if (portHandle == nullptr || portHandle == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }

    message.Command = static_cast<USBP_UINT32>(command);

    hr = FilterSendMessage(portHandle,
                           &message,
                           sizeof(message),
                           reply,
                           reply != nullptr ? sizeof(*reply) : 0,
                           &bytesReturned);
    if (FAILED(hr)) {
        SetLastError(HRESULT_CODE(hr));
        return false;
    }

    if (reply != nullptr && bytesReturned < sizeof(*reply)) {
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }

    return true;
}

bool UsbProtectionConnect(HANDLE* portHandle)
{
    HRESULT hr;

    if (portHandle == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    *portHandle = INVALID_HANDLE_VALUE;

    hr = FilterConnectCommunicationPort(USB_PROTECTION_PORT_NAME,
                                        0,
                                        nullptr,
                                        0,
                                        nullptr,
                                        portHandle);
    if (FAILED(hr)) {
        SetLastError(HRESULT_CODE(hr));
        return false;
    }

    return true;
}

void UsbProtectionDisconnect(HANDLE portHandle)
{
    if (portHandle != nullptr && portHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(portHandle);
    }
}

bool UsbProtectionSendEnable(HANDLE portHandle)
{
    return SendCommand(portHandle, UsbProtectionEnable, nullptr);
}

bool UsbProtectionSendDisable(HANDLE portHandle)
{
    return SendCommand(portHandle, UsbProtectionDisable, nullptr);
}

bool UsbProtectionSendQueryStatus(HANDLE portHandle, bool* enabled)
{
    USB_PROTECTION_REPLY reply = {};

    if (enabled == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    if (!SendCommand(portHandle, UsbProtectionQueryStatus, &reply)) {
        return false;
    }

    *enabled = (reply.ProtectionEnabled != 0);
    return true;
}
