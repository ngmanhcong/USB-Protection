#define _WIN32_WINNT 0x0A00

#include "UsbDeviceMonitor.h"

#include <Windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <winioctl.h>
#include <setupapi.h>
#include <usbiodef.h>
#include <stdarg.h>
#include <stdio.h>
#include <strsafe.h>

#include "../Common/PolicyStore.h"
#include "../Common/UsbDevices.h"

#define USBP_DESCRIPTOR_BUFFER_SIZE 1024
#define USBP_MAX_TRACKED_DISABLED_DEVICES 64
#define USBP_MAX_ANCESTOR_DEPTH 16
#define USBP_PNP_SETTLE_RETRY_COUNT 8
#define USBP_PNP_SETTLE_RETRY_MS 100
#define USBP_LOG_DIRECTORY L"C:\\ProgramData\\UsbProtection"
#define USBP_LOG_PATH USBP_LOG_DIRECTORY L"\\DeviceControl.log"

typedef struct _USBP_DISABLED_DEVICE {
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    ULONGLONG DeviceHash;
} USBP_DISABLED_DEVICE;

static HCMNOTIFICATION gDiskNotification = NULL;
static HCMNOTIFICATION gUsbNotification = NULL;
static HANDLE gMonitorStopEvent = NULL;
static HANDLE gMonitorScanEvent = NULL;
static HANDLE gMonitorThread = NULL;
static HANDLE gPolicyChangedEvent = NULL;
static HKEY gPolicyKey = NULL;
static USBP_DISABLED_DEVICE
    gDisabledDevices[USBP_MAX_TRACKED_DISABLED_DEVICES];
static DWORD gDisabledDeviceCount = 0;

static void DeviceControlLog(const wchar_t* format, ...)
{
    wchar_t message[512];
    wchar_t line[600];
    char utf8Line[1800];
    SYSTEMTIME localTime;
    HANDLE logFile;
    DWORD bytesToWrite;
    DWORD bytesWritten;
    va_list args;

    va_start(args, format);
    if (FAILED(StringCchVPrintfW(message, ARRAYSIZE(message), format, args))) {
        va_end(args);
        return;
    }
    va_end(args);

    GetLocalTime(&localTime);
    if (FAILED(StringCchPrintfW(line,
                                ARRAYSIZE(line),
                                L"[%04u-%02u-%02u %02u:%02u:%02u] "
                                L"[DeviceControl] %s\r\n",
                                localTime.wYear,
                                localTime.wMonth,
                                localTime.wDay,
                                localTime.wHour,
                                localTime.wMinute,
                                localTime.wSecond,
                                message))) {
        return;
    }

    OutputDebugStringW(line);
    fwprintf(stderr, L"%ls", line);

    CreateDirectoryW(USBP_LOG_DIRECTORY, NULL);
    logFile = CreateFileW(USBP_LOG_PATH,
                          FILE_APPEND_DATA,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL,
                          OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL,
                          NULL);
    if (logFile == INVALID_HANDLE_VALUE) {
        return;
    }

    bytesToWrite = (DWORD)WideCharToMultiByte(CP_UTF8,
                                               0,
                                               line,
                                               -1,
                                               utf8Line,
                                               sizeof(utf8Line),
                                               NULL,
                                               NULL);
    if (bytesToWrite > 1) {
        WriteFile(logFile,
                  utf8Line,
                  bytesToWrite - 1,
                  &bytesWritten,
                  NULL);
    }
    CloseHandle(logFile);
}

static DWORD ConfigRetToWin32(CONFIGRET result)
{
    return CM_MapCrToWin32Err(result, ERROR_GEN_FAILURE);
}

