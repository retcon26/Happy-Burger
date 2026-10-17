# Builds release\PSP\GAME\RetconsMod\ (EBOOT.PBP + data\), laid out the way a PSP memory stick or PPSSPP
# expects. Files are hard links to the build's own (no extra disk space); the PS2-only NPC pack and saves are left out.
# Copy the release\PSP folder onto the memory stick / PPSSPP's folder, merging with any PSP folder there.
#
#   .\make_release.ps1          everything (release\)
#   .\make_release.ps1 -Dev     the development build (release_dev\): only the maps in dev_maps.txt, under 500 MB,
#                               quicker to copy to a PSP and to test with. Everything else is the same as a release.
#   .\make_release.ps1 -Update  an update for an install you already have (update\): the game and only the data
#                               that changed since the last release or update folder was made here, or since
#                               -Since DATE. Copy it over the install the same way: it replaces just those files.
#   -Folder NAME                the game's folder on the memory stick (RetconsMod; PSPSandbox for an install from
#                               before 1.10.0, so an update lands on top of it)
param([switch]$Dev, [switch]$Update, [datetime]$Since, [string]$Folder = "RetconsMod", [int]$MaxMB = 500)
$ErrorActionPreference = "Stop"
$psp = $PSScriptRoot
$name = if ($Update) { "update" } elseif ($Dev) { "release_dev" } else { "release" }
$out = Join-Path $psp "$name\PSP\GAME\$Folder"
# When a release, development build or update folder was last made: an update takes what changed after it
$stamp = Join-Path $psp "release.stamp"
$started = Get-Date
if ($Update -and -not $Since) {
    if (-not (Test-Path $stamp)) {
        throw "No release or update folder made here yet to compare with: give -Since DATE (e.g. -Since 2026-10-16)"
    }
    $Since = [datetime]::Parse((Get-Content $stamp -Raw).Trim(), [Globalization.CultureInfo]::InvariantCulture)
}
if (Test-Path (Join-Path $psp $name)) { Remove-Item (Join-Path $psp $name) -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null

# The development build's maps: one name per line (# comments)
$maps = @()
if ($Dev) {
    $maps = Get-Content (Join-Path $psp "dev_maps.txt") | ForEach-Object { ($_ -replace "#.*$", "").Trim() } |
        Where-Object { $_ }
}
# A map's files are name.pmap, name.low.pmap, name.ppak, name.low.ppak and name.pnav (its NPC node graph)
function Wanted($file) {
    if ($Update) { return $file.LastWriteTime -gt $Since }
    if (-not $Dev -or $file.DirectoryName -notlike "*\data\maps") { return $true }
    if ($file.Name -notmatch "\.(pmap|ppak|pnav)$") { return $true } # workshop.txt, the Workshop maps' list
    $base = $file.Name -replace "(\.low)?\.(pmap|ppak|pnav)$", ""
    return $maps -contains $base
}

# Your own saves and test files stay out: a fresh install starts with default settings, no achievements, no
# saved builds (data\saves) and no servers added
$skip = @("npcs.ps2.pnpc", "settings.cfg", "achievements.cfg", "autoload.txt", "servers.cfg", "update.log", "achievements.tmp")
function Link($src, $dst) {
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    try { New-Item -ItemType HardLink -Path $dst -Target $src | Out-Null } catch { Copy-Item $src $dst }
}
Link (Join-Path $psp "EBOOT.PBP") (Join-Path $out "EBOOT.PBP")
Link (Join-Path $psp "CONTROLS.txt") (Join-Path $out "CONTROLS.txt")
$data = Join-Path $psp "data"
Get-ChildItem $data -Recurse -File | Where-Object { $skip -notcontains $_.Name -and $_.DirectoryName -notlike "*\data\saves*" -and (Wanted $_) } | ForEach-Object {
    Link $_.FullName (Join-Path $out ("data" + $_.FullName.Substring($data.Length)))
}
Set-Content $stamp $started.ToString("o", [Globalization.CultureInfo]::InvariantCulture)
$files = Get-ChildItem $out -Recurse -File
$mb = ($files | Measure-Object Length -Sum).Sum / 1MB
"{0} files, {1:N0} MB in {2}" -f $files.Count, $mb, $out
if ($Update) {
    "Changed since {0:yyyy-MM-dd HH:mm}:" -f $Since
    $files | Where-Object { $_.DirectoryName -notlike "*\data\maps" } | ForEach-Object {
        "  {0,-28} {1,8:N0} KB" -f $_.FullName.Substring($out.Length + 1), ($_.Length / 1KB)
    }
    $mapFiles = @($files | Where-Object { $_.DirectoryName -like "*\data\maps" })
    if ($mapFiles.Count) { "  data\maps: {0} files, {1:N0} MB" -f $mapFiles.Count, (($mapFiles | Measure-Object Length -Sum).Sum / 1MB) }
    "Copy update\PSP onto the memory stick, merging it with the PSP folder there, so PSP\GAME\$Folder gets these files."
}
if ($Dev) {
    $have = (Get-ChildItem (Join-Path $out "data\maps") -Filter *.pmap | Where-Object { $_.Name -notlike "*.low.pmap" }).Count
    "{0} of the {1} maps in dev_maps.txt" -f $have, $maps.Count
    if ($have -lt $maps.Count) {
        $missing = $maps | Where-Object { -not (Test-Path (Join-Path $out "data\maps\$_.pmap")) }
        "Not converted: " + ($missing -join ", ")
    }
    if ($mb -gt $MaxMB) { "Over $MaxMB MB: take some maps out of dev_maps.txt" }
}
