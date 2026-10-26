# Launches the deployed build in one of the project's portable emulators (default: Azahar).
# Each emulator's <emu>/user folder is its data folder; the app lives on <emu>/user/sdmc/3ds/psnap.
# Usage: powershell -File tools/run_emu.ps1 [-Emu azahar|citra]
param([ValidateSet("azahar", "citra")][string]$Emu = "azahar")

$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root $Emu
$exe = if ($Emu -eq "azahar") { "azahar.exe" } else { "citra-qt.exe" }
$app = Join-Path $dir "user\sdmc\3ds\psnap\psnap-3ds.3dsx"
Start-Process -FilePath (Join-Path $dir $exe) -ArgumentList "`"$app`"" -WorkingDirectory $dir
