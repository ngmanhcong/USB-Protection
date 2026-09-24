#define _WIN32_WINNT 0x0A00

#include <Windows.h>
#include <CommCtrl.h>
#include <dbt.h>
#include <strsafe.h>
#include <uxtheme.h>

#include "../Common/PolicyStore.h"
#include "../Common/UsbDevices.h"
#include "../Service/DriverCommunication.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "UxTheme.lib")

#define APP_CLASS_NAME L"UsbProtectionManagementWindow"
#define APP_TITLE L"USB Protection"

#define IDC_TOGGLE_DLP 1001
#define IDC_TOGGLE_EXECUTABLE 1002
#define IDC_TOGGLE_APPROVED_ONLY 1003
#define IDC_DEVICE_LIST 1101
#define IDC_APPROVE_DEVICE 1201
#define IDC_REVOKE_DEVICE 1202
#define IDC_REFRESH_DEVICES 1203
#define IDC_STATUS_TEXT 1301

#define USBP_COLOR_BACKGROUND RGB(23, 22, 34)
#define COLOR_PANEL RGB(30, 29, 44)
#define COLOR_BORDER RGB(65, 64, 82)
#define COLOR_BLUE RGB(74, 103, 255)
#define COLOR_TEXT RGB(241, 241, 247)
#define COLOR_MUTED RGB(170, 168, 184)
#define COLOR_OFF RGB(82, 80, 98)
#define COLOR_GREEN RGB(70, 196, 132)

static HANDLE gDriverPort = INVALID_HANDLE_VALUE;
static USBP_SAVED_POLICY gPolicy;
static USBP_DEVICE_INFO gDevices[USBP_MAX_CONNECTED_DEVICES];
static DWORD gDeviceCount = 0;
static HWND gMainWindow = NULL;
static HWND gDeviceList = NULL;
static HWND gStatusText = NULL;
static HFONT gTitleFont = NULL;
static HFONT gHeadingFont = NULL;
static HFONT gBodyFont = NULL;
static HFONT gSmallFont = NULL;

static BOOL DriverConnected(void)
{
    return gDriverPort != NULL && gDriverPort != INVALID_HANDLE_VALUE;
}

static void SetStatus(const wchar_t* text)
{
    if (gStatusText != NULL) {
        SetWindowTextW(gStatusText, text);
    }
}

static void ShowLastErrorMessage(const wchar_t* operation)
{
    DWORD error = GetLastError();
    wchar_t systemMessage[256];
    wchar_t message[512];

    systemMessage[0] = L'\0';
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL,
                   error,
                   0,
                   systemMessage,
                   ARRAYSIZE(systemMessage),
                   NULL);
    StringCchPrintfW(message,
                     ARRAYSIZE(message),
                     L"%s không thành công (lỗi %lu). %s",
                     operation,
                     error,
                     systemMessage);
    MessageBoxW(gMainWindow, message, APP_TITLE, MB_OK | MB_ICONERROR);
}

static BOOL PersistPolicy(void)
{
    if (!UsbPolicySave(&gPolicy)) {
        ShowLastErrorMessage(L"Lưu chính sách vào Registry");
        return FALSE;
    }
    return TRUE;
}

static BOOL GetToggleValue(UINT controlId)
{
    switch (controlId) {
    case IDC_TOGGLE_DLP:
        return gPolicy.DataLeakProtectionEnabled != 0;
    case IDC_TOGGLE_EXECUTABLE:
        return gPolicy.ExecutableBlockingEnabled != 0;
    case IDC_TOGGLE_APPROVED_ONLY:
        return gPolicy.ApprovedOnlyEnabled != 0;
    default:
        return FALSE;
    }
}

