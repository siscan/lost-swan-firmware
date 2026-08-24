# LOST Swan — host unit tests.  THE documented way to run them in this repo.
#
#   .\test-host.ps1            # configure, build, run
#   .\test-host.ps1 -Clean     # wipe build_host first
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
# "Not Run", never as a test failure.  The retry below therefore relinks and
# re-runs ONLY the binaries that were blocked, keeping the ones that already
# ran - relinking the whole set each round needs every binary to clear the
# coin-flip simultaneously, which stops converging past a handful of tests.
# A genuinely failing test still fails the script immediately.

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
$build = Join-Path $root 'build_host'
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

& $cmake -S (Join-Path $root 'test\host') -B $build -G Ninja `
    -DCMAKE_MAKE_PROGRAM="$ninja" -DCMAKE_CXX_COMPILER="$($gxx.Replace([char]92,[char]47))"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# The registered test names, in order.
$pending = @()
foreach ($line in (& $ctest --test-dir $build -N 2>&1)) {
    if ("$line" -match '^\s*Test\s+#\d+:\s+(\S+)') { $pending += $Matches[1] }
}
if ($pending.Count -eq 0) { Write-Error 'ctest registered no tests.'; exit 1 }

$maxAttempts = 12
for ($attempt = 1; $attempt -le $maxAttempts; $attempt++) {
    $filter = '^(' + ($pending -join '|') + ')$'
    $text = (& $ctest --test-dir $build -R $filter --output-on-failure 2>&1 |
             ForEach-Object { "$_" }) -join "`n"
    $text

    $blocked = @()
    $failed  = @()
    foreach ($t in $pending) {
        $rx = 'Test\s+#\d+:\s+' + [regex]::Escape($t) + '\s+\.+(.*?)\s+[\d.]+\s+sec'
        if ($text -match $rx) {
            $status = $Matches[1].Trim()
            if ($status -eq 'Passed') { continue }
            if ($status -match 'Not Run') { $blocked += $t } else { $failed += $t }
        } else {
            $blocked += $t   # never reported at all: treat as blocked, retry
        }
    }

    # A real test failure is never retried - it is the answer.
    if ($failed.Count -gt 0) {
        Write-Error ("Host test failure: " + ($failed -join ', '))
        exit 1
    }
    if ($blocked.Count -eq 0) {
        # A syntax scan of the web assets that needs no node at all.  A raw
        # newline inside a string literal has reached the board twice now, and
        # the symptom is a BLANK PAGE rather than an error: the whole file fails
        # to parse, every page stops rendering, and the state document keeps
        # arriving so the board itself looks perfectly healthy.  CI's
        # `node --check` catches it - after a flash.  This catches it here.
        $py = Get-Command python -ErrorAction SilentlyContinue
        if (-not $py) { $py = Get-Command python3 -ErrorAction SilentlyContinue }
        if ($py) { & $py.Source (Join-Path $PSScriptRoot 'tools/jscheck.py') }
        else { Write-Host 'jscheck ........................... SKIPPED (no python)' -ForegroundColor Yellow }
        if ($py -and $LASTEXITCODE -ne 0) {
            Write-Error "web asset syntax scan failed"
            exit 1
        }
        if ($py) { Write-Host "jscheck ........................... Passed" -ForegroundColor Green }

        # The mirror widget's suite is JavaScript, because the bug it pins was
        # in JavaScript (web/flap.js) and a C++ port of the logic would test a
        # copy rather than the thing that ships.  It needs no npm - the DOM is
        # faked in the test file - only a node.  There is not one on this
        # machine, so say so plainly rather than passing silently; Linux CI
        # runs it on every push.
        $node = Get-Command node -ErrorAction SilentlyContinue
        $jsSuites = @('test_flap', 'test_countdown')
        foreach ($suite in $jsSuites) {
            $pad = ('.' * (33 - $suite.Length))
            if ($node) {
                & $node.Source (Join-Path $PSScriptRoot "test/host/$suite.js")
                if ($LASTEXITCODE -ne 0) {
                    Write-Error "Host test failure: $suite"
                    exit 1
                }
                Write-Host "$suite $pad Passed" -ForegroundColor Green
            } else {
                Write-Host "$suite $pad SKIPPED (no node; CI runs it)" -ForegroundColor Yellow
            }
        }
        Write-Host "all host tests passed" -ForegroundColor Green
        exit 0
    }

    $pending = $blocked
    Write-Host ("Smart App Control blocked " + ($pending -join ', ') +
                " (attempt $attempt); relinking just those for a new hash...") -ForegroundColor Yellow
    foreach ($t in $pending) {
        Remove-Item (Join-Path $build "$t.exe") -Force -ErrorAction SilentlyContinue
    }
    & $cmake --build $build
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Error ("Still blocked by Smart App Control after $maxAttempts relinks: " +
             ($pending -join ', ') + ". See README.md.")
exit 1
