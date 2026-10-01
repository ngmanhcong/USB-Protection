# Device-level USB protection test plan

Run these tests only in a Windows 10/11 VM with a snapshot. Build and install
the existing minifilter and `UsbProtectionService`, then capture service output
or DebugView messages containing `[DeviceControl]`.

Device-level enforcement follows the existing **Approved Only** policy. Turn
that policy on for blocking tests and off while initially approving a device.

## Test 1 - Empty approved list

1. Clear/revoke all approved devices.
2. Turn Approved Only on.
3. Insert a USB mass-storage device.

Expected: the log reports the VID/PID, hash, `Not approved`, and a successful
disable. The storage device is unavailable.

## Test 2 - Approve and reconnect

1. Turn Approved Only off so the USB can enumerate.
2. Approve it in the UI.
3. Turn Approved Only on, then enable/replug the USB.

Expected: the log reports `Approved -> allow`; Windows mounts the device and
the existing minifilter policies still apply.

## Test 3 - Unapproved USB and VMware

With Approved Only on, insert an unapproved USB and try to connect it to a VM.

Expected: the Host disables the physical USB devnode and the guest cannot use
the device. Record any virtualization race; user-mode notification cannot make
an absolute pre-claim guarantee.

## Test 4 - Approved USB and VMware

Approve the USB before insertion, keep Approved Only on, and request
passthrough.

Expected: this new device-level layer leaves the USB enabled. If passthrough is
allowed by VMware, Host minifilter file policies do not execute inside the
guest; that is the expected boundary of the current architecture.

## Test 5 - Repeated removal/reinsert

Remove and insert approved and unapproved USB storage repeatedly.

Expected: policy is re-evaluated every time, removal state is cleaned up, and
the service does not crash or retain device handles.

## Test 6 - Keyboard and mouse safety

Insert/reinsert USB keyboard and mouse devices.

Expected: they remain enabled. They do not publish `GUID_DEVINTERFACE_DISK`,
are never confirmed as `BusTypeUsb` storage, and never enter the disable path.

## Test 7 - Restart service with USB present

Leave an enabled, unapproved USB connected with Approved Only on, then restart
`UsbProtectionService`.

Expected: startup enumeration finds and disables the device before the service
reports normal running operation.

## Test 8 - Reboot safety

Test one approved and one unapproved USB, reboot, and inspect Device Manager,
the service state, minifilter state, keyboard, mouse, hubs, and controllers.

Expected: no boot loop or BSOD; system USB devices are untouched. Because the
service does not request persistent PnP disable, boot recovery remains possible
and the startup scan reapplies policy to enabled USB storage.
