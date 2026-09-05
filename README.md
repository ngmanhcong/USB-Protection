# USB Protection

Proof-of-Concept Windows USB data-leak prevention project:

- Kernel-mode File System Minifilter Driver in C.
- Windows Service in C.
- Console control utility in C.
- Communication through a Filter Manager communication port named `\UsbProtectionPort`.

The driver allows USB reads and USB-to-local copies, but when protection is ON it blocks create/write/overwrite/delete-like file modifications on USB/removable storage by returning `STATUS_ACCESS_DENIED` from minifilter pre-operation callbacks.

## Architecture

Application or Explorer -> I/O Manager -> Filter Manager -> `UsbProtection` minifilter -> File System -> Storage.

The minifilter attaches to file system volumes. At `InstanceSetup`, it queries the backing disk device with `IOCTL_STORAGE_QUERY_PROPERTY` and caches `BusTypeUsb` plus `RemovableMedia` in a non-paged instance context. `IRP_MJ_CREATE` and `IRP_MJ_WRITE` callbacks then only read cached context and an atomic `ProtectionEnabled` flag.

`UsbProtectionService` connects to `\UsbProtectionPort` with `FilterConnectCommunicationPort` and keeps the connection open while the service runs. `UsbProtectionCtl.exe` can connect independently and send `enable`, `disable`, and `status` commands with `FilterSendMessage`.

## Source Layout

- `Driver/Driver.c`, `Driver.h`: registration, unload, instance context, global protection state.
- `Driver/Callbacks.c`, `Callbacks.h`: `IRP_MJ_CREATE` and `IRP_MJ_WRITE` blocking logic.
- `Driver/UsbDetection.c`, `UsbDetection.h`: storage property query and USB/removable detection.
- `Driver/Communication.c`, `Communication.h`: Filter Manager communication port.
- `Driver/SharedProtocol.h`: shared command/reply protocol.
- `Driver/UsbProtection.inf`: development/test minifilter INF.
- `Service/*`: C Windows service and driver communication helper.
- `Control/*`: C `UsbProtectionCtl.exe` command-line utility.
- `UsbProtection.sln`: Visual Studio solution.

## Prerequisites

- Windows 10/11 x64 test machine or VM.
- Visual Studio 2022 with Desktop development with C++.
- Windows SDK.
- Windows Driver Kit matching the SDK.
- Administrator command prompt.
- Kernel debugging setup is strongly recommended.

Test in a VM with a snapshot. A kernel driver bug can crash Windows.

## Build

Open `UsbProtection.sln` in Visual Studio and build `Debug|x64`.

Expected outputs are under each project output directory:

- `UsbProtectionDriver.sys`
- `UsbProtection.inf`
- `UsbProtectionService.exe`
- `UsbProtectionCtl.exe`

This workspace may not have Visual Studio/WDK installed, so build verification must be performed on a configured WDK machine.

## Driver Signing and Test Mode

This is a development driver. Unsigned x64 kernel drivers do not load normally on Windows.

For a test VM:

```cmd
bcdedit /set testsigning on
shutdown /r /t 0
```

Build with test signing enabled in Visual Studio or sign the package with a test certificate. Production deployment requires proper driver signing and a real Microsoft-assigned minifilter altitude.

## Install and Run

From an elevated command prompt in the driver package output directory:

```cmd
pnputil /add-driver UsbProtection.inf /install
fltmc load UsbProtection
fltmc filters
```

`fltmc filters` should show `UsbProtection`.

Install and start the service:

```cmd
sc create UsbProtectionService binPath= "C:\Path\To\UsbProtectionService.exe" start= demand
sc start UsbProtectionService
```

Use the control utility:

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

## Demo Tests

Create local test folders:

```cmd
mkdir C:\Temp
mkdir C:\Temp2
echo test>C:\Temp\a.txt
```

TEST 1 - Driver loaded:

```cmd
fltmc filters
```

Expected: `UsbProtection` appears.

TEST 2 - Local write:

```cmd
copy C:\Temp\a.txt C:\Temp2\a.txt
```

Expected: success.

TEST 3 - USB read:

```cmd
copy E:\a.txt C:\Temp\a-from-usb.txt
```

Expected: success, where `E:` is the USB drive.

TEST 4 - USB write with protection ON:

```cmd
UsbProtectionCtl.exe enable
copy C:\Temp\a.txt E:\a.txt
```

Expected: blocked with access denied.

TEST 5 - Disable:

```cmd
UsbProtectionCtl.exe disable
copy C:\Temp\a.txt E:\a.txt
```

Expected: success.

TEST 6 - Enable again:

```cmd
UsbProtectionCtl.exe enable
copy C:\Temp\a.txt E:\a2.txt
```

Expected: blocked with access denied.

## Debugging

Driver logging uses `DbgPrintEx` with prefix `[UsbProtect]`. Capture it with WinDbg or DebugView configured for kernel output.

Useful commands:

```cmd
fltmc filters
fltmc instances UsbProtection
fltmc unload UsbProtection
sc query UsbProtectionService
sc query UsbProtection
```

Common issues:

- Driver does not load: check test signing, architecture, INF install, catalog/signature, and Event Viewer.
- `FltRegisterFilter` fails: confirm the service name, INF registration, and that `FltMgr` is present.
- `FltStartFiltering` fails: inspect DbgPrint output and service registry entries.
- Instance does not attach: check `fltmc instances`, file system support, and altitude conflicts.
- USB detected as local: inspect the storage stack; some external disks expose `BusTypeUsb` but `RemovableMedia=false`, while some card readers expose removable media differently. This PoC treats either `BusTypeUsb` or `RemovableMedia` as protected.
- USB not detected: verify `IOCTL_STORAGE_QUERY_PROPERTY` succeeds against the volume disk device and debug `UsbProtectQueryVolumeUsbState`.
- `PreCreate` blocks too much: review desired access, create disposition, and delete-on-close cases in `Callbacks.c`.
- `PreWrite` does not run: confirm the filter is attached to the USB volume and the target operation actually writes file data.
- Service connect fails: ensure the driver is loaded and `\UsbProtectionPort` was created.
- `FilterSendMessage` fails: check protocol buffer sizes and that the port connection handle is valid.
- Unload fails: stop the service/control clients and retry `fltmc unload UsbProtection`.
- BSOD: use a VM snapshot and collect crash dump with WinDbg; inspect callback IRQL, context references, and storage query path.

## Notes

The INF uses altitude `370030` only as a development/test placeholder. Production minifilter drivers must request and use an official altitude from Microsoft.

The PoC intentionally does not implement GUI, encryption, malware scanning, executable blocking, USB whitelists, or drive-letter based decisions.
