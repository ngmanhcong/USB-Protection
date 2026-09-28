# Hướng dẫn Functional Test USB Protection bằng UI

Tài liệu này hướng dẫn kiểm thử driver bằng giao diện `UsbProtectionUI.exe`. Việc bật/tắt chính sách và phê duyệt thiết bị phải thực hiện trên UI. File Explorer, Notepad và thao tác nhấp đúp được dùng để tạo hành vi thực tế của người dùng.

> Chỉ thử nghiệm trong máy ảo và trên USB không chứa dữ liệu quan trọng. Nên tạo snapshot VM trước khi bắt đầu.

## 1. Điều kiện trước khi test

Driver và service phải đang chạy. Có thể kiểm tra một lần trong PowerShell Administrator:

```powershell
fltmc filters
fltmc instances -f UsbProtection
sc.exe query UsbProtection
sc.exe query UsbProtectionService
```

Kết quả yêu cầu:

- `UsbProtection` xuất hiện trong danh sách filter.
- Driver `UsbProtection` có trạng thái `RUNNING`.
- Service `UsbProtectionService` có trạng thái `RUNNING`.

Sau bước kiểm tra này, mọi thay đổi chính sách phải thực hiện trên UI.

## 2. Mở UI và nhận diện USB

1. Trong thư mục `x64\Debug`, nhấp đúp `UsbProtectionUI.exe`.
2. Nếu Windows hỏi quyền Administrator, chọn **Yes**.
3. Xác nhận UI hiển thị ba công tắc:
   - **Ngăn chặn rò rỉ dữ liệu**.
   - **Chặn tệp tin thực thi**.
   - **Chỉ cho phép thiết bị đã phê duyệt**.
4. Gắn USB vào máy ảo.
5. Bấm **Làm mới**.
6. Xác nhận USB xuất hiện trong bảng với tên thiết bị, ổ đĩa, dấu vân tay và trạng thái phê duyệt.
7. Ghi lại ký tự ổ USB, ví dụ `E:`.

## 3. Chuẩn bị dữ liệu thử nghiệm

### 3.1. Tắt các chính sách

Trong UI, đặt cả ba công tắc thành OFF:

```text
Ngăn chặn rò rỉ dữ liệu: OFF
Chặn tệp tin thực thi: OFF
Chỉ cho phép thiết bị đã phê duyệt: OFF
```

### 3.2. Tạo file trên máy tính

1. Mở File Explorer.
2. Vào ổ `C:`.
3. Tạo thư mục `C:\UsbProtectionTest`.
4. Trong thư mục đó, nhấp chuột phải → **New → Text Document**.
5. Đặt tên file là `PC-source.txt`.
6. Mở file bằng Notepad và nhập:

```text
Du lieu tu may tinh
```

7. Bấm `Ctrl + S` và đóng Notepad.

### 3.3. Tạo file trên USB

1. Trong File Explorer, mở USB.
2. Tạo thư mục `UsbProtectionTest`.
3. Trong thư mục đó, tạo file `USB-existing.txt`.
4. Mở file bằng Notepad và nhập:

```text
Du lieu tren USB
```

5. Lưu và đóng Notepad.

### 3.4. Chuẩn bị EXE thử nghiệm

1. Mở `C:\Windows\System32`.
2. Tìm `notepad.exe`.
3. Copy file vào thư mục `UsbProtectionTest` trên USB.
4. Đổi tên bản copy thành `USB-notepad.exe`.

## 4. Test DLP khi protection OFF

Trong UI giữ:

```text
Ngăn chặn rò rỉ dữ liệu: OFF
Chặn tệp tin thực thi: OFF
Chỉ cho phép thiết bị đã phê duyệt: OFF
```

### FT-01 — Copy PC sang USB

1. Mở `C:\UsbProtectionTest`.
2. Copy `PC-source.txt`.
3. Paste vào thư mục `UsbProtectionTest` trên USB.

Expected Result:

```text
File được copy thành công.
```

### FT-02 — Tạo file trên USB

