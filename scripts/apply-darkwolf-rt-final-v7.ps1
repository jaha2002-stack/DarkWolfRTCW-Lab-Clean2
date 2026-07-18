[CmdletBinding()]
param([string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Install-Snapshot([string]$Relative) {
    $source = Join-Path (Join-Path $RepoRoot 'source-overrides') $Relative
    $destination = Join-Path $RepoRoot $Relative
    if (!(Test-Path -LiteralPath $source)) { throw "Missing v7 source snapshot: $source" }
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    [IO.File]::WriteAllBytes($destination, [IO.File]::ReadAllBytes($source))
    Write-Host "Installed v7 snapshot: $Relative"
}

Push-Location $RepoRoot
try {
    $files = @(
        'src/opengl/gl_d3d12raylight.cpp',
        'src/opengl/opengl.h',
        'src/opengl/gl_d3d12shim.cpp',
        'src/renderer/tr_backend.cpp',
        'src/renderer/tr_bsp.cpp',
        'src/renderer/tr_init.cpp',
        'src/renderer/tr_local.h',
        'src/botlib/be_aas_route.cpp'
    )
    foreach ($file in $files) { Install-Snapshot $file }
    Write-Host 'DarkWolfRTCW Final Stable Visible v7 source installation succeeded.'
}
finally { Pop-Location }
$global:LASTEXITCODE = 0
exit 0
