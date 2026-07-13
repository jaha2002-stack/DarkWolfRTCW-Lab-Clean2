[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-GitChecked {
    param([string[]]$Arguments)

    $output = & git @Arguments 2>&1
    $code = $LASTEXITCODE
    $global:LASTEXITCODE = 0

    [pscustomobject]@{
        Code = $code
        Output = ($output -join [Environment]::NewLine)
    }
}

function Test-LabMarkers {
    param([string]$Root)

    $checks = @(
        @{ Path='src/opengl/gl_d3d12raylight.cpp'; Text='GL_RAYTRACING_LAB_DESCRIPTORS_PER_PASS = 9' },
        @{ Path='src/opengl/opengl.h'; Text='typedef struct glRaytracingEffectsOptions_s' },
        @{ Path='src/renderer/tr_backend.cpp'; Text='RB_BuildDXREffectsOptions' },
        @{ Path='src/renderer/tr_bsp.cpp'; Text='cachedFrame = -1' },
        @{ Path='src/renderer/tr_init.cpp'; Text='r_dxrShadows = ri.Cvar_Get' },
        @{ Path='src/renderer/tr_local.h'; Text='extern cvar_t   *r_dxrShadows' }
    )

    foreach ($check in $checks) {
        $path = Join-Path $Root $check.Path
        if (!(Test-Path -LiteralPath $path)) { return $false }
        $text = [IO.File]::ReadAllText($path)
        if (!$text.Contains($check.Text)) { return $false }
    }
    return $true
}

function Test-HlslChunkFix {
    param([string]$Root)

    $path = Join-Path $Root 'src/opengl/gl_d3d12raylight.cpp'
    if (!(Test-Path -LiteralPath $path)) { return $false }
    $text = [IO.File]::ReadAllText($path)
    return $text.Contains('g_glRaytracingLightingHlslParts[]') -and
        $text.Contains('glRaytracingBuildLightingHlslSource()') -and
        $text.Contains('glRaytracingLightingCompileLibrary(hlslSource.c_str(), hlslSource.size())')
}

function Install-HlslChunkSourceOverride {
    param([string]$Root)

    $relative = 'src/opengl/gl_d3d12raylight.cpp'
    $source = Join-Path (Join-Path $Root 'source-overrides') $relative
    $destination = Join-Path $Root $relative
    if (!(Test-Path -LiteralPath $source)) {
        throw "Missing MSVC-safe HLSL source snapshot: $source"
    }

    [IO.File]::WriteAllBytes($destination, [IO.File]::ReadAllBytes($source))
    Write-Host "Installed MSVC-safe split-HLSL source snapshot: $relative"

    if (!(Test-HlslChunkFix -Root $Root)) {
        throw 'The split embedded HLSL source snapshot was copied, but its required markers are missing.'
    }
}

function Apply-HlslChunkPatchOrOverride {
    param([string]$Root, [string]$PatchPath)

    if (Test-HlslChunkFix -Root $Root) {
        Write-Host 'MSVC-safe split embedded HLSL is already present; skipping build-fix application.'
        return
    }

    if (Test-Path -LiteralPath $PatchPath) {
        $check = Invoke-GitChecked @('apply', '--check', $PatchPath)
        if ($check.Code -eq 0) {
            $apply = Invoke-GitChecked @('apply', '--whitespace=nowarn', $PatchPath)
            if ($apply.Code -eq 0 -and (Test-HlslChunkFix -Root $Root)) {
                Write-Host "Applied MSVC embedded-HLSL build fix: $PatchPath"
                return
            }
            Write-Warning "Patch 09 could not be installed cleanly; using deterministic source snapshot instead.`n$($apply.Output)"
        }
        else {
            $reverse = Invoke-GitChecked @('apply', '--reverse', '--check', $PatchPath)
            if ($reverse.Code -eq 0 -and (Test-HlslChunkFix -Root $Root)) {
                Write-Host "MSVC embedded-HLSL build fix already applied: $PatchPath"
                return
            }
            Write-Warning "Patch 09 does not match this checkout's exact source context; using deterministic source snapshot."
        }
    }

    Install-HlslChunkSourceOverride -Root $Root
}

function Install-LabSourceOverrides {
    param([string]$Root)

    $overrideRoot = Join-Path $Root 'source-overrides'
    $relativeFiles = @(
        'src/opengl/gl_d3d12raylight.cpp',
        'src/opengl/opengl.h',
        'src/renderer/tr_backend.cpp',
        'src/renderer/tr_bsp.cpp',
        'src/renderer/tr_init.cpp',
        'src/renderer/tr_local.h'
    )

    if (!(Test-Path -LiteralPath $overrideRoot)) {
        throw "RT Effects Lab source-overrides directory is missing: $overrideRoot"
    }

    Write-Host 'Installing deterministic RT Effects Lab source snapshots...'
    foreach ($relative in $relativeFiles) {
        $source = Join-Path $overrideRoot $relative
        $destination = Join-Path $Root $relative
        if (!(Test-Path -LiteralPath $source)) {
            throw "Missing RT Effects Lab source snapshot: $source"
        }
        $destinationDirectory = Split-Path -Parent $destination
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
        [IO.File]::WriteAllBytes($destination, [IO.File]::ReadAllBytes($source))
        Write-Host "Installed source snapshot: $relative"
    }

    if (!(Test-LabMarkers -Root $Root)) {
        throw 'RT Effects Lab source snapshots were copied, but required source markers are still missing.'
    }
}

function Apply-LabPatchOrOverride {
    param([string]$Root, [string]$PatchPath)

    if (!(Test-Path -LiteralPath $PatchPath)) {
        throw "Patch file not found: $PatchPath"
    }

    if (Test-LabMarkers -Root $Root) {
        Write-Host 'RT Effects Lab source markers are already present; skipping patch application.'
        return
    }

    $check = Invoke-GitChecked @('apply', '--check', $PatchPath)
    if ($check.Code -eq 0) {
        $apply = Invoke-GitChecked @('apply', '--whitespace=nowarn', $PatchPath)
        if ($apply.Code -ne 0) {
            Write-Warning "git apply passed validation but failed during application. Falling back to deterministic source snapshots.`n$($apply.Output)"
            Install-LabSourceOverrides -Root $Root
            return
        }
        Write-Host "Applied patch: $PatchPath"
        return
    }

    $reverse = Invoke-GitChecked @('apply', '--reverse', '--check', $PatchPath)
    if ($reverse.Code -eq 0) {
        Write-Host "Patch already applied, skipping: $PatchPath"
        return
    }

    Write-Warning @"
Patch 08 does not match this checkout's exact source context.
This is expected when the repository contains an earlier sequence of DarkWolf kits.
CHECK:
$($check.Output)
REVERSE CHECK:
$($reverse.Output)
Using deterministic RT Effects Lab source snapshots instead.
"@
    Install-LabSourceOverrides -Root $Root
}

Push-Location $RepoRoot
try {
    $raySource = Join-Path $RepoRoot 'src/opengl/gl_d3d12raylight.cpp'
    if (Test-Path -LiteralPath $raySource) {
        $rayText = [IO.File]::ReadAllText($raySource)
        $labAlreadyPresent = $rayText.Contains('GL_RAYTRACING_LAB_DESCRIPTORS_PER_PASS')
        $trueRtExperimentalPresent = $rayText.Contains('RadianceClosestHit') -or $rayText.Contains('GL_RAYTRACING_MAX_MATERIALS')
        if ($trueRtExperimentalPresent -and !$labAlreadyPresent) {
            throw 'An older TrueRT/M2M3 experimental renderer is present. Apply this kit to the stable Clean Visual Performance v2 branch, not on top of M1/M2/M3.'
        }
    }

    $baseApply = Join-Path $RepoRoot 'scripts/apply-dxr-clean-visual-performance.ps1'
    if (!(Test-Path -LiteralPath $baseApply)) {
        throw "Base Performance v2 apply script is missing: $baseApply"
    }

    Write-Host 'Applying/validating the Clean Visual Performance v2 baseline...'
    # The base script intentionally ends with exit 0. Run it in a child
    # PowerShell process so it cannot terminate this Lab apply script.
    $pwshPath = (Get-Process -Id $PID).Path
    & $pwshPath -NoProfile -ExecutionPolicy Bypass -File $baseApply -RepoRoot $RepoRoot
    $baseCode = $LASTEXITCODE
    $global:LASTEXITCODE = 0
    if ($baseCode -ne 0) {
        throw "Base Performance v2 apply script failed with exit code $baseCode"
    }

    Write-Host 'Applying the isolated RT Effects Lab source changes...'
    Apply-LabPatchOrOverride -Root $RepoRoot -PatchPath (Join-Path $RepoRoot 'patches/08-dxr-cleanvisual-rt-effects-lab.patch')

    if (!(Test-LabMarkers -Root $RepoRoot)) {
        throw 'RT Effects Lab source application completed without all required markers.'
    }

    Write-Host 'Applying the MSVC embedded-HLSL size build fix...'
    Apply-HlslChunkPatchOrOverride -Root $RepoRoot -PatchPath (Join-Path $RepoRoot 'patches/09-dxr-lab-msvc-split-embedded-hlsl.patch')
}
finally {
    Pop-Location
}

Write-Host 'DXR CleanVisual RT Effects Lab patch stack applied successfully.'
$global:LASTEXITCODE = 0
exit 0
