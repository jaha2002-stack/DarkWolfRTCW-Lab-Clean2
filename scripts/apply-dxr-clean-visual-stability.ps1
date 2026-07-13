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
    return [pscustomobject]@{ Code = $code; Output = ($output -join [Environment]::NewLine) }
}

function Apply-PatchIfNeeded {
    param([string]$PatchPath)
    if (!(Test-Path -LiteralPath $PatchPath)) {
        throw "Patch file not found: $PatchPath"
    }

    $check = Invoke-GitChecked @('apply', '--check', $PatchPath)
    if ($check.Code -eq 0) {
        $apply = Invoke-GitChecked @('apply', '--whitespace=nowarn', $PatchPath)
        if ($apply.Code -ne 0) {
            throw "git apply failed for $PatchPath`n$($apply.Output)"
        }
        Write-Host "Applied patch: $PatchPath"
        return
    }

    $reverseCheck = Invoke-GitChecked @('apply', '--reverse', '--check', $PatchPath)
    if ($reverseCheck.Code -eq 0) {
        Write-Host "Patch already applied, skipping: $PatchPath"
        return
    }

    throw "Patch cannot be applied cleanly: $PatchPath`n`nForward check:`n$($check.Output)`n`nReverse check:`n$($reverseCheck.Output)"
}

Push-Location $RepoRoot
try {
    $patches = @(
        'patches/00-build-fix-Com_Printf.patch',
        'patches/02-dxr-stable-mode.patch',
        'patches/03-pvs-decompressvis-x64.patch',
        'patches/04-dxr-visibility-debug.patch',
        'patches/05-dxr-clean-visual-stability-only.patch'
    )

    foreach ($rel in $patches) {
        Apply-PatchIfNeeded -PatchPath (Join-Path $RepoRoot $rel)
    }
}
finally {
    Pop-Location
}
