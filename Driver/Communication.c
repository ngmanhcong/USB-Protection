#include "Communication.h"

static NTSTATUS UsbProtectForwardHubPolicy(
    _In_reads_bytes_(InputBufferLength)
        const USB_PROTECTION_HUB_POLICY_MESSAGE* Message,
    _In_ ULONG InputBufferLength);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, UsbProtectCreateCommunicationPort)
#pragma alloc_text(PAGE, UsbProtectCloseCommunicationPort)
#pragma alloc_text(PAGE, UsbProtectConnectNotify)
#pragma alloc_text(PAGE, UsbProtectDisconnectNotify)
#pragma alloc_text(PAGE, UsbProtectMessageNotify)
#pragma alloc_text(PAGE, UsbProtectForwardHubPolicy)
#endif

static NTSTATUS
UsbProtectForwardHubPolicy(
    _In_reads_bytes_(InputBufferLength)
        const USB_PROTECTION_HUB_POLICY_MESSAGE* Message,
    _In_ ULONG InputBufferLength
    )
{
    UNICODE_STRING deviceName;
    PFILE_OBJECT fileObject = NULL;
    PDEVICE_OBJECT deviceObject = NULL;
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    NTSTATUS status;

    PAGED_CODE();

    RtlInitUnicodeString(&deviceName, USB_PROTECTION_HUBFILTER_NT_NAME);
    status = IoGetDeviceObjectPointer(&deviceName,
                                      FILE_WRITE_DATA,
                                      &fileObject,
                                      &deviceObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&ioStatus, sizeof(ioStatus));
    irp = IoBuildDeviceIoControlRequest(IOCTL_USB_PROTECTION_SET_HUB_POLICY,
                                        deviceObject,
                                        (PVOID)Message,
                                        InputBufferLength,
                                        NULL,
                                        0,
                                        FALSE,
                                        &event,
                                        &ioStatus);
    if (irp == NULL) {
        ObDereferenceObject(fileObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(deviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        status = ioStatus.Status;
    }

    ObDereferenceObject(fileObject);
    return status;
}

NTSTATUS
UsbProtectCreateCommunicationPort(
    VOID
    )
{
    NTSTATUS status;
    PSECURITY_DESCRIPTOR securityDescriptor = NULL;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING portName;

    PAGED_CODE();

    status = FltBuildDefaultSecurityDescriptor(&securityDescriptor, FLT_PORT_ALL_ACCESS);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlInitUnicodeString(&portName, USB_PROTECTION_PORT_NAME);

    InitializeObjectAttributes(&objectAttributes,
                               &portName,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               securityDescriptor);

    status = FltCreateCommunicationPort(gUsbProtectFilter,
                                        &gUsbProtectServerPort,
                                        &objectAttributes,
                                        NULL,
                                        UsbProtectConnectNotify,
                                        UsbProtectDisconnectNotify,
                                        UsbProtectMessageNotify,
                                        8);

    FltFreeSecurityDescriptor(securityDescriptor);
    return status;
}

VOID
UsbProtectCloseCommunicationPort(
    VOID
    )
{
    PAGED_CODE();

    if (gUsbProtectServerPort != NULL) {
        FltCloseCommunicationPort(gUsbProtectServerPort);
        gUsbProtectServerPort = NULL;
    }
}

NTSTATUS
UsbProtectConnectNotify(
    _In_ PFLT_PORT ClientPort,
    _In_opt_ PVOID ServerPortCookie,
    _In_reads_bytes_opt_(SizeOfContext) PVOID ConnectionContext,
    _In_ ULONG SizeOfContext,
    _Outptr_result_maybenull_ PVOID *ConnectionCookie
    )
{
    UNREFERENCED_PARAMETER(ServerPortCookie);
    UNREFERENCED_PARAMETER(ConnectionContext);
    UNREFERENCED_PARAMETER(SizeOfContext);

    PAGED_CODE();

    if (ConnectionCookie == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *ConnectionCookie = ClientPort;
    USBP_LOG("Service connected");
    return STATUS_SUCCESS;
}

VOID
UsbProtectDisconnectNotify(
    _In_opt_ PVOID ConnectionCookie
    )
{
    PFLT_PORT clientPort;

    PAGED_CODE();

    clientPort = (PFLT_PORT)ConnectionCookie;
    if (clientPort != NULL && gUsbProtectFilter != NULL) {
        FltCloseClientPort(gUsbProtectFilter, &clientPort);
    }

    USBP_LOG("Service disconnected");
}

NTSTATUS
UsbProtectMessageNotify(
    _In_opt_ PVOID PortCookie,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ReturnOutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG ReturnOutputBufferLength
    )
{
    PUSB_PROTECTION_MESSAGE message;
    PUSB_PROTECTION_REPLY reply;
    PUSB_PROTECTION_POLICY_MESSAGE policyMessage;
    PUSB_PROTECTION_POLICY_REPLY policyReply;

    UNREFERENCED_PARAMETER(PortCookie);

    PAGED_CODE();

    if (ReturnOutputBufferLength == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *ReturnOutputBufferLength = 0;

    if (InputBuffer == NULL || InputBufferLength < sizeof(USB_PROTECTION_MESSAGE)) {
        return STATUS_INVALID_PARAMETER;
    }

    message = (PUSB_PROTECTION_MESSAGE)InputBuffer;

    switch (message->Command) {
    case UsbProtectionEnable:
        UsbProtectSetProtectionEnabled(TRUE);
        USBP_LOG("Protection enabled");
        return STATUS_SUCCESS;

    case UsbProtectionDisable:
        UsbProtectSetProtectionEnabled(FALSE);
        USBP_LOG("Protection disabled");
        return STATUS_SUCCESS;

    case UsbProtectionQueryStatus:
        if (OutputBuffer == NULL || OutputBufferLength < sizeof(USB_PROTECTION_REPLY)) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        reply = (PUSB_PROTECTION_REPLY)OutputBuffer;
        reply->ProtectionEnabled = UsbProtectIsProtectionEnabled() ? 1 : 0;
        *ReturnOutputBufferLength = sizeof(USB_PROTECTION_REPLY);
        return STATUS_SUCCESS;

    case UsbProtectionSetApprovedOnly:
    case UsbProtectionAddApprovedDevice:
    case UsbProtectionRemoveApprovedDevice:
    case UsbProtectionQueryApprovedDevice:
    case UsbProtectionClearApprovedDevices:
        /* Authorization is owned by the user-mode device-control service. */
        return STATUS_NOT_SUPPORTED;

    case UsbProtectionSetExecutableBlocking:
        if (InputBufferLength < sizeof(USB_PROTECTION_POLICY_MESSAGE)) {
            return STATUS_INVALID_PARAMETER;
        }

        policyMessage = (PUSB_PROTECTION_POLICY_MESSAGE)InputBuffer;
        UsbProtectSetExecutableBlockingEnabled(policyMessage->Value != 0);
        USBP_LOG("Executable blocking %s",
                 policyMessage->Value != 0 ? "enabled" : "disabled");
        return STATUS_SUCCESS;

    case UsbProtectionSetHubFilterPolicy:
        if (InputBufferLength < sizeof(USB_PROTECTION_HUB_POLICY_MESSAGE)) {
            return STATUS_INVALID_PARAMETER;
        }
        return UsbProtectForwardHubPolicy(
            (const USB_PROTECTION_HUB_POLICY_MESSAGE*)InputBuffer,
            InputBufferLength);

    case UsbProtectionQueryPolicy:
        if (OutputBuffer == NULL ||
            OutputBufferLength < sizeof(USB_PROTECTION_POLICY_REPLY)) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        policyReply = (PUSB_PROTECTION_POLICY_REPLY)OutputBuffer;
        RtlZeroMemory(policyReply, sizeof(*policyReply));
        policyReply->DataLeakProtectionEnabled =
            UsbProtectIsProtectionEnabled() ? 1 : 0;
        policyReply->ExecutableBlockingEnabled =
            UsbProtectIsExecutableBlockingEnabled() ? 1 : 0;
        policyReply->ApprovedOnlyEnabled = 0;
        policyReply->ApprovedDeviceCount = 0;
        *ReturnOutputBufferLength = sizeof(*policyReply);
        return STATUS_SUCCESS;

    default:
        return STATUS_INVALID_PARAMETER;
    }
}