1. Trong thư mục USB, nhấp chuột phải.
2. Chọn **New → Text Document**.
3. Đặt tên `Create-When-Off.txt`.

Expected Result:

```text
File được tạo thành công.
```

### FT-03 — Đổi tên và xóa

1. Đổi tên `Create-When-Off.txt` thành `Renamed-When-Off.txt`.
2. Xóa `Renamed-When-Off.txt`.

Expected Result:

```text
Đổi tên thành công.
Xóa thành công.
```

## 5. Test DLP khi protection ON

Trong UI bật:

```text
Ngăn chặn rò rỉ dữ liệu: ON
```

Giữ hai chính sách còn lại OFF.

### FT-04 — Copy PC sang USB

1. Tạo thêm file `PC-block-test.txt` trong `C:\UsbProtectionTest`.
2. Copy file đó.
3. Paste vào thư mục USB.

Expected Result:

```text
Windows báo Access Denied hoặc không cho phép copy.
```

Nếu copy thành công thì kết quả là FAIL.

### FT-05 — Tạo file mới trên USB

1. Trong thư mục USB, nhấp chuột phải.
2. Chọn **New → Text Document**.

Expected Result:

```text
Không tạo được file.
Windows báo Access Denied.
```

### FT-06 — Sửa file hiện có

1. Mở `USB-existing.txt` bằng Notepad.
2. Nhập thêm một dòng.
3. Bấm `Ctrl + S`.

Expected Result:

```text
Đọc được file nhưng không lưu được thay đổi.
```

### FT-07 — Đổi tên file

1. Nhấp chuột phải `USB-existing.txt`.
2. Chọn **Rename**.
3. Thử đổi tên file.

Expected Result:

```text
Không đổi tên được.
```

### FT-08 — Xóa file

1. Chọn `USB-existing.txt`.
2. Nhấn `Delete`.

Expected Result:

```text
Không xóa được.
```

### FT-09 — Copy USB về PC

1. Copy `USB-existing.txt` từ USB.
2. Paste vào `C:\UsbProtectionTest`.

Expected Result:

```text
Copy thành công.
```

Nếu thao tác này bị chặn thì kết quả là **FAIL LOGIC**.

### FT-10 — Ổ C vẫn hoạt động bình thường

Trong `C:\UsbProtectionTest`:

1. Tạo một file mới.
2. Sửa và lưu file.
3. Đổi tên file.
4. Xóa file.

Expected Result:

```text
Mọi thao tác đều thành công.
```

### FT-11 — Tắt DLP

1. Trong UI chuyển **Ngăn chặn rò rỉ dữ liệu** thành OFF.
2. Copy `PC-block-test.txt` sang USB lần nữa.

Expected Result:

```text
Copy thành công.
```

## 6. Test chặn tệp tin thực thi

Trong UI đặt:

```text
Ngăn chặn rò rỉ dữ liệu: OFF
Chặn tệp tin thực thi: ON
Chỉ cho phép thiết bị đã phê duyệt: OFF
```

### FT-12 — Chạy EXE từ USB

Trong USB, nhấp đúp `USB-notepad.exe`.

Expected Result:

```text
Notepad không chạy.
Windows báo Access Denied hoặc không cho phép mở chương trình.
```

### FT-13 — Đọc file văn bản

Nhấp đúp `USB-existing.txt`.

Expected Result:

```text
Notepad mở và đọc được nội dung file.
```

### FT-14 — Tắt chặn thực thi

1. Trong UI chuyển **Chặn tệp tin thực thi** thành OFF.
2. Nhấp đúp lại `USB-notepad.exe`.

Expected Result:

```text
Notepad chạy thành công.
```

## 7. Test danh sách thiết bị được phê duyệt

Trong UI đặt:

```text
Ngăn chặn rò rỉ dữ liệu: OFF
Chặn tệp tin thực thi: OFF
Chỉ cho phép thiết bị đã phê duyệt: OFF
```

### FT-15 — Phê duyệt thiết bị

