#include "DriverCommunication.h"

#include <fltuser.h>
#include <stdio.h>

#include "SharedProtocol.h"

static BOOL SendCommand(HANDLE portHandle, USB_PROTECTION_COMMAND command, USB_PROTECTION_REPLY* reply)
{
    USB_PROTECTION_MESSAGE message;
    DWORD bytesReturned = 0;
    HRESULT hr;

    ZeroMemory(&message, sizeof(message));

    if (portHandle == NULL || portHandle == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    message.Command = (USBP_UINT32)command;

    hr = FilterSendMessage(portHandle,
                           &message,
                           sizeof(message),
                           reply,
                           reply != NULL ? sizeof(*reply) : 0,
                           &bytesReturned);
    if (FAILED(hr)) {
        SetLastError(HRESULT_CODE(hr));
        return FALSE;
    }

    if (reply != NULL && bytesReturned < sizeof(*reply)) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    return TRUE;
}

static BOOL SendPolicyCommand(HANDLE portHandle,
                              USB_PROTECTION_COMMAND command,
                              DWORD value,
                              ULONGLONG deviceHash,
                              USB_PROTECTION_POLICY_REPLY* reply)
{
    USB_PROTECTION_POLICY_MESSAGE message;
    DWORD bytesReturned = 0;
    HRESULT hr;

    ZeroMemory(&message, sizeof(message));

    if (portHandle == NULL || portHandle == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    message.Command = (USBP_UINT32)command;
    message.Value = value;
    message.DeviceHash = deviceHash;

    hr = FilterSendMessage(portHandle,
                           &message,
                           sizeof(message),
                           reply,
                           reply != NULL ? sizeof(*reply) : 0,
                           &bytesReturned);
    if (FAILED(hr)) {
        SetLastError(HRESULT_CODE(hr));
        return FALSE;
    }

    if (reply != NULL && bytesReturned < sizeof(*reply)) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    return TRUE;
}

BOOL UsbProtectionConnect(HANDLE* portHandle)
{
    HRESULT hr;

    if (portHandle == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    *portHandle = INVALID_HANDLE_VALUE;

    hr = FilterConnectCommunicationPort(USB_PROTECTION_PORT_NAME,
                                        0,
                                        NULL,
                                        0,
                                        NULL,
                                        portHandle);
    if (FAILED(hr)) {
        SetLastError(HRESULT_CODE(hr));
        return FALSE;
    }

    return TRUE;
}

void UsbProtectionDisconnect(HANDLE portHandle)
{
    if (portHandle != NULL && portHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(portHandle);
    }
}

BOOL UsbProtectionSendEnable(HANDLE portHandle)
{
    return SendCommand(portHandle, UsbProtectionEnable, NULL);
}

BOOL UsbProtectionSendDisable(HANDLE portHandle)
{
    return SendCommand(portHandle, UsbProtectionDisable, NULL);
}

BOOL UsbProtectionSendQueryStatus(HANDLE portHandle, BOOL* enabled)
{
    USB_PROTECTION_REPLY reply;

    ZeroMemory(&reply, sizeof(reply));

    if (enabled == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!SendCommand(portHandle, UsbProtectionQueryStatus, &reply)) {
        return FALSE;
    }

    *enabled = (reply.ProtectionEnabled != 0);
    return TRUE;
}

BOOL UsbProtectionSendSetExecutableBlocking(HANDLE portHandle, BOOL enabled)
{
    return SendPolicyCommand(portHandle,
                             UsbProtectionSetExecutableBlocking,
                             enabled ? 1 : 0,
                             0,
                             NULL);
}

BOOL UsbProtectionSendSetApprovedOnly(HANDLE portHandle, BOOL enabled)
{
    return SendPolicyCommand(portHandle,
                             UsbProtectionSetApprovedOnly,
                             enabled ? 1 : 0,
                             0,
                             NULL);
}

BOOL UsbProtectionSendClearApprovedDevices(HANDLE portHandle)
{
    return SendPolicyCommand(portHandle,
                             UsbProtectionClearApprovedDevices,
                             0,
                             0,
                             NULL);
}

BOOL UsbProtectionSendAddApprovedDevice(HANDLE portHandle, ULONGLONG deviceHash)
{
    return SendPolicyCommand(portHandle,
                             UsbProtectionAddApprovedDevice,
                             0,
                             deviceHash,
                             NULL);
}

BOOL UsbProtectionSendRemoveApprovedDevice(HANDLE portHandle, ULONGLONG deviceHash)
{
    return SendPolicyCommand(portHandle,
                             UsbProtectionRemoveApprovedDevice,
                             0,
                             deviceHash,
                             NULL);
}

BOOL UsbProtectionSendQueryPolicy(HANDLE portHandle, USB_PROTECTION_POLICY_REPLY* policy)
{
    if (policy == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    ZeroMemory(policy, sizeof(*policy));
    return SendPolicyCommand(portHandle,
                             UsbProtectionQueryPolicy,
                             0,
                             0,
                             policy);
}
