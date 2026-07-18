[CmdletBinding()]
param([string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-Text([string]$Relative) {
    $path = Join-Path $RepoRoot $Relative
    if (!(Test-Path -LiteralPath $path)) { throw "Missing file: $Relative" }
    return [IO.File]::ReadAllText($path)
}
function Assert-Contains([string]$Relative, [string]$Needle) {
    if (!(Read-Text $Relative).Contains($Needle)) { throw "Missing marker '$Needle' in $Relative" }
}
function Assert-Same([string]$Relative) {
    $source = Join-Path (Join-Path $RepoRoot 'source-overrides') $Relative
    $destination = Join-Path $RepoRoot $Relative
    if (!(Test-Path $source) -or !(Test-Path $destination)) { throw "Snapshot file missing: $Relative" }
    if ((Get-FileHash $source -Algorithm SHA256).Hash -ne (Get-FileHash $destination -Algorithm SHA256).Hash) {
        throw "Applied source differs from v7 snapshot: $Relative"
    }
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
foreach ($file in $sourceFiles) { Assert-Same $file }

Assert-Contains 'src/opengl/gl_d3d12raylight.cpp' 'LegacyVisibleGameplayComposite'
Assert-Contains 'src/opengl/gl_d3d12raylight.cpp' 'Final v7 gameplay composite'
Assert-Contains 'src/opengl/gl_d3d12raylight.cpp' 'log2f(1.0f + rawIntensity) * 0.55f'
Assert-Contains 'src/opengl/gl_d3d12raylight.cpp' 'DXR v7 CPU CB: composite=clean-release-visible'
Assert-Contains 'src/opengl/gl_d3d12raylight.cpp' 'uint geoFlag = (uint)round(max(positionSample.w, 0.0))'
Assert-Contains 'src/opengl/gl_d3d12raylight.cpp' 'float surfaceRoughness = saturate(normalSample.w)'
Assert-Contains 'src/renderer/tr_backend.cpp' 'The original Clean Release image used this warm camera-side fill light.'
Assert-Contains 'src/renderer/tr_backend.cpp' 'r_dxrAsyncSubmit ? r_dxrAsyncSubmit->integer : 0'
Assert-Contains 'src/renderer/tr_init.cpp' 'r_dxrPointLightIntensityCap = ri.Cvar_Get( "r_dxrPointLightIntensityCap", "8.00"'
Assert-Contains 'src/renderer/tr_init.cpp' 'r_dxrRectLightIntensityCap = ri.Cvar_Get( "r_dxrRectLightIntensityCap", "4.50"'

$ray = Read-Text 'src/opengl/gl_d3d12raylight.cpp'
$chunkStart = $ray.IndexOf('static const char* const g_glRaytracingLightingHlslParts[] =', [StringComparison]::Ordinal)
$chunkEnd = $ray.IndexOf('static std::string glRaytracingBuildLightingHlslSource()', $chunkStart, [StringComparison]::Ordinal)
if ($chunkStart -lt 0 -or $chunkEnd -lt 0) { throw 'Embedded HLSL block missing.' }
$chunks = [regex]::Matches($ray.Substring($chunkStart, $chunkEnd - $chunkStart), '(?s)R"DXRHLSL\((.*?)\)DXRHLSL"')
if ($chunks.Count -lt 2) { throw 'Expected multiple embedded HLSL chunks.' }
$builder = [Text.StringBuilder]::new()
foreach ($match in $chunks) {
    $bytes = [Text.Encoding]::UTF8.GetByteCount($match.Groups[1].Value)
    if ($bytes -gt 8000) { throw "Embedded HLSL chunk exceeds 8000 bytes: $bytes" }
    [void]$builder.Append($match.Groups[1].Value)
}
$hlsl = $builder.ToString()
foreach ($marker in @('LegacyVisibleGameplayComposite','visibleShadow','visibleAO','ComputeReflection','ComputeGI','ComputeSpecular','ResolveLabPixel')) {
    if (!$hlsl.Contains($marker)) { throw "HLSL marker missing: $marker" }
}
foreach ($pair in @(@('{','}'),@('(',')'),@('[',']'))) {
    $open = ($hlsl.ToCharArray() | Where-Object { $_ -eq $pair[0] }).Count
    $close = ($hlsl.ToCharArray() | Where-Object { $_ -eq $pair[1] }).Count
    if ($open -ne $close) { throw "Unbalanced HLSL delimiters $($pair[0])$($pair[1]): $open / $close" }
}

$profiles = @(
    'main/dxr_v7_final_stable.cfg',
    'main/dxr_v7_screenshot_look.cfg',
    'main/dxr_v7_real_lights_only.cfg',
    'main/dxr_v7_debug.cfg',
    'test-bats/RUN_RT_V7_FINAL_STABLE.bat',
    'test-bats/RUN_RT_V7_SCREENSHOT_LOOK.bat',
    'test-bats/RUN_RT_V7_REAL_LIGHTS_ONLY.bat',
    'test-bats/RUN_RT_V7_DEBUG.bat'
)
foreach ($profile in $profiles) {
    $content = Read-Text $profile
    if ($content -match '(?i)\bvid_restart\b') { throw "Forbidden vid_restart in $profile" }
}

$final = Read-Text 'main/dxr_v7_final_stable.cfg'
foreach ($setting in @(
    'set r_dxrAsyncSubmit 0','set r_dxrCpuSync 1','set r_dxrBuildInterval 1','set r_dxrDispatchInterval 1',
    'set r_dxrFallbackLight 1','set r_dxrShadows 1','set r_dxrDynamicLights 1','set r_dxrSun 1',
    'set r_dxrAO 1','set r_dxrReflections 1','set r_dxrSky 1','set r_dxrGI 1','set r_dxrSpecular 1',
    'set r_dxrDenoiser 1','set r_dxrTemporal 0','set r_dxrDebugEffect 0')) {
    if (!$final.Contains($setting)) { throw "Final profile missing: $setting" }
}

$screen = Read-Text 'main/dxr_v7_screenshot_look.cfg'
foreach ($setting in @('set r_dxrFallbackLightIntensity 8.0','set r_dxrAmbientIntensity 1.35','set r_dxrLegacyBlend 0.65','set r_dxrExposure 1.15','set r_dxrShadowBias 0.050')) {
    if (!$screen.Contains($setting)) { throw "Screenshot profile missing original look setting: $setting" }
}

Write-Host "Final Stable Visible v7 static verification passed. HLSL chunks: $($chunks.Count), bytes: $([Text.Encoding]::UTF8.GetByteCount($hlsl))."
$global:LASTEXITCODE = 0
exit 0
