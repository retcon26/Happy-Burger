# Publishes the built EBOOT.PBP as a GitHub release (vX.Y.Z), which is where Options > Updates (src/update.c)
# finds new versions. Only the engine goes up: the game data is built from your own games and never published.
#
# First commit and push the version, and build it (build.sh), then:
#   powershell -File psp/publish_release.ps1
# Needs the GitHub CLI (gh), signed in to an account that can publish to the repo.
# The notes are the version's section of docs/CHANGELOG.md, plus a hidden <!-- data N --> line
# (GAME_DATA_VERSION): a PSP whose data is older doesn't install a release that needs newer data.
param([string]$Repo = "retcon26/psp-sandbox")
$ErrorActionPreference = "Stop"
$psp = $PSScriptRoot
$header = Get-Content -Raw (Join-Path $psp "src\version.h")
$ver = [regex]::Match($header, 'GAME_VERSION "([^"]+)"').Groups[1].Value
$data = [regex]::Match($header, 'GAME_DATA_VERSION (\d+)').Groups[1].Value
if (-not $ver -or -not $data) { throw "Couldn't read GAME_VERSION and GAME_DATA_VERSION from src\version.h" }

# The EBOOT must be this version's build: its version string is stored in it as plain text
$eboot = Join-Path $psp "EBOOT.PBP"
$text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($eboot))
if (-not $text.Contains("$ver`0")) { throw "EBOOT.PBP isn't version ${ver}: build it first (psp/build.sh)" }

# The release is made from what's on GitHub, so the version must be pushed
$branch = (git -C $psp rev-parse --abbrev-ref HEAD).Trim()
git -C $psp fetch -q origin $branch
if ((git -C $psp rev-parse HEAD).Trim() -ne (git -C $psp rev-parse "origin/$branch").Trim()) {
    throw "Push first: the release is tagged on GitHub's $branch, which isn't this commit"
}
if (git -C $psp status --porcelain -- src) { throw "src has uncommitted changes: commit and push them first" }

# Notes: the version's section of the changelog
$log = Get-Content (Join-Path $psp "..\docs\CHANGELOG.md")
$start = [array]::IndexOf($log, "## $ver")
if ($start -lt 0) { throw "docs\CHANGELOG.md has no '## $ver' section" }
$notes = @()
for ($i = $start + 1; $i -lt $log.Count -and $log[$i] -notmatch "^## "; $i++) { $notes += $log[$i] }
$notes = ($notes -join "`n").Trim() + "`n`nOnly the game itself (EBOOT.PBP): Options > Updates on a PSP installs it. " +
    "The game data is built on a PC from your own games; see the README.`n`n<!-- data $data -->`n"
$file = Join-Path ([IO.Path]::GetTempPath()) "retconsmod-notes-$ver.md"
[IO.File]::WriteAllText($file, $notes)

gh release create "v$ver" $eboot --repo $Repo --target $branch --title "Retcon's Mod $ver" --notes-file $file
Remove-Item $file
Write-Host "Published v$ver ($([math]::Round((Get-Item $eboot).Length / 1KB)) KB) to https://github.com/$Repo/releases"
