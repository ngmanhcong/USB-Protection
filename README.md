# USB Protection

Proof-of-Concept Windows USB protection project written in C:

- Kernel-mode File System Minifilter Driver.
- Windows Service.
- Console control utility.
- Native Win32 management UI.
- Filter Manager communication through `\UsbProtectionPort`.

## Protection policies

The project provides three independent policies:

1. **Data-leak protection** blocks create, write, overwrite, rename, delete, and other file modifications on USB/removable storage. USB reads and USB-to-local copies remain available.
2. **Executable blocking** denies executable image mappings from USB and blocks script types such as `.bat`, `.cmd`, `.ps1`, `.vbs`, `.js`, and `.hta` when they are opened.
3. **Approved-device-only mode** denies file access to USB storage whose fingerprint is not in the allowlist.

The fingerprint is an FNV-1a hash of the storage descriptor's vendor, product, revision, and serial fields. Policies and up to 64 approved fingerprints are persisted under `HKLM\SOFTWARE\UsbProtection` and restored by the Windows service.

## Architecture

Application or Explorer -> I/O Manager -> Filter Manager -> `UsbProtection` minifilter -> File System -> Storage.

At `InstanceSetup`, the minifilter queries the backing disk using `IOCTL_STORAGE_QUERY_PROPERTY`. It caches `BusTypeUsb`, `RemovableMedia`, and the device fingerprint in a non-paged instance context. `IRP_MJ_CREATE`, `IRP_MJ_READ`, `IRP_MJ_WRITE`, `IRP_MJ_SET_INFORMATION`, and executable-section callbacks use this cached context, so no storage query runs on the I/O hot path.

`UsbProtectionService` connects to `\UsbProtectionPort`, restores the saved policies and allowlist, and keeps the connection open. `UsbProtectionUI.exe` manages all policies and connected USB devices. `UsbProtectionCtl.exe` remains available for the original data-leak `enable`, `disable`, and `status` commands.

## Source layout

- `Driver/Driver.c`, `Driver.h`: registration, instance context, global policies, and the in-kernel allowlist.
- `Driver/Callbacks.c`, `Callbacks.h`: file-access, modification, and executable mapping enforcement.
- `Driver/UsbDetection.c`, `UsbDetection.h`: USB/removable detection and fingerprinting.
- `Driver/Communication.c`, `Communication.h`: Filter Manager communication port.
- `Driver/SharedProtocol.h`: kernel/user-mode command protocol.
- `Driver/UsbProtection.inf`: development/test minifilter INF.
- `Service/*`: Windows service and communication helper.
- `Control/*`: command-line utility.
- `Common/*`: Registry policy store and user-mode USB enumeration/fingerprinting.
- `UI/*`: dark native Win32 management application.
- `scripts/Install-Test.ps1`: elevated test-VM installer and launcher.

## Prerequisites and build

- Windows 10/11 x64 test machine or VM.
- Visual Studio 2022 with Desktop development with C++.
- Windows SDK and matching WDK.
- Administrator access.
- A VM snapshot and kernel debugging are strongly recommended.

Open `UsbProtection.sln` and build `Debug|x64`. Expected outputs in `x64\Debug` are:

- `UsbProtectionDriver.sys` plus the `UsbProtectionDriver` package directory.
- `UsbProtectionService.exe`.
- `UsbProtectionCtl.exe`.
- `UsbProtectionUI.exe`.

An unsigned x64 kernel driver does not load normally. For a test VM, enable test signing and reboot:

```cmd
bcdedit /set testsigning on
shutdown /r /t 0
```

In Visual Studio, select a valid test certificate under the driver project's **Driver Signing** properties and build again. If the certificate was created on the development machine, import the generated `UsbProtectionDriver.cer` into the VM's `Trusted Root Certification Authorities` and `Trusted Publishers` stores. Production deployment requires Microsoft signing and an assigned minifilter altitude.

## Install and run in the VM

