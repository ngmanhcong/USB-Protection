#include "UsbDevices.h"

#include <winioctl.h>
#include <ntddstor.h>
#include <strsafe.h>

#define USBP_DESCRIPTOR_BUFFER_SIZE 1024

static ULONGLONG HashByte(ULONGLONG hash, BYTE value)
{
    hash ^= value;
    return hash * 1099511628211ULL;
}

static ULONGLONG HashDescriptorField(const BYTE* descriptorBuffer,
                                     DWORD descriptorLength,
                                     DWORD offset,
                                     ULONGLONG hash,
                                     BOOL* hasIdentity)
{
    DWORD start;
    DWORD end;
    DWORD index;
    BYTE value;

    if (offset == 0 || offset >= descriptorLength) {
        return HashByte(hash, '|');
    }

    start = offset;
    while (start < descriptorLength && descriptorBuffer[start] == ' ') {
        start++;
    }

    end = start;
    while (end < descriptorLength && descriptorBuffer[end] != '\0') {
        end++;
    }

    while (end > start && descriptorBuffer[end - 1] == ' ') {
        end--;
    }

    for (index = start; index < end; index++) {
        value = descriptorBuffer[index];
        if (value >= 'a' && value <= 'z') {
            value = (BYTE)(value - ('a' - 'A'));
        }
        hash = HashByte(hash, value);
        *hasIdentity = TRUE;
    }

    return HashByte(hash, '|');
}

static ULONGLONG HashStorageDescriptor(const BYTE* descriptorBuffer,
                                       DWORD descriptorLength)
{
    const STORAGE_DEVICE_DESCRIPTOR* descriptor;
    ULONGLONG hash = 14695981039346656037ULL;
    BOOL hasIdentity = FALSE;

    if (descriptorLength < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        return 0;
    }

    descriptor = (const STORAGE_DEVICE_DESCRIPTOR*)descriptorBuffer;
    hash = HashDescriptorField(descriptorBuffer,
                               descriptorLength,
                               descriptor->VendorIdOffset,
                               hash,
                               &hasIdentity);
    hash = HashDescriptorField(descriptorBuffer,
                               descriptorLength,
                               descriptor->ProductIdOffset,
                               hash,
                               &hasIdentity);
    hash = HashDescriptorField(descriptorBuffer,
                               descriptorLength,
                               descriptor->ProductRevisionOffset,
                               hash,
                               &hasIdentity);
    hash = HashDescriptorField(descriptorBuffer,
                               descriptorLength,
                               descriptor->SerialNumberOffset,
                               hash,
                               &hasIdentity);

    return hasIdentity ? hash : 0;
}

static void CopyDescriptorText(const BYTE* descriptorBuffer,
                               DWORD descriptorLength,
                               DWORD offset,
                               WCHAR* output,
                               size_t outputCount)
{
    DWORD start;
    DWORD end;
    DWORD index;
    size_t target = 0;

    if (output == NULL || outputCount == 0) {
        return;
    }

    output[0] = L'\0';
    if (offset == 0 || offset >= descriptorLength) {
        return;
    }

    start = offset;
    while (start < descriptorLength && descriptorBuffer[start] == ' ') {
        start++;
    }

    end = start;
    while (end < descriptorLength && descriptorBuffer[end] != '\0') {
        end++;
    }
    while (end > start && descriptorBuffer[end - 1] == ' ') {
        end--;
    }

    for (index = start; index < end && target + 1 < outputCount; index++) {
        output[target++] = (WCHAR)descriptorBuffer[index];
    }
    output[target] = L'\0';
}

static void MergeDrive(PUSBP_DEVICE_INFO device, const WCHAR* drive)
{
    if (device->Drives[0] != L'\0') {
        StringCchCatW(device->Drives, ARRAYSIZE(device->Drives), L", ");
    }
    StringCchCatW(device->Drives, ARRAYSIZE(device->Drives), drive);
}

