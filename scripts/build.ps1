# Configures and builds LAB X-11: the VST3, the Standalone and the LabX11Render test harness (Release by default).
param(
    [string]$Config = "Release",
    [string]$JucePath = "F:/_DEV/tools/JUCE"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = "F:\_DEV\tools\cmake\cmake-4.4.4-windows-x86_64\bin\cmake.exe" }

& $cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64 "-DLABX11_JUCE_PATH=$JucePath"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build "$root\build" --config $Config --target LabX11_VST3 LabX11_Standalone LabX11Render --parallel
exit $LASTEXITCODE
