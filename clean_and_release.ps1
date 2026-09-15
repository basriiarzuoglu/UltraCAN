<#
.SYNOPSIS
    One-shot clean-slate release for UltraCAN v1.0.0.

.DESCRIPTION
    Deletes the old company assets, wipes the git history, rebuilds, packages
    with windeployqt, and publishes a GitHub release.

    *** THIS IS DESTRUCTIVE AND IRREVERSIBLE. ***
    It deletes .git and force-pushes. Every previous commit is gone.

    NOTE ON ORDER. The steps do NOT run in the order they were specified.
    Cleanup, build, deploy and zip all run FIRST, while everything is still
    recoverable; the history is only destroyed once a finished .zip exists on
    disk. Wiping history first and discovering the build is broken afterwards
    leaves you with no history AND no artifact, and nothing to go back to.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\clean_and_release.ps1

.EXAMPLE
    # rebuild already done, skip straight to packaging
    powershell -ExecutionPolicy Bypass -File .\clean_and_release.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [string] $Tag     = 'v1.0.0',
    [string] $Config  = 'Desktop_Qt_6_8_3_MinGW_64_bit-Release',
    [string] $QtBin   = 'C:\Qt\6.8.3\mingw_64\bin',
    [string] $RepoUrl = 'https://github.com/basriiarzuoglu/UltraCAN.git',
    [string] $Account = 'basriiarzuoglu',
    [switch] $SkipBuild,
    [switch] $Force            # skip the typed confirmation
)

$ErrorActionPreference = 'Stop'
function Step($m) { Write-Host "`n==> $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "    $m"   -ForegroundColor DarkGray }
function Die($m)  { Write-Host "`nSTOPPED: $m`n" -ForegroundColor Red; exit 1 }

$RepoRoot = $PSScriptRoot
$Src      = Join-Path $RepoRoot 'UltraCAN'
$Build    = Join-Path $Src "build\$Config"
$Name     = "UltraCAN_${Tag}_Windows"
# Stage + zip live INSIDE build\, which .gitignore already excludes. Putting
# them at the repo root would make `git add .` commit a 60 MB zip.
$Stage    = Join-Path $Build $Name
$Zip      = Join-Path $Build "$Name.zip"

Write-Host @"

  UltraCAN clean-slate release
  repo    $RepoRoot
  tag     $Tag  ->  $RepoUrl
  WARNING this wipes git history and force-pushes.

"@ -ForegroundColor Yellow

# ============================================================ 0. PREFLIGHT ===
Step 'Preflight'

if (-not (Test-Path $Src))   { Die "Project folder not found: $Src" }
if (-not (Test-Path $Build)) { Die "No Release build folder: $Build`n  Configure it in Qt Creator, or pass -Config <name>." }

foreach ($t in 'git','gh','cmake') {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) {
        Die "'$t' is not on PATH.  git: https://git-scm.com  |  gh: https://cli.github.com  |  cmake ships with Qt."
    }
}
$WinDeploy = Join-Path $QtBin 'windeployqt.exe'
if (-not (Test-Path $WinDeploy)) { Die "windeployqt.exe not in $QtBin - pass -QtBin <path>." }

# git identity - `git commit` fails without it, and it would fail AFTER the
# history was already deleted.
$gitName  = (git config user.name)  2>$null
$gitEmail = (git config user.email) 2>$null
if (-not $gitName -or -not $gitEmail) {
    Die "git identity not set. Run:`n  git config --global user.name  ""Basri Arzuoglu""`n  git config --global user.email ""you@example.com"""
}
Ok "git identity: $gitName <$gitEmail>"

# The commit author is written into the very first commit of the new public
# history. An old-employer address there is exactly the trace this release is
# meant to remove, and it cannot be corrected later without another history
# rewrite - so stop now, while it costs ten seconds.
if ($gitEmail -match 'robeff' -or $gitName -match 'robeff') {
    Die @"
git identity still points at the old company: $gitName <$gitEmail>
That address would be permanently recorded as the author of the initial
public commit. Set a personal identity first:

  git config --global user.name  "Basri Arzuoglu"
  git config --global user.email "your.personal@email.com"

(or drop --global to set it for this repository only), then re-run.
"@
}

