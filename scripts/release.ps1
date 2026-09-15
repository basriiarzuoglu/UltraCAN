<#
    UltraCAN release script  -  run from anywhere, paths resolve from this file.

        powershell -ExecutionPolicy Bypass -File scripts\release.ps1 -Tag v1.0.1

    Packages the Release build with windeployqt, zips it, and publishes a
    GitHub release with the zip attached.
#>
param(
    [Parameter(Mandatory = $true)] [string] $Tag,
    [string] $Config    = 'Desktop_Qt_6_8_3_MinGW_64_bit-Release',
    [string] $QtBin     = 'C:\Qt\6.8.3\mingw_64\bin',
    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'
function Step($m) { Write-Host "`n==> $m" -ForegroundColor Cyan }
function Die($m)  { Write-Host "`nSTOP: $m" -ForegroundColor Red; exit 1 }

$RepoRoot = Split-Path -Parent $PSScriptRoot          # ...\Third\UltraCAN
$Src      = Join-Path $RepoRoot 'UltraCAN'            # ...\Third\UltraCAN\UltraCAN
$Build    = Join-Path $Src "build\$Config"
# Staged INSIDE build\, never at the repo root. .gitignore covers build\, so
# a package left lying around here cannot be swept into a commit by `git add .`
# - which is exactly what an earlier version of this script caused.
$Stage    = Join-Path $Build "UltraCAN_${Tag}_Windows"
$Zip      = "$Stage.zip"

# ---------------------------------------------------------------- 0. preflight
Step 'Preflight'

if (-not (Test-Path $Build)) { Die "No release build at $Build - configure and build it first." }

foreach ($exe in 'gh', 'cmake') {
    if (-not (Get-Command $exe -ErrorAction SilentlyContinue)) {
        Die "'$exe' is not on PATH. gh: https://cli.github.com/  |  cmake ships with Qt."
    }
}
if (-not (Test-Path (Join-Path $QtBin 'windeployqt.exe'))) {
    Die "windeployqt.exe not found in $QtBin - pass -QtBin <path to Qt bin>."
}

gh auth status
if ($LASTEXITCODE -ne 0) { Die 'gh is not authenticated. Run:  gh auth login' }

# A release tags a COMMIT. If the tree is dirty or unpushed, the tag will point
# at something that is not what you just packaged.
Push-Location $RepoRoot
if ((git status --porcelain).Length -gt 0) {
    git status --short
    Die 'Working tree is dirty. Commit or stash before tagging a release.'
}
git fetch --quiet origin
$ahead = git rev-list --count '@{u}..HEAD' 2>$null
if ($ahead -and [int]$ahead -gt 0) { Die "$ahead commit(s) not pushed. Run: git push" }
if (git tag --list $Tag)           { Die "Tag $Tag already exists locally." }
Pop-Location

# ------------------------------------------------------------------- 1. deploy
# Uses the project's own Deploy target rather than calling windeployqt by hand:
# it copies the exe into deploy\ FIRST (so Qt's DLLs do not scatter through the
# build tree), passes --qmldir AND --qmlimport, and adds the three MinGW runtime
# DLLs that windeployqt only copies if the compiler's bin dir is on PATH.
# Without those the exe does not start on a machine with no Qt.
if (-not $SkipBuild) {
    Step 'Building Release'
    cmake --build $Build
    if ($LASTEXITCODE -ne 0) { Die 'Build failed.' }
}

Step 'Running windeployqt (Deploy target)'
cmake --build $Build --target Deploy
if ($LASTEXITCODE -ne 0) { Die 'Deploy failed.' }

$Deploy = Join-Path $Build 'deploy'

# ----------------------------------------------------------------- 2. stage it
Step 'Staging'
Remove-Item $Stage, $Zip -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item $Deploy $Stage -Recurse

# The exe users double-click must match what the README tells them to run.
$exePath = Join-Path $Stage 'UltraCAN.exe'
if (-not (Test-Path $exePath)) {
    $found = (Get-ChildItem $Stage -Filter '*.exe' | Select-Object -First 1).Name
    Die "Expected UltraCAN.exe in the package but found '$found'. Add OUTPUT_NAME UltraCAN to set_target_properties() in CMakeLists.txt and rebuild."
}

# Cheap sanity checks on the payload - each of these has silently shipped broken before.
foreach ($need in 'platforms\qwindows.dll', 'canbus', 'qml\QtQuick', 'libgcc_s_seh-1.dll') {
    if (-not (Test-Path (Join-Path $Stage $need))) { Die "Package is missing $need - windeployqt did not do its job." }
}
$plugins = (Get-ChildItem (Join-Path $Stage 'canbus') -Filter '*.dll').Count
Write-Host "    ok - $plugins CAN backend plugin(s), $([math]::Round((Get-ChildItem $Stage -Recurse | Measure-Object Length -Sum).Sum/1MB)) MB total"

# -------------------------------------------------------------------- 3. zip it
Step "Compressing -> $(Split-Path -Leaf $Zip)"
Compress-Archive -Path "$Stage\*" -DestinationPath $Zip -CompressionLevel Optimal
Write-Host "    $([math]::Round((Get-Item $Zip).Length/1MB)) MB"

# ---------------------------------------------------------------- 4. publish it
$notes = @"
First public release of **UltraCAN** - a CAN bus analysis, diagnostics and
reverse-engineering workbench built on Qt 6 and C++20.

### Highlights

- **UDS / OBD-II diagnostics** - a full ISO 14229 tester over an ISO-TP
  (ISO 15765-2) transport: Single/First/Consecutive frames with Flow Control,
  block size and STmin, 11-bit and 29-bit addressing. Read DTCs decoded to
  standard notation with their status bits, Clear DTCs behind a confirmation,
  session control, tester present, and a raw-hex request console.
- **Raw Data Analyzer (sniffer)** - a live bit and byte heatmap across every
  arbitration ID, so the fields that move on an undocumented bus are visible
  before you know what any of them are called, then promoted straight to signal
  definitions.
- **Log replay / restbus simulation** - replay a recording onto the physical bus
  with its original inter-frame timing, driven by a dedicated pacer thread, and
  gated behind an explicit arm interlock so nothing reaches the wire by accident.
- Plus: triggered logging with a pre-trigger buffer and pass filters, splittable
  real-time graph plotting with math channels and image export, a DBC editor
  with live decoded values, and a cyclic message sender.

### Install

Download the zip, extract anywhere, run ``UltraCAN.exe``. No installation, no Qt
required. See the README for supported hardware.

> Note: the ISO-TP and replay timing paths are verified by an automated test
> harness but have not yet been validated against a wide range of real ECUs.
> Bug reports from real hardware are very welcome.
"@

Step "Creating release $Tag"
Push-Location $RepoRoot
gh release create $Tag $Zip `
    --title "UltraCAN $Tag - Initial Release" `
    --notes  $notes
$rc = $LASTEXITCODE
Pop-Location
if ($rc -ne 0) { Die 'gh release create failed.' }

Step 'Done'
gh release view $Tag --web
