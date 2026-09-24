#include "UsbDetection.h"

#include <ntddstor.h>

#define USBP_STORAGE_DESCRIPTOR_SIZE 1024

static
NTSTATUS
UsbProtectSendStorageQuery(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Out_writes_bytes_(BufferLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG BytesReturned
    )
{
    STORAGE_PROPERTY_QUERY query;
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    NTSTATUS status;

    PAGED_CODE();

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *BytesReturned = 0;

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

    if (NT_SUCCESS(status)) {
        *BytesReturned = (ULONG)ioStatus.Information;
    }

    return status;
}

static
ULONGLONG
UsbProtectHashByte(
    _In_ ULONGLONG Hash,
    _In_ UCHAR Value
    )
{
    Hash ^= Value;
    return Hash * 1099511628211ULL;
}

static
ULONGLONG
UsbProtectHashDescriptorField(
    _In_reads_bytes_(DescriptorLength) const UCHAR *DescriptorBuffer,
    _In_ ULONG DescriptorLength,
    _In_ ULONG Offset,
    _In_ ULONGLONG Hash,
    _Inout_ PBOOLEAN HasIdentity
    )
{
    ULONG start;
    ULONG end;
    ULONG index;
    UCHAR value;

    if (Offset == 0 || Offset >= DescriptorLength) {
        return UsbProtectHashByte(Hash, '|');
    }

    start = Offset;
    while (start < DescriptorLength && DescriptorBuffer[start] == ' ') {
        start++;
    }

    end = start;
    while (end < DescriptorLength && DescriptorBuffer[end] != '\0') {
        end++;
    }

    while (end > start && DescriptorBuffer[end - 1] == ' ') {
        end--;
    }

    for (index = start; index < end; index++) {
        value = DescriptorBuffer[index];
        if (value >= 'a' && value <= 'z') {
            value = (UCHAR)(value - ('a' - 'A'));
        }
        Hash = UsbProtectHashByte(Hash, value);
        *HasIdentity = TRUE;
    }

    return UsbProtectHashByte(Hash, '|');
}

static
ULONGLONG
UsbProtectHashStorageDescriptor(
    _In_reads_bytes_(DescriptorLength) const UCHAR *DescriptorBuffer,
    _In_ ULONG DescriptorLength
    )
{
    const STORAGE_DEVICE_DESCRIPTOR *descriptor;
    ULONGLONG hash = 14695981039346656037ULL;
    BOOLEAN hasIdentity = FALSE;

    if (DescriptorLength < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        return 0;
    }

    descriptor = (const STORAGE_DEVICE_DESCRIPTOR *)DescriptorBuffer;
    hash = UsbProtectHashDescriptorField(DescriptorBuffer,
                                         DescriptorLength,
                                         descriptor->VendorIdOffset,
                                         hash,
                                         &hasIdentity);
    hash = UsbProtectHashDescriptorField(DescriptorBuffer,
                                         DescriptorLength,
                                         descriptor->ProductIdOffset,
                                         hash,
                                         &hasIdentity);
    hash = UsbProtectHashDescriptorField(DescriptorBuffer,
                                         DescriptorLength,
                                         descriptor->ProductRevisionOffset,
                                         hash,
                                         &hasIdentity);
    hash = UsbProtectHashDescriptorField(DescriptorBuffer,
                                         DescriptorLength,
                                         descriptor->SerialNumberOffset,
                                         hash,
                                         &hasIdentity);

    return hasIdentity ? hash : 0;
}

NTSTATUS
UsbProtectQueryVolumeUsbState(
    _In_ PFLT_VOLUME Volume,
    _Out_ PBOOLEAN IsUsb,
    _Out_ PBOOLEAN IsRemovable,
    _Out_ PULONGLONG DeviceHash
    )
{
    NTSTATUS status;
    PDEVICE_OBJECT diskDeviceObject = NULL;
    UCHAR descriptorBuffer[USBP_STORAGE_DESCRIPTOR_SIZE];
    PSTORAGE_DEVICE_DESCRIPTOR descriptor;
    BOOLEAN deviceIsRemovable;
    ULONG bytesReturned = 0;
    ULONG descriptorLength;

    PAGED_CODE();

    if (IsUsb == NULL || IsRemovable == NULL || DeviceHash == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *IsUsb = FALSE;
    *IsRemovable = FALSE;
    *DeviceHash = 0;

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
                                        sizeof(descriptorBuffer),
                                        &bytesReturned);
    ObDereferenceObject(diskDeviceObject);

    if (!NT_SUCCESS(status)) {
        *IsRemovable = deviceIsRemovable;
        return deviceIsRemovable ? STATUS_SUCCESS : status;
    }

    descriptor = (PSTORAGE_DEVICE_DESCRIPTOR)descriptorBuffer;
    if (bytesReturned < sizeof(STORAGE_DEVICE_DESCRIPTOR) ||
        descriptor->Size < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        return STATUS_DEVICE_DATA_ERROR;
    }

    descriptorLength = descriptor->Size;
    if (descriptorLength > bytesReturned) {
        descriptorLength = bytesReturned;
    }

    *IsUsb = (descriptor->BusType == BusTypeUsb) ? TRUE : FALSE;
    *IsRemovable = (descriptor->RemovableMedia || deviceIsRemovable) ? TRUE : FALSE;
    *DeviceHash = UsbProtectHashStorageDescriptor(descriptorBuffer, descriptorLength);

    return STATUS_SUCCESS;
}
