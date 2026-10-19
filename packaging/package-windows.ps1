# Packages a Windows Release build of Sidecar.
#
#   pwsh packaging/package-windows.ps1 -BuildDir build -Version 0.1.1-beta.2 -OutDir dist [-Installer]
#
# Writes <OutDir>/Sidecar-<Version>-Windows-x64.zip and, with -Installer (needs Inno
# Setup 6's iscc), <OutDir>/Sidecar-<Version>-Windows-x64-Setup.exe.
param(
    [Parameter(Mandatory = $true)] [string] $BuildDir,
    [Parameter(Mandatory = $true)] [string] $Version,
    [Parameter(Mandatory = $true)] [string] $OutDir,
    [switch] $Installer
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $BuildDir 'LiveDAW_artefacts/Release/Sidecar.exe'
$name = "Sidecar-$Version-Windows-x64"
if (-not (Test-Path $exe)) { throw "no Release build at $exe" }

New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$stageRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("sidecar-pkg-" + [guid]::NewGuid())
$stage = Join-Path $stageRoot $name
New-Item -ItemType Directory -Force $stage | Out-Null

try {
    Copy-Item $exe $stage
    # LICENSE and THIRD_PARTY_NOTICES.md are optional so older commits still package.
    foreach ($f in 'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'CHANGELOG.md') {
        $src = Join-Path $root $f
        if (Test-Path $src) { Copy-Item $src $stage }
    }
    Copy-Item (Join-Path $root 'resources/fonts/OFL.txt') (Join-Path $stage 'FONT-LICENSE.txt')

    $zip = Join-Path $OutDir "$name.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path $stage -DestinationPath $zip
    Write-Host "wrote $zip"

    if ($Installer) {
        $iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
        if (-not $iscc) { $iscc = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe' }
        if (-not (Test-Path $iscc)) { throw "Inno Setup (iscc) not found" }

        $numeric = ($Version -replace '^([0-9]+\.[0-9]+\.[0-9]+).*$', '$1')
        & $iscc /Qp "/DAppVersion=$Version" "/DAppVersionNumeric=$numeric" "/DStageDir=$stage" `
            "/DOutputDir=$OutDir" "/DOutputBaseName=$name-Setup" (Join-Path $root 'packaging/windows/sidecar.iss')
        if ($LASTEXITCODE -ne 0) { throw "iscc failed ($LASTEXITCODE)" }
        Write-Host "wrote $(Join-Path $OutDir "$name-Setup.exe")"
    }
}
finally {
    Remove-Item -Recurse -Force $stageRoot -ErrorAction SilentlyContinue
}
