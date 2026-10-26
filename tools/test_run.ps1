# Automated test run: launches the deployed build in a project emulator for N seconds, sampling
# status.txt, then closes it and prints runtime.log (locked while running) and a PNG screenshot.
# The input script (default tools/autoinput_beach.txt; -Script none for no presses) is copied to the
# SD card as autoinput.txt for this run only and removed afterwards, so normal launches of the
# emulator get no scripted presses. -Pokes copies a pokes.txt the same way.
# Saves: the user's own saves folder is set aside for the run and put back afterwards, so tests never
# touch it. The run starts with no save, or with -Save <file> (e.g. tools/devsave/pokemonsnap_100.bin,
# everything unlocked) as saves/pokemonsnap.bin; -KeepSave <file> copies the save the run ended with.
# Usage: powershell -File tools/test_run.ps1 [-Emu azahar|citra] [-Seconds 30] [-LogLines 80] [-SampleEvery 10]
#        [-Script path|none] [-Pokes path] [-Save path] [-KeepSave path] [-SettingsFile path]
#        [-PhotosFrom dir] [-PhotosTo dir]
# -SettingsFile <file> is used as settings.txt for the run (e.g. "gyro=1"); the user's is put back after.
# -PhotosFrom <dir> starts the run with those photo thumbnails; -PhotosTo <dir> copies the run's out.
param([ValidateSet("azahar", "citra")][string]$Emu = "azahar", [int]$Seconds = 30, [int]$LogLines = 80, [int]$SampleEvery = 10,
      [string]$Script = "", [string]$Pokes = "", [string]$Save = "", [string]$KeepSave = "", [string]$SettingsFile = "", [string]$PhotosFrom = "", [string]$PhotosTo = "")

$root = Split-Path -Parent $PSScriptRoot
$emuDir = Join-Path $root $Emu
$sd = Join-Path $emuDir "user\sdmc\3ds\psnap"

function Stop-ProjectEmulators {
    Get-Process citra-qt, azahar -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root\*" } | Stop-Process -Confirm:$false
}

Stop-ProjectEmulators
Start-Sleep -Seconds 1

# The user plays in this emulator too: their saves, settings and photo thumbnails are set aside for
# the run (*.user) and put back afterwards. A run that was killed before putting them back is
# repaired here first: the *.user copies are the user's.
$saves = Join-Path $sd "saves"
$settings = Join-Path $sd "settings.txt"
$photos = Join-Path $sd "photos"
$autoinput = Join-Path $sd "autoinput.txt"
$pokesFile = Join-Path $sd "pokes.txt"
function Restore-UserFiles {
    foreach ($f in $autoinput, $pokesFile, (Join-Path $sd "snapshots.txt"), (Join-Path $sd "snapshots_full.txt")) {
        Remove-Item -LiteralPath $f -ErrorAction SilentlyContinue
    }
    foreach ($pair in @(@($saves, "$saves.user"), @($photos, "$photos.user"))) {
        if (Test-Path -LiteralPath $pair[1]) {
            Remove-Item -LiteralPath $pair[0] -Recurse -Force -ErrorAction SilentlyContinue
            Move-Item -LiteralPath $pair[1] -Destination $pair[0]
        }
    }
    if (Test-Path -LiteralPath "$settings.user") {
        Move-Item -LiteralPath "$settings.user" -Destination $settings -Force
    } elseif ($script:settingsMoved) {
        Remove-Item -LiteralPath $settings -ErrorAction SilentlyContinue
    }
}
function Set-AsideUserFiles {
    foreach ($dir in $saves, $photos) {
        if (Test-Path -LiteralPath $dir) { Move-Item -LiteralPath $dir -Destination "$dir.user" }
        New-Item -ItemType Directory -Path $dir | Out-Null
        if (-not (Test-Path -LiteralPath "$dir.user")) { New-Item -ItemType Directory -Path "$dir.user" | Out-Null }
    }
    if (Test-Path -LiteralPath $settings) { Move-Item -LiteralPath $settings -Destination "$settings.user" -Force }
    $script:settingsMoved = $true
}
$script:settingsMoved = $false
if ((Test-Path -LiteralPath "$saves.user") -or (Test-Path -LiteralPath "$photos.user") -or (Test-Path -LiteralPath "$settings.user")) {
    "(restoring the user's files left aside by an interrupted run)"
    $script:settingsMoved = $true
    Restore-UserFiles
    $script:settingsMoved = $false
}

foreach ($f in "status.txt", "screenshot.bmp") { Remove-Item -LiteralPath (Join-Path $sd $f) -ErrorAction SilentlyContinue }
$alive = $false
try {
    Set-AsideUserFiles
    if ($Save) { Copy-Item -LiteralPath $Save -Destination (Join-Path $saves "pokemonsnap.bin") }
    if (-not $Script) { $Script = Join-Path $PSScriptRoot "autoinput_beach.txt" }
    if ($Script -ne "none") { Copy-Item -LiteralPath $Script -Destination $autoinput -Force }
    if ($Pokes) { Copy-Item -LiteralPath $Pokes -Destination $pokesFile -Force }
    if ($SettingsFile) { Copy-Item -LiteralPath $SettingsFile -Destination $settings -Force }
    if ($PhotosFrom) { Copy-Item -Path (Join-Path $PhotosFrom "*") -Destination $photos -Force }

    & (Join-Path $PSScriptRoot "run_emu.ps1") -Emu $Emu
    for ($t = $SampleEvery; $t -le $Seconds; $t += $SampleEvery) {
        Start-Sleep -Seconds $SampleEvery
        "t=${t}s " + (Get-Content -LiteralPath (Join-Path $sd "status.txt") -ErrorAction SilentlyContinue)
    }
    $alive = [bool](Get-Process citra-qt, azahar -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root\*" })
} finally {
    Stop-ProjectEmulators
    Start-Sleep -Seconds 2
    if ($KeepSave) {
        Copy-Item -LiteralPath (Join-Path $saves "pokemonsnap.bin") -Destination $KeepSave -Force -ErrorAction SilentlyContinue
        Get-ChildItem -LiteralPath $saves | ForEach-Object { "save file: " + $_.Name + " " + $_.Length }
    }
    if ($PhotosTo) {
        New-Item -ItemType Directory -Force -Path $PhotosTo | Out-Null
        Copy-Item -Path (Join-Path $photos "*") -Destination $PhotosTo -Force -ErrorAction SilentlyContinue
    }
    Restore-UserFiles
}

"alive at end: $alive"
"--- runtime.log"
Get-Content -LiteralPath (Join-Path $sd "runtime.log") -ErrorAction SilentlyContinue | Select-Object -Last $LogLines
"--- screenshot"
$bmp = Join-Path $sd "screenshot.bmp"
if (Test-Path -LiteralPath $bmp) {
    $png = Join-Path $root "tools\tmp\screenshot.png"
    python -c "from PIL import Image; Image.open(r'$bmp').save(r'$png')"
    $png
} else { "(none)" }
