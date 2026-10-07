param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [switch]$TrustTestCertificate
)

$ErrorActionPreference = "Stop"

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated PowerShell window."
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $projectRoot "x64\$Configuration"
$driverPackage = Join-Path $outputDirectory "UsbProtectionDriver"
$driverInf = Join-Path $driverPackage "UsbProtection.inf"
$driverSys = Join-Path $driverPackage "UsbProtectionDriver.sys"
$driverCatalog = Join-Path $driverPackage "usbprotection.cat"
$serviceExe = Join-Path $outputDirectory "UsbProtectionService.exe"
$uiExe = Join-Path $outputDirectory "UsbProtectionUI.exe"
$certificate = Join-Path $outputDirectory "UsbProtectionDriver.cer"

foreach ($requiredFile in @(
    $driverInf,
    $driverSys,
    $driverCatalog,
    $serviceExe,
    $uiExe
)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Missing build output: $requiredFile"
    }
}

function Remove-ObsoleteAuthorizationFilter {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ClassGuid
    )

    $filterName = "UsbAuthorizationFilter"
    $classKey = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$ClassGuid"
    $classProperties = Get-ItemProperty -LiteralPath $classKey
    $lowerFiltersProperty = $classProperties.PSObject.Properties["LowerFilters"]
    $currentFilters = @()

    if ($null -ne $lowerFiltersProperty) {
        $currentFilters = @($lowerFiltersProperty.Value) |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    }

    $updatedFilters = @($currentFilters |
        Where-Object { -not [string]::Equals($_, $filterName, [StringComparison]::OrdinalIgnoreCase) })

    if ($updatedFilters.Count -eq 0) {
        Remove-ItemProperty -LiteralPath $classKey -Name "LowerFilters" -ErrorAction SilentlyContinue
    } elseif ($null -eq $lowerFiltersProperty) {
        New-ItemProperty -LiteralPath $classKey `
            -Name "LowerFilters" `
            -PropertyType MultiString `
            -Value $updatedFilters | Out-Null
    } else {
        Set-ItemProperty -LiteralPath $classKey `
            -Name "LowerFilters" `
            -Value $updatedFilters
    }
}

# Remove the retired class-wide filter from packages used in earlier tests.
# Never delete the whole LowerFilters value when unrelated filters are present.
Remove-ObsoleteAuthorizationFilter `
    -ClassGuid "{4D36E97B-E325-11CE-BFC1-08002BE10318}"
Remove-ObsoleteAuthorizationFilter `
    -ClassGuid "{36FC9E60-C465-11CF-8056-444553540000}"
& sc.exe stop UsbAuthorizationFilter *> $null
& sc.exe delete UsbAuthorizationFilter *> $null

$catalogSignature = Get-AuthenticodeSignature -LiteralPath $driverCatalog
if ($null -eq $catalogSignature.SignerCertificate) {
    throw "The driver catalog is unsigned. Select a test certificate in Visual Studio Driver Signing settings, then rebuild."
}

if ($TrustTestCertificate) {
    if (-not (Test-Path -LiteralPath $certificate -PathType Leaf)) {
        throw "Test certificate was not found: $certificate"
    }

    & certutil.exe -addstore -f Root $certificate
    if ($LASTEXITCODE -ne 0) { throw "Could not add the certificate to Root." }
    & certutil.exe -addstore -f TrustedPublisher $certificate
    if ($LASTEXITCODE -ne 0) { throw "Could not add the certificate to TrustedPublisher." }
}

& pnputil.exe /add-driver $driverInf /install
if ($LASTEXITCODE -ne 0) { throw "Driver package installation failed." }

& fltmc.exe load UsbProtection
if ($LASTEXITCODE -ne 0) {
    $loadedFilter = & fltmc.exe filters | Select-String -SimpleMatch "UsbProtection"
    if (-not $loadedFilter) { throw "The UsbProtection minifilter could not be loaded." }
}

$serviceName = "UsbProtectionService"
& sc.exe query $serviceName *> $null
if ($LASTEXITCODE -eq 0) {
    & sc.exe stop $serviceName *> $null
    & sc.exe config $serviceName binPath= $serviceExe start= auto
    if ($LASTEXITCODE -ne 0) { throw "Could not update the Windows service." }
} else {
    & sc.exe create $serviceName binPath= $serviceExe start= auto
    if ($LASTEXITCODE -ne 0) { throw "Could not create the Windows service." }
}

& sc.exe start $serviceName
if ($LASTEXITCODE -ne 0) { throw "Could not start the Windows service." }

Write-Host "USB Protection is installed and running." -ForegroundColor Green
Start-Process -FilePath $uiExe
