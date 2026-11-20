# backup-partitions.ps1 — Windows PowerShell script to backup S10e partitions
param(
    [string]$AdbPath = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe",
    [string]$OutputDir = ".\backup-$(Get-Date -Format 'yyyyMMdd_HHmmss')"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $AdbPath)) {
    # Check if adb is in PATH
    $cmd = Get-Command adb -ErrorAction SilentlyContinue
    if ($cmd) {
        $AdbPath = "adb"
    } else {
        Write-Error "Cannot find adb.exe. Please install Android Platform Tools or pass -AdbPath."
    }
}

Write-Host "=== S10e Partition Backup (Windows) ===" -ForegroundColor Cyan
Write-Host "Output Directory: $OutputDir"
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

# 1. Check recovery state
$state = (& $AdbPath get-state 2>$null)
if ($state -ne "recovery") {
    Write-Error "Device is not in TWRP recovery. Current state: '$state'"
}

# 2. Check root
$uid = (& $AdbPath shell "id -u").Trim()
if ($uid -ne "0") {
    Write-Error "Device is not running as root (uid=$uid)"
}

# 3. Check model
$model = (& $AdbPath shell "getprop ro.boot.em.model").Trim()
Write-Host "Confirmed device: $model in TWRP (root)" -ForegroundColor Green

# Partitions to backup
$partitions = @("efs", "sec_efs", "cpefs", "boot", "dtb", "dtbo", "recovery", "vbmeta")

foreach ($name in $partitions) {
    Write-Host "`nBacking up partition: $name..." -ForegroundColor Yellow
    $dev = (& $AdbPath shell "readlink -f /dev/block/by-name/$name 2>/dev/null").Trim()
    if (-not $dev) {
        Write-Warning "Partition $name not found on device, skipping."
        continue
    }

    $dest = Join-Path $OutputDir "$name.img"
    Write-Host "  Pulling $dev to $dest..."
    & $AdbPath pull "/dev/block/by-name/$name" $dest
    
    if (Test-Path $dest) {
        $hash = (Get-FileHash -Path $dest -Algorithm SHA256).Hash.ToLower()
        Write-Host "  Saved $name.img (SHA256: $hash)" -ForegroundColor Green
        "$hash  $name.img" | Out-File -FilePath (Join-Path $OutputDir "$name.sha256") -Encoding ascii
    } else {
        Write-Error "Failed to pull $name.img"
    }
}

Write-Host "`n=== Backup Complete! ===" -ForegroundColor Green
Write-Host "Keep this backup folder safe! It contains your IMEI and radio calibration data."
