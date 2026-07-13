[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Validation guard only; the real compiler check is performed below by DXC.
$MaxEmbeddedHlslChunkBytes = 12000

$sourcePath = Join-Path $RepoRoot 'src/opengl/gl_d3d12raylight.cpp'
if (!(Test-Path -LiteralPath $sourcePath)) {
    throw "Source file not found: $sourcePath"
}

$text = [IO.File]::ReadAllText($sourcePath)
$chunkMarker = 'static const char* const g_glRaytracingLightingHlslParts[] ='
$chunkStart = $text.IndexOf($chunkMarker, [StringComparison]::Ordinal)
if ($chunkStart -lt 0) { throw 'Embedded Playable v6 HLSL chunk array was not found.' }
$chunkEndMarker = 'static std::string glRaytracingBuildLightingHlslSource()'
$chunkEnd = $text.IndexOf($chunkEndMarker, $chunkStart, [StringComparison]::Ordinal)
if ($chunkEnd -lt 0) { throw 'Embedded Playable v6 HLSL chunk block end marker was not found.' }

$chunkBlock = $text.Substring($chunkStart, $chunkEnd - $chunkStart)
$chunkMatches = [regex]::Matches($chunkBlock, '(?s)R"DXRHLSL\((.*?)\)DXRHLSL"')
if ($chunkMatches.Count -lt 2) { throw 'No MSVC-safe embedded HLSL chunks were found.' }

$builder = [Text.StringBuilder]::new()
foreach ($match in $chunkMatches) {
    $chunk = $match.Groups[1].Value
    $chunkBytes = [Text.Encoding]::UTF8.GetByteCount($chunk)
    if ($chunkBytes -gt $MaxEmbeddedHlslChunkBytes) {
        throw "Embedded HLSL chunk exceeds the validation guard of $MaxEmbeddedHlslChunkBytes bytes: $chunkBytes bytes."
    }
    [void]$builder.Append($chunk)
}
$hlsl = $builder.ToString()

$tempRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$hlslPath = Join-Path $tempRoot 'darkwolf_rteffects_playable_v6.hlsl'
$dxilPath = Join-Path $tempRoot 'darkwolf_rteffects_playable_v6.dxil'
[IO.File]::WriteAllText($hlslPath, $hlsl, [Text.UTF8Encoding]::new($false))

$dxc = Get-Command dxc.exe -ErrorAction SilentlyContinue
if (!$dxc) {
    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (Test-Path -LiteralPath $sdkRoot) {
        $candidate = Get-ChildItem -Path $sdkRoot -Recurse -Filter dxc.exe -File -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\dxc\.exe$' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($candidate) { $dxc = Get-Command $candidate.FullName -ErrorAction Stop }
    }
}
if (!$dxc) { throw 'dxc.exe was not found; Playable v6 embedded HLSL cannot be validated.' }

Write-Host "Validating $($chunkMatches.Count) HLSL chunks ($([Text.Encoding]::UTF8.GetByteCount($hlsl)) bytes) with $($dxc.Source)"
& $dxc.Source -T lib_6_3 -O3 -all_resources_bound -Fo $dxilPath $hlslPath
$code = $LASTEXITCODE
$global:LASTEXITCODE = 0
if ($code -ne 0) { throw "DXC validation failed with exit code $code" }
if (!(Test-Path -LiteralPath $dxilPath)) { throw 'DXC returned success but no DXIL file was produced.' }

Write-Host "Playable v6 HLSL validation passed. DXIL size: $((Get-Item $dxilPath).Length) bytes."
exit 0
