#Requires -RunAsAdministrator
#Requires -Version 5.1

<#
.SYNOPSIS
    Register ClawSpan Hyper-V socket service GUID for vsock port 100.

.DESCRIPTION
    Scheme D requires pre-registering GuestCommunicationServices service GUID:
    00000064-facb-11e6-bd58-64006a7986d3

    This script performs one-time registration in HKLM.
#>

$ErrorActionPreference = "Stop"

$serviceGuid = "00000064-facb-11e6-bd58-64006a7986d3"
$regPath = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Virtualization\GuestCommunicationServices\$serviceGuid"

Write-Host "Registering GCS service GUID for ClawSpan vsock port 100 ..." -ForegroundColor Cyan
New-Item -Path $regPath -Force | Out-Null
Set-ItemProperty -Path $regPath -Name "ElementName" -Value "ClawSpan vsock port 100"

Write-Host "[OK] Registered: $regPath" -ForegroundColor Green
