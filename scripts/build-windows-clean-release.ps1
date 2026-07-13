[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64')]
    [string]$Platform = 'x64',

    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Convert-ToolsetToVs2022 {
    Get-ChildItem -Path $RepoRoot -Recurse -Filter *.vcxproj |
        Where-Object { $_.FullName -notmatch '\\src\\tools\\radiant\\' } |
        ForEach-Object {
            $content = Get-Content -LiteralPath $_.FullName -Raw
            $updated = $content -replace 'v145', 'v143'
            if ($updated -ne $content) {
                Set-Content -LiteralPath $_.FullName -Value $updated -Encoding UTF8
                Write-Host "Converted toolset in $($_.FullName)"
            }
        }
}

function Disable-RadiantMfcProjectReference {
    $wolfProject = Join-Path $RepoRoot 'src\wolf.vcxproj'
    if (-not (Test-Path -LiteralPath $wolfProject)) { return }

    $content = Get-Content -LiteralPath $wolfProject -Raw
    $pattern = '(?s)\s*<ProjectReference Include="tools\\radiant\\Radiant\.vcxproj">.*?</ProjectReference>'
    $updated = [regex]::Replace($content, $pattern, '')
    if ($updated -ne $content) {
        Set-Content -LiteralPath $wolfProject -Value $updated -Encoding UTF8
        Write-Host 'Removed tools\radiant\Radiant.vcxproj project reference from src\wolf.vcxproj for runtime CI build.'
    }
}

function Find-MSBuild {
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $path = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
        if ($path) { return $path }
    }

    throw 'msbuild.exe was not found.'
}

function Invoke-MSBuildProject {
    param(
        [string]$Project,
        [bool]$BuildProjectReferences = $true
    )

    $msbuild = Find-MSBuild
    $args = @(
        $Project,
        "/p:Configuration=$Configuration",
        "/p:Platform=$Platform",
        '/m',
        '/verbosity:minimal'
    )
    if (-not $BuildProjectReferences) {
        $args += '/p:BuildProjectReferences=false'
    }

    Write-Host "Building $Project"
    & $msbuild @args
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed for $Project with exit code $LASTEXITCODE"
    }
}

function Copy-IfExists {
    param([string]$Path, [string]$Destination)
    if (Test-Path -LiteralPath $Path) {
        Copy-Item -LiteralPath $Path -Destination $Destination -Force
        Write-Host "Copied $Path"
    }
}

Push-Location $RepoRoot
try {
    Convert-ToolsetToVs2022
    Disable-RadiantMfcProjectReference

    Invoke-MSBuildProject 'src\splines\Splines.vcxproj'
    Invoke-MSBuildProject 'src\botlib\botlib.vcxproj'
    Invoke-MSBuildProject 'src\opengl\opengl.vcxproj'
    Invoke-MSBuildProject 'src\renderer\renderer.vcxproj'
    Invoke-MSBuildProject 'src\cgame\cgame.vcxproj'
    Invoke-MSBuildProject 'src\ui\ui.vcxproj'
    Invoke-MSBuildProject 'src\game\game.vcxproj'
    Invoke-MSBuildProject 'src\wolf.vcxproj' $false

    $dist = Join-Path $RepoRoot 'dist'
    if (Test-Path -LiteralPath $dist) { Remove-Item -LiteralPath $dist -Recurse -Force }
    New-Item -ItemType Directory -Path $dist | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $dist 'main') | Out-Null

    foreach ($candidate in @(
        'WolfSP.exe',
        'WolfSP_d.exe',
        'src\Release\WolfSP.exe',
        'src\Debug\WolfSP_d.exe',
        'src\x64\Release\WolfSP.exe',
        'src\x64\Debug\WolfSP_d.exe'
    )) {
        Copy-IfExists (Join-Path $RepoRoot $candidate) $dist
    }

    foreach ($pattern in @('cgamex64*.dll', 'qagamex64*.dll', 'uix64*.dll')) {
        Get-ChildItem -Path (Join-Path $RepoRoot 'main') -Filter $pattern -File -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $dist 'main') -Force
            Write-Host "Copied main/$($_.Name)"
        }
    }

    foreach ($pattern in @('OpenAL32.dll','dxcompiler.dll','dxil.dll','D3D12Core.dll','d3d12SDKLayers.dll')) {
        Get-ChildItem -Path $RepoRoot -Filter $pattern -File -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $dist -Force
            Write-Host "Copied $($_.Name)"
        }
    }

    $batDir = Join-Path $RepoRoot 'test-bats'
    if (Test-Path -LiteralPath $batDir) {
        Get-ChildItem -Path $batDir -Filter '*.bat' -File | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $dist -Force
            Write-Host "Copied BAT $($_.Name)"
        }
        if (Test-Path -LiteralPath (Join-Path $batDir 'README_CLEAN_VISUAL_RU.txt')) {
            Copy-Item -LiteralPath (Join-Path $batDir 'README_CLEAN_VISUAL_RU.txt') -Destination (Join-Path $dist 'README_CLEAN_VISUAL_RU.txt') -Force
        }
    }

    $cfgDir = Join-Path $RepoRoot 'main'
    foreach ($cfg in @('dxr_clean_visual_stable.cfg','dxr_clean_visual_original_risky.cfg','dxr_reset_safe.cfg')) {
        $src = Join-Path $cfgDir $cfg
        if (Test-Path -LiteralPath $src) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $dist 'main') -Force
            Write-Host "Copied main/$cfg"
        }
    }

    $readme = @'
DarkWolf RTCW DXR Clean Visual Stability runtime artifact
=========================================================

This artifact is intentionally based on the older Clean Release visual path.
It does not use the later half-res / smooth / material-lite presets that changed the look.

Main launcher:
  RUN_CLEAN_VISUAL_RT_STABLE.bat

Riskier original-looking launcher:
  RUN_CLEAN_VISUAL_RT_ORIGINAL_RISKY.bat

Reset launcher:
  RUN_RESET_SAFE_NO_DXR.bat

Copy the artifact files into your DarkWolf/RTCW game folder and run the BAT files from the game root.
'@
    Set-Content -LiteralPath (Join-Path $dist 'README_RUN_RU.txt') -Value $readme -Encoding UTF8

    Write-Host 'Clean visual runtime package prepared in dist:'
    Get-ChildItem -Path $dist -Recurse | ForEach-Object { Write-Host $_.FullName }
}
finally {
    Pop-Location
}