1. Bấm **Làm mới**.
2. Chọn USB trong danh sách.
3. Bấm **Phê duyệt**.
4. Xác nhận trạng thái chuyển thành **Đã phê duyệt**.
5. Bật **Chỉ cho phép thiết bị đã phê duyệt**.
6. Mở USB bằng File Explorer.
7. Thử mở, tạo và copy file.

Expected Result:

```text
USB đã phê duyệt vẫn truy cập bình thường.
```

### FT-16 — Thu hồi thiết bị

1. Trong UI chọn USB.
2. Bấm **Thu hồi**.
3. Đóng mọi cửa sổ File Explorer đang mở USB.
4. Mở lại USB.
5. Thử mở `USB-existing.txt`.

Expected Result:

```text
Không truy cập được file.
Windows báo Access Denied.
```

USB vẫn có thể xuất hiện trong Explorer vì minifilter chặn truy cập file, không vô hiệu hóa phần cứng USB.

### FT-17 — Phê duyệt lại

1. Trong UI bấm **Làm mới**.
2. Chọn USB.
3. Bấm **Phê duyệt**.
4. Mở lại `USB-existing.txt`.

Expected Result:

```text
Truy cập được file trở lại.
```

### FT-18 — Rút và cắm lại USB

1. Giữ approved-only ON.
2. Eject USB đúng cách.
3. Rút USB khỏi VM.
4. Đợi khoảng 5 giây.
5. Cắm lại USB.
6. Bấm **Làm mới** trong UI.
7. Mở lại `USB-existing.txt`.

Expected Result:

```text
USB vẫn có trạng thái Đã phê duyệt.
File vẫn truy cập được.
```

## 8. Integration Test — Bật cả ba chức năng

USB phải đang được phê duyệt. Trong UI bật cả ba công tắc:

```text
Ngăn chặn rò rỉ dữ liệu: ON
Chặn tệp tin thực thi: ON
Chỉ cho phép thiết bị đã phê duyệt: ON
```

### IT-01 — Đọc file văn bản

Nhấp đúp `USB-existing.txt`.

Expected Result:

```text
Mở và đọc được nội dung.
```

### IT-02 — Sửa file văn bản

1. Nhập thêm nội dung vào `USB-existing.txt`.
2. Bấm `Ctrl + S`.

Expected Result:

```text
Không lưu được vì DLP đang ON.
```

### IT-03 — Chạy EXE

Nhấp đúp `USB-notepad.exe`.

Expected Result:

```text
Không chạy được vì executable blocking đang ON.
```

### IT-04 — Copy PC sang USB

Kéo hoặc copy `PC-source.txt` từ ổ C sang USB.

Expected Result:

```text
Không copy được vì DLP đang ON.
```

### IT-05 — Thu hồi USB

1. Trong UI chọn USB.
2. Bấm **Thu hồi**.
3. Đóng File Explorer đang mở USB.
4. Mở lại USB và thử truy cập file.

Expected Result:

```text
Không truy cập được file nào vì thiết bị không còn được phê duyệt.
```

## 9. Test lưu cấu hình sau khi khởi động lại

1. Phê duyệt lại USB.
2. Đặt trạng thái ba công tắc theo ý muốn.
3. Ghi lại trạng thái hiện tại.
4. Đóng UI.
5. Khởi động lại VM.
6. Mở lại `UsbProtectionUI.exe` bằng quyền Administrator.

Expected Result:

```text
Trạng thái ba công tắc được khôi phục.
USB vẫn có trạng thái Đã phê duyệt.
Các chính sách vẫn có hiệu lực.
```

## 10. Test lại khi Driver Verifier bật

Chỉ bật Driver Verifier sau khi toàn bộ Functional Test phía trên đã PASS.

1. Tạo snapshot VM tên `Before Driver Verifier`.
2. Mở CMD Administrator.
3. Chạy:

```cmd
verifier /reset
verifier /standard /driver UsbProtectionDriver.sys
verifier /bootmode oneboot
shutdown /r /t 0
```

