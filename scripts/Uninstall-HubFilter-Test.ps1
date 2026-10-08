param()

$ErrorActionPreference = "Stop"

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated PowerShell window."
}

function Remove-ClassUpperFilter {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ClassGuid,
        [Parameter(Mandatory = $true)]
        [string]$FilterName
    )

    $classKey = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$ClassGuid"
    $properties = Get-ItemProperty -LiteralPath $classKey
    $property = $properties.PSObject.Properties["UpperFilters"]
    if ($null -eq $property) {
        return
    }

    $remaining = @($property.Value) | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_) -and
        -not [string]::Equals($_,
                              $FilterName,
                              [StringComparison]::OrdinalIgnoreCase)
    }
    if ($remaining.Count -eq 0) {
        Remove-ItemProperty -LiteralPath $classKey -Name "UpperFilters"
    } else {
        Set-ItemProperty -LiteralPath $classKey -Name "UpperFilters" -Value $remaining
    }
}

$serviceName = "UsbProtectionHubFilter"
$parameters = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\$serviceName\Parameters"
if (Test-Path -LiteralPath $parameters) {
    New-ItemProperty -LiteralPath $parameters `
        -Name "HubFilterEnabled" `
        -PropertyType DWord `
        -Value 0 `
        -Force | Out-Null
}

Remove-ClassUpperFilter `
    -ClassGuid "{36FC9E60-C465-11CF-8056-444553540000}" `
    -FilterName $serviceName
Remove-ClassUpperFilter `
    -ClassGuid "{88BAE032-5A81-49F0-BC3D-A4FF138216D6}" `
    -FilterName $serviceName

& sc.exe stop $serviceName *> $null
$packages = @(Get-WindowsDriver -Online | Where-Object {
    -not [string]::IsNullOrWhiteSpace($_.OriginalFileName) -and
    (Split-Path -Leaf $_.OriginalFileName) -ieq "UsbProtectionHubFilter.inf"
})
$packageRemovalFailed = $false
foreach ($package in $packages) {
    & pnputil.exe /delete-driver $package.Driver /uninstall
    if ($LASTEXITCODE -ne 0) {
        $packageRemovalFailed = $true
    }
}

if (-not $packageRemovalFailed) {
    & sc.exe delete $serviceName *> $null
    Write-Host "Hub filter and its class registrations were removed." -ForegroundColor Green
} else {
    Write-Host "Class registrations were removed and the emergency switch is off." -ForegroundColor Yellow
    Write-Host "Reboot, then run this script again to remove the package after the driver unloads." -ForegroundColor Yellow
}
