[CmdletBinding()]
param([string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path)
$ErrorActionPreference = 'Stop'
$checks = @(
    @{ Path='src/opengl/gl_d3d12raylight.cpp'; Text='glRaytracingCleanVisualPerformance_t' },
    @{ Path='src/opengl/gl_d3d12raylight.cpp'; Text='Queue submission order provides the GPU dependency' },
    @{ Path='src/opengl/gl_d3d12raylight.cpp'; Text='No low-resolution upscale or block composite is used' },
    @{ Path='src/renderer/tr_backend.cpp'; Text='The same RTCW surface can be visited by several shader stages' },
    @{ Path='src/renderer/tr_backend.cpp'; Text='r_dxrDispatchInterval' },
    @{ Path='src/renderer/tr_init.cpp'; Text='r_dxrAsyncSubmit = ri.Cvar_Get' },
    @{ Path='src/renderer/tr_local.h'; Text='extern cvar_t   *r_dxrCpuSync;' },
    @{ Path='src/opengl/opengl.h'; Text='glRaytracingSetCleanVisualPerformanceOptions' }
)
foreach ($c in $checks) {
    $p = Join-Path $RepoRoot $c.Path
    if (!(Test-Path $p)) { throw "Missing file: $p" }
    $t = [IO.File]::ReadAllText($p)
    if (!$t.Contains($c.Text)) { throw "Verification failed: '$($c.Text)' not found in $($c.Path)" }
}
Write-Host 'DXR Clean Visual Performance source verification passed.'
