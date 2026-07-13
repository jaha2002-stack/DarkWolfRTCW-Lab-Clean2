[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$checks = @(
    @{ Path='src/opengl/opengl.h'; Text='typedef struct glRaytracingEffectsOptions_s' },
    @{ Path='src/opengl/opengl.h'; Text='glRaytracingLightingSetEffectsOptions' },
    @{ Path='src/opengl/gl_d3d12raylight.cpp'; Text='GL_RAYTRACING_LAB_DESCRIPTORS_PER_PASS = 9' },
    @{ Path='src/opengl/gl_d3d12raylight.cpp'; Text='g_glRaytracingLightingHlslParts[]' },
    @{ Path='src/opengl/gl_d3d12raylight.cpp'; Text='static_assert(sizeof(glRaytracingLightingConstants_t) == 480' },
    @{ Path='src/opengl/gl_d3d12raylight.cpp'; Text='float4 ResolveLabPixel(uint2 pixel)' },
    @{ Path='src/opengl/gl_d3d12raylight.cpp'; Text='Filter only the RT/light residual' },
    @{ Path='src/opengl/gl_d3d12raylight.cpp'; Text='Store the un-tonemapped full-resolution raw lighting history' },
    @{ Path='src/opengl/gl_d3d12raylight.cpp'; Text='resource-lifetime race without adding another fence wait' },
    @{ Path='src/renderer/tr_backend.cpp'; Text='Deduplicate per surface, not per brush model' },
    @{ Path='src/renderer/tr_backend.cpp'; Text='RB_BuildDXREffectsOptions' },
    @{ Path='src/renderer/tr_init.cpp'; Text='r_dxrShadows = ri.Cvar_Get' },
    @{ Path='src/renderer/tr_init.cpp'; Text='r_dxrSun = ri.Cvar_Get' },
    @{ Path='src/renderer/tr_init.cpp'; Text='r_dxrDenoiser = ri.Cvar_Get' },
    @{ Path='src/renderer/tr_init.cpp'; Text='r_dxrTemporal = ri.Cvar_Get' },
    @{ Path='src/renderer/tr_init.cpp'; Text='r_dxrNativeResolution = ri.Cvar_Get' },
    @{ Path='src/renderer/tr_local.h'; Text='int cachedFrame = -1;' }
)

foreach ($check in $checks) {
    $path = Join-Path $RepoRoot $check.Path
    if (!(Test-Path -LiteralPath $path)) {
        throw "Missing file: $path"
    }
    $text = [IO.File]::ReadAllText($path)
    if (!$text.Contains($check.Text)) {
        throw "Verification failed: '$($check.Text)' not found in $($check.Path)"
    }
}

$backendPath = Join-Path $RepoRoot 'src/renderer/tr_backend.cpp'
$backend = [IO.File]::ReadAllText($backendPath)
if ($backend.Contains('if (mesh->cachedFrame == currentFrame)')) {
    throw 'The old whole-mesh cache shortcut is still present; it can remove grille/door/torch-bracket shadow casters.'
}
if (!$backend.Contains('if (surf->cachedFrame == currentFrame)')) {
    throw 'The per-surface DXR cache guard is missing.'
}

$headerPath = Join-Path $RepoRoot 'src/opengl/opengl.h'
$header = [IO.File]::ReadAllText($headerPath)
$structMatch = [regex]::Match(
    $header,
    '(?s)typedef struct glRaytracingEffectsOptions_s\s*\{(?<body>.*?)\}\s*glRaytracingEffectsOptions_t;')
if (!$structMatch.Success) {
    throw 'Could not parse glRaytracingEffectsOptions_t.'
}
$scalarCount = [regex]::Matches(
    $structMatch.Groups['body'].Value,
    '(?m)^\s*(?:uint32_t|float)\s+[A-Za-z_][A-Za-z0-9_]*(?:\[(?<array>\d+)\])?\s*;') |
    ForEach-Object {
        if ($_.Groups['array'].Success) { [int]$_.Groups['array'].Value } else { 1 }
    } |
    Measure-Object -Sum
if ($scalarCount.Sum -ne 64) {
    throw "Unexpected RT effects constant layout: expected 64 32-bit scalars, found $($scalarCount.Sum)."
}

$rayPath = Join-Path $RepoRoot 'src/opengl/gl_d3d12raylight.cpp'
$ray = [IO.File]::ReadAllText($rayPath)
$chunkMarker = 'static const char* const g_glRaytracingLightingHlslParts[] ='
$hlslStart = $ray.IndexOf($chunkMarker, [StringComparison]::Ordinal)
if ($hlslStart -lt 0) { throw 'MSVC-safe embedded Lab HLSL chunk array is missing.' }
$chunkEndMarker = 'static std::string glRaytracingBuildLightingHlslSource()'
$hlslEnd = $ray.IndexOf($chunkEndMarker, $hlslStart, [StringComparison]::Ordinal)
if ($hlslEnd -lt 0) { throw 'Embedded Lab HLSL chunk block end marker is missing.' }
$chunkBlock = $ray.Substring($hlslStart, $hlslEnd - $hlslStart)
$chunkMatches = [regex]::Matches($chunkBlock, '(?s)R"DXRHLSL\((.*?)\)DXRHLSL"')
if ($chunkMatches.Count -lt 2) { throw "Expected multiple MSVC-safe HLSL chunks, found $($chunkMatches.Count)." }
$builder = [Text.StringBuilder]::new()
foreach ($match in $chunkMatches) {
    $chunk = $match.Groups[1].Value
    $chunkBytes = [Text.Encoding]::UTF8.GetByteCount($chunk)
    if ($chunkBytes -gt 8000) {
        throw "Embedded HLSL chunk exceeds the conservative 8000-byte limit: $chunkBytes bytes."
    }
    [void]$builder.Append($chunk)
}
$hlsl = $builder.ToString()
if (!$ray.Contains('glRaytracingLightingCompileLibrary(hlslSource.c_str(), hlslSource.size())')) {
    throw 'Runtime HLSL assembly/compile call is missing.'
}

foreach ($pair in @(@('{','}'), @('(',')'), @('[',']'))) {
    $openCount = ($hlsl.ToCharArray() | Where-Object { $_ -eq $pair[0] }).Count
    $closeCount = ($hlsl.ToCharArray() | Where-Object { $_ -eq $pair[1] }).Count
    if ($openCount -ne $closeCount) {
        throw "Unbalanced HLSL delimiters '$($pair[0])$($pair[1])': $openCount versus $closeCount"
    }
}

Write-Host "DXR CleanVisual RT Effects Lab static source verification passed; HLSL chunks: $($chunkMatches.Count), total bytes: $([Text.Encoding]::UTF8.GetByteCount($hlsl))."
Write-Host 'Effect constants: 64 x 32-bit scalars (256 bytes); total lighting cbuffer payload expected: 480 bytes.'
$global:LASTEXITCODE = 0
exit 0