static BOOL QueryStorageIdentity(const wchar_t* devicePath,
                                 BOOL* isUsb,
                                 BOOL* removableMedia,
                                 ULONGLONG* deviceHash)
{
    HANDLE device;
    STORAGE_PROPERTY_QUERY query;
    BYTE descriptorBuffer[USBP_DESCRIPTOR_BUFFER_SIZE];
    PSTORAGE_DEVICE_DESCRIPTOR descriptor;
    DWORD bytesReturned = 0;
    DWORD descriptorLength;

    if (devicePath == NULL || isUsb == NULL ||
        removableMedia == NULL || deviceHash == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    *isUsb = FALSE;
    *removableMedia = FALSE;
    *deviceHash = 0;

    device = CreateFileW(devicePath,
                         0,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL,
                         OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL,
                         NULL);
    if (device == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    ZeroMemory(&query, sizeof(query));
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    ZeroMemory(descriptorBuffer, sizeof(descriptorBuffer));

    if (!DeviceIoControl(device,
                         IOCTL_STORAGE_QUERY_PROPERTY,
                         &query,
                         sizeof(query),
                         descriptorBuffer,
                         sizeof(descriptorBuffer),
                         &bytesReturned,
                         NULL)) {
        DWORD error = GetLastError();
        CloseHandle(device);
        SetLastError(error);
        return FALSE;
    }
    CloseHandle(device);

    if (bytesReturned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    descriptor = (PSTORAGE_DEVICE_DESCRIPTOR)descriptorBuffer;
    if (descriptor->Size < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    descriptorLength = descriptor->Size;
    if (descriptorLength > bytesReturned) {
        descriptorLength = bytesReturned;
    }

    *isUsb = descriptor->BusType == BusTypeUsb;
    *removableMedia = descriptor->RemovableMedia != FALSE;
    if (*isUsb) {
        *deviceHash = UsbDevicesHashStorageDescriptor(descriptorBuffer,
                                                      descriptorLength);
    }

    return TRUE;
}

static BOOL ExtractUsbIdPart(const wchar_t* instanceId,
                             const wchar_t* marker,
                             wchar_t output[5])
{
    const wchar_t* cursor;
    size_t markerLength;

    if (instanceId == NULL || marker == NULL || output == NULL) {
        return FALSE;
    }

    markerLength = wcslen(marker);
    for (cursor = instanceId; *cursor != L'\0'; cursor++) {
        if (_wcsnicmp(cursor, marker, markerLength) == 0 &&
            wcslen(cursor + markerLength) >= 4) {
            CopyMemory(output, cursor + markerLength, 4 * sizeof(wchar_t));
            output[4] = L'\0';
            return TRUE;
        }
    }

    return FALSE;
}

static BOOL FindPhysicalUsbDevice(DEVINST diskDevInst,
                                  DEVINST* usbDevInst,
                                  wchar_t instanceId[MAX_DEVICE_ID_LEN],
                                  wchar_t vid[5],
                                  wchar_t pid[5])
{
    DEVINST current = diskDevInst;
    ULONG depth;

    if (usbDevInst == NULL || instanceId == NULL || vid == NULL || pid == NULL) {
        return FALSE;
    }

    instanceId[0] = L'\0';
    vid[0] = L'\0';
    pid[0] = L'\0';

    for (depth = 0; depth < USBP_MAX_ANCESTOR_DEPTH; depth++) {
        WCHAR currentId[MAX_DEVICE_ID_LEN];
        DEVINST parent;
        CONFIGRET result;

        result = CM_Get_Device_IDW(current,
                                   currentId,
                                   ARRAYSIZE(currentId),
                                   0);
        if (result != CR_SUCCESS) {
            return FALSE;
        }

        /*
         * We only disable a physical USB VID/PID node reached from a disk
         * interface that was independently confirmed as BusTypeUsb. This
         * excludes host controllers, root hubs, HID devices and unrelated
         * USB peripherals from the disable path.
         */
        if (_wcsnicmp(currentId, L"USB\\VID_", 8) == 0 &&
            ExtractUsbIdPart(currentId, L"VID_", vid) &&
            ExtractUsbIdPart(currentId, L"PID_", pid)) {
            *usbDevInst = current;
            if (FAILED(StringCchCopyW(instanceId,
                                      MAX_DEVICE_ID_LEN,
                                      currentId))) {
                return FALSE;
            }
            return TRUE;
        }

        result = CM_Get_Parent(&parent, current, 0);
        if (result != CR_SUCCESS) {
            break;
        }
        current = parent;
    }

    return FALSE;
}

static BOOL CopyUsbStorIdentityPart(const wchar_t* start,
                                    const wchar_t* end,
                                    wchar_t* output,
                                    size_t outputCount)
{
    size_t target = 0;

    if (start == NULL || end == NULL || output == NULL ||
        outputCount == 0 || end < start) {
        return FALSE;
    }

    while (start < end && target + 1 < outputCount) {
        /* PnP replaces spaces in USBSTOR hardware IDs with underscores. */
        output[target++] = *start == L'_' ? L' ' : *start;
        start++;
    }
    output[target] = L'\0';
    return start == end;
}

static const wchar_t* FindCaseInsensitiveSubstring(const wchar_t* value,
                                                   const wchar_t* pattern)
{
    size_t patternLength;

    if (value == NULL || pattern == NULL) {
        return NULL;
    }

    patternLength = wcslen(pattern);
    if (patternLength == 0) {
        return value;
    }

    while (*value != L'\0') {
        if (_wcsnicmp(value, pattern, patternLength) == 0) {
            return value;
        }
        value++;
    }

    return NULL;
}

static ULONGLONG HashUsbStorInstanceId(const wchar_t* instanceId,
                                       DEVINST usbStorDevInst)
{
    const wchar_t* vendorMarker;
    const wchar_t* productMarker;
    const wchar_t* revisionMarker;
    const wchar_t* serialMarker;
    const wchar_t* serialEnd;
    const wchar_t* suffix;
    wchar_t vendor[64];
    wchar_t product[96];
    wchar_t revision[32];
    wchar_t serial[128];
    ULONG capabilities = 0;
    ULONG capabilitiesSize = sizeof(capabilities);

    if (instanceId == NULL ||
        _wcsnicmp(instanceId, L"USBSTOR\\", 8) != 0) {
        return 0;
    }

    vendorMarker = FindCaseInsensitiveSubstring(instanceId, L"&Ven_");
    productMarker = FindCaseInsensitiveSubstring(instanceId, L"&Prod_");
    revisionMarker = FindCaseInsensitiveSubstring(instanceId, L"&Rev_");
    if (vendorMarker == NULL || productMarker == NULL ||
        revisionMarker == NULL ||
        !(vendorMarker < productMarker && productMarker < revisionMarker)) {
        return 0;
    }

    serialMarker = wcschr(revisionMarker + 5, L'\\');
    if (serialMarker == NULL || serialMarker[1] == L'\0') {
        return 0;
    }
    serialMarker++;
    serialEnd = serialMarker + wcslen(serialMarker);

    /* Windows normally appends an instance suffix such as "&0". */
    suffix = wcsrchr(serialMarker, L'&');
    if (suffix != NULL && suffix + 1 < serialEnd) {
        const wchar_t* digit;
        BOOL numericSuffix = TRUE;

        for (digit = suffix + 1; digit < serialEnd; digit++) {
            if (*digit < L'0' || *digit > L'9') {
                numericSuffix = FALSE;
                break;
            }
        }
        if (numericSuffix) {
            serialEnd = suffix;
        }
    }

    if (!CopyUsbStorIdentityPart(vendorMarker + 5,
                                 productMarker,
                                 vendor,
                                 ARRAYSIZE(vendor)) ||
        !CopyUsbStorIdentityPart(productMarker + 6,
                                 revisionMarker,
                                 product,
                                 ARRAYSIZE(product)) ||
        !CopyUsbStorIdentityPart(revisionMarker + 5,
                                 serialMarker - 1,
                                 revision,
                                 ARRAYSIZE(revision)) ||
        !CopyUsbStorIdentityPart(serialMarker,
                                 serialEnd,
                                 serial,
                                 ARRAYSIZE(serial))) {
        return 0;
    }

    /*
     * If the USB device does not report a unique serial, Windows synthesizes
     * the final instance-ID component. The storage descriptor has no serial
     * in that case, so omit the synthetic value to keep the existing hash.
     */
    if (CM_Get_DevNode_Registry_PropertyW(usbStorDevInst,
                                          CM_DRP_CAPABILITIES,
                                          NULL,
                                          &capabilities,
                                          &capabilitiesSize,
                                          0) != CR_SUCCESS ||
        (capabilities & CM_DEVCAP_UNIQUEID) == 0) {
        serial[0] = L'\0';
    }

    return UsbDevicesHashIdentityStrings(vendor, product, revision, serial);
}

static DWORD FindTrackedDevice(const wchar_t* instanceId)
{
    DWORD index;

    for (index = 0; index < gDisabledDeviceCount; index++) {
        if (_wcsicmp(gDisabledDevices[index].InstanceId, instanceId) == 0) {
            return index;
        }
    }

    return MAXDWORD;
}

static void RemoveTrackedDeviceAt(DWORD index)
{
    if (index >= gDisabledDeviceCount) {
        return;
    }

    gDisabledDeviceCount--;
    if (index != gDisabledDeviceCount) {
        gDisabledDevices[index] = gDisabledDevices[gDisabledDeviceCount];
    }
    ZeroMemory(&gDisabledDevices[gDisabledDeviceCount],
               sizeof(gDisabledDevices[gDisabledDeviceCount]));
}

static void TrackDisabledDevice(const wchar_t* instanceId,
                                ULONGLONG deviceHash)
{
    if (FindTrackedDevice(instanceId) != MAXDWORD) {
        return;
    }

    if (gDisabledDeviceCount >= USBP_MAX_TRACKED_DISABLED_DEVICES) {
        DeviceControlLog(L"Disabled-device tracking list is full");
        return;
    }

    if (FAILED(StringCchCopyW(
            gDisabledDevices[gDisabledDeviceCount].InstanceId,
            ARRAYSIZE(gDisabledDevices[gDisabledDeviceCount].InstanceId),
            instanceId))) {
        return;
    }

    gDisabledDevices[gDisabledDeviceCount].DeviceHash = deviceHash;
    gDisabledDeviceCount++;
}

static void ReconcileTrackedDevices(const USBP_SAVED_POLICY* policy)
{
    DWORD index = 0;

    while (index < gDisabledDeviceCount) {
        DEVINST devInst;
        CONFIGRET result;
        BOOL shouldEnable;

        result = CM_Locate_DevNodeW(&devInst,
                                    gDisabledDevices[index].InstanceId,
                                    CM_LOCATE_DEVNODE_NORMAL);
        if (result != CR_SUCCESS) {
            /* The physical device was removed; forget its transient state. */
            RemoveTrackedDeviceAt(index);
            continue;
        }

        shouldEnable = policy->ApprovedOnlyEnabled == 0 ||
                       UsbPolicyContainsDevice(
                           policy,
                           gDisabledDevices[index].DeviceHash);
        if (!shouldEnable) {
            index++;
            continue;
        }

        result = CM_Enable_DevNode(devInst, 0);
        if (result == CR_SUCCESS) {
            DeviceControlLog(L"Previously disabled USB is now approved; device enabled");
            RemoveTrackedDeviceAt(index);
            continue;
        }

        DeviceControlLog(L"Failed to enable device, error=%lu (CR=0x%08lX)",
                         ConfigRetToWin32(result),
                         result);
        index++;
    }
}

static BOOL WasProcessed(const wchar_t processed[][MAX_DEVICE_ID_LEN],
                         DWORD processedCount,
                         const wchar_t* instanceId)
{
    DWORD index;

    for (index = 0; index < processedCount; index++) {
        if (_wcsicmp(processed[index], instanceId) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

static void EvaluateUsbStorDevNodes(
    const USBP_SAVED_POLICY* policy,
    wchar_t processed[][MAX_DEVICE_ID_LEN],
    DWORD* processedCount)
{
    HDEVINFO deviceInfoSet;
    DWORD deviceIndex;

    deviceInfoSet = SetupDiGetClassDevsW(NULL,
                                         L"USBSTOR",
                                         NULL,
                                         DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        DeviceControlLog(L"Early USBSTOR enumeration failed, error=%lu",
                         GetLastError());
        return;
    }

    for (deviceIndex = 0;; deviceIndex++) {
        SP_DEVINFO_DATA deviceInfoData;
        WCHAR usbStorInstanceId[MAX_DEVICE_ID_LEN];
        WCHAR physicalInstanceId[MAX_DEVICE_ID_LEN];
        WCHAR vid[5];
        WCHAR pid[5];
        DEVINST usbDevInst;
        ULONGLONG deviceHash;
        CONFIGRET result;

        ZeroMemory(&deviceInfoData, sizeof(deviceInfoData));
        deviceInfoData.cbSize = sizeof(deviceInfoData);
        if (!SetupDiEnumDeviceInfo(deviceInfoSet,
                                   deviceIndex,
                                   &deviceInfoData)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) {
                DeviceControlLog(L"Early USBSTOR enumeration stopped, error=%lu",
                                 GetLastError());
            }
            break;
        }

        result = CM_Get_Device_IDW(deviceInfoData.DevInst,
                                   usbStorInstanceId,
                                   ARRAYSIZE(usbStorInstanceId),
                                   0);
        if (result != CR_SUCCESS) {
            continue;
        }

        if (!FindPhysicalUsbDevice(deviceInfoData.DevInst,
                                   &usbDevInst,
                                   physicalInstanceId,
                                   vid,
                                   pid)) {
            continue;
        }

        if (WasProcessed(processed, *processedCount, physicalInstanceId)) {
            continue;
        }
        if (*processedCount < USBP_MAX_TRACKED_DISABLED_DEVICES &&
            SUCCEEDED(StringCchCopyW(processed[*processedCount],
                                     MAX_DEVICE_ID_LEN,
                                     physicalInstanceId))) {
            (*processedCount)++;
        }

        deviceHash = HashUsbStorInstanceId(usbStorInstanceId,
                                            deviceInfoData.DevInst);
        DeviceControlLog(L"USB storage PnP node: VID=%s PID=%s",
                         vid,
                         pid);
        DeviceControlLog(L"DeviceHash=%016I64X", deviceHash);

        if (UsbPolicyContainsDevice(policy, deviceHash)) {
            DeviceControlLog(L"Approved -> allow");
            continue;
        }

        DeviceControlLog(L"Not approved -> disabling physical USB node");
        result = CM_Disable_DevNode(usbDevInst, CM_DISABLE_UI_NOT_OK);
        if (result == CR_SUCCESS) {
            TrackDisabledDevice(physicalInstanceId, deviceHash);
            DeviceControlLog(L"Device disabled successfully");
        } else {
            DeviceControlLog(L"Failed to disable device, error=%lu (CR=0x%08lX)",
                             ConfigRetToWin32(result),
                             result);
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
}

static void EvaluatePresentUsbStorage(void)
{
    USBP_SAVED_POLICY policy;
    HDEVINFO deviceInfoSet;
    DWORD interfaceIndex;
    WCHAR processed[USBP_MAX_TRACKED_DISABLED_DEVICES][MAX_DEVICE_ID_LEN];
    DWORD processedCount = 0;

    if (!UsbPolicyLoad(&policy)) {
        DeviceControlLog(L"Could not load approved-device policy, error=%lu; "
                         L"device-level enforcement skipped",
                         GetLastError());
        return;
    }

    ReconcileTrackedDevices(&policy);

    if (policy.ApprovedOnlyEnabled == 0) {
        DeviceControlLog(L"Approved-only policy is off; USB device-level enforcement is idle");
        return;
    }

    ZeroMemory(processed, sizeof(processed));
    EvaluateUsbStorDevNodes(&policy, processed, &processedCount);

    deviceInfoSet = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK,
                                         NULL,
                                         NULL,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        DeviceControlLog(L"Disk interface enumeration failed, error=%lu",
                         GetLastError());
        return;
    }

    for (interfaceIndex = 0;; interfaceIndex++) {
        SP_DEVICE_INTERFACE_DATA interfaceData;
        SP_DEVINFO_DATA deviceInfoData;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detailData = NULL;
        DWORD requiredSize = 0;
        BOOL isUsb;
        BOOL removableMedia;
        ULONGLONG deviceHash;
        DEVINST usbDevInst;
        WCHAR instanceId[MAX_DEVICE_ID_LEN];
        WCHAR vid[5];
        WCHAR pid[5];
        CONFIGRET result;

        ZeroMemory(&interfaceData, sizeof(interfaceData));
        interfaceData.cbSize = sizeof(interfaceData);

        if (!SetupDiEnumDeviceInterfaces(deviceInfoSet,
                                         NULL,
                                         &GUID_DEVINTERFACE_DISK,
                                         interfaceIndex,
                                         &interfaceData)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) {
                DeviceControlLog(L"Disk interface enumeration stopped, error=%lu",
                                 GetLastError());
            }
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
            DeviceControlLog(L"Could not allocate device-interface buffer");
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

        if (!QueryStorageIdentity(detailData->DevicePath,
                                  &isUsb,
                                  &removableMedia,
                                  &deviceHash)) {
            HeapFree(GetProcessHeap(), 0, detailData);
            continue;
        }
        HeapFree(GetProcessHeap(), 0, detailData);

        if (!isUsb) {
            continue;
        }

        if (!FindPhysicalUsbDevice(deviceInfoData.DevInst,
                                   &usbDevInst,
                                   instanceId,
                                   vid,
                                   pid)) {
            DeviceControlLog(L"USB storage found but no safe VID/PID devnode was found; "
                             L"device left unchanged");
            continue;
        }

        if (WasProcessed(processed, processedCount, instanceId)) {
            continue;
        }
        if (processedCount < ARRAYSIZE(processed) &&
            SUCCEEDED(StringCchCopyW(processed[processedCount],
                                     ARRAYSIZE(processed[processedCount]),
                                     instanceId))) {
            processedCount++;
        }

        DeviceControlLog(L"USB arrived/present: VID=%s PID=%s removable=%s",
                         vid,
                         pid,
                         removableMedia ? L"yes" : L"no");
        DeviceControlLog(L"DeviceHash=%016I64X", deviceHash);

        if (UsbPolicyContainsDevice(&policy, deviceHash)) {
            DeviceControlLog(L"Approved -> allow");
            continue;
        }

        DeviceControlLog(L"Not approved -> disabling device");
        result = CM_Disable_DevNode(usbDevInst, CM_DISABLE_UI_NOT_OK);
        if (result == CR_SUCCESS) {
            TrackDisabledDevice(instanceId, deviceHash);
            DeviceControlLog(L"Device disabled successfully");
        } else {
            DeviceControlLog(L"Failed to disable device, error=%lu (CR=0x%08lX)",
                             ConfigRetToWin32(result),
                             result);
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
}

static DWORD CALLBACK DeviceNotificationCallback(
    HCMNOTIFICATION notification,
    PVOID context,
    CM_NOTIFY_ACTION action,
    PCM_NOTIFY_EVENT_DATA eventData,
    DWORD eventDataSize)
{
    UNREFERENCED_PARAMETER(notification);
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(eventData);
    UNREFERENCED_PARAMETER(eventDataSize);

    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) {
        OutputDebugStringW(L"[DeviceControl] USB/disk interface arrival event\r\n");
        if (gMonitorScanEvent != NULL) {
            SetEvent(gMonitorScanEvent);
        }
    } else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
        OutputDebugStringW(L"[DeviceControl] USB/disk interface removed\r\n");
        if (gMonitorScanEvent != NULL) {
            SetEvent(gMonitorScanEvent);
        }
    }

    return ERROR_SUCCESS;
}

static DWORD WINAPI DeviceMonitorThread(LPVOID parameter)
{
    HANDLE waitHandles[3];
    DWORD waitHandleCount;

    UNREFERENCED_PARAMETER(parameter);

    waitHandles[0] = gMonitorStopEvent;
    waitHandles[1] = gMonitorScanEvent;
    waitHandleCount = 2;
    if (gPolicyChangedEvent != NULL && gPolicyKey != NULL) {
        waitHandles[2] = gPolicyChangedEvent;
        waitHandleCount = 3;
    }

    for (;;) {
        DWORD waitResult;

        if (waitHandleCount == 3) {
            LONG result = RegNotifyChangeKeyValue(gPolicyKey,
                                                  FALSE,
                                                  REG_NOTIFY_CHANGE_LAST_SET,
                                                  gPolicyChangedEvent,
                                                  TRUE);
            if (result != ERROR_SUCCESS) {
                DeviceControlLog(L"Registry notification failed, error=%ld", result);
                waitHandleCount = 2;
            }
        }

        waitResult = WaitForMultipleObjects(waitHandleCount,
                                            waitHandles,
                                            FALSE,
                                            INFINITE);
        if (waitResult == WAIT_OBJECT_0) {
            break;
        }

        if (waitResult == WAIT_OBJECT_0 + 1) {
            DWORD retry;

            /*
             * A physical USB notification can precede creation of its
             * USBSTOR child. Retry briefly so the service can disable the
             * physical node before a virtualization stack redirects it.
             */
            for (retry = 0; retry < USBP_PNP_SETTLE_RETRY_COUNT; retry++) {
                EvaluatePresentUsbStorage();
                if (retry + 1 < USBP_PNP_SETTLE_RETRY_COUNT &&
                    WaitForSingleObject(gMonitorStopEvent,
                                        USBP_PNP_SETTLE_RETRY_MS) ==
                        WAIT_OBJECT_0) {
                    return 0;
                }
            }
            continue;
        }

        if (waitHandleCount == 3 && waitResult == WAIT_OBJECT_0 + 2) {
            /* Coalesce the sequence of values written by UsbPolicySave. */
            if (WaitForSingleObject(gMonitorStopEvent, 100) == WAIT_OBJECT_0) {
                break;
            }
            DeviceControlLog(L"Approved-device policy changed; re-evaluating USB storage");
            EvaluatePresentUsbStorage();
            continue;
        }

        DeviceControlLog(L"Monitor wait failed, error=%lu", GetLastError());
        break;
    }

    return 0;
}

BOOL UsbDeviceMonitorStart(void)
{
    CM_NOTIFY_FILTER filter;
    CONFIGRET result;
    DWORD disposition;
    LONG registryResult;

    if (gMonitorThread != NULL) {
        return TRUE;
    }

    gMonitorStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    gMonitorScanEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    gPolicyChangedEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (gMonitorStopEvent == NULL || gMonitorScanEvent == NULL ||
        gPolicyChangedEvent == NULL) {
        goto Failure;
    }

    registryResult = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                                     USBP_POLICY_REGISTRY_PATH,
                                     0,
                                     NULL,
                                     REG_OPTION_NON_VOLATILE,
                                     KEY_QUERY_VALUE | KEY_NOTIFY,
                                     NULL,
                                     &gPolicyKey,
                                     &disposition);
    UNREFERENCED_PARAMETER(disposition);
    if (registryResult != ERROR_SUCCESS) {
        DeviceControlLog(L"Policy registry watch unavailable, error=%ld",
                         registryResult);
        gPolicyKey = NULL;
        CloseHandle(gPolicyChangedEvent);
        gPolicyChangedEvent = NULL;
    }

    /*
     * This is deliberately a user-mode, best-effort control. Notification is
     * requested for both the physical USB interface and the later disk
     * interface. User-mode delivery still cannot be an absolute pre-claim
     * barrier against a competing virtualization stack. A guaranteed barrier
     * would require a separately designed USB/PnP kernel filter; the existing
     * file-system minifilter remains unchanged.
     */
    ZeroMemory(&filter, sizeof(filter));
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_DISK;

    result = CM_Register_Notification(&filter,
                                      NULL,
                                      DeviceNotificationCallback,
                                      &gDiskNotification);
    if (result != CR_SUCCESS) {
        SetLastError(ConfigRetToWin32(result));
        goto Failure;
    }

    filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_USB_DEVICE;
    result = CM_Register_Notification(&filter,
                                      NULL,
                                      DeviceNotificationCallback,
                                      &gUsbNotification);
    if (result != CR_SUCCESS) {
        SetLastError(ConfigRetToWin32(result));
        goto Failure;
    }

    /*
     * Register first, then enumerate. An arrival racing with this initial
     * scan sets gMonitorScanEvent and is evaluated again by the worker.
     */
    EvaluatePresentUsbStorage();

    gMonitorThread = CreateThread(NULL,
                                  0,
                                  DeviceMonitorThread,
                                  NULL,
                                  0,
                                  NULL);
    if (gMonitorThread == NULL) {
        goto Failure;
    }

    DeviceControlLog(L"USB storage monitor started");
    return TRUE;

Failure:
    {
        DWORD error = GetLastError();

        if (gUsbNotification != NULL) {
            CM_Unregister_Notification(gUsbNotification);
            gUsbNotification = NULL;
        }
        if (gDiskNotification != NULL) {
            CM_Unregister_Notification(gDiskNotification);
            gDiskNotification = NULL;
        }
        if (gPolicyKey != NULL) {
            RegCloseKey(gPolicyKey);
            gPolicyKey = NULL;
        }
        if (gPolicyChangedEvent != NULL) {
            CloseHandle(gPolicyChangedEvent);
            gPolicyChangedEvent = NULL;
        }
        if (gMonitorScanEvent != NULL) {
            CloseHandle(gMonitorScanEvent);
            gMonitorScanEvent = NULL;
        }
        if (gMonitorStopEvent != NULL) {
            CloseHandle(gMonitorStopEvent);
            gMonitorStopEvent = NULL;
        }
        SetLastError(error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE);
        return FALSE;
    }
}

void UsbDeviceMonitorStop(void)
{
    if (gUsbNotification != NULL) {
        CM_Unregister_Notification(gUsbNotification);
        gUsbNotification = NULL;
    }
    if (gDiskNotification != NULL) {
        CM_Unregister_Notification(gDiskNotification);
        gDiskNotification = NULL;
    }

    if (gMonitorStopEvent != NULL) {
        SetEvent(gMonitorStopEvent);
    }
    if (gMonitorThread != NULL) {
        WaitForSingleObject(gMonitorThread, INFINITE);
        CloseHandle(gMonitorThread);
        gMonitorThread = NULL;
    }

    if (gPolicyKey != NULL) {
        RegCloseKey(gPolicyKey);
        gPolicyKey = NULL;
    }
    if (gPolicyChangedEvent != NULL) {
        CloseHandle(gPolicyChangedEvent);
        gPolicyChangedEvent = NULL;
    }
    if (gMonitorScanEvent != NULL) {
        CloseHandle(gMonitorScanEvent);
        gMonitorScanEvent = NULL;
    }
    if (gMonitorStopEvent != NULL) {
        CloseHandle(gMonitorStopEvent);
        gMonitorStopEvent = NULL;
    }

    ZeroMemory(gDisabledDevices, sizeof(gDisabledDevices));
    gDisabledDeviceCount = 0;
    DeviceControlLog(L"USB storage monitor stopped");
}
