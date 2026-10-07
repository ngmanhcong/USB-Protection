#include "UsbDevices.h"

#include <cfgmgr32.h>
#include <setupapi.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <stdint.h>
#include <strsafe.h>

#define USBP_DESCRIPTOR_BUFFER_SIZE 1024

static ULONGLONG HashByte(ULONGLONG hash, BYTE value)
{
    hash ^= value;
    return hash * 1099511628211ULL;
}

static ULONGLONG HashIdentityString(const WCHAR* value,
                                    ULONGLONG hash,
                                    BOOL* hasIdentity)
{
    const WCHAR* start;
    const WCHAR* end;
    const WCHAR* cursor;
    WCHAR character;

    if (value == NULL || hasIdentity == NULL) {
        return HashByte(hash, '|');
    }

    start = value;
    while (*start == L' ') {
        start++;
    }

    end = start + wcslen(start);
    while (end > start && end[-1] == L' ') {
        end--;
    }

    for (cursor = start; cursor < end; cursor++) {
        character = *cursor;
        if (character >= L'a' && character <= L'z') {
            character = (WCHAR)(character - (L'a' - L'A'));
        }

        /* Storage descriptor identity strings are ASCII. */
        if (character <= 0x7f) {
            hash = HashByte(hash, (BYTE)character);
            *hasIdentity = TRUE;
        }
    }

    return HashByte(hash, '|');
}

ULONGLONG UsbDevicesHashIdentityStrings(const WCHAR* vendor,
                                        const WCHAR* product,
                                        const WCHAR* revision,
                                        const WCHAR* serial)
{
    ULONGLONG hash = 14695981039346656037ULL;
    BOOL hasIdentity = FALSE;

    hash = HashIdentityString(vendor, hash, &hasIdentity);
    hash = HashIdentityString(product, hash, &hasIdentity);
    hash = HashIdentityString(revision, hash, &hasIdentity);
    hash = HashIdentityString(serial, hash, &hasIdentity);

    return hasIdentity ? hash : 0;
}