DWORD UsbDevicesEnumerate(PUSBP_DEVICE_INFO devices, DWORD capacity)
{
    DWORD driveMask;
    DWORD letterIndex;
    DWORD count = 0;
    WCHAR rootPath[] = L"A:\\";
    WCHAR volumePath[] = L"\\\\.\\A:";

    if (devices == NULL || capacity == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    ZeroMemory(devices, sizeof(*devices) * capacity);
    driveMask = GetLogicalDrives();

    for (letterIndex = 0; letterIndex < 26 && count < capacity; letterIndex++) {
        HANDLE volume;
        STORAGE_PROPERTY_QUERY query;
        BYTE descriptorBuffer[USBP_DESCRIPTOR_BUFFER_SIZE];
        DWORD bytesReturned = 0;
        DWORD descriptorLength;
        PSTORAGE_DEVICE_DESCRIPTOR descriptor;
        UINT driveType;
        BOOL isUsbOrRemovable;
        ULONGLONG hash;
        WCHAR vendor[64];
        WCHAR product[96];
        WCHAR revision[32];
        WCHAR serial[128];
        WCHAR driveLabel[4];
        DWORD existing;

        if ((driveMask & (1UL << letterIndex)) == 0) {
            continue;
        }

        rootPath[0] = (WCHAR)(L'A' + letterIndex);
        volumePath[4] = (WCHAR)(L'A' + letterIndex);
        driveType = GetDriveTypeW(rootPath);

        volume = CreateFileW(volumePath,
                             0,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL,
                             OPEN_EXISTING,
                             0,
                             NULL);
        if (volume == INVALID_HANDLE_VALUE) {
            continue;
        }

        ZeroMemory(&query, sizeof(query));
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        ZeroMemory(descriptorBuffer, sizeof(descriptorBuffer));

        if (!DeviceIoControl(volume,
                             IOCTL_STORAGE_QUERY_PROPERTY,
                             &query,
                             sizeof(query),
                             descriptorBuffer,
                             sizeof(descriptorBuffer),
                             &bytesReturned,
                             NULL)) {
            CloseHandle(volume);
            continue;
        }
        CloseHandle(volume);

        if (bytesReturned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            continue;
        }

        descriptor = (PSTORAGE_DEVICE_DESCRIPTOR)descriptorBuffer;
        descriptorLength = descriptor->Size;
        if (descriptorLength > bytesReturned) {
            descriptorLength = bytesReturned;
        }

        isUsbOrRemovable = descriptor->BusType == BusTypeUsb ||
                           descriptor->RemovableMedia ||
                           driveType == DRIVE_REMOVABLE;
        if (!isUsbOrRemovable) {
            continue;
        }

        hash = HashStorageDescriptor(descriptorBuffer, descriptorLength);
        StringCchPrintfW(driveLabel, ARRAYSIZE(driveLabel), L"%c:",
                         L'A' + letterIndex);

        for (existing = 0; existing < count; existing++) {
            if (hash != 0 && devices[existing].DeviceHash == hash) {
                MergeDrive(&devices[existing], driveLabel);
                break;
            }
        }
        if (existing < count) {
            continue;
        }

        CopyDescriptorText(descriptorBuffer,
                           descriptorLength,
                           descriptor->VendorIdOffset,
                           vendor,
                           ARRAYSIZE(vendor));
        CopyDescriptorText(descriptorBuffer,
                           descriptorLength,
                           descriptor->ProductIdOffset,
                           product,
                           ARRAYSIZE(product));
        CopyDescriptorText(descriptorBuffer,
                           descriptorLength,
                           descriptor->ProductRevisionOffset,
                           revision,
                           ARRAYSIZE(revision));
        CopyDescriptorText(descriptorBuffer,
                           descriptorLength,
                           descriptor->SerialNumberOffset,
                           serial,
                           ARRAYSIZE(serial));

        MergeDrive(&devices[count], driveLabel);
        if (vendor[0] != L'\0' && product[0] != L'\0') {
            StringCchPrintfW(devices[count].Name,
                             ARRAYSIZE(devices[count].Name),
                             L"%s %s",
                             vendor,
                             product);
        } else if (product[0] != L'\0') {
            StringCchCopyW(devices[count].Name,
                           ARRAYSIZE(devices[count].Name),
                           product);
        } else {
            StringCchCopyW(devices[count].Name,
                           ARRAYSIZE(devices[count].Name),
                           L"USB Storage Device");
        }

        if (serial[0] != L'\0') {
            StringCchPrintfW(devices[count].Serial,
                             ARRAYSIZE(devices[count].Serial),
                             L"S/N %s · Rev %s",
                             serial,
                             revision[0] != L'\0' ? revision : L"-");
        } else {
            StringCchCopyW(devices[count].Serial,
                           ARRAYSIZE(devices[count].Serial),
                           L"Không có số sê-ri; phê duyệt theo model");
        }
        devices[count].DeviceHash = hash;
        devices[count].RemovableMedia = descriptor->RemovableMedia;
        count++;
    }

    return count;
}