gh auth status 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { Die 'gh is not authenticated. Run:  gh auth login' }
$who = (gh api user --jq .login) 2>$null
if ($LASTEXITCODE -ne 0 -or -not $who) { Die 'Could not read the gh account. Run:  gh auth login' }
if ($who -ne $Account) { Die "gh is authenticated as '$who', not '$Account'.`n  Run:  gh auth switch  (or gh auth login)" }
Ok "gh account: $who"

# Does the remote repo exist? Creating the release later fails if it does not.
gh repo view "$Account/UltraCAN" 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Die "https://github.com/$Account/UltraCAN does not exist or is not visible to this account.`n  Create it first:  gh repo create $Account/UltraCAN --public"
}
Ok "remote repo reachable"

# Release packages left in the REPO ROOT by an older scripts\release.ps1,
# which staged there instead of under build\. .gitignore does not cover the
# root, so `git add .` would sweep tens of MB of Qt DLLs into the new public
# history. This used to be caught after `git init`, i.e. AFTER the old history
# was already deleted - which left the repo mid-operation. Catch it here, while
# nothing has been touched, and just remove them: they are build outputs, fully
# regenerable, and the live package for this run is built under build\.
$stray = @(Get-ChildItem $RepoRoot -Force -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -like 'UltraCAN_v*_Windows' -or $_.Name -like '*.zip' })
if ($stray) {
    foreach ($s in $stray) {
        $mb = if ($s.PSIsContainer) {
                  (Get-ChildItem $s -Recurse -File | Measure-Object Length -Sum).Sum
              } else { $s.Length }
        Remove-Item $s.FullName -Recurse -Force
        Ok "removed stale package $($s.Name)  ($([math]::Round($mb/1MB,1)) MB)"
    }
} else {
    Ok 'repo root clean of release packages'
}

# ======================================================= 1. FILE CLEANUP =====
Step 'Removing old company assets and unused media'

$targets = @(
    (Join-Path $Src 'assets\icons\robeff_*')
    (Join-Path $Src 'assets\images')
    (Join-Path $Src 'assets\videos')
)
$freed = 0
foreach ($t in $targets) {
    $items = Get-Item $t -ErrorAction SilentlyContinue
    foreach ($i in $items) {
        $bytes = if ($i.PSIsContainer) {
            (Get-ChildItem $i -Recurse -File | Measure-Object Length -Sum).Sum
        } else { $i.Length }
        $freed += $bytes
        Remove-Item $i -Recurse -Force
        Ok "removed $($i.Name)  ($([math]::Round($bytes/1KB)) KB)"
    }
}
Ok "freed $([math]::Round($freed/1MB,1)) MB"

# Nothing in the build references these, but prove it rather than assume.
$leftover = Get-ChildItem $Src -Recurse -File -Filter 'robeff*' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -notlike "*\build\*" }
if ($leftover) { Die "robeff files still present: $($leftover.FullName -join ', ')" }
Ok 'no robeff_* files remain outside build/'

# ============================================================== 2. BUILD =====
if (-not $SkipBuild) {
    Step 'Building Release (required - the version bump and OUTPUT_NAME need it)'
    cmake --build $Build
    if ($LASTEXITCODE -ne 0) { Die 'Build failed. Nothing has been destroyed - fix and re-run.' }
}

