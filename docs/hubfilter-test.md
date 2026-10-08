# USB hub filter test

Run this only on an isolated test machine with a snapshot and either PS/2 input or reliable remote access. The filter is installed emergency-disabled unless `-EnableHubFilter` is supplied.

## Build and install

Build `Debug | x64` or `Release | x64` in Visual Studio, then run elevated PowerShell from the repository root:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\scripts\Install-Test.ps1 -Configuration Debug -TrustTestCertificate -EnableHubFilter
Restart-Computer
```

After reboot:

```powershell
sc.exe query UsbProtectionHubFilter
sc.exe query UsbProtectionService

$usbClass = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\{36FC9E60-C465-11CF-8056-444553540000}"
$usbDeviceClass = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\{88BAE032-5A81-49F0-BC3D-A4FF138216D6}"
(Get-ItemProperty $usbClass).UpperFilters
(Get-ItemProperty $usbDeviceClass).UpperFilters
```

Both filter lists must contain `UsbProtectionHubFilter` without losing any pre-existing entries.

## Kernel log and stack position

Capture kernel debug output with WinDbg or DbgView as administrator. Enable **Capture Kernel** in DbgView. Filter for:

```text
[UsbProtectionHub]
```

The log records every attached stack, mass-storage instance ID and hash, `allow` or `block`, the fail-closed reason, filtered `BusRelations`, and any mass-storage device that reaches `START_DEVICE`.

In DeviceTree, select each USB root hub, external hub and composite USB parent. Confirm `UsbProtectionHubFilter` appears in the stack. Also note the location of VMware `hcmon`.

In kernel WinDbg, use the device object address shown by DeviceTree or the attachment log:

```text
!devstack <device-object-address>
!devnode <pdo-address> 1
```

Record whether `hcmon` is above or below `UsbProtectionHubFilter`. The test result, together with the log timestamps, determines whether VMware observes the child PDO before this filter removes it. Do not claim VMware interception is solved solely because the driver loaded.

## Functional cases

1. Start the service and approve USB A in the UI. Unplug and reconnect A. Confirm the host sees it, then choose **Connect to VM** and confirm the guest sees it.
2. Connect unapproved USB B. Confirm no host drive appears. Choose **Connect to VM** if VMware offers the choice; confirm the guest does not receive a drive. The kernel log must contain `BusRelations block` or `START blocked` for B.
3. Approve B in the UI, unplug it, and reconnect it. Confirm both host and guest paths work.
4. Reconnect USB keyboard and mouse devices throughout the test. Confirm they keep working. For a mixed HID/storage composite device, confirm the parent is allowed and only its storage interface is evaluated.
5. Stop `UsbProtectionService`, connect an unapproved mass-storage device, and confirm the driver uses its initial registry snapshot or logs `policy unavailable (fail-closed)`. HID and hubs must remain allowed.
6. Test the emergency switch, then reboot so every filtered stack is recreated:

```powershell
New-ItemProperty `
  "HKLM:\SYSTEM\CurrentControlSet\Services\UsbProtectionHubFilter\Parameters" `
  -Name HubFilterEnabled -PropertyType DWord -Value 0 -Force
Restart-Computer
```

With the switch set to `0`, the hub filter must only pass IRPs through. The existing user-mode `CM_Disable_DevNode` layer remains active while its approved-only policy is enabled.

## Recovery and clean removal

From elevated PowerShell:

```powershell
.\scripts\Uninstall-HubFilter-Test.ps1
```

If the driver is still attached, the script removes only its entries from both `UpperFilters` lists, preserves unrelated filters, disables enforcement, and asks for a reboot. After reboot, run the script once more to delete the service and Driver Store package.