4. Sau khi VM khởi động lại, kiểm tra:

```cmd
verifier /querysettings
```

5. Xác nhận `UsbProtectionDriver.sys` xuất hiện trong danh sách.
6. Lặp lại toàn bộ FT-01 đến IT-05 bằng UI và File Explorer.
7. Thực hiện thêm thao tác bật/tắt công tắc nhiều lần, đóng/mở UI và rút/cắm USB.

Điều kiện PASS:

```text
Không BSOD.
Không treo máy.
Không sai logic Allow/Block.
```

Sau khi hoàn thành:

```cmd
verifier /reset
shutdown /r /t 0
```

## 11. Nếu xảy ra BSOD

Sau khi VM khởi động lại, kiểm tra:

```text
C:\Windows\MEMORY.DMP
```

Copy dump sang máy phát triển và giữ lại file PDB của đúng cùng build:

```text
x64\Debug\UsbProtectionDriver.pdb
```

Trong WinDbg, mở dump rồi chạy:

```text
.symfix
.sympath+ C:\Users\nguye\NCS\USB-Protection\x64\Debug
.reload /f
!analyze -v
lmvm UsbProtectionDriver
```

Nếu VM bị boot loop, restore snapshot `Before Driver Verifier`.

## 12. Mẫu ghi kết quả

```text
ID: FT-04
Test: Copy PC sang USB

Policy:
  DLP: ON
  Executable Blocking: OFF
  Approved Only: OFF

Expected: ACCESS_DENIED
Actual: ACCESS_DENIED
Result: PASS
```

## 13. Bảng tổng kết

| ID | Nội dung | Expected | Actual | Result |
|---|---|---|---|---|
| FT-01 | Copy PC → USB khi DLP OFF | Allow |  |  |
| FT-02 | Create trên USB khi DLP OFF | Allow |  |  |
| FT-03 | Rename/Delete khi DLP OFF | Allow |  |  |
| FT-04 | Copy PC → USB khi DLP ON | Block |  |  |
| FT-05 | Create trên USB khi DLP ON | Block |  |  |
| FT-06 | Write trên USB khi DLP ON | Block |  |  |
| FT-07 | Rename trên USB khi DLP ON | Block |  |  |
| FT-08 | Delete trên USB khi DLP ON | Block |  |  |
| FT-09 | Copy USB → PC khi DLP ON | Allow |  |  |
| FT-10 | Thao tác trên ổ C | Allow |  |  |
| FT-11 | Copy PC → USB sau khi tắt DLP | Allow |  |  |
| FT-12 | Chạy EXE khi executable blocking ON | Block |  |  |
| FT-13 | Đọc TXT khi executable blocking ON | Allow |  |  |
| FT-14 | Chạy EXE sau khi tắt executable blocking | Allow |  |  |
| FT-15 | Truy cập USB đã phê duyệt | Allow |  |  |
| FT-16 | Truy cập USB sau khi thu hồi | Block |  |  |
| FT-17 | Truy cập sau khi phê duyệt lại | Allow |  |  |
| FT-18 | USB đã duyệt sau khi rút/cắm lại | Allow |  |  |
| IT-01 | Đọc TXT khi cả ba policy ON | Allow |  |  |
| IT-02 | Sửa TXT khi cả ba policy ON | Block |  |  |
| IT-03 | Chạy EXE khi cả ba policy ON | Block |  |  |
| IT-04 | Copy PC → USB khi cả ba policy ON | Block |  |  |
| IT-05 | Truy cập sau khi thu hồi USB | Block |  |  |

## 14. Tiêu chí kết luận

Chỉ kết luận driver đạt khi:

```text
Functional Test: PASS
Integration Test: PASS
Driver Verifier Test: PASS
Không có BSOD hoặc treo máy
Không có trường hợp Allow/Block sai yêu cầu
```

Sau khi hoàn thành trên Windows 10 VM, lặp lại toàn bộ quy trình trên Windows 11 VM.