# The generated installer script carries PROJECT_VERSION, so it is a cheap
# proof that the 1.0.0 bump actually reached this build rather than a stale one.
#
# Match the #define line, NOT `AppVersion=`. The latter reads
#     AppVersion={#MyAppVersion}
# which is an Inno Setup PREPROCESSOR reference - ISCC expands it when the
# installer is compiled, so CMake never writes a number there and reading it
# yields the literal "{#MyAppVersion}".
$iss = Join-Path $Build 'installer.iss'
if (Test-Path $iss) {
    $m = Select-String -Path $iss -Pattern '^#define\s+MyAppVersion\s+"([^"]+)"' | Select-Object -First 1
    if (-not $m) {
        Write-Host "    (version not readable from installer.iss - skipping that check)" -ForegroundColor DarkYellow
    } else {
        $v = $m.Matches.Groups[1].Value.Trim()
        if ($v -ne $Tag.TrimStart('v')) {
            Die "This build is version '$v' but you are tagging '$Tag'.`
  Either project() in CMakeLists.txt does not match the tag, or the build is stale.`
  Re-run without -SkipBuild, or pass  -Tag v$v"
        }
        Ok "build version: $v  (matches $Tag)"
    }
}

$exe = Join-Path $Build 'UltraCAN.exe'
if (-not (Test-Path $exe)) {
    $alt = Get-ChildItem $Build -Filter '*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    Die "UltraCAN.exe not found in $Build (found '$($alt.Name)').`n  CMakeLists.txt needs  OUTPUT_NAME UltraCAN  in set_target_properties(), then rebuild."
}
Ok "executable: $(Split-Path -Leaf $exe)  ($([math]::Round((Get-Item $exe).Length/1MB,1)) MB)"

# ====================================================== 3. DEPLOY (Qt) =======
Step 'windeployqt'

Remove-Item $Stage, $Zip -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $Stage | Out-Null
Copy-Item $exe $Stage

Push-Location $Src        # so the relative --qmldir qml\ resolves
& $WinDeploy `
    --release `
    --qmldir    (Join-Path $Src 'qml') `
    --qmlimport (Join-Path $Build 'qml') `
    --no-translations `
    --no-system-d3d-compiler `
    --verbose 1 `
    (Join-Path $Stage 'UltraCAN.exe')
$rc = $LASTEXITCODE
Pop-Location
if ($rc -ne 0) { Die 'windeployqt failed.' }

# --qmlimport points at this project's own compiled QML modules so the scanner
# resolves UltraCAN.Theme/.Core instead of warning. Deliberately NOT passing
# --no-opengl-sw: it saves ~20 MB and costs a blank window on any machine with
# no usable GPU driver - an industrial PC, a VM, an RDP session. All three are
# normal places to run a bus analyzer.

# windeployqt only copies the MinGW runtime if the compiler's bin dir happens
# to be on PATH. Derive it from the compiler this build actually used.
#
# The cache entry's TYPE is not fixed: Qt Creator writes
#     CMAKE_CXX_COMPILER:STRING=C:/Qt/Tools/mingw1310_64/bin/g++.exe
# while a plain command-line configure writes :FILEPATH. Matching one of them
# found nothing, and indexing the empty result threw "Cannot index into a null
# array" - so match ANY type, and never index an unchecked match. CMakeCache is
# CRLF, hence the Trim().
$cxxBin = $null
$cache  = Join-Path $Build 'CMakeCache.txt'
if (Test-Path $cache) {
    $m = Select-String -Path $cache -Pattern '^CMAKE_CXX_COMPILER:[A-Za-z]+=(.+)$' | Select-Object -First 1
    if ($m) {
        $candidate = Split-Path -Parent ($m.Matches.Groups[1].Value.Trim())
        if (Test-Path $candidate) { $cxxBin = $candidate }
    }
}
if (-not $cxxBin) {                       # fall back to whatever g++ is on PATH
    $g = Get-Command g++ -ErrorAction SilentlyContinue
    if ($g) { $cxxBin = Split-Path -Parent $g.Source }
}
if (-not $cxxBin) {                       # last resort: Qt's bundled toolchain
    $guess = Get-ChildItem 'C:\Qt\Tools' -Directory -Filter 'mingw*' -ErrorAction SilentlyContinue |
             Sort-Object Name -Descending | Select-Object -First 1
    if ($guess) { $cxxBin = Join-Path $guess.FullName 'bin' }
}

if ($cxxBin) {
    Ok "MinGW runtime from $cxxBin"
    foreach ($pattern in 'libgcc_s_*.dll','libstdc++-*.dll','libwinpthread-*.dll') {
        Get-ChildItem (Join-Path $cxxBin $pattern) -ErrorAction SilentlyContinue |
            ForEach-Object { Copy-Item $_.FullName $Stage -Force; Ok "  + $($_.Name)" }
    }
} else {
    Write-Host '    (MinGW bin directory not found - the package check below will catch it)' -ForegroundColor DarkYellow
}

# Each of these has silently shipped broken before.
foreach ($need in 'platforms\qwindows.dll','canbus','qml\QtQuick','libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll') {
    if (-not (Test-Path (Join-Path $Stage $need))) { Die "Package is missing '$need'." }
}
$plugins = (Get-ChildItem (Join-Path $Stage 'canbus') -Filter '*.dll').Count
$sizeMb  = [math]::Round((Get-ChildItem $Stage -Recurse -File | Measure-Object Length -Sum).Sum/1MB,1)
Ok "package OK - $plugins CAN plugins, $sizeMb MB"

# ================================================================ 4. ZIP =====
Step "Compressing -> $Name.zip"
Compress-Archive -Path "$Stage\*" -DestinationPath $Zip -CompressionLevel Optimal
Ok "$([math]::Round((Get-Item $Zip).Length/1MB,1)) MB"

# ==================================================== 5. POINT OF NO RETURN ==
Write-Host @"

  --------------------------------------------------------------------
   Everything above is done and the .zip exists. The next step DELETES
   your entire git history and force-pushes a single new commit to
     $RepoUrl
   Every existing commit, branch and tag on that remote is destroyed.
  --------------------------------------------------------------------
"@ -ForegroundColor Yellow

if (-not $Force) {
    $answer = Read-Host 'Type NUKE to continue (anything else aborts)'
    if ($answer -cne 'NUKE') {
        Write-Host "`nAborted. The package is still at:`n  $Zip`nHistory untouched.`n" -ForegroundColor Green
        exit 0
    }
}

# ============================================================ 6. GIT NUKE ====
Step 'Wiping git history'
Push-Location $RepoRoot

if (Test-Path (Join-Path $RepoRoot '.git')) {
    # .git holds read-only pack files; -Force is required or the delete fails
    # part-way and leaves an unusable repository.
    Remove-Item (Join-Path $RepoRoot '.git') -Recurse -Force
    Ok 'removed .git'
}

git init -b main            ; if ($LASTEXITCODE -ne 0) { Pop-Location; Die 'git init failed.' }
git add .                   ; if ($LASTEXITCODE -ne 0) { Pop-Location; Die 'git add failed.' }

# Guard: the zip and the deploy folder must NOT be in the commit.
$staged = git diff --cached --name-only
$bad = @($staged | Where-Object { $_ -like '*UltraCAN_v*_Windows*' -or $_ -like '*.zip' })
if ($bad) {
    Pop-Location
    Die @"
A release package is staged for commit and would enter the new history:
$($bad | Select-Object -First 5 | ForEach-Object { "  $_" } | Out-String)
The old .git has already been replaced, but NOTHING has been committed or
pushed - the remote is untouched. To recover, delete the offending paths and
re-run this script; it starts from a clean .git each time:

  Remove-Item '$RepoRoot\UltraCAN_v*_Windows' -Recurse -Force
  Remove-Item '$RepoRoot\*.zip' -Force
  .\clean_and_release.ps1 -SkipBuild
"@
}
Ok "$(($staged | Measure-Object).Count) files staged"

git commit -m "UltraCAN v1.0.0 - Initial Release"
if ($LASTEXITCODE -ne 0) { Pop-Location; Die 'git commit failed.' }

git remote add origin $RepoUrl
git push -f origin main
$rc = $LASTEXITCODE
Pop-Location
if ($rc -ne 0) { Die "git push failed. The local history is now a single commit; fix the remote and re-run:`n  git push -f origin main" }
Ok 'pushed'

# ========================================================= 7. GH RELEASE =====
Step "Creating release $Tag"
Push-Location $RepoRoot
gh release create $Tag $Zip `
    --title "UltraCAN v1.0.0 - Initial Release" `
    --notes "First public release featuring UDS Diagnostics, Sniffer, and Restbus Simulation."
$rc = $LASTEXITCODE
Pop-Location
if ($rc -ne 0) { Die "gh release create failed. The code is pushed; retry just the release with:`n  gh release create $Tag `"$Zip`" --title ""UltraCAN v1.0.0 - Initial Release""" }

Step 'Done'
Write-Host "  Release is live." -ForegroundColor Green
gh release view $Tag --web
