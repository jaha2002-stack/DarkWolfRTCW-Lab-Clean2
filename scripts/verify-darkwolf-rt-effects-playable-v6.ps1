[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-Text([string]$Relative) {
    $path = Join-Path $RepoRoot $Relative
    if (!(Test-Path -LiteralPath $path)) { throw "Missing file: $Relative" }
    return [IO.File]::ReadAllText($path)
}

function Assert-Contains([string]$Relative, [string]$Needle) {
    $text = Read-Text $Relative
    if (!$text.Contains($Needle)) { throw "Verification failed: '$Needle' not found in $Relative" }
}

function Assert-Same([string]$Relative) {
    $source = Join-Path (Join-Path $RepoRoot 'source-overrides') $Relative
    $destination = Join-Path $RepoRoot $Relative
    if (!(Test-Path -LiteralPath $source) -or !(Test-Path -LiteralPath $destination)) {
        throw "Snapshot comparison file missing: $Relative"
    }
    $a = (Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash
    $b = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
    if ($a -ne $b) { throw "Applied source differs from deterministic snapshot: $Relative" }
}

$sourceFiles = @(
    'src/opengl/gl_d3d12raylight.cpp',
    'src/opengl/opengl.h',
    'src/opengl/gl_d3d12shim.cpp',
    'src/renderer/tr_backend.cpp',
    'src/renderer/tr_bsp.cpp',
    'src/renderer/tr_init.cpp',
    'src/renderer/tr_local.h',
    'src/botlib/be_aas_route.cpp'
)
foreach ($relative in $sourceFiles) { Assert-Same $relative }

Assert-Contains 'src/opengl/gl_d3d12raylight.cpp' 'static_assert(sizeof(glRaytracingEffectsOptions_t) == 320'
Assert-Contains 'src/opengl/gl_d3d12raylight.cpp' 'static_assert(sizeof(glRaytracingLightingConstants_t) == 544'
Assert-Contains 'src/opengl/gl_d3d12raylight.cpp' 'glRaytracingLightingSelectLights'
Assert-Contains 'src/opengl/gl_d3d12raylight.cpp' 'ApplyRadianceGuard'
Assert-Contains 'src/opengl/gl_d3d12raylight.cpp' 'previously resolved, post-tonemapped frame'
Assert-Contains 'src/opengl/gl_d3d12raylight.cpp' 'glRaytracingLightingGetSelectedLightCount'
Assert-Contains 'src/renderer/tr_backend.cpp' 'diagnostics-only tool'
Assert-Contains 'src/renderer/tr_backend.cpp' 'r_dxrAsyncSubmit ? r_dxrAsyncSubmit->integer : 0'
Assert-Contains 'src/renderer/tr_init.cpp' 'r_dxrAsyncSubmit = ri.Cvar_Get( "r_dxrAsyncSubmit", "0"'
Assert-Contains 'src/renderer/tr_init.cpp' 'r_dxrCpuSync = ri.Cvar_Get( "r_dxrCpuSync", "1"'
Assert-Contains 'src/renderer/tr_init.cpp' 'r_dxrLightSelectionMode = ri.Cvar_Get'
Assert-Contains 'src/renderer/tr_bsp.cpp' 'normalizes them logarithmically'

$header = Read-Text 'src/opengl/opengl.h'
$structMatch = [regex]::Match($header, '(?s)typedef struct glRaytracingEffectsOptions_s\s*\{(?<body>.*?)\}\s*glRaytracingEffectsOptions_t;')
if (!$structMatch.Success) { throw 'Could not parse glRaytracingEffectsOptions_t.' }
$scalarCount = [regex]::Matches(
    $structMatch.Groups['body'].Value,
    '(?m)^\s*(?:uint32_t|float)\s+[A-Za-z_][A-Za-z0-9_]*(?:\[(?<array>\d+)\])?\s*;') |
    ForEach-Object { if ($_.Groups['array'].Success) { [int]$_.Groups['array'].Value } else { 1 } } |
    Measure-Object -Sum
if ($scalarCount.Sum -ne 80) {
    throw "Unexpected Playable v6 effects layout: expected 80 32-bit scalars, found $($scalarCount.Sum)."
}

$ray = Read-Text 'src/opengl/gl_d3d12raylight.cpp'
$chunkStart = $ray.IndexOf('static const char* const g_glRaytracingLightingHlslParts[] =', [StringComparison]::Ordinal)
$chunkEnd = $ray.IndexOf('static std::string glRaytracingBuildLightingHlslSource()', $chunkStart, [StringComparison]::Ordinal)
if ($chunkStart -lt 0 -or $chunkEnd -lt 0) { throw 'Embedded HLSL chunk block is missing.' }
$chunks = [regex]::Matches($ray.Substring($chunkStart, $chunkEnd - $chunkStart), '(?s)R"DXRHLSL\((.*?)\)DXRHLSL"')
if ($chunks.Count -lt 2) { throw 'Expected multiple embedded HLSL chunks.' }
$hlslBuilder = [Text.StringBuilder]::new()
foreach ($match in $chunks) {
    $bytes = [Text.Encoding]::UTF8.GetByteCount($match.Groups[1].Value)
    if ($bytes -gt 8000) { throw "Embedded HLSL chunk exceeds 8000 bytes: $bytes" }
    [void]$hlslBuilder.Append($match.Groups[1].Value)
}
$hlsl = $hlslBuilder.ToString()
foreach ($marker in @('gDirectLightingStrength', 'gRadianceClamp', 'gLightSelectionMode', 'ApplyRadianceGuard', 'ResolveLabPixel')) {
    if (!$hlsl.Contains($marker)) { throw "HLSL marker missing: $marker" }
}
foreach ($pair in @(@('{','}'), @('(',')'), @('[',']'))) {
    $open = ($hlsl.ToCharArray() | Where-Object { $_ -eq $pair[0] }).Count
    $close = ($hlsl.ToCharArray() | Where-Object { $_ -eq $pair[1] }).Count
    if ($open -ne $close) { throw "Unbalanced HLSL delimiters $($pair[0])$($pair[1]): $open / $close" }
}

$requiredProfiles = @(
    'main/dxr_v6_all_balanced.cfg',
    'main/dxr_v6_all_quality.cfg',
    'main/dxr_v6_all_performance.cfg',
    'main/dxr_v6_disable_all.cfg',
    'test-bats/RUN_RT_ALL_BALANCED.bat',
    'test-bats/RUN_RT_ALL_QUALITY.bat',
    'test-bats/RUN_RT_ALL_PERFORMANCE.bat',
    'test-bats/RUN_RT_ALL_BALANCED_ESCAPE1_TEST.bat',
    'test-bats/RUN_RT_DISABLE_V6.bat',
    'test-bats/VERIFY_RT_PLAYABLE_V6_INSTALL.bat'
)
foreach ($relative in $requiredProfiles) {
    $text = Read-Text $relative
    if ($text -match '(?i)\bvid_restart\b') { throw "Playable v6 profile contains forbidden vid_restart: $relative" }
}

$balanced = Read-Text 'main/dxr_v6_all_balanced.cfg'
foreach ($setting in @(
    'set r_dxrFallbackLight 0',
    'set r_dxrAsyncSubmit 0',
    'set r_dxrCpuSync 1',
    'set r_dxrShadows 1',
    'set r_dxrSun 1',
    'set r_dxrDynamicLights 1',
    'set r_dxrAO 1',
    'set r_dxrReflections 1',
    'set r_dxrSky 1',
    'set r_dxrGI 1',
    'set r_dxrSpecular 1',
    'set r_dxrDenoiser 1',
    'set r_dxrTemporal 1')) {
    if (!$balanced.Contains($setting)) { throw "Balanced profile is missing: $setting" }
}

Write-Host "Playable v6 static verification passed. Effects constants: 80 scalars / 320 bytes; total cbuffer: 544 bytes."
Write-Host "Embedded HLSL chunks: $($chunks.Count); total UTF-8 bytes: $([Text.Encoding]::UTF8.GetByteCount($hlsl))."
$global:LASTEXITCODE = 0
exit 0
