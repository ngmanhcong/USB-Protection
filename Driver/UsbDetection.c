#include "UsbDetection.h"

#include <ntddstor.h>

#define USBP_STORAGE_DESCRIPTOR_SIZE 1024

static
NTSTATUS
UsbProtectSendStorageQuery(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Out_writes_bytes_(BufferLength) PVOID Buffer,
    _In_ ULONG BufferLength
    )
{
    STORAGE_PROPERTY_QUERY query;
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    NTSTATUS status;

    PAGED_CODE();

    RtlZeroMemory(&query, sizeof(query));
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&ioStatus, sizeof(ioStatus));

    irp = IoBuildDeviceIoControlRequest(IOCTL_STORAGE_QUERY_PROPERTY,
                                        DeviceObject,
                                        &query,
                                        sizeof(query),
                                        Buffer,
                                        BufferLength,
                                        FALSE,
                                        &event,
                                        &ioStatus);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(DeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }

    return status;
}

NTSTATUS
UsbProtectQueryVolumeUsbState(
    _In_ PFLT_VOLUME Volume,
    _Out_ PBOOLEAN IsUsb,
    _Out_ PBOOLEAN IsRemovable
    )
{
    NTSTATUS status;
    PDEVICE_OBJECT diskDeviceObject = NULL;
    UCHAR descriptorBuffer[USBP_STORAGE_DESCRIPTOR_SIZE];
    PSTORAGE_DEVICE_DESCRIPTOR descriptor;
    BOOLEAN deviceIsRemovable;

    PAGED_CODE();

    if (IsUsb == NULL || IsRemovable == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *IsUsb = FALSE;
    *IsRemovable = FALSE;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = FltGetDiskDeviceObject(Volume, &diskDeviceObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    deviceIsRemovable =
        ((diskDeviceObject->Characteristics & FILE_REMOVABLE_MEDIA) != 0) ? TRUE : FALSE;

    RtlZeroMemory(descriptorBuffer, sizeof(descriptorBuffer));
    status = UsbProtectSendStorageQuery(diskDeviceObject,
                                        descriptorBuffer,
                                        sizeof(descriptorBuffer));
    ObDereferenceObject(diskDeviceObject);

    if (!NT_SUCCESS(status)) {
        *IsRemovable = deviceIsRemovable;
        return deviceIsRemovable ? STATUS_SUCCESS : status;
    }

    descriptor = (PSTORAGE_DEVICE_DESCRIPTOR)descriptorBuffer;
    if (descriptor->Size < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        return STATUS_DEVICE_DATA_ERROR;
    }

    *IsUsb = (descriptor->BusType == BusTypeUsb) ? TRUE : FALSE;
    *IsRemovable = (descriptor->RemovableMedia || deviceIsRemovable) ? TRUE : FALSE;

    return STATUS_SUCCESS;
}
