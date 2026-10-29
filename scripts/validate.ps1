# Runs pluginval against the built LAB X-11 bundle. Strictness 5 is the minimum for host compatibility; 10 adds fuzzing.
param([int]$Strictness = 5)

$root = Split-Path -Parent $PSScriptRoot
$pluginval = (Get-Command pluginval -ErrorAction SilentlyContinue).Source
if (-not $pluginval) { $pluginval = "F:\_DEV\tools\pluginval\pluginval.exe" }

$bundle = "$root\build\LabX11_artefacts\Release\VST3\LAB X-11.vst3"
if (-not (Test-Path $bundle)) { Write-Error "Build LabX11 first (scripts/build.ps1)"; exit 1 }
$log = "$root\build\pluginval-level$Strictness.log"
& $pluginval --strictness-level $Strictness --validate $bundle | Out-File -FilePath $log -Encoding utf8
$code = $LASTEXITCODE
Write-Output ("LAB X-11 strictness {0}: exit {1} (log: {2})" -f $Strictness, $code, $log)
exit $code