From an elevated PowerShell window at the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\Install-Test.ps1 -Configuration Debug -TrustTestCertificate
```

Only use `-TrustTestCertificate` inside a disposable test VM and for a certificate you generated. The script installs and loads the driver, creates or updates the service, starts it, and opens the UI.

The equivalent manual driver commands from the package output directory are:

```cmd
pnputil /add-driver UsbProtection.inf /install
fltmc load UsbProtection
fltmc filters
```

Install and start the service manually if needed:

```cmd
sc create UsbProtectionService binPath= "C:\Path\To\UsbProtectionService.exe" start= auto
sc start UsbProtectionService
```

Run `UsbProtectionUI.exe` as administrator. The legacy CLI controls data-leak protection only:

```cmd
UsbProtectionCtl.exe status
UsbProtectionCtl.exe disable
UsbProtectionCtl.exe enable
```

Stop/unload:

```cmd
sc stop UsbProtectionService
fltmc unload UsbProtection
```

## VM test plan

### Data-leak protection

```cmd
mkdir C:\Temp
mkdir C:\Temp2
echo test>C:\Temp\a.txt
copy C:\Temp\a.txt C:\Temp2\a.txt
copy E:\a.txt C:\Temp\a-from-usb.txt
copy C:\Temp\a.txt E:\a.txt
```

With data-leak protection on, local writes and USB-to-local reads succeed, while the final local-to-USB copy is denied. With that toggle off, the local-to-USB copy succeeds.

### Executable blocking

1. Temporarily disable data-leak protection.
2. Copy harmless test files such as `notepad.exe`, `test.bat`, and `test.ps1` to the USB.
3. Enable **Chặn tệp tin thực thi**.
4. Try to run each file from the USB.

Expected: Windows reports access denied. Ordinary document reads remain available.

### Approved devices

1. Keep **Chỉ cho phép thiết bị đã phê duyệt** off.
2. Connect the first USB, select it in the UI, and click **Phê duyệt**.
3. Enable approved-device-only mode. The approved USB remains accessible.
4. Connect a second, unapproved USB and try to open or copy a file from it.

Expected: file access on the second USB is denied. The drive can still appear in Explorer or Device Manager because this project enforces access in the file-system minifilter; it does not disable the physical PnP device. Revoking the first USB blocks subsequent file access on it too.

## Debugging

Driver logging uses `DbgPrintEx` with prefix `[UsbProtect]`. Capture it with WinDbg or DebugView configured for kernel output.

Useful commands:

```cmd
fltmc filters
fltmc instances UsbProtection
sc query UsbProtectionService
sc query UsbProtection
```

Common checks:

- `DPVerifierTask` reports a missing `x86\InfVerif.dll`: repair/reinstall the matching WDK. A source-only local build can temporarily use `/p:SkipPackageVerification=true`, but validate the INF and sign the package before VM installation.
- Driver does not load: verify test-signing mode, architecture, INF/catalog signature, trusted certificate, and Event Viewer.
- Filter does not attach: inspect `fltmc instances`, file-system support, and altitude conflicts.
- USB is misclassified: inspect the storage stack; this PoC treats either `BusTypeUsb` or `RemovableMedia` as protected.
- UI cannot connect: ensure the driver is loaded and `\UsbProtectionPort` exists.
- Policy is lost after reboot: ensure `UsbProtectionService` is set to auto-start and can read `HKLM\SOFTWARE\UsbProtection`.
- BSOD: revert the VM snapshot, collect a crash dump, and inspect callback IRQL and context references in WinDbg.

## Limitations

- The INF altitude `370030` is a development placeholder. Production minifilters must use an altitude assigned by Microsoft.
- Script formats are blocked at file-open time because interpreters read scripts as data rather than map them as executable images. This also prevents reading/copying those script files from USB while executable blocking is enabled.
- Devices without usable vendor/product/serial descriptor data cannot be safely fingerprinted and cannot be approved.
- Fingerprints without a serial number identify a model rather than one physical unit; the UI warns about this case.
- The PoC does not implement encryption, malware scanning, content inspection, or physical USB/PnP device disablement.
