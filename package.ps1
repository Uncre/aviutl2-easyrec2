param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$')]
    [string]$Version,

    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root 'build'
$binary = Join-Path $buildDir 'EasyRec2.aux2'
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist '.stage'
$pluginDir = Join-Path $stage 'Plugin\EasyRec2'

if (-not $SkipBuild) {
    & (Join-Path $root 'build.ps1') -Configuration Release
}
if (-not (Test-Path -LiteralPath $binary)) {
    throw "Built plug-in was not found: $binary"
}

if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null

Copy-Item -LiteralPath $binary -Destination (Join-Path $pluginDir 'EasyRec2.aux2')
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination (Join-Path $pluginDir 'README.md')
Copy-Item -LiteralPath (Join-Path $root 'CHANGELOG.md') -Destination (Join-Path $pluginDir 'CHANGELOG.md')
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $pluginDir 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'third_party\aviutl2_sdk\license.txt') -Destination (Join-Path $pluginDir 'AviUtl2-SDK-LICENSE.txt')

$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$packageIni = @"
[package]
id=EasyRec2
name=簡易録音2
information=簡易録音2 v$Version - AviUtl ExEdit2用 音声録音プラグイン
uninstallSubFolderFile=1
"@
[System.IO.File]::WriteAllText((Join-Path $stage 'package.ini'), $packageIni, $utf8NoBom)

$packageText = (Get-Content -LiteralPath (Join-Path $root 'package\package.txt') -Raw) -replace '簡易録音2 v[^\r\n]+', "簡易録音2 v$Version"
[System.IO.File]::WriteAllText((Join-Path $stage 'package.txt'), $packageText, $utf8NoBom)

New-Item -ItemType Directory -Force -Path $dist | Out-Null
$archive = Join-Path $dist "EasyRec2-v$Version.au2pkg.zip"
if (Test-Path -LiteralPath $archive) {
    Remove-Item -LiteralPath $archive -Force
}
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -CompressionLevel Optimal

$hash = Get-FileHash -LiteralPath $archive -Algorithm SHA256
$checksumLine = '{0}  {1}' -f $hash.Hash.ToLowerInvariant(), (Split-Path -Leaf $archive)
[System.IO.File]::WriteAllText((Join-Path $dist 'SHA256SUMS.txt'), "$checksumLine`n", $utf8NoBom)

Remove-Item -LiteralPath $stage -Recurse -Force
Write-Host "Package: $archive"
Write-Host "SHA-256: $($hash.Hash.ToLowerInvariant())"
