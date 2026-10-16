# Builds release\PSP\GAME\PSPSandbox\ (EBOOT.PBP + data\), laid out the way a PSP memory stick or PPSSPP
# expects. Files are hard links to the build's own (no extra disk space); the PS2-only NPC pack and saves are left out.
# Copy the release\PSP folder onto the memory stick / PPSSPP's folder, merging with any PSP folder there.
#
#   .\make_release.ps1          everything (release\)
#   .\make_release.ps1 -Dev     the development build (release_dev\): only the maps in dev_maps.txt, under 500 MB,
#                               quicker to copy to a PSP and to test with. Everything else is the same as a release.
param([switch]$Dev, [int]$MaxMB = 500)
$ErrorActionPreference = "Stop"
$psp = $PSScriptRoot
$name = if ($Dev) { "release_dev" } else { "release" }
$out = Join-Path $psp "$name\PSP\GAME\PSPSandbox"
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
    if (-not $Dev -or $file.DirectoryName -notlike "*\data\maps") { return $true }
    $base = $file.Name -replace "(\.low)?\.(pmap|ppak|pnav)$", ""
    return $maps -contains $base
}

# Your own saves and test files stay out: a fresh install starts with default settings, no achievements, no
# saved builds (data\saves) and no servers added
$skip = @("npcs.ps2.pnpc", "settings.cfg", "achievements.cfg", "autoload.txt", "servers.cfg")
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
$files = Get-ChildItem $out -Recurse -File
$mb = ($files | Measure-Object Length -Sum).Sum / 1MB
"{0} files, {1:N0} MB in {2}" -f $files.Count, $mb, $out
if ($Dev) {
    $have = (Get-ChildItem (Join-Path $out "data\maps") -Filter *.pmap | Where-Object { $_.Name -notlike "*.low.pmap" }).Count
    "{0} of the {1} maps in dev_maps.txt" -f $have, $maps.Count
    if ($have -lt $maps.Count) {
        $missing = $maps | Where-Object { -not (Test-Path (Join-Path $out "data\maps\$_.pmap")) }
        "Not converted: " + ($missing -join ", ")
    }
    if ($mb -gt $MaxMB) { "Over $MaxMB MB: take some maps out of dev_maps.txt" }
}