ULONGLONG UsbDevicesHashInstanceId(const WCHAR* instanceId)
{
    ULONGLONG hash = 14695981039346656037ULL;
    const WCHAR* cursor;

    if (instanceId == NULL || instanceId[0] == L'\0') {
        return 0;
    }

    for (cursor = instanceId; *cursor != L'\0'; cursor++) {
        WCHAR character = *cursor;

        if (character >= L'a' && character <= L'z') {
            character = (WCHAR)(character - (L'a' - L'A'));
        }
        if (character > 0x7f) {
            return 0;
        }

        hash = HashByte(hash, (BYTE)character);
    }

    return hash;
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

ULONGLONG UsbDevicesHashStorageDescriptor(const BYTE* descriptorBuffer,
                                          DWORD descriptorLength)
{
    const STORAGE_DEVICE_DESCRIPTOR* descriptor;
    ULONGLONG hash = 14695981039346656037ULL;
    BOOL hasIdentity = FALSE;

    if (descriptorBuffer == NULL ||
        descriptorLength < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
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

static BOOL StartsWithInsensitive(const WCHAR* value, const WCHAR* prefix)
{
    size_t valueLength;
    size_t prefixLength;

    if (value == NULL || prefix == NULL) {
        return FALSE;
    }

    prefixLength = wcslen(prefix);
    valueLength = wcslen(value);
    if (valueLength < prefixLength) {
        return FALSE;
    }
    return _wcsnicmp(value, prefix, prefixLength) == 0;
}

static BOOL GetUsbAncestorInstanceId(DEVINST devInst,
                                     WCHAR* instanceId,
                                     size_t instanceIdCount)
{
    DWORD depth;

    if (instanceId == NULL || instanceIdCount == 0) {
        return FALSE;
    }

    instanceId[0] = L'\0';
    for (depth = 0; depth < 16; depth++) {
        WCHAR currentId[MAX_DEVICE_ID_LEN];
        DEVINST parent;

        if (CM_Get_Device_IDW(devInst,
                             currentId,
                             ARRAYSIZE(currentId),
                             0) != CR_SUCCESS) {
            return FALSE;
        }

        if (StartsWithInsensitive(currentId, L"USB\\VID_")) {
            return SUCCEEDED(StringCchCopyW(instanceId,
                                            instanceIdCount,
                                            currentId));
        }

        if (CM_Get_Parent(&parent, devInst, 0) != CR_SUCCESS) {
            break;
        }
        devInst = parent;
    }

    return FALSE;
}

static BOOL GetVolumeDeviceNumber(HANDLE volume, STORAGE_DEVICE_NUMBER* number)
{
    DWORD bytesReturned = 0;

    ZeroMemory(number, sizeof(*number));
    return DeviceIoControl(volume,
                           IOCTL_STORAGE_GET_DEVICE_NUMBER,
                           NULL,
                           0,
                           number,
                           sizeof(*number),
                           &bytesReturned,
                           NULL) &&
           bytesReturned >= sizeof(*number);
}

static BOOL FindUsbInstanceIdForVolume(HANDLE volume,
                                       WCHAR* instanceId,
                                       size_t instanceIdCount)
{
    STORAGE_DEVICE_NUMBER targetNumber;
    HDEVINFO deviceInfoSet;
    DWORD interfaceIndex;
    BOOL found = FALSE;

    if (!GetVolumeDeviceNumber(volume, &targetNumber)) {
        return FALSE;
    }

    deviceInfoSet = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK,
                                         NULL,
                                         NULL,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    for (interfaceIndex = 0; !found; interfaceIndex++) {
        SP_DEVICE_INTERFACE_DATA interfaceData;
        SP_DEVINFO_DATA deviceInfoData;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detailData;
        DWORD requiredSize = 0;
        HANDLE disk;
        STORAGE_DEVICE_NUMBER candidateNumber;

        ZeroMemory(&interfaceData, sizeof(interfaceData));
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(deviceInfoSet,
                                         NULL,
                                         &GUID_DEVINTERFACE_DISK,
                                         interfaceIndex,
                                         &interfaceData)) {
            break;
        }

        SetupDiGetDeviceInterfaceDetailW(deviceInfoSet,
                                         &interfaceData,
                                         NULL,
                                         0,
                                         &requiredSize,
                                         NULL);
        if (requiredSize == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }

        detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, requiredSize);
        if (detailData == NULL) {
            break;
        }

        detailData->cbSize = sizeof(*detailData);
        ZeroMemory(&deviceInfoData, sizeof(deviceInfoData));
        deviceInfoData.cbSize = sizeof(deviceInfoData);
        if (!SetupDiGetDeviceInterfaceDetailW(deviceInfoSet,
                                              &interfaceData,
                                              detailData,
                                              requiredSize,
                                              NULL,
                                              &deviceInfoData)) {
            HeapFree(GetProcessHeap(), 0, detailData);
            continue;
        }

        disk = CreateFileW(detailData->DevicePath,
                           0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           NULL,
                           OPEN_EXISTING,
                           0,
                           NULL);
        HeapFree(GetProcessHeap(), 0, detailData);
        if (disk == INVALID_HANDLE_VALUE) {
            continue;
        }

        if (GetVolumeDeviceNumber(disk, &candidateNumber) &&
            candidateNumber.DeviceType == targetNumber.DeviceType &&
            candidateNumber.DeviceNumber == targetNumber.DeviceNumber) {
            found = GetUsbAncestorInstanceId(deviceInfoData.DevInst,
                                             instanceId,
                                             instanceIdCount);
        }
        CloseHandle(disk);
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return found;
}

static BOOL MultiSzIsUsbMassStorage(const WCHAR* values, DWORD byteCount)
{
    DWORD remaining = byteCount / sizeof(WCHAR);
    const WCHAR* current = values;

    if (values == NULL || byteCount < (2 * sizeof(WCHAR)) ||
        (byteCount % sizeof(WCHAR)) != 0) {
        return FALSE;
    }

    while (remaining > 1 && current[0] != L'\0') {
        DWORD length = 0;

        while (length < remaining && current[length] != L'\0') {
            length++;
        }
        if (length >= remaining) {
            break;
        }
        if (StartsWithInsensitive(current, L"USB\\Class_08")) {
            return TRUE;
        }
        current += length + 1;
        remaining -= length + 1;
    }

    return FALSE;
}

static DWORD FindDeviceByHash(const USBP_DEVICE_INFO* devices,
                              DWORD count,
                              ULONGLONG deviceHash)
{
    DWORD index;

    for (index = 0; index < count; index++) {
        if (deviceHash != 0 && devices[index].DeviceHash == deviceHash) {
            return index;
        }
    }

    return MAXDWORD;
}

static DWORD AppendPresentUsbMassStorage(PUSBP_DEVICE_INFO devices,
                                         DWORD count,
                                         DWORD capacity)
{
    HDEVINFO deviceInfoSet;
    DWORD deviceIndex;

    if (devices == NULL || count > capacity) {
        return count;
    }

    deviceInfoSet = SetupDiGetClassDevsW(NULL,
                                         L"USB",
                                         NULL,
                                         DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return count;
    }

    for (deviceIndex = 0; count < capacity; deviceIndex++) {
        SP_DEVINFO_DATA deviceInfoData;
        WCHAR compatibleIds[1024];
        WCHAR instanceId[512];
        WCHAR displayName[128];
        DWORD propertyType = 0;
        DWORD propertySize = 0;
        ULONGLONG deviceHash;
        const WCHAR* serial;
        BOOL nameFound;

        ZeroMemory(&deviceInfoData, sizeof(deviceInfoData));
        deviceInfoData.cbSize = sizeof(deviceInfoData);
        if (!SetupDiEnumDeviceInfo(deviceInfoSet,
                                   deviceIndex,
                                   &deviceInfoData)) {
            break;
        }

        ZeroMemory(compatibleIds, sizeof(compatibleIds));
        if (!SetupDiGetDeviceRegistryPropertyW(deviceInfoSet,
                                               &deviceInfoData,
                                               SPDRP_COMPATIBLEIDS,
                                               &propertyType,
                                               (PBYTE)compatibleIds,
                                               sizeof(compatibleIds),
                                               &propertySize) ||
            propertyType != REG_MULTI_SZ ||
            propertySize > sizeof(compatibleIds) ||
            !MultiSzIsUsbMassStorage(compatibleIds, propertySize)) {
            continue;
        }

        if (!SetupDiGetDeviceInstanceIdW(deviceInfoSet,
                                         &deviceInfoData,
                                         instanceId,
                                         ARRAYSIZE(instanceId),
                                         NULL)) {
            continue;
        }

        deviceHash = UsbDevicesHashInstanceId(instanceId);
        if (deviceHash == 0 ||
            FindDeviceByHash(devices, count, deviceHash) != MAXDWORD) {
            continue;
        }

        ZeroMemory(displayName, sizeof(displayName));
        propertySize = 0;
        nameFound = SetupDiGetDeviceRegistryPropertyW(deviceInfoSet,
                                                       &deviceInfoData,
                                                       SPDRP_FRIENDLYNAME,
                                                       &propertyType,
                                                       (PBYTE)displayName,
                                                       sizeof(displayName),
                                                       &propertySize) &&
                    (propertyType == REG_SZ || propertyType == REG_EXPAND_SZ) &&
                    propertySize <= sizeof(displayName);
        if (!nameFound) {
            ZeroMemory(displayName, sizeof(displayName));
            propertySize = 0;
            nameFound = SetupDiGetDeviceRegistryPropertyW(
                            deviceInfoSet,
                            &deviceInfoData,
                            SPDRP_DEVICEDESC,
                            &propertyType,
                            (PBYTE)displayName,
                            sizeof(displayName),
                            &propertySize) &&
                        (propertyType == REG_SZ ||
                         propertyType == REG_EXPAND_SZ) &&
                        propertySize <= sizeof(displayName);
        }
        displayName[ARRAYSIZE(displayName) - 1] = L'\0';
        if (!nameFound || displayName[0] == L'\0') {
            StringCchCopyW(displayName,
                           ARRAYSIZE(displayName),
                           L"USB Mass Storage Device");
        }

        StringCchCopyW(devices[count].Name,
                       ARRAYSIZE(devices[count].Name),
                       displayName);
        StringCchCopyW(devices[count].InstanceId,
                       ARRAYSIZE(devices[count].InstanceId),
                       instanceId);
        devices[count].DeviceHash = deviceHash;
        devices[count].RemovableMedia = TRUE;

        serial = wcsrchr(instanceId, L'\\');
        if (serial != NULL && serial[1] != L'\0') {
            StringCchPrintfW(devices[count].Serial,
                             ARRAYSIZE(devices[count].Serial),
                             L"ID %s",
                             serial + 1);
        } else {
            StringCchCopyW(devices[count].Serial,
                           ARRAYSIZE(devices[count].Serial),
                           L"Không có định danh thiết bị");
        }
        count++;
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return count;
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

    if ((size_t)capacity > (SIZE_MAX / sizeof(*devices))) {
        SetLastError(ERROR_ARITHMETIC_OVERFLOW);
        return 0;
    }

    ZeroMemory(devices, sizeof(*devices) * (size_t)capacity);
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
        WCHAR instanceId[512];
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
        ZeroMemory(instanceId, sizeof(instanceId));

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
        FindUsbInstanceIdForVolume(volume,
                                   instanceId,
                                   ARRAYSIZE(instanceId));
        CloseHandle(volume);

        if (bytesReturned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            continue;
        }

        descriptor = (PSTORAGE_DEVICE_DESCRIPTOR)descriptorBuffer;
        if (descriptor->Size < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            continue;
        }
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

        hash = instanceId[0] != L'\0'
                   ? UsbDevicesHashInstanceId(instanceId)
                   : UsbDevicesHashStorageDescriptor(descriptorBuffer,
                                                     descriptorLength);
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

        if (count >= capacity) {
            break;
        }

        MergeDrive(&devices[count], driveLabel);
        if (instanceId[0] != L'\0') {
            StringCchCopyW(devices[count].InstanceId,
                           ARRAYSIZE(devices[count].InstanceId),
                           instanceId);
        }
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

    return AppendPresentUsbMassStorage(devices, count, capacity);
}

BOOL UsbDevicesRestart(const WCHAR* instanceId)
{
    HDEVINFO deviceInfoSet;
    SP_DEVINFO_DATA deviceInfoData;
    SP_PROPCHANGE_PARAMS changeParameters;
    CONFIGRET enableResult;
    BOOL result;

    if (instanceId == NULL || instanceId[0] == L'\0') {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    deviceInfoSet = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    ZeroMemory(&deviceInfoData, sizeof(deviceInfoData));
    deviceInfoData.cbSize = sizeof(deviceInfoData);
    if (!SetupDiOpenDeviceInfoW(deviceInfoSet,
                                instanceId,
                                NULL,
                                0,
                                &deviceInfoData)) {
        SetupDiDestroyDeviceInfoList(deviceInfoSet);
        return FALSE;
    }

    ZeroMemory(&changeParameters, sizeof(changeParameters));
    changeParameters.ClassInstallHeader.cbSize =
        sizeof(changeParameters.ClassInstallHeader);
    changeParameters.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    changeParameters.StateChange = DICS_PROPCHANGE;
    changeParameters.Scope = DICS_FLAG_GLOBAL;
    changeParameters.HwProfile = 0;

    /* A device disabled by CM_Disable_DevNode must be enabled explicitly. */
    enableResult = CM_Enable_DevNode(deviceInfoData.DevInst, 0);
    result = SetupDiSetClassInstallParamsW(
                 deviceInfoSet,
                 &deviceInfoData,
                 &changeParameters.ClassInstallHeader,
                 sizeof(changeParameters)) &&
             SetupDiCallClassInstaller(DIF_PROPERTYCHANGE,
                                       deviceInfoSet,
                                       &deviceInfoData);

    if (!result && enableResult == CR_SUCCESS) {
        /* Enabling the node is sufficient even if a separate restart fails. */
        result = TRUE;
    } else if (!result) {
        SetLastError(CM_MapCrToWin32Err(enableResult, ERROR_GEN_FAILURE));
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return result;
}
