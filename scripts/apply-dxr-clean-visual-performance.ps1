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

    # git apply --check and --reverse --check are intentionally allowed to
    # return non-zero while the script chooses the correct patch path.
    # Clear the native-process exit code after capturing it; otherwise pwsh
    # can finish the GitHub Actions step with exit code 1 even after every
    # validated fallback edit succeeded.
    $global:LASTEXITCODE = 0

    [pscustomobject]@{ Code = $code; Output = ($output -join [Environment]::NewLine) }
}

function Apply-PatchIfNeeded {
    param([string]$PatchPath)
    if (!(Test-Path -LiteralPath $PatchPath)) { throw "Patch file not found: $PatchPath" }

    $check = Invoke-GitChecked @('apply', '--check', $PatchPath)
    if ($check.Code -eq 0) {
        $apply = Invoke-GitChecked @('apply', '--whitespace=nowarn', $PatchPath)
        if ($apply.Code -ne 0) { throw "git apply failed for $PatchPath`n$($apply.Output)" }
        Write-Host "Applied patch: $PatchPath"
        return $true
    }

    $reverse = Invoke-GitChecked @('apply', '--reverse', '--check', $PatchPath)
    if ($reverse.Code -eq 0) {
        Write-Host "Patch already applied, skipping: $PatchPath"
        return $true
    }

    return $false
}

