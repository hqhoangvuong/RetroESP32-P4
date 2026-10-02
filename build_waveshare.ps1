# build_waveshare.ps1 — Build the launcher and all emulator apps for the
# Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 (handheld/LCD), then merge the image.
# Usage: .\build_waveshare.ps1           # Rev3.x silicon (current retail boards)
#        .\build_waveshare.ps1 -Rev1     # older Rev1.x silicon boards
# Uses $env:IDF_PATH and $env:IDF_PYTHON_ENV_PATH if set; auto-detects otherwise.
# Output: firmware_waveshare\*.bin and RetroESP32_P4_Waveshare_v1.bin

param([switch]$Rev1)

$ErrorActionPreference = "Stop"

$ROOT = $PSScriptRoot
. (Join-Path $ROOT 'tools\Resolve-IdfEnv.ps1')
Initialize-IdfEnv

$BINS = Join-Path $ROOT 'firmware_waveshare'
New-Item -ItemType Directory -Path $BINS -Force | Out-Null

# Overlays merged after each project's own sdkconfig.defaults
$overlays = @("$ROOT\launcher\sdkconfig.waveshare.defaults")
if (-not $Rev1) { $overlays += "$ROOT\launcher\sdkconfig.p4rev3.defaults" }
$OVERLAYS = $overlays -join ";"
Write-Host "Silicon profile: $(if ($Rev1) { 'Rev1.x' } else { 'Rev3.x' })" -ForegroundColor Cyan

# ── Build Launcher (Waveshare) ────────────────────────────────
Write-Host "`n=== Building Launcher (Waveshare) ===" -ForegroundColor Cyan
Push-Location "$ROOT\launcher"
# Clean build directory and sdkconfig to avoid a stale board/silicon config
if (Test-Path "build") { Remove-Item -Recurse -Force "build" }
Remove-Item -Force "sdkconfig" -ErrorAction SilentlyContinue
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;$OVERLAYS" build
if ($LASTEXITCODE -ne 0) { Pop-Location; throw "Launcher Waveshare build failed" }
Copy-Item "build\launcher.bin" "$BINS\launcher.bin" -Force
Copy-Item "build\bootloader\bootloader.bin" "$BINS\bootloader.bin" -Force
Copy-Item "build\partition_table\partition-table.bin" "$BINS\partition-table.bin" -Force
Copy-Item "build\ota_data_initial.bin" "$BINS\ota_data_initial.bin" -Force
Pop-Location
Write-Host "Launcher (Waveshare): OK" -ForegroundColor Green

# ── Build Emulator Apps (Waveshare) ───────────────────────────
$apps = @(
    @{ Name = "nes";        Dir = "apps\nes";       Bin = "nes_app.bin" },
    @{ Name = "gb";         Dir = "apps\gb";        Bin = "gb_app.bin" },
    @{ Name = "sms";        Dir = "apps\sms";       Bin = "sms_app.bin" },
    @{ Name = "spectrum";   Dir = "apps\spectrum";  Bin = "spectrum_app.bin" },
    @{ Name = "stella";     Dir = "apps\stella";    Bin = "stella_app.bin" },
    @{ Name = "prosystem";  Dir = "apps\prosystem"; Bin = "prosystem_app.bin" },
    @{ Name = "handy";      Dir = "apps\handy";     Bin = "handy_app.bin" },
    @{ Name = "pce";        Dir = "apps\pce";       Bin = "pce_app.bin" },
    @{ Name = "atari800";   Dir = "apps\atari800";  Bin = "atari800_app.bin" },
    @{ Name = "snes";       Dir = "apps\snes";      Bin = "snes_app.bin" },
    @{ Name = "genesis";    Dir = "apps\genesis";   Bin = "genesis_app.bin" },
    @{ Name = "neogeo";     Dir = "apps\neogeo";    Bin = "neogeo_app.bin" }
)

foreach ($app in $apps) {
    Write-Host "`n=== Building $($app.Name) (Waveshare) ===" -ForegroundColor Cyan
    Push-Location "$ROOT\$($app.Dir)"
    if (Test-Path "build") { Remove-Item -Recurse -Force "build" }
    Remove-Item -Force "sdkconfig" -ErrorAction SilentlyContinue
    if (Test-Path "sdkconfig.defaults") {
        idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;$OVERLAYS" build
    } else {
        idf.py -DSDKCONFIG_DEFAULTS="$OVERLAYS" build
    }
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "$($app.Name) Waveshare build failed" }
    Copy-Item "build\$($app.Bin)" "$BINS\$($app.Bin)" -Force
    Pop-Location
    Write-Host "$($app.Name) (Waveshare): OK" -ForegroundColor Green
}

Write-Host "`n=== All Waveshare builds complete! ===" -ForegroundColor Green
Write-Host "Binaries in: $BINS"
Get-ChildItem $BINS -Filter "*.bin" | Format-Table Name, @{N="Size(KB)";E={[math]::Round($_.Length/1024)}}

& (Join-Path $ROOT 'generate_merged_bin_waveshare.ps1')
