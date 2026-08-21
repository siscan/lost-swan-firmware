# LOST Swan — build wrapper.  THE documented way to run idf.py in this repo.
#
# Sources ESP-IDF into this shell and forwards everything to idf.py, so the
# activation step can never be forgotten or half-applied.  There is no Bash
# equivalent: ESP-IDF dropped MSys/Mingw support at v4.0 and install.sh/export.sh
# refuse to run here, so Git Bash callers must go through PowerShell.
#
#   .\build.ps1                      # same as: idf.py build
#   .\build.ps1 set-target esp32c5
#   .\build.ps1 -p COM5 flash monitor
#   .\build.ps1 -B build-xiao -DSWAN_BOARD=xiao build
#
# No param() block on purpose: it keeps PowerShell from claiming idf.py's own
# flags (-p, -B) as parameters of this script.

# Deliberately NOT 'Stop': export.ps1 and idf.py write progress to stderr, and
# Windows PowerShell 5.1 wraps native stderr in ErrorRecords - with 'Stop' that
# aborts the build on ordinary output.  Failures are caught via $LASTEXITCODE.
$ErrorActionPreference = 'Continue'

# winget installs Python user-scope; a shell started before that still has the
# Windows Store stub first on PATH, which is not a usable interpreter.
$python = "$env:LOCALAPPDATA\Programs\Python\Python313"
if (Test-Path $python) { $env:PATH = "$python;$python\Scripts;$env:PATH" }

$export = Join-Path $HOME 'esp\esp-idf\export.ps1'
if (-not (Test-Path $export)) {
    Write-Error "ESP-IDF not found at $export. See README.md > Activating the toolchain."
    exit 1
}
. $export *> $null

# @() on both sides matters: a one-element array unwraps to a bare string, and
# splatting a string iterates its characters (".\build.ps1 build" became -b -u -i...).
$idfArgs = @($args)
if ($idfArgs.Count -eq 0) { $idfArgs = @('build') }
Write-Host "idf.py $($idfArgs -join ' ')" -ForegroundColor Cyan
idf.py @idfArgs
exit $LASTEXITCODE
