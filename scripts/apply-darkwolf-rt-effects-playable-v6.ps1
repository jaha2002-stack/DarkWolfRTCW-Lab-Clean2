[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Install-SourceSnapshot {
    param([string]$Root, [string]$Relative)

    $source = Join-Path (Join-Path $Root 'source-overrides') $Relative
    $destination = Join-Path $Root $Relative
    if (!(Test-Path -LiteralPath $source)) {
        throw "Missing Playable v6 source snapshot: $source"
    }

    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    [IO.File]::WriteAllBytes($destination, [IO.File]::ReadAllBytes($source))
    Write-Host "Installed Playable v6 source snapshot: $Relative"
}

function Assert-Contains {
    param([string]$Root, [string]$Relative, [string]$Text)

    $path = Join-Path $Root $Relative
    if (!(Test-Path -LiteralPath $path)) {
        throw "Required file is missing: $Relative"
    }
    $content = [IO.File]::ReadAllText($path)
    if (!$content.Contains($Text)) {
        throw "Required Playable v6 marker missing in $Relative : $Text"
    }
}

Push-Location $RepoRoot
try {
    Write-Host 'Installing deterministic DarkWolfRTCW RT Effects Playable v6 source snapshots...'

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

    foreach ($relative in $files) {
        Install-SourceSnapshot -Root $RepoRoot -Relative $relative
    }

    Assert-Contains -Root $RepoRoot -Relative 'src/opengl/gl_d3d12raylight.cpp' -Text 'DXR Playable v6 effects constants must stay cbuffer-compatible'
    Assert-Contains -Root $RepoRoot -Relative 'src/opengl/gl_d3d12raylight.cpp' -Text 'glRaytracingLightingSelectLights'
    Assert-Contains -Root $RepoRoot -Relative 'src/opengl/gl_d3d12raylight.cpp' -Text 'ApplyRadianceGuard'
    Assert-Contains -Root $RepoRoot -Relative 'src/opengl/gl_d3d12raylight.cpp' -Text 'gTemporalPositionThreshold'
    Assert-Contains -Root $RepoRoot -Relative 'src/opengl/opengl.h' -Text 'lightSelectionHysteresis'
    Assert-Contains -Root $RepoRoot -Relative 'src/renderer/tr_backend.cpp' -Text 'The camera-attached fallback light is a diagnostics-only tool'
    Assert-Contains -Root $RepoRoot -Relative 'src/renderer/tr_init.cpp' -Text 'r_dxrRadianceClamp = ri.Cvar_Get'
    Assert-Contains -Root $RepoRoot -Relative 'src/renderer/tr_local.h' -Text 'extern cvar_t   *r_dxrTemporalMaxFrames'

    Write-Host 'Playable v6 source installation succeeded.'
}
finally {
    Pop-Location
}

$global:LASTEXITCODE = 0
exit 0
