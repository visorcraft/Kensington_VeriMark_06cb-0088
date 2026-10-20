# identify-device-windows.ps1
#
# Locates the Kensington VeriMark (06cb:0088) on a Windows box and prints
# its USB topology (bus/hub/port) so the right USBPcapNN interface can be
# picked in Wireshark without guessing. Read-only — queries PnP only,
# touches nothing.
#
# Run in an ordinary PowerShell window (no admin needed for this part;
# Wireshark capture itself needs Administrator).

$vidpid = 'VID_06CB&PID_0088'

Write-Host "Searching for VeriMark (VID_06CB&PID_0088)..." -ForegroundColor Cyan

$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match $vidpid }

if (-not $dev) {
    Write-Host "Not found. Is it plugged in? Listing all USB devices instead:" -ForegroundColor Yellow
    Get-PnpDevice -Class USB | Format-Table FriendlyName, InstanceId, Status -AutoSize
    exit 1
}

$dev | Format-List FriendlyName, InstanceId, Status, Class, Manufacturer

Write-Host "`n--- Location info (helps pick the right USBPcap interface) ---" -ForegroundColor Cyan
foreach ($d in $dev) {
    $loc = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_LocationInfo' -ErrorAction SilentlyContinue
    $bus = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_BusNumber' -ErrorAction SilentlyContinue
    $parent = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue
    Write-Host "InstanceId : $($d.InstanceId)"
    Write-Host "LocationInfo: $($loc.Data)"
    Write-Host "BusNumber   : $($bus.Data)"
    Write-Host "Parent      : $($parent.Data)"
    Write-Host ""
}

Write-Host "--- Root hubs (USBPcap captures per-root-hub, not per-device) ---" -ForegroundColor Cyan
Get-PnpDevice -Class USB | Where-Object { $_.FriendlyName -match 'Root Hub' } | Format-Table FriendlyName, InstanceId -AutoSize

Write-Host @"

Next steps:
1. Open Wireshark AS ADMINISTRATOR.
2. In the capture-interface list you'll see USBPcap1, USBPcap2, ... one per
   root hub shown above. There's no direct name mapping shown here — start
   a capture on one, touch/unplug-replug the VeriMark, and see which
   interface shows traffic (idVendor 0x06cb). Stop, pick the right one,
   restart the real capture there.
3. Capture filter isn't very useful for USBPcap; just filter the DISPLAY
   after capturing, e.g.:  usb.idVendor == 0x06cb
4. Do NOT delete/re-enroll the Windows Hello fingerprint. Just lock and
   unlock (Win+L, then unlock with the reader) 3-4 times in a row while
   capturing. If you can deliberately give one bad/partial touch before a
   proper one on any single attempt, that's extra useful data.
5. Stop capture, File > Save As > .pcapng, send the file back.
"@ -ForegroundColor Green
