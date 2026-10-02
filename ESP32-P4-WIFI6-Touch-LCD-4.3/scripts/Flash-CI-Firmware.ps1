[CmdletBinding()]
param(
    [string]$Port = '',
    [string]$Artifact = '',
    [string]$RunId = '',
    [switch]$ListOnly,
    [switch]$List,
    [switch]$SelfTest,
    [switch]$PreflightOnly,
    [switch]$Preflight
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$python = Get-Command python -ErrorAction SilentlyContinue
$usePyLauncher = $false
if (-not $python) {
    $py = Get-Command py -ErrorAction SilentlyContinue
    if (-not $py) { throw 'Python 3 is required (python or py -3).' }
    $python = $py; $usePyLauncher = $true
}
$arguments = [System.Collections.Generic.List[string]]::new()
if ($SelfTest) {
    [void]$arguments.Add('self-test')
    if ($ListOnly) { [void]$arguments.Add('--catalog-only') }
} elseif ($ListOnly) {
    # Historical alias remains hardware/network-free; use `list` for live CI data.
    [void]$arguments.Add('self-test'); [void]$arguments.Add('--catalog-only')
} elseif ($List) {
    [void]$arguments.Add('list')
} elseif ($PreflightOnly -or $Preflight) {
    [void]$arguments.Add('preflight')
} else {
    [void]$arguments.Add('flash')
}
if ($Port) { [void]$arguments.Add('--port'); [void]$arguments.Add($Port) }
if ($Artifact) { [void]$arguments.Add('--artifact'); [void]$arguments.Add($Artifact) }
if ($RunId) {
    if ($RunId -notmatch '^[1-9]\d*$') { throw 'RunId must be a positive integer.' }
    [void]$arguments.Add('--run-id'); [void]$arguments.Add($RunId)
}
if ($usePyLauncher) { & $python.Source -3 (Join-Path $root 'scripts\ci_firmware.py') @arguments } else { & $python.Source (Join-Path $root 'scripts\ci_firmware.py') @arguments }
exit $LASTEXITCODE