static void DrawToggle(const DRAWITEMSTRUCT* drawItem)
{
    BOOL enabled = GetToggleValue(drawItem->CtlID);
    RECT rect = drawItem->rcItem;
    HBRUSH trackBrush;
    HBRUSH knobBrush;
    HPEN oldPen;
    HGDIOBJ oldBrush;
    HPEN nullPen;
    int diameter;
    int knobLeft;

    trackBrush = CreateSolidBrush(enabled ? COLOR_BLUE : COLOR_OFF);
    knobBrush = CreateSolidBrush(RGB(255, 255, 255));
    nullPen = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
    oldPen = (HPEN)SelectObject(drawItem->hDC, nullPen);
    oldBrush = SelectObject(drawItem->hDC, trackBrush);

    RoundRect(drawItem->hDC,
              rect.left,
              rect.top,
              rect.right,
              rect.bottom,
              rect.bottom - rect.top,
              rect.bottom - rect.top);

    diameter = (rect.bottom - rect.top) - 8;
    knobLeft = enabled ? rect.right - diameter - 4 : rect.left + 4;
    SelectObject(drawItem->hDC, knobBrush);
    Ellipse(drawItem->hDC,
            knobLeft,
            rect.top + 4,
            knobLeft + diameter,
            rect.top + 4 + diameter);

    SelectObject(drawItem->hDC, oldBrush);
    SelectObject(drawItem->hDC, oldPen);
    DeleteObject(trackBrush);
    DeleteObject(knobBrush);
    DeleteObject(nullPen);
}

