# Updates Lost World Native to the latest version on GitHub and rebuilds it.
# Started by update.bat (double click). No git needed: it downloads the source
# as a zip, copies over the files that changed and runs build.bat.
#
# What it never touches: anything that is not part of the repository, so the
# ROM (lostwsga.zip, data\), save\ (settings, records), build\, out\,
# LostWorld.exe, third_party\ngx\, third_party\menu\, the Jurassic Park font,
# crosshair*.png and your own textures in textures\ with names of their own.
#
# A repository file you changed yourself (a texture you redrew with the same
# name, an edited source file) is copied to out\update_backup\<date>\ before it
# is replaced. save\update\manifest.txt keeps the hash of every file the last
# update installed: that is how it tells your changes from an old version, and
# how it removes the files a new version no longer has. Without it (the first
# update after a "Download ZIP") every file that differs is backed up.
#
# A folder cloned with git is updated with git pull instead.
#
# Usage: update.bat [-Branch master] [-Check] [-Force] [-NoBuild]
#   -Check    only say whether there is a new version
#   -Force    download and rebuild even if it is already up to date
#   -NoBuild  update the files but do not run build.bat
param(
    [string]$Branch = "master",
    [string]$Repo = "snicolast/LostWorldNative",
    [switch]$Check,
    [switch]$Force,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
$StateDir = Join-Path $Root "save\update"
$ManifestPath = Join-Path $StateDir "manifest.txt"
$VersionPath = Join-Path $StateDir "version.txt"

# Windows PowerShell 5.1 defaults to TLS 1.0, which GitHub refuses
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
# Invoke-WebRequest's progress bar slows big downloads down a lot in 5.1
$ProgressPreference = "SilentlyContinue"
$UserAgent = "LostWorldNative-updater"

function Say([string]$text, [string]$color = "Gray") { Write-Host $text -ForegroundColor $color }

function Get-Sha256([string]$path) {
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Read-Manifest {
    $map = @{}
    if (Test-Path -LiteralPath $ManifestPath) {
        foreach ($line in Get-Content -LiteralPath $ManifestPath) {
            $i = $line.IndexOf(' ')
            if ($i -gt 0) { $map[$line.Substring($i + 1)] = $line.Substring(0, $i) }
        }
    }
    return $map
}

function Test-GameRunning {
    $exe = Join-Path $Root "LostWorld.exe"
    foreach ($p in Get-Process -Name LostWorld -ErrorAction SilentlyContinue) {
        try { if ($p.Path -and ([IO.Path]::GetFullPath($p.Path) -ieq $exe)) { return $true } } catch { return $true }
    }
    return $false
}

function Invoke-Build {
    if ($NoBuild) { Say "Files updated. Run build.bat to rebuild LostWorld.exe." "Yellow"; return 0 }
    while (Test-GameRunning) {
        Say "LostWorld.exe is running: close the game and press Enter to rebuild it." "Yellow"
        [void](Read-Host)
    }
    Say ""
    Say "Rebuilding (build.bat)..." "Cyan"
    # update.bat pauses at the end, so build.bat does not
    $env:LW_NO_PAUSE = "1"
    # Out-Host: to the screen, not into this function's return value
    & (Join-Path $Root "build.bat") | Out-Host
    return $LASTEXITCODE
}

# ---------------------------------------------------------------- git clone
if (Test-Path -LiteralPath (Join-Path $Root ".git")) {
    # git writes its progress to stderr: not an error
    $ErrorActionPreference = "Continue"
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Say "This folder is a git clone but git is not installed or not in PATH." "Red"
        Say "Install git, or download the game again as a ZIP to use the updater without git." "Red"
        exit 1
    }
    $before = (& git rev-parse HEAD).Trim()
    if ($Check) {
        & git fetch --quiet origin $Branch
        if ($LASTEXITCODE -ne 0) { exit 1 }
        $behind = (& git rev-list --count "HEAD..origin/$Branch").Trim()
        if ($behind -eq "0") { Say "Up to date." "Green" } else { Say "$behind new change(s) on GitHub. Run update.bat to install them." "Yellow" }
        exit 0
    }
    Say "Git clone: updating with git pull..." "Cyan"
    & git pull --ff-only origin $Branch
    if ($LASTEXITCODE -ne 0) {
        Say "git pull failed (see above). Your local changes or commits are kept; sort them out with git and run update.bat again." "Red"
        exit 1
    }
    $after = (& git rev-parse HEAD).Trim()
    if ($after -eq $before -and -not $Force -and (Test-Path -LiteralPath (Join-Path $Root "LostWorld.exe"))) {
        Say "Already up to date." "Green"
        exit 0
    }
    if ($after -ne $before) { & git --no-pager log --oneline "$before..$after" }
    exit (Invoke-Build)
}

# ---------------------------------------------------------------- zip update
Say "Checking GitHub for the latest version ($Repo, $Branch)..." "Cyan"
try {
    $commit = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/commits/$Branch" -UserAgent $UserAgent -Headers @{ Accept = "application/vnd.github+json" }
} catch {
    Say "Could not reach GitHub: $($_.Exception.Message)" "Red"
    exit 1
}
$sha = $commit.sha
$date = ([datetime]$commit.commit.committer.date).ToString("yyyy-MM-dd")
$current = $null
if (Test-Path -LiteralPath $VersionPath) { $current = (Get-Content -LiteralPath $VersionPath -TotalCount 1).Trim() }

if ($current -eq $sha -and -not $Force) {
    Say "Up to date (version $($sha.Substring(0, 7)), $date)." "Green"
    exit 0
}
if ($current) {
    # What changed since the installed version, from the commit messages
    try {
        $cmp = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/compare/$current...$sha" -UserAgent $UserAgent
        Say "New version $($sha.Substring(0, 7)) ($date), $($cmp.total_commits) change(s):" "Yellow"
        foreach ($c in ($cmp.commits | Select-Object -Last 20)) {
            Say ("  - " + ($c.commit.message -split "`n")[0])
        }
    } catch {
        Say "New version $($sha.Substring(0, 7)) ($date)." "Yellow"
    }
} else {
    Say "Latest version: $($sha.Substring(0, 7)) ($date). The installed version is unknown (first update)." "Yellow"
}
if ($Check) { Say "Run update.bat to install it." ; exit 0 }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("lw_update_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $zip = Join-Path $tmp "source.zip"
    Say "Downloading the source..." "Cyan"
    Invoke-WebRequest -Uri "https://codeload.github.com/$Repo/zip/$sha" -OutFile $zip -UserAgent $UserAgent -UseBasicParsing
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $src = Join-Path $tmp "src"
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $src)
    $top = Get-ChildItem -LiteralPath $src -Directory | Select-Object -First 1
    if (-not $top -or -not (Test-Path -LiteralPath (Join-Path $top.FullName "build.bat"))) { throw "The download does not look like the game's source." }
    $srcRoot = $top.FullName

    $old = Read-Manifest
    $haveManifest = $old.Count -gt 0
    $new = @{}
    $backup = Join-Path $Root ("out\update_backup\" + (Get-Date -Format "yyyy-MM-dd_HHmmss"))
    $added = 0; $updated = 0; $removed = 0; $backedUp = New-Object System.Collections.Generic.List[string]

    function Save-Backup([string]$rel) {
        $dst = Join-Path $backup $rel
        New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $Root $rel) -Destination $dst -Force
        $backedUp.Add($rel)
    }

    $prefix = $srcRoot.Length + 1
    foreach ($f in Get-ChildItem -LiteralPath $srcRoot -Recurse -File -Force) {
        $rel = $f.FullName.Substring($prefix)
        $hash = Get-Sha256 $f.FullName
        $new[$rel] = $hash
        $dst = Join-Path $Root $rel
        if (Test-Path -LiteralPath $dst -PathType Leaf) {
            $local = Get-Sha256 $dst
            if ($local -eq $hash) { continue }
            # Changed here since the last update (or no record of it): keep a copy
            if (-not $haveManifest -or -not $old.ContainsKey($rel) -or $old[$rel] -ne $local) { Save-Backup $rel }
            $updated++
        } else {
            New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
            $added++
        }
        Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
    }

    # Files the previous version had and this one does not: removed (backed up
    # first if they were changed here)
    foreach ($rel in @($old.Keys)) {
        if ($new.ContainsKey($rel)) { continue }
        $path = Join-Path $Root $rel
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        if ((Get-Sha256 $path) -ne $old[$rel]) { Save-Backup $rel }
        Remove-Item -LiteralPath $path -Force
        $removed++
    }

    New-Item -ItemType Directory -Path $StateDir -Force | Out-Null
    $lines = foreach ($k in ($new.Keys | Sort-Object)) { "$($new[$k]) $k" }
    [IO.File]::WriteAllLines($ManifestPath, [string[]]$lines)
    [IO.File]::WriteAllText($VersionPath, "$sha`r`n")

    Say "Files: $added new, $updated updated, $removed removed." "Green"
    if ($backedUp.Count -gt 0) {
        if ($haveManifest) {
            Say "$($backedUp.Count) file(s) you had changed were copied to $backup before being replaced:" "Yellow"
        } else {
            Say "First update: it cannot tell your own changes from an older version, so the $($backedUp.Count) file(s) replaced were copied to $backup :" "Yellow"
        }
        foreach ($b in ($backedUp | Select-Object -First 15)) { Say "  $b" }
        if ($backedUp.Count -gt 15) { Say "  ..." }
    }
} catch {
    Say "Update failed: $($_.Exception.Message)" "Red"
    exit 1
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

exit (Invoke-Build)
