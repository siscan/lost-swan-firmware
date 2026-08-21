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
#
# Smart App Control (enforced on this machine) blocks freshly linked unsigned
# binaries by hash, essentially at random: a relink embeds a new timestamp, gets
# a new hash, and usually passes.  ctest reports such a block as BAD_COMMAND /
# "Not Run", never as a test failure, so the retry below only fires on blocked
# binaries - a genuinely failing test still fails the script immediately.

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

for ($attempt = 1; $attempt -le 4; $attempt++) {
    $out = & $ctest --test-dir $build --output-on-failure 2>&1
    $out | ForEach-Object { "$_" }
    if ($LASTEXITCODE -eq 0) { exit 0 }

    $blocked = ($out | Out-String) -match 'BAD_COMMAND|Not Run'
    if (-not $blocked) { exit $LASTEXITCODE }   # a real test failure - report it

    Write-Host "Smart App Control blocked a test binary (attempt $attempt); relinking for a new hash..." -ForegroundColor Yellow
    Remove-Item (Join-Path $build '*.exe') -Force -ErrorAction SilentlyContinue
    & $cmake --build $build
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
Write-Error "Test binaries still blocked by Smart App Control after 4 relinks. See README.md."
exit 1
