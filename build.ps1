param(
    [ValidateSet('Release','Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio 2022 with the Desktop development with C++ workload is required.'
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools were not found.' }
$devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$opt = if ($Configuration -eq 'Release') { '/O2 /MT /DNDEBUG' } else { '/Od /MTd /Zi' }
$src = Join-Path $root 'src\plugin.cpp'
$inc = Join-Path $root 'third_party\aviutl2_sdk'
$dll = Join-Path $out 'EasyRec2.aux2'
$cmd = '"{0}" -arch=x64 && cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /permissive- /LD /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX {1} /I"{2}" "{3}" /link /OUT:"{4}" ole32.lib uuid.lib propsys.lib comctl32.lib shell32.lib user32.lib gdi32.lib winmm.lib' -f $devcmd,$opt,$inc,$src,$dll
cmd.exe /d /s /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
Write-Host "Built: $dll"
