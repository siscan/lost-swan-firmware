# LOST Swan — host unit tests.  THE documented way to run them in this repo.
#
#   .\test-host.ps1            # configure, build, run
#   .\test-host.ps1 -Clean     # wipe build/host first
#
# Uses the CMake and Ninja that install.ps1 already put under ~/.espressif/tools
# (no separate CMake install) plus a user-scope MinGW-w64 GCC from winget.
# No Visual Studio, nothing needing admin.
#
# Ninja rather than MinGW Makefiles on purpose: the Makefiles generator archives
# objects with `ar` before linking, and Application Control policy blocks ar.exe
# on this machine.  test/host/CMakeLists.txt likewise compiles the pure sources
# straight into each test binary instead of building a static library.

param([switch] $Clean)

# Not 'Stop': cmake, ninja and g++ write to stderr in normal operation, and
# Windows PowerShell 5.1 turns native stderr into ErrorRecords.  Failures are
# caught via $LASTEXITCODE; missing tools throw explicitly below.
$ErrorActionPreference = 'Continue'

function Find-One($pattern, $what) {
    $hit = Get-Item $pattern -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
    if (-not $hit) { throw "$what not found at $pattern. See README.md > Host unit tests." }
    return $hit.FullName
}

$tools = Join-Path $HOME '.espressif\tools'
$cmake = Find-One "$tools\cmake\*\bin\cmake.exe" 'IDF CMake'
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
$ninja = Find-One "$tools\ninja\*\ninja.exe" 'IDF Ninja'
$gxx   = Find-One "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*\mingw64\bin\g++.exe" 'MinGW-w64 g++'

# g++ needs its own bin directory on PATH to find the runtime DLLs it links to.
$env:PATH = "$(Split-Path $gxx);$env:PATH"

Write-Host "cmake : $cmake"
Write-Host "ninja : $ninja"
Write-Host "g++   : $gxx"

$root = $PSScriptRoot
$build = Join-Path $root 'build\host'
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

& $cmake -S (Join-Path $root 'test\host') -B $build -G Ninja `
    -DCMAKE_MAKE_PROGRAM="$ninja" -DCMAKE_CXX_COMPILER="$($gxx -replace '\\','/')"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $ctest --test-dir $build --output-on-failure
exit $LASTEXITCODE