function Write-Utf8NoBom {
    param([string]$Path, [string]$Text)
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Replace-RegexOnce {
    param(
        [string]$Path,
        [string]$Marker,
        [string]$Pattern,
        [string]$Replacement
    )

    $text = [IO.File]::ReadAllText($Path)
    if ($text.Contains($Marker)) {
        Write-Host "Performance edit already present in $Path : $Marker"
        return
    }

    $rx = [regex]::new($Pattern, [Text.RegularExpressions.RegexOptions]::Singleline)
    $matches = $rx.Matches($text)
    if ($matches.Count -ne 1) {
        throw "Expected exactly one match in $Path for marker '$Marker', found $($matches.Count)."
    }

    $updated = $rx.Replace($text, $Replacement, 1)
    Write-Utf8NoBom -Path $Path -Text $updated
    Write-Host "Applied performance edit to $Path : $Marker"
}

function Apply-PerformanceFallbackEdits {
    $ray = Join-Path $RepoRoot 'src/opengl/gl_d3d12raylight.cpp'
    $ogl = Join-Path $RepoRoot 'src/opengl/opengl.h'
    $backend = Join-Path $RepoRoot 'src/renderer/tr_backend.cpp'
    $init = Join-Path $RepoRoot 'src/renderer/tr_init.cpp'
    $local = Join-Path $RepoRoot 'src/renderer/tr_local.h'

    $perfStruct = @'
struct glRaytracingCleanVisualPerformance_t
{
	int asyncSubmit;
	int buildInterval;
	int dispatchInterval;
};

static glRaytracingCleanVisualPerformance_t g_glRaytracingCleanVisualPerformance = { 1, 2, 1 };
static int g_glRaytracingCleanVisualBuildFrame = 0;
static int g_glRaytracingCleanVisualDispatchFrame = 0;
static int g_glRaytracingCleanVisualHasOutput = 0;
static ID3D12Resource* g_glRaytracingCleanVisualLastOutput = nullptr;
'@
    Replace-RegexOnce -Path $ray -Marker 'glRaytracingCleanVisualPerformance_t' `
        -Pattern '(static glRaytracingCleanVisualSafety_t g_glRaytracingCleanVisualSafety = \{ 1, 2, 2500 \};\r?\n)' `
        -Replacement ('$1' + $perfStruct)

    $perfSetter = @'

void glRaytracingSetCleanVisualPerformanceOptions(int asyncSubmit, int buildInterval, int dispatchInterval)
{
	g_glRaytracingCleanVisualPerformance.asyncSubmit = asyncSubmit ? 1 : 0;
	g_glRaytracingCleanVisualPerformance.buildInterval = glRaytracingClamp<int>(buildInterval, 1, 8);
	g_glRaytracingCleanVisualPerformance.dispatchInterval = glRaytracingClamp<int>(dispatchInterval, 1, 8);
}
'@
    Replace-RegexOnce -Path $ray -Marker 'glRaytracingSetCleanVisualPerformanceOptions' `
        -Pattern '(void glRaytracingSetCleanVisualSafetyOptions\(int safeMode, int errorLimit, int fenceWaitMs\)\s*\{.*?\r?\n\})(\r?\n\r?\nint glRaytracingHasDeviceLost)' `
        -Replacement ('$1' + $perfSetter + '$2')

    $buildSkip = @'

	// Clean visual performance: keep the exact full-resolution lighting path,
	// but reuse the last valid TLAS on interval frames. Static world appearance
	// is unchanged; only moving RT geometry updates at the selected cadence.
	if (g_glRaytracingScene.activeInstanceCount > 0 &&
		g_glRaytracingCleanVisualPerformance.buildInterval > 1)
	{
		++g_glRaytracingCleanVisualBuildFrame;
		if ((g_glRaytracingCleanVisualBuildFrame % g_glRaytracingCleanVisualPerformance.buildInterval) != 0)
			return 1;
	}
'@
    Replace-RegexOnce -Path $ray -Marker 'Clean visual performance: keep the exact full-resolution lighting path' `
        -Pattern '(int glRaytracingBuildScene\(void\)\s*\{\s*if \(glRaytracingShouldAbortWork\(\)\)\s*return 0;\s*if \(!g_glRaytracingScene\.initialized\)\s*return 0;)' `
        -Replacement ('$1' + $buildSkip)

    $dispatchReuse = @'

	// Re-use the previous full-resolution DXR lighting texture on optional
	// interval frames. No low-resolution upscale or block composite is used.
	if (pass->outputTexture != g_glRaytracingCleanVisualLastOutput)
	{
		g_glRaytracingCleanVisualLastOutput = pass->outputTexture;
		g_glRaytracingCleanVisualDispatchFrame = 0;
		g_glRaytracingCleanVisualHasOutput = 0;
	}
	if (g_glRaytracingCleanVisualHasOutput &&
		g_glRaytracingCleanVisualPerformance.dispatchInterval > 1)
	{
		++g_glRaytracingCleanVisualDispatchFrame;
		if ((g_glRaytracingCleanVisualDispatchFrame % g_glRaytracingCleanVisualPerformance.dispatchInterval) != 0)
			return true;
	}
'@
    Replace-RegexOnce -Path $ray -Marker 'Re-use the previous full-resolution DXR lighting texture' `
        -Pattern '(bool glRaytracingLightingExecute\(const glRaytracingLightingPassDesc_t\* pass\)\s*\{\s*if \(glRaytracingShouldAbortWork\(\)\)\s*return false;\s*if \(!g_glRaytracingLighting\.initialized \|\| !pass\)\s*return false;)' `
        -Replacement ('$1' + $dispatchReuse)

    $asyncTail = @'
	// The DXR and compatibility renderer command lists use the same D3D12
	// queue. Queue submission order provides the GPU dependency; an immediate
	// CPU fence wait only serializes every frame and destroys performance.
	if (!g_glRaytracingCleanVisualPerformance.asyncSubmit)
		glRaytracingWaitFenceValue(g_glRaytracingCmd.cmdLastFenceValue);

	g_glRaytracingCleanVisualHasOutput = 1;
'@
    Replace-RegexOnce -Path $ray -Marker 'Queue submission order provides the GPU dependency' `
        -Pattern '(if \(!glRaytracingEndCmd\(\)\)\s*return false;\s*)(?:// Stable mode.*?\r?\n\s*//.*?\r?\n\s*)?glRaytracingWaitFenceValue\(g_glRaytracingCmd\.cmdLastFenceValue\);(\s*return true;)' `
        -Replacement ('$1' + $asyncTail + '$2')

    $prototype = 'void                       glRaytracingSetCleanVisualPerformanceOptions(int asyncSubmit, int buildInterval, int dispatchInterval);' + [Environment]::NewLine
    Replace-RegexOnce -Path $ogl -Marker 'glRaytracingSetCleanVisualPerformanceOptions' `
        -Pattern '(void\s+glRaytracingSetCleanVisualSafetyOptions\(int safeMode, int errorLimit, int fenceWaitMs\);\r?\n)' `
        -Replacement ('$1' + $prototype)

    $meshDedupe = @'
	// The same RTCW surface can be visited by several shader stages in one
	// frame. Uploading and dirtying its BLAS repeatedly is pure duplicate work.
	if (mesh->cachedFrame == currentFrame)
	{
		return;
	}
	mesh->cachedFrame = currentFrame;

'@
    Replace-RegexOnce -Path $backend -Marker 'The same RTCW surface can be visited by several shader stages' `
        -Pattern '(void RB_UpdateDXRMesh\([^\{]+\{\s*)' `
        -Replacement ('$1' + $meshDedupe)

    $backendSubmit = @'
	glRaytracingSetCleanVisualPerformanceOptions(
		r_dxrAsyncSubmit ? r_dxrAsyncSubmit->integer : 1,
		r_dxrBuildInterval ? r_dxrBuildInterval->integer : 2,
		r_dxrDispatchInterval ? r_dxrDispatchInterval->integer : 1);

	// QD3D12_FlushQueuedBatches() in glLightScene submits prior raster work on
	// the same queue. A global glFinish here forces a full GPU idle every frame.
	if (r_dxrCpuSync && r_dxrCpuSync->integer)
		glFinish();
	glLightScene();
'@
    Replace-RegexOnce -Path $backend -Marker 'QD3D12_FlushQueuedBatches() in glLightScene' `
        -Pattern '\tglFinish\(\);\r?\n\tglLightScene\(\);' `
        -Replacement $backendSubmit

    $cvarDecls = @'
cvar_t  *r_dxrAsyncSubmit;
cvar_t  *r_dxrBuildInterval;
cvar_t  *r_dxrDispatchInterval;
cvar_t  *r_dxrCpuSync;
'@
    Replace-RegexOnce -Path $init -Marker 'cvar_t  *r_dxrAsyncSubmit;' `
        -Pattern '(cvar_t\s+\*r_dxrFenceWaitMs;\r?\n)' `
        -Replacement ('$1' + $cvarDecls)

    $cvarRegs = @'
	r_dxrAsyncSubmit = ri.Cvar_Get( "r_dxrAsyncSubmit", "1", CVAR_ARCHIVE );
	r_dxrBuildInterval = ri.Cvar_Get( "r_dxrBuildInterval", "2", CVAR_ARCHIVE );
	r_dxrDispatchInterval = ri.Cvar_Get( "r_dxrDispatchInterval", "1", CVAR_ARCHIVE );
	r_dxrCpuSync = ri.Cvar_Get( "r_dxrCpuSync", "0", CVAR_ARCHIVE );
'@
    Replace-RegexOnce -Path $init -Marker 'r_dxrAsyncSubmit = ri.Cvar_Get' `
        -Pattern '(\tr_dxrFenceWaitMs = ri\.Cvar_Get\( "r_dxrFenceWaitMs", "2500", CVAR_ARCHIVE \);\r?\n)' `
        -Replacement ('$1' + $cvarRegs)

    $externs = @'
extern cvar_t   *r_dxrAsyncSubmit;      // do not block the CPU after each full-resolution DXR dispatch
extern cvar_t   *r_dxrBuildInterval;    // reuse the valid TLAS between dynamic-scene updates
extern cvar_t   *r_dxrDispatchInterval; // reuse the full-resolution lighting output between dispatches
extern cvar_t   *r_dxrCpuSync;          // legacy full glFinish before DXR, for diagnosis only
'@
    Replace-RegexOnce -Path $local -Marker 'extern cvar_t   *r_dxrAsyncSubmit;' `
        -Pattern '(extern cvar_t\s+\*r_dxrFenceWaitMs;[^\r\n]*\r?\n)' `
        -Replacement ('$1' + $externs)
}

Push-Location $RepoRoot
try {
    $basePatches = @(
        'patches/00-build-fix-Com_Printf.patch',
        'patches/02-dxr-stable-mode.patch',
        'patches/03-pvs-decompressvis-x64.patch',
        'patches/04-dxr-visibility-debug.patch',
        'patches/05-dxr-clean-visual-stability-only.patch'
    )

    foreach ($rel in $basePatches) {
        $ok = Apply-PatchIfNeeded -PatchPath (Join-Path $RepoRoot $rel)
        if (!$ok) { throw "Base patch cannot be applied cleanly: $rel" }
    }

    $perfPatch = Join-Path $RepoRoot 'patches/06-dxr-clean-visual-performance-pipeline.patch'
    if (!(Apply-PatchIfNeeded -PatchPath $perfPatch)) {
        Write-Host 'Patch 06 did not match exact line context; applying validated source transformations.'
        Apply-PerformanceFallbackEdits
    }
}
finally {
    Pop-Location
}

Write-Host 'DXR Clean Visual Performance patch stack applied successfully.'
$global:LASTEXITCODE = 0
exit 0