static void DrawActionButton(const DRAWITEMSTRUCT* drawItem)
{
    BOOL primary = drawItem->CtlID == IDC_APPROVE_DEVICE;
    BOOL pressed = (drawItem->itemState & ODS_SELECTED) != 0;
    COLORREF fillColor = primary ? COLOR_BLUE : COLOR_PANEL;
    HBRUSH fillBrush;
    HPEN borderPen;
    HGDIOBJ oldBrush;
    HGDIOBJ oldPen;
    wchar_t text[64];
    RECT textRect = drawItem->rcItem;

    if (pressed) {
        fillColor = primary ? RGB(58, 82, 210) : RGB(44, 43, 60);
    }

    fillBrush = CreateSolidBrush(fillColor);
    borderPen = CreatePen(PS_SOLID, 1, primary ? COLOR_BLUE : COLOR_BORDER);
    oldBrush = SelectObject(drawItem->hDC, fillBrush);
    oldPen = SelectObject(drawItem->hDC, borderPen);
    RoundRect(drawItem->hDC,
              drawItem->rcItem.left,
              drawItem->rcItem.top,
              drawItem->rcItem.right,
              drawItem->rcItem.bottom,
              10,
              10);

    GetWindowTextW(drawItem->hwndItem, text, ARRAYSIZE(text));
    SetBkMode(drawItem->hDC, TRANSPARENT);
    SetTextColor(drawItem->hDC, COLOR_TEXT);
    SelectObject(drawItem->hDC, gBodyFont);
    DrawTextW(drawItem->hDC,
              text,
              -1,
              &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(drawItem->hDC, oldBrush);
    SelectObject(drawItem->hDC, oldPen);
    DeleteObject(fillBrush);
    DeleteObject(borderPen);
}

static void DrawPolicyRow(HDC dc,
                          int top,
                          const wchar_t* icon,
                          const wchar_t* title,
                          const wchar_t* description)
{
    RECT iconRect = { 28, top + 15, 78, top + 65 };
    RECT titleRect = { 96, top + 13, 675, top + 38 };
    RECT descriptionRect = { 96, top + 38, 675, top + 64 };
    HBRUSH panelBrush = CreateSolidBrush(COLOR_PANEL);
    HBRUSH iconBrush = CreateSolidBrush(RGB(34, 38, 66));
    HPEN iconPen = CreatePen(PS_SOLID, 2, COLOR_BLUE);
    HGDIOBJ oldBrush;
    HGDIOBJ oldPen;

    FillRect(dc, &(RECT){ 18, top, 802, top + 78 }, panelBrush);
    oldBrush = SelectObject(dc, iconBrush);
    oldPen = SelectObject(dc, iconPen);
    RoundRect(dc,
              iconRect.left,
              iconRect.top,
              iconRect.right,
              iconRect.bottom,
              12,
              12);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, COLOR_BLUE);
    SelectObject(dc, gHeadingFont);
    DrawTextW(dc, icon, -1, &iconRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SetTextColor(dc, RGB(115, 137, 255));
    SelectObject(dc, gHeadingFont);
    DrawTextW(dc, title, -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SetTextColor(dc, COLOR_MUTED);
    SelectObject(dc, gSmallFont);
    DrawTextW(dc,
              description,
              -1,
              &descriptionRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(panelBrush);
    DeleteObject(iconBrush);
    DeleteObject(iconPen);
}

static void PaintWindow(HWND window)
{
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(window, &paint);
    RECT clientRect;
    HBRUSH backgroundBrush = CreateSolidBrush(USBP_COLOR_BACKGROUND);
    HPEN linePen = CreatePen(PS_SOLID, 1, COLOR_BORDER);
    HGDIOBJ oldPen;
    RECT titleRect = { 28, 20, 780, 62 };
    RECT deviceTitleRect = { 28, 342, 780, 372 };
    RECT deviceHelpRect = { 28, 365, 780, 389 };

    GetClientRect(window, &clientRect);
    FillRect(dc, &clientRect, backgroundBrush);
    SetBkMode(dc, TRANSPARENT);

    SetTextColor(dc, COLOR_TEXT);
    SelectObject(dc, gTitleFont);
    DrawTextW(dc,
              L"Cài đặt nâng cao",
              -1,
              &titleRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    oldPen = SelectObject(dc, CreatePen(PS_SOLID, 2, COLOR_BLUE));
    MoveToEx(dc, 18, 68, NULL);
    LineTo(dc, 802, 68);
    DeleteObject(SelectObject(dc, oldPen));

    DrawPolicyRow(dc,
                  80,
                  L"D",
                  L"Ngăn chặn rò rỉ dữ liệu",
                  L"Chặn thao tác ghi, sửa, đổi tên và xóa tệp trên thiết bị USB");
    DrawPolicyRow(dc,
                  161,
                  L"EXE",
                  L"Chặn tệp tin thực thi",
                  L"Ngăn chạy EXE, DLL, BAT, CMD, PowerShell và script khác từ USB");
    DrawPolicyRow(dc,
                  242,
                  L"USB",
                  L"Chỉ cho phép thiết bị đã phê duyệt",
                  L"Chặn truy cập tệp trên USB không có trong danh sách trắng");

    SetTextColor(dc, COLOR_TEXT);
    SelectObject(dc, gHeadingFont);
    DrawTextW(dc,
              L"Danh sách thiết bị USB",
              -1,
              &deviceTitleRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, COLOR_MUTED);
    SelectObject(dc, gSmallFont);
    DrawTextW(dc,
              L"Chọn thiết bị đang kết nối để phê duyệt hoặc thu hồi quyền truy cập.",
              -1,
              &deviceHelpRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, linePen);
    MoveToEx(dc, 18, 329, NULL);
    LineTo(dc, 802, 329);
    SelectObject(dc, oldPen);

    DeleteObject(linePen);
    DeleteObject(backgroundBrush);
    EndPaint(window, &paint);
}

static void UpdateDeviceList(void)
{
    DWORD index;

    ListView_DeleteAllItems(gDeviceList);

    for (index = 0; index < gDeviceCount; index++) {
        LVITEMW item;
        wchar_t deviceText[300];
        wchar_t hashText[32];
        const wchar_t* approvalText;

        StringCchPrintfW(deviceText,
                         ARRAYSIZE(deviceText),
                         L"%s — %s",
                         gDevices[index].Name,
                         gDevices[index].Serial);
        if (gDevices[index].DeviceHash != 0) {
            StringCchPrintfW(hashText,
                             ARRAYSIZE(hashText),
                             L"%016I64X",
                             gDevices[index].DeviceHash);
        } else {
            StringCchCopyW(hashText, ARRAYSIZE(hashText), L"Không xác định");
        }

        approvalText = UsbPolicyContainsDevice(&gPolicy,
                                                gDevices[index].DeviceHash)
                           ? L"Đã phê duyệt"
                           : L"Chưa phê duyệt";

        ZeroMemory(&item, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)index;
        item.pszText = deviceText;
        item.lParam = (LPARAM)index;
        ListView_InsertItem(gDeviceList, &item);
        ListView_SetItemText(gDeviceList, (int)index, 1, gDevices[index].Drives);
        ListView_SetItemText(gDeviceList, (int)index, 2, hashText);
        ListView_SetItemText(gDeviceList,
                             (int)index,
                             3,
                             (LPWSTR)approvalText);
    }

    if (gDeviceCount == 0) {
        SetStatus(L"Không tìm thấy USB lưu trữ đang kết nối.");
    } else {
        wchar_t status[128];
        StringCchPrintfW(status,
                         ARRAYSIZE(status),
                         L"Đã phát hiện %lu thiết bị USB lưu trữ.",
                         gDeviceCount);
        SetStatus(status);
    }
}

static void RefreshDevices(void)
{
    gDeviceCount = UsbDevicesEnumerate(gDevices, ARRAYSIZE(gDevices));
    UpdateDeviceList();
}

static BOOL GetSelectedDevice(PUSBP_DEVICE_INFO* device)
{
    int selected;
    LVITEMW item;

    if (device == NULL) {
        return FALSE;
    }

    selected = ListView_GetNextItem(gDeviceList, -1, LVNI_SELECTED);
    if (selected < 0) {
        MessageBoxW(gMainWindow,
                    L"Hãy chọn một thiết bị USB trong danh sách.",
                    APP_TITLE,
                    MB_OK | MB_ICONINFORMATION);
        return FALSE;
    }

    ZeroMemory(&item, sizeof(item));
    item.mask = LVIF_PARAM;
    item.iItem = selected;
    if (!ListView_GetItem(gDeviceList, &item) ||
        (DWORD)item.lParam >= gDeviceCount) {
        return FALSE;
    }

    *device = &gDevices[item.lParam];
    return TRUE;
}

static void ApproveSelectedDevice(void)
{
    PUSBP_DEVICE_INFO device;

    if (!GetSelectedDevice(&device)) {
        return;
    }
    if (device->DeviceHash == 0) {
        MessageBoxW(gMainWindow,
                    L"Thiết bị này không cung cấp đủ thông tin nhận dạng để phê duyệt.",
                    APP_TITLE,
                    MB_OK | MB_ICONWARNING);
        return;
    }
    if (UsbPolicyContainsDevice(&gPolicy, device->DeviceHash)) {
        SetStatus(L"Thiết bị đã có trong danh sách được phê duyệt.");
        return;
    }

    if (!UsbProtectionSendAddApprovedDevice(gDriverPort, device->DeviceHash)) {
        ShowLastErrorMessage(L"Gửi thiết bị được phê duyệt tới driver");
        return;
    }
    if (!UsbPolicyAddDevice(&gPolicy, device->DeviceHash) || !PersistPolicy()) {
        UsbProtectionSendRemoveApprovedDevice(gDriverPort, device->DeviceHash);
        UsbPolicyRemoveDevice(&gPolicy, device->DeviceHash);
        return;
    }

    UpdateDeviceList();
    SetStatus(L"Đã phê duyệt thiết bị. Cấu hình sẽ được nạp lại sau khi khởi động.");
}

static void RevokeSelectedDevice(void)
{
    PUSBP_DEVICE_INFO device;

    if (!GetSelectedDevice(&device)) {
        return;
    }
    if (!UsbPolicyContainsDevice(&gPolicy, device->DeviceHash)) {
        SetStatus(L"Thiết bị chưa có trong danh sách được phê duyệt.");
        return;
    }

    if (!UsbProtectionSendRemoveApprovedDevice(gDriverPort, device->DeviceHash)) {
        ShowLastErrorMessage(L"Thu hồi thiết bị trong driver");
        return;
    }
    if (!UsbPolicyRemoveDevice(&gPolicy, device->DeviceHash) || !PersistPolicy()) {
        UsbProtectionSendAddApprovedDevice(gDriverPort, device->DeviceHash);
        UsbPolicyAddDevice(&gPolicy, device->DeviceHash);
        return;
    }

    UpdateDeviceList();
    SetStatus(L"Đã thu hồi quyền truy cập của thiết bị.");
}

static void TogglePolicy(UINT controlId)
{
    BOOL oldValue = GetToggleValue(controlId);
    BOOL newValue = !oldValue;
    BOOL sent = FALSE;

    if (!DriverConnected()) {
        MessageBoxW(gMainWindow,
                    L"Không kết nối được driver. Hãy cài và tải UsbProtection trước.",
                    APP_TITLE,
                    MB_OK | MB_ICONERROR);
        return;
    }

    if (controlId == IDC_TOGGLE_APPROVED_ONLY && newValue &&
        gPolicy.ApprovedDeviceCount == 0) {
        if (MessageBoxW(gMainWindow,
                        L"Danh sách phê duyệt đang trống. Bật tùy chọn này sẽ chặn truy cập tệp trên tất cả USB. Tiếp tục?",
                        APP_TITLE,
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }

    switch (controlId) {
    case IDC_TOGGLE_DLP:
        sent = newValue ? UsbProtectionSendEnable(gDriverPort)
                        : UsbProtectionSendDisable(gDriverPort);
        if (sent) {
            gPolicy.DataLeakProtectionEnabled = newValue ? 1 : 0;
        }
        break;
    case IDC_TOGGLE_EXECUTABLE:
        sent = UsbProtectionSendSetExecutableBlocking(gDriverPort, newValue);
        if (sent) {
            gPolicy.ExecutableBlockingEnabled = newValue ? 1 : 0;
        }
        break;
    case IDC_TOGGLE_APPROVED_ONLY:
        sent = UsbProtectionSendSetApprovedOnly(gDriverPort, newValue);
        if (sent) {
            gPolicy.ApprovedOnlyEnabled = newValue ? 1 : 0;
        }
        break;
    default:
        return;
    }

    if (!sent) {
        ShowLastErrorMessage(L"Cập nhật chính sách trong driver");
        return;
    }

    if (!PersistPolicy()) {
        switch (controlId) {
        case IDC_TOGGLE_DLP:
            oldValue ? UsbProtectionSendEnable(gDriverPort)
                     : UsbProtectionSendDisable(gDriverPort);
            gPolicy.DataLeakProtectionEnabled = oldValue ? 1 : 0;
            break;
        case IDC_TOGGLE_EXECUTABLE:
            UsbProtectionSendSetExecutableBlocking(gDriverPort, oldValue);
            gPolicy.ExecutableBlockingEnabled = oldValue ? 1 : 0;
            break;
        case IDC_TOGGLE_APPROVED_ONLY:
            UsbProtectionSendSetApprovedOnly(gDriverPort, oldValue);
            gPolicy.ApprovedOnlyEnabled = oldValue ? 1 : 0;
            break;
        }
    } else {
        SetStatus(newValue ? L"Đã bật chính sách." : L"Đã tắt chính sách.");
    }

    InvalidateRect(GetDlgItem(gMainWindow, controlId), NULL, TRUE);
}

static void CreateFonts(void)
{
    gTitleFont = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    gHeadingFont = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
    gBodyFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, L"Segoe UI");
    gSmallFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
}

static HWND CreateOwnerButton(HWND parent,
                              UINT id,
                              const wchar_t* text,
                              int x,
                              int y,
                              int width,
                              int height)
{
    return CreateWindowExW(0,
                           L"BUTTON",
                           text,
                           WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                           x,
                           y,
                           width,
                           height,
                           parent,
                           (HMENU)(UINT_PTR)id,
                           GetModuleHandleW(NULL),
                           NULL);
}

static void InitializeDeviceList(HWND list)
{
    LVCOLUMNW column;

    ListView_SetExtendedListViewStyle(list,
                                      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                          LVS_EX_GRIDLINES);
    ListView_SetBkColor(list, COLOR_PANEL);
    ListView_SetTextBkColor(list, COLOR_PANEL);
    ListView_SetTextColor(list, COLOR_TEXT);
    SendMessageW(list, WM_SETFONT, (WPARAM)gSmallFont, TRUE);
    SetWindowTheme(list, L"DarkMode_Explorer", NULL);

    ZeroMemory(&column, sizeof(column));
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = L"Thiết bị";
    column.cx = 315;
    ListView_InsertColumn(list, 0, &column);
    column.pszText = L"Ổ đĩa";
    column.cx = 75;
    column.iSubItem = 1;
    ListView_InsertColumn(list, 1, &column);
    column.pszText = L"Dấu vân tay";
    column.cx = 180;
    column.iSubItem = 2;
    ListView_InsertColumn(list, 2, &column);
    column.pszText = L"Trạng thái";
    column.cx = 165;
    column.iSubItem = 3;
    ListView_InsertColumn(list, 3, &column);
}

static BOOL InitializeApplication(HWND window)
{
    USB_PROTECTION_POLICY_REPLY livePolicy;
    DWORD approvedIndex;
    BOOL allowlistSynchronized;

    UsbPolicyLoad(&gPolicy);

    CreateOwnerButton(window, IDC_TOGGLE_DLP, L"", 724, 104, 56, 30);
    CreateOwnerButton(window, IDC_TOGGLE_EXECUTABLE, L"", 724, 185, 56, 30);
    CreateOwnerButton(window, IDC_TOGGLE_APPROVED_ONLY, L"", 724, 266, 56, 30);

    gDeviceList = CreateWindowExW(WS_EX_CLIENTEDGE,
                                  WC_LISTVIEWW,
                                  L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                                      LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                  28,
                                  394,
                                  764,
                                  182,
                                  window,
                                  (HMENU)(UINT_PTR)IDC_DEVICE_LIST,
                                  GetModuleHandleW(NULL),
                                  NULL);
    InitializeDeviceList(gDeviceList);

    CreateOwnerButton(window,
                      IDC_APPROVE_DEVICE,
                      L"Phê duyệt",
                      487,
                      590,
                      100,
                      36);
    CreateOwnerButton(window,
                      IDC_REVOKE_DEVICE,
                      L"Thu hồi",
                      595,
                      590,
                      90,
                      36);
    CreateOwnerButton(window,
                      IDC_REFRESH_DEVICES,
                      L"Làm mới",
                      693,
                      590,
                      99,
                      36);

    gStatusText = CreateWindowExW(0,
                                  L"STATIC",
                                  L"Đang kết nối driver...",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  28,
                                  644,
                                  764,
                                  24,
                                  window,
                                  (HMENU)(UINT_PTR)IDC_STATUS_TEXT,
                                  GetModuleHandleW(NULL),
                                  NULL);
    SendMessageW(gStatusText, WM_SETFONT, (WPARAM)gSmallFont, TRUE);

    if (!UsbProtectionConnect(&gDriverPort)) {
        SetStatus(L"Không kết nối được driver. Hãy cài và tải UsbProtection trước.");
        RefreshDevices();
        return FALSE;
    }

    /*
     * Normally the service has already restored this list. Re-synchronizing
     * here also makes the UI self-contained when the driver was loaded
     * manually and the service has not started yet.
     */
    allowlistSynchronized = UsbProtectionSendClearApprovedDevices(gDriverPort);
    if (allowlistSynchronized) {
        for (approvedIndex = 0;
             approvedIndex < gPolicy.ApprovedDeviceCount;
             approvedIndex++) {
            if (!UsbProtectionSendAddApprovedDevice(
                    gDriverPort,
                    gPolicy.ApprovedDevices[approvedIndex])) {
                ShowLastErrorMessage(L"Đồng bộ danh sách thiết bị được phê duyệt");
                allowlistSynchronized = FALSE;
                break;
            }
        }
    }

    if (gPolicy.DataLeakProtectionEnabled) {
        UsbProtectionSendEnable(gDriverPort);
    } else {
        UsbProtectionSendDisable(gDriverPort);
    }
    UsbProtectionSendSetExecutableBlocking(
        gDriverPort,
        gPolicy.ExecutableBlockingEnabled != 0);
    if (allowlistSynchronized) {
        UsbProtectionSendSetApprovedOnly(gDriverPort,
                                         gPolicy.ApprovedOnlyEnabled != 0);
    }

    if (UsbProtectionSendQueryPolicy(gDriverPort, &livePolicy)) {
        gPolicy.DataLeakProtectionEnabled = livePolicy.DataLeakProtectionEnabled;
        gPolicy.ExecutableBlockingEnabled = livePolicy.ExecutableBlockingEnabled;
        gPolicy.ApprovedOnlyEnabled = livePolicy.ApprovedOnlyEnabled;
    }

    RefreshDevices();
    return TRUE;
}

static LRESULT CALLBACK WindowProcedure(HWND window,
                                        UINT message,
                                        WPARAM wParam,
                                        LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        gMainWindow = window;
        CreateFonts();
        InitializeApplication(window);
        return 0;

    case WM_PAINT:
        PaintWindow(window);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT* drawItem = (const DRAWITEMSTRUCT*)lParam;
        if (drawItem->CtlID == IDC_TOGGLE_DLP ||
            drawItem->CtlID == IDC_TOGGLE_EXECUTABLE ||
            drawItem->CtlID == IDC_TOGGLE_APPROVED_ONLY) {
            DrawToggle(drawItem);
        } else {
            DrawActionButton(drawItem);
        }
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wParam, COLOR_MUTED);
        SetBkColor((HDC)wParam, USBP_COLOR_BACKGROUND);
        return (LRESULT)GetStockObject(NULL_BRUSH);

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_TOGGLE_DLP:
        case IDC_TOGGLE_EXECUTABLE:
        case IDC_TOGGLE_APPROVED_ONLY:
            TogglePolicy(LOWORD(wParam));
            return 0;
        case IDC_APPROVE_DEVICE:
            ApproveSelectedDevice();
            return 0;
        case IDC_REVOKE_DEVICE:
            RevokeSelectedDevice();
            return 0;
        case IDC_REFRESH_DEVICES:
            RefreshDevices();
            return 0;
        }
        break;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
            SetTimer(window, 1, 800, NULL);
        }
        return TRUE;

    case WM_TIMER:
        if (wParam == 1) {
            KillTimer(window, 1);
            RefreshDevices();
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        if (DriverConnected()) {
            UsbProtectionDisconnect(gDriverPort);
            gDriverPort = INVALID_HANDLE_VALUE;
        }
        DeleteObject(gTitleFont);
        DeleteObject(gHeadingFont);
        DeleteObject(gBodyFont);
        DeleteObject(gSmallFont);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance,
                    HINSTANCE previousInstance,
                    PWSTR commandLine,
                    int showCommand)
{
    INITCOMMONCONTROLSEX commonControls;
    WNDCLASSEXW windowClass;
    HWND window;
    MSG message;

    UNREFERENCED_PARAMETER(previousInstance);
    UNREFERENCED_PARAMETER(commandLine);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    ZeroMemory(&commonControls, sizeof(commonControls));
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&commonControls);

    ZeroMemory(&windowClass, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass.hIcon = LoadIconW(NULL, IDI_SHIELD);
    windowClass.hbrBackground = CreateSolidBrush(USBP_COLOR_BACKGROUND);
    windowClass.lpszClassName = APP_CLASS_NAME;
    windowClass.hIconSm = windowClass.hIcon;

    if (!RegisterClassExW(&windowClass)) {
        return 1;
    }

    window = CreateWindowExW(0,
                             APP_CLASS_NAME,
                             APP_TITLE,
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT,
                             CW_USEDEFAULT,
                             836,
                             726,
                             NULL,
                             NULL,
                             instance,
                             NULL);
    if (window == NULL) {
        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return (int)message.wParam;
}
