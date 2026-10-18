# Installs the built LAB X-11 into the standard VST3 folder as one single .vst3 file (FL Studio 20 reliably scans
# those). Refuses while any FL64.exe runs (an old instance can outlive its window), when the file is in use, and when a
# bundle folder of the same name exists (FL would list the plugin twice).
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$fl = Get-Process -Name FL64, FL -ErrorAction SilentlyContinue
if ($fl) {
    $fl | ForEach-Object { Write-Output ("FL Studio is running: pid {0}, started {1}, window '{2}'" -f $_.Id, $_.StartTime, $_.MainWindowTitle) }
    Write-Error "Close FL Studio (check Task Manager for a windowless FL64.exe) and run this again."
    exit 1
}

$file = "LAB X-11.vst3"
$source = "$root\build\LabX11_artefacts\Release\VST3\$file\Contents\x86_64-win\$file"
if (-not (Test-Path $source -PathType Leaf)) { Write-Error "Build LabX11 first (scripts/build.ps1)" }
$dest = "C:\Program Files\Common Files\VST3\$file"
if (Test-Path $dest -PathType Container) { Write-Error "A bundle folder exists at $dest. Remove it so FL does not list the effect twice." }
if (Test-Path $dest -PathType Leaf) {
    try { $s = [System.IO.File]::Open($dest, 'Open', 'ReadWrite', 'None'); $s.Close() }
    catch { Write-Error "$file is in use (a host has it loaded). Close it and run this again." }
}
Copy-Item $source $dest -Force
$a = (Get-FileHash $source).Hash; $b = (Get-FileHash $dest).Hash
$item = Get-Item $dest
Write-Output ("Installed {0} ({1:N0} bytes, hash {2})" -f $item.FullName, $item.Length, $(if ($a -eq $b) { "matches the build" } else { "MISMATCH" }))
