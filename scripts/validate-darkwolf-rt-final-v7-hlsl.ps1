[CmdletBinding()]
param([string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$sourcePath = Join-Path $RepoRoot 'src/opengl/gl_d3d12raylight.cpp'
$text = [IO.File]::ReadAllText($sourcePath)
$start = $text.IndexOf('static const char* const g_glRaytracingLightingHlslParts[] =', [StringComparison]::Ordinal)
$end = $text.IndexOf('static std::string glRaytracingBuildLightingHlslSource()', $start, [StringComparison]::Ordinal)
if ($start -lt 0 -or $end -lt 0) { throw 'Embedded v7 HLSL block missing.' }
$matches = [regex]::Matches($text.Substring($start, $end - $start), '(?s)R"DXRHLSL\((.*?)\)DXRHLSL"')
$builder = [Text.StringBuilder]::new()
foreach ($match in $matches) {
    $chunk = $match.Groups[1].Value
    $bytes = [Text.Encoding]::UTF8.GetByteCount($chunk)
    if ($bytes -gt 8000) { throw "Embedded HLSL chunk exceeds 8000 bytes: $bytes" }
    [void]$builder.Append($chunk)
}
$hlsl = $builder.ToString()
$tempRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$hlslPath = Join-Path $tempRoot 'darkwolf_rt_final_v7.hlsl'
$dxilPath = Join-Path $tempRoot 'darkwolf_rt_final_v7.dxil'
[IO.File]::WriteAllText($hlslPath, $hlsl, [Text.UTF8Encoding]::new($false))
$dxc = Get-Command dxc.exe -ErrorAction SilentlyContinue
if (!$dxc) {
    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (Test-Path $sdkRoot) {
        $candidate = Get-ChildItem $sdkRoot -Recurse -Filter dxc.exe -File -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\dxc\.exe$' } | Sort-Object FullName -Descending | Select-Object -First 1
        if ($candidate) { $dxc = Get-Command $candidate.FullName -ErrorAction Stop }
    }
}
if (!$dxc) { throw 'dxc.exe not found.' }
& $dxc.Source -T lib_6_3 -O3 -all_resources_bound -Fo $dxilPath $hlslPath
$code = $LASTEXITCODE
$global:LASTEXITCODE = 0
if ($code -ne 0 -or !(Test-Path $dxilPath)) { throw "DXC validation failed: $code" }
Write-Host "Final v7 HLSL validation passed. DXIL: $((Get-Item $dxilPath).Length) bytes."
exit 0
