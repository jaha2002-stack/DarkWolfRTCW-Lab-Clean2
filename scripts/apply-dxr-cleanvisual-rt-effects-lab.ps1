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
        throw "Missing source snapshot: $source"
    }

    $destinationDirectory = Split-Path -Parent $destination
    New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    [IO.File]::WriteAllBytes($destination, [IO.File]::ReadAllBytes($source))
    Write-Host "Installed clean-fork source snapshot: $Relative"
}

function Assert-Contains {
    param([string]$Root, [string]$Relative, [string]$Text)

    $path = Join-Path $Root $Relative
    if (!(Test-Path -LiteralPath $path)) {
        throw "Required file is missing after source snapshot install: $Relative"
    }
    $content = [IO.File]::ReadAllText($path)
    if (!$content.Contains($Text)) {
        throw "Required marker missing in $Relative : $Text"
    }
}

Push-Location $RepoRoot
try {
    Write-Host 'Installing deterministic CleanVisual RT Effects Lab source snapshots for clean web fork...'
    Write-Host 'This bypasses fragile line-context git patches, which do not match the current upstream web-fork source layout.'

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

    Write-Host 'Validating required RT Effects Lab markers...'
    Assert-Contains -Root $RepoRoot -Relative 'src/opengl/gl_d3d12raylight.cpp' -Text 'GL_RAYTRACING_LAB_DESCRIPTORS_PER_PASS = 9'
    Assert-Contains -Root $RepoRoot -Relative 'src/opengl/gl_d3d12raylight.cpp' -Text 'g_glRaytracingLightingHlslParts[]'
    Assert-Contains -Root $RepoRoot -Relative 'src/opengl/gl_d3d12raylight.cpp' -Text 'glRaytracingBuildLightingHlslSource()'
    Assert-Contains -Root $RepoRoot -Relative 'src/opengl/opengl.h' -Text 'typedef struct glRaytracingEffectsOptions_s'
    Assert-Contains -Root $RepoRoot -Relative 'src/opengl/gl_d3d12shim.cpp' -Text 'glRaytracingLightingIsInitialized()'
    Assert-Contains -Root $RepoRoot -Relative 'src/opengl/gl_d3d12shim.cpp' -Text 'g_lightingTextureState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE'
    Assert-Contains -Root $RepoRoot -Relative 'src/renderer/tr_backend.cpp' -Text 'RB_BuildDXREffectsOptions'
    Assert-Contains -Root $RepoRoot -Relative 'src/renderer/tr_bsp.cpp' -Text 'cachedFrame = -1'
    Assert-Contains -Root $RepoRoot -Relative 'src/renderer/tr_init.cpp' -Text 'r_dxrShadows = ri.Cvar_Get'
    Assert-Contains -Root $RepoRoot -Relative 'src/renderer/tr_local.h' -Text 'extern cvar_t   *r_dxrShadows'
    Assert-Contains -Root $RepoRoot -Relative 'src/botlib/be_aas_route.cpp' -Text 'AAS_DecompressVis: encountered 0 repeat'

    Write-Host 'DXR CleanVisual RT Effects Lab clean-fork bootstrap source install succeeded.'
}
finally {
    Pop-Location
}

$global:LASTEXITCODE = 0
exit 0
