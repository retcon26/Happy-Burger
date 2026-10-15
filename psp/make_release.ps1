# Builds release\PSP\GAME\PSPSandbox\ (EBOOT.PBP + data\), laid out the way a PSP memory stick or PPSSPP
# expects. Files are hard links to the build's own (no extra disk space); the PS2-only NPC pack and saves are left out.
# Copy the release\PSP folder onto the memory stick / PPSSPP's folder, merging with any PSP folder there.
$ErrorActionPreference = "Stop"
$psp = $PSScriptRoot
$out = Join-Path $psp "release\PSP\GAME\PSPSandbox"
if (Test-Path (Join-Path $psp "release")) { Remove-Item (Join-Path $psp "release") -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null

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
Get-ChildItem $data -Recurse -File | Where-Object { $skip -notcontains $_.Name -and $_.DirectoryName -notlike "*\data\saves*" } | ForEach-Object {
    Link $_.FullName (Join-Path $out ("data" + $_.FullName.Substring($data.Length)))
}
$files = Get-ChildItem $out -Recurse -File
"{0} files, {1:N0} MB in {2}" -f $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB), $out
