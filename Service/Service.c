#include "Service.h"

#include <stdio.h>

#include "DriverCommunication.h"

static SERVICE_STATUS gServiceStatus = { 0 };
static SERVICE_STATUS_HANDLE gServiceStatusHandle = NULL;
static HANDLE gStopEvent = NULL;
static HANDLE gDriverPort = INVALID_HANDLE_VALUE;

static void LogLastError(const wchar_t* message)
{
    fwprintf(stderr, L"[UsbProtectionService] %s failed, error=%lu\n", message, GetLastError());
}

static void SetServiceState(DWORD state, DWORD win32ExitCode, DWORD waitHint)
{
    gServiceStatus.dwCurrentState = state;
    gServiceStatus.dwWin32ExitCode = win32ExitCode;
    gServiceStatus.dwWaitHint = waitHint;

    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        gServiceStatus.dwControlsAccepted = 0;
    } else {
        gServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }

    if (gServiceStatusHandle != NULL) {
        SetServiceStatus(gServiceStatusHandle, &gServiceStatus);
    }
}

DWORD WINAPI UsbProtectionServiceHandler(DWORD control, DWORD eventType, LPVOID eventData, LPVOID context)
{
    UNREFERENCED_PARAMETER(eventType);
    UNREFERENCED_PARAMETER(eventData);
    UNREFERENCED_PARAMETER(context);

    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        if (gServiceStatus.dwCurrentState != SERVICE_RUNNING) {
            break;
        }

        SetServiceState(SERVICE_STOP_PENDING, NO_ERROR, 3000);
        if (gStopEvent != NULL) {
            SetEvent(gStopEvent);
        }
        break;

    default:
        break;
    }

    return NO_ERROR;
}

void WINAPI UsbProtectionServiceMain(DWORD argc, LPWSTR* argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    gServiceStatusHandle = RegisterServiceCtrlHandlerExW(USB_PROTECTION_SERVICE_NAME,
                                                         UsbProtectionServiceHandler,
                                                         NULL);
    if (gServiceStatusHandle == NULL) {
        LogLastError(L"RegisterServiceCtrlHandlerExW");
        return;
    }

    gServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gServiceStatus.dwServiceSpecificExitCode = 0;

    SetServiceState(SERVICE_START_PENDING, NO_ERROR, 3000);

    gStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (gStopEvent == NULL) {
        DWORD error = GetLastError();
        LogLastError(L"CreateEventW");
        SetServiceState(SERVICE_STOPPED, error, 0);
        return;
    }

    if (!UsbProtectionConnect(&gDriverPort)) {
        DWORD error = GetLastError();
        LogLastError(L"FilterConnectCommunicationPort");
        CloseHandle(gStopEvent);
        gStopEvent = NULL;
        SetServiceState(SERVICE_STOPPED, error, 0);
        return;
    }

    if (!UsbProtectionSendEnable(gDriverPort)) {
        DWORD error = GetLastError();
        LogLastError(L"UsbProtectionSendEnable");
        UsbProtectionDisconnect(gDriverPort);
        gDriverPort = INVALID_HANDLE_VALUE;
        CloseHandle(gStopEvent);
        gStopEvent = NULL;
        SetServiceState(SERVICE_STOPPED, error, 0);
        return;
    }

    SetServiceState(SERVICE_RUNNING, NO_ERROR, 0);

    WaitForSingleObject(gStopEvent, INFINITE);

    SetServiceState(SERVICE_STOP_PENDING, NO_ERROR, 3000);

    UsbProtectionDisconnect(gDriverPort);
    gDriverPort = INVALID_HANDLE_VALUE;

    CloseHandle(gStopEvent);
    gStopEvent = NULL;

    SetServiceState(SERVICE_STOPPED, NO_ERROR, 0);
}
