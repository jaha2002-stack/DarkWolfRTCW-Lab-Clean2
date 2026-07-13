#include "opengl.h"

#if defined(_MSC_VER)
extern void __cdecl Com_Printf(const char* fmt, ...);
#endif

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <stdint.h>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <string>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

#pragma comment(lib, "dxcompiler.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

// ============================================================
// Logging / checks
// ============================================================

static void glRaytracingLog(const char* fmt, ...)
{
	char buffer[4096];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	OutputDebugStringA(buffer);
	OutputDebugStringA("\n");
}

static void glRaytracingFatal(const char* fmt, ...)
{
	char buffer[4096];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	OutputDebugStringA(buffer);
	OutputDebugStringA("\n");
	Com_Printf("^1DXR ERROR:^7 %s\n", buffer);
}

static int glRaytracingHandleFailure(HRESULT hr, const char* what, const char* file, int line);
static int glRaytracingShouldAbortWork(void);

#define GLR_CHECK(x) \
    do { HRESULT _hr = (x); if (FAILED(_hr)) { glRaytracingHandleFailure(_hr, #x, __FILE__, __LINE__); return 0; } } while (0)

#define GLR_CHECKV(x) \
    do { HRESULT _hr = (x); if (FAILED(_hr)) { glRaytracingHandleFailure(_hr, #x, __FILE__, __LINE__); return; } } while (0)

// ============================================================
// Helpers
// ============================================================

static UINT64 glRaytracingAlignUp(UINT64 v, UINT64 a)
{
	return (v + (a - 1)) & ~(a - 1);
}

template<typename T>
static T glRaytracingClamp(T v, T lo, T hi)
{
	return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static DXGI_FORMAT glRaytracingGetSrvFormatForDepth(DXGI_FORMAT fmt)
{
	switch (fmt)
	{
	case DXGI_FORMAT_D32_FLOAT:         return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_D16_UNORM:         return DXGI_FORMAT_R16_UNORM;
	default:                            return fmt;
	}
}

struct glRaytracingBuffer_t
{
	ComPtr<ID3D12Resource> resource;
	UINT64 size;
	D3D12_GPU_VIRTUAL_ADDRESS gpuVA;

	glRaytracingBuffer_t()
	{
		size = 0;
		gpuVA = 0;
	}
};

static glRaytracingBuffer_t glRaytracingCreateBuffer(
	ID3D12Device* device,
	UINT64 size,
	D3D12_HEAP_TYPE heapType,
	D3D12_RESOURCE_STATES initialState,
	D3D12_RESOURCE_FLAGS flags)
{
	glRaytracingBuffer_t out;

	if (glRaytracingShouldAbortWork() || !device || size == 0)
		return out;

	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = heapType;

	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = size;
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.Format = DXGI_FORMAT_UNKNOWN;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	rd.Flags = flags;

	HRESULT hr = device->CreateCommittedResource(
		&hp,
		D3D12_HEAP_FLAG_NONE,
		&rd,
		initialState,
		nullptr,
		IID_PPV_ARGS(&out.resource));

	if (FAILED(hr))
	{
		glRaytracingHandleFailure(hr, "CreateCommittedResource", __FILE__, __LINE__);
		return out;
	}

	out.size = size;
	out.gpuVA = out.resource->GetGPUVirtualAddress();
	return out;
}

static void glRaytracingMapCopy(ID3D12Resource* res, const void* src, size_t bytes)
{
	if (glRaytracingShouldAbortWork() || !res || !src || bytes == 0)
		return;

	void* dst = nullptr;
	HRESULT hr = res->Map(0, nullptr, &dst);
	if (FAILED(hr) || !dst)
	{
		glRaytracingHandleFailure(FAILED(hr) ? hr : E_FAIL, "Map", __FILE__, __LINE__);
		return;
	}

	memcpy(dst, src, bytes);
	res->Unmap(0, nullptr);
}

static void glRaytracingTransition(
	ID3D12GraphicsCommandList* cmd,
	ID3D12Resource* res,
	D3D12_RESOURCE_STATES before,
	D3D12_RESOURCE_STATES after)
{
	if (!res || before == after)
		return;

	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd->ResourceBarrier(1, &b);
}

static D3D12_CPU_DESCRIPTOR_HANDLE glRaytracingOffsetCpu(D3D12_CPU_DESCRIPTOR_HANDLE h, UINT stride, UINT idx)
{
	h.ptr += UINT64(stride) * UINT64(idx);
	return h;
}

static D3D12_GPU_DESCRIPTOR_HANDLE glRaytracingOffsetGpu(D3D12_GPU_DESCRIPTOR_HANDLE h, UINT stride, UINT idx)
{
	h.ptr += UINT64(stride) * UINT64(idx);
	return h;
}

// ============================================================
// Shared command context
// ============================================================

struct glRaytracingCmdContext_t
{
	ComPtr<ID3D12Device5> device;
	ComPtr<ID3D12CommandQueue> queue;

	ComPtr<ID3D12CommandAllocator> cmdAlloc;
	ComPtr<ID3D12GraphicsCommandList4> cmdList;
	UINT64 cmdLastFenceValue;

	ComPtr<ID3D12CommandAllocator> blasCmdAlloc;
	ComPtr<ID3D12GraphicsCommandList4> blasCmdList;
	UINT64 blasLastFenceValue;

	ComPtr<ID3D12CommandAllocator> tlasCmdAlloc;
	ComPtr<ID3D12GraphicsCommandList4> tlasCmdList;
	UINT64 tlasLastFenceValue;

	ComPtr<ID3D12Fence> fence;
	HANDLE fenceEvent;
	UINT64 nextFenceValue;
	bool initialized;

	glRaytracingCmdContext_t()
	{
		cmdLastFenceValue = 0;
		blasLastFenceValue = 0;
		tlasLastFenceValue = 0;
		fenceEvent = nullptr;
		nextFenceValue = 0;
		initialized = false;
	}
};

static glRaytracingCmdContext_t g_glRaytracingCmd;

struct glRaytracingCleanVisualSafety_t
{
	int safeMode;
	int errorLimit;
	int fenceWaitMs;
};

static glRaytracingCleanVisualSafety_t g_glRaytracingCleanVisualSafety = { 1, 2, 2500 };
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
static volatile LONG g_glRaytracingDeviceLost = 0;
static HRESULT g_glRaytracingDeviceLostHr = S_OK;
static HRESULT g_glRaytracingDeviceRemovedReason = S_OK;
static int g_glRaytracingErrorCount = 0;

static int glRaytracingIsDeviceRemovedLike(HRESULT hr)
{
	return hr == DXGI_ERROR_DEVICE_REMOVED ||
		hr == DXGI_ERROR_DEVICE_RESET ||
		hr == DXGI_ERROR_DEVICE_HUNG;
}

static int glRaytracingCanLogError(void)
{
	int limit = g_glRaytracingCleanVisualSafety.errorLimit;
	if (limit < 1)
		limit = 1;

	if (g_glRaytracingErrorCount < limit)
	{
		++g_glRaytracingErrorCount;
		return 1;
	}
	return 0;
}

static int glRaytracingHandleFailure(HRESULT hr, const char* what, const char* file, int line)
{
	if (glRaytracingIsDeviceRemovedLike(hr))
	{
		if (InterlockedCompareExchange(&g_glRaytracingDeviceLost, 1, 0) == 0)
		{
			g_glRaytracingDeviceLostHr = hr;
			g_glRaytracingDeviceRemovedReason = hr;
			if (g_glRaytracingCmd.device)
				g_glRaytracingDeviceRemovedReason = g_glRaytracingCmd.device->GetDeviceRemovedReason();

			Com_Printf(
				"^1DXR DEVICE LOST:^7 %s failed 0x%08X at %s:%d, removedReason=0x%08X. Clean-visual DXR is disabled until vid_restart.\n",
				what ? what : "D3D12 call",
				(unsigned)hr,
				file ? file : "?",
				line,
				(unsigned)g_glRaytracingDeviceRemovedReason);
		}
		return 0;
	}

	if (glRaytracingCanLogError())
	{
		glRaytracingFatal(
			"%s failed 0x%08X at %s:%d",
			what ? what : "D3D12 call",
			(unsigned)hr,
			file ? file : "?",
			line);
	}
	return 0;
}

static int glRaytracingShouldAbortWork(void)
{
	return g_glRaytracingCleanVisualSafety.safeMode && g_glRaytracingDeviceLost;
}

void glRaytracingSetCleanVisualSafetyOptions(int safeMode, int errorLimit, int fenceWaitMs)
{
	g_glRaytracingCleanVisualSafety.safeMode = safeMode ? 1 : 0;
	g_glRaytracingCleanVisualSafety.errorLimit = glRaytracingClamp<int>(errorLimit, 1, 64);
	g_glRaytracingCleanVisualSafety.fenceWaitMs = glRaytracingClamp<int>(fenceWaitMs, 250, 30000);
}

void glRaytracingSetCleanVisualPerformanceOptions(int asyncSubmit, int buildInterval, int dispatchInterval)
{
	g_glRaytracingCleanVisualPerformance.asyncSubmit = asyncSubmit ? 1 : 0;
	g_glRaytracingCleanVisualPerformance.buildInterval = glRaytracingClamp<int>(buildInterval, 1, 8);
	g_glRaytracingCleanVisualPerformance.dispatchInterval = glRaytracingClamp<int>(dispatchInterval, 1, 8);
}

int glRaytracingHasDeviceLost(void)
{
	return g_glRaytracingDeviceLost ? 1 : 0;
}

static void glRaytracingWaitFenceValue(UINT64 value)
{
	if (glRaytracingShouldAbortWork())
		return;

	if (!value || !g_glRaytracingCmd.fence || !g_glRaytracingCmd.fenceEvent)
		return;

	if (g_glRaytracingCmd.fence->GetCompletedValue() >= value)
		return;

	HRESULT hr = g_glRaytracingCmd.fence->SetEventOnCompletion(value, g_glRaytracingCmd.fenceEvent);
	if (FAILED(hr))
	{
		glRaytracingHandleFailure(hr, "SetEventOnCompletion", __FILE__, __LINE__);
		return;
	}

	const DWORD waitMs = g_glRaytracingCleanVisualSafety.safeMode ? (DWORD)g_glRaytracingCleanVisualSafety.fenceWaitMs : INFINITE;
	DWORD waitResult = WaitForSingleObject(g_glRaytracingCmd.fenceEvent, waitMs);
	if (waitResult == WAIT_TIMEOUT)
	{
		glRaytracingHandleFailure(DXGI_ERROR_DEVICE_HUNG, "Fence wait timeout", __FILE__, __LINE__);
		return;
	}
	if (waitResult != WAIT_OBJECT_0)
	{
		glRaytracingHandleFailure(E_FAIL, "Fence wait failed", __FILE__, __LINE__);
		return;
	}
}

static UINT64 glRaytracingSignalQueue(void)
{
	if (!g_glRaytracingCmd.queue || !g_glRaytracingCmd.fence)
		return 0;

	const UINT64 value = ++g_glRaytracingCmd.nextFenceValue;
	g_glRaytracingCmd.queue->Signal(g_glRaytracingCmd.fence.Get(), value);
	return value;
}

static void glRaytracingWaitIdle(void)
{
	UINT64 value = glRaytracingSignalQueue();
	glRaytracingWaitFenceValue(value);
}

static int glRaytracingInitCmdContext(void)
{
	if (g_glRaytracingCmd.initialized)
		return 1;

	ID3D12Device* baseDevice = QD3D12_GetDevice();
	ID3D12CommandQueue* baseQueue = QD3D12_GetQueue();

	if (!baseDevice || !baseQueue)
	{
		glRaytracingFatal("glRaytracingInitCmdContext: missing device or queue");
		return 0;
	}

	GLR_CHECK(baseDevice->QueryInterface(IID_PPV_ARGS(&g_glRaytracingCmd.device)));

	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
	HRESULT optionsHr = g_glRaytracingCmd.device->CheckFeatureSupport(
		D3D12_FEATURE_D3D12_OPTIONS5,
		&options5,
		sizeof(options5));
	if (FAILED(optionsHr) || options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
	{
		glRaytracingFatal("D3D12 device does not report DXR raytracing support");
		return 0;
	}

	g_glRaytracingCmd.queue = baseQueue;

	GLR_CHECK(g_glRaytracingCmd.device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(&g_glRaytracingCmd.cmdAlloc)));

	GLR_CHECK(g_glRaytracingCmd.device->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		g_glRaytracingCmd.cmdAlloc.Get(),
		nullptr,
		IID_PPV_ARGS(&g_glRaytracingCmd.cmdList)));

	GLR_CHECK(g_glRaytracingCmd.cmdList->Close());

	GLR_CHECK(g_glRaytracingCmd.device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(&g_glRaytracingCmd.blasCmdAlloc)));

	GLR_CHECK(g_glRaytracingCmd.device->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		g_glRaytracingCmd.blasCmdAlloc.Get(),
		nullptr,
		IID_PPV_ARGS(&g_glRaytracingCmd.blasCmdList)));

	GLR_CHECK(g_glRaytracingCmd.blasCmdList->Close());

	GLR_CHECK(g_glRaytracingCmd.device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(&g_glRaytracingCmd.tlasCmdAlloc)));

	GLR_CHECK(g_glRaytracingCmd.device->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		g_glRaytracingCmd.tlasCmdAlloc.Get(),
		nullptr,
		IID_PPV_ARGS(&g_glRaytracingCmd.tlasCmdList)));

	GLR_CHECK(g_glRaytracingCmd.tlasCmdList->Close());

	GLR_CHECK(g_glRaytracingCmd.device->CreateFence(
		0,
		D3D12_FENCE_FLAG_NONE,
		IID_PPV_ARGS(&g_glRaytracingCmd.fence)));

	g_glRaytracingCmd.fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	if (!g_glRaytracingCmd.fenceEvent)
	{
		glRaytracingFatal("CreateEventA failed");
		return 0;
	}

	g_glRaytracingCmd.initialized = true;
	return 1;
}

static void glRaytracingShutdownCmdContext(void)
{
	if (!g_glRaytracingCmd.initialized)
		return;

	glRaytracingWaitIdle();

	if (g_glRaytracingCmd.fenceEvent)
	{
		CloseHandle(g_glRaytracingCmd.fenceEvent);
		g_glRaytracingCmd.fenceEvent = nullptr;
	}

	g_glRaytracingCmd = glRaytracingCmdContext_t();
}

static int glRaytracingBeginCmd(void)
{
	if (glRaytracingShouldAbortWork())
		return 0;

	glRaytracingWaitFenceValue(g_glRaytracingCmd.cmdLastFenceValue);
	if (glRaytracingShouldAbortWork())
		return 0;

	GLR_CHECK(g_glRaytracingCmd.cmdAlloc->Reset());
	GLR_CHECK(g_glRaytracingCmd.cmdList->Reset(g_glRaytracingCmd.cmdAlloc.Get(), nullptr));
	return 1;
}

static int glRaytracingEndCmd(void)
{
	GLR_CHECK(g_glRaytracingCmd.cmdList->Close());

	ID3D12CommandList* lists[] = { g_glRaytracingCmd.cmdList.Get() };
	g_glRaytracingCmd.queue->ExecuteCommandLists(1, lists);

	g_glRaytracingCmd.cmdLastFenceValue = glRaytracingSignalQueue();

	return 1;
}

static int glRaytracingBeginBlasCmd(void)
{
	if (glRaytracingShouldAbortWork())
		return 0;

	glRaytracingWaitFenceValue(g_glRaytracingCmd.blasLastFenceValue);
	if (glRaytracingShouldAbortWork())
		return 0;

	GLR_CHECK(g_glRaytracingCmd.blasCmdAlloc->Reset());
	GLR_CHECK(g_glRaytracingCmd.blasCmdList->Reset(g_glRaytracingCmd.blasCmdAlloc.Get(), nullptr));
	return 1;
}

static UINT64 glRaytracingEndBlasCmd(void)
{
	GLR_CHECK(g_glRaytracingCmd.blasCmdList->Close());

	ID3D12CommandList* lists[] = { g_glRaytracingCmd.blasCmdList.Get() };
	g_glRaytracingCmd.queue->ExecuteCommandLists(1, lists);

	g_glRaytracingCmd.blasLastFenceValue = glRaytracingSignalQueue();
	return g_glRaytracingCmd.blasLastFenceValue;
}

static int glRaytracingBeginTlasCmd(void)
{
	if (glRaytracingShouldAbortWork())
		return 0;

	glRaytracingWaitFenceValue(g_glRaytracingCmd.tlasLastFenceValue);
	if (glRaytracingShouldAbortWork())
		return 0;

	GLR_CHECK(g_glRaytracingCmd.tlasCmdAlloc->Reset());
	GLR_CHECK(g_glRaytracingCmd.tlasCmdList->Reset(g_glRaytracingCmd.tlasCmdAlloc.Get(), nullptr));
	return 1;
}

static UINT64 glRaytracingEndTlasCmd(void)
{
	GLR_CHECK(g_glRaytracingCmd.tlasCmdList->Close());

	ID3D12CommandList* lists[] = { g_glRaytracingCmd.tlasCmdList.Get() };
	g_glRaytracingCmd.queue->ExecuteCommandLists(1, lists);

	g_glRaytracingCmd.tlasLastFenceValue = glRaytracingSignalQueue();
	return g_glRaytracingCmd.tlasLastFenceValue;
}

// ============================================================
// Scene builder state
// ============================================================

struct glRaytracingMeshRecord_t
{
	uint32_t handle;
	int      alive;

	glRaytracingMeshDesc_t descCpu;

	std::vector<glRaytracingVertex_t> verticesCpu;
	std::vector<uint32_t>             indicesCpu;

	glRaytracingBuffer_t vertexBuffer;
	glRaytracingBuffer_t indexBuffer;

	glRaytracingBuffer_t blasScratch;
	glRaytracingBuffer_t blasResult[2];

	UINT64 blasScratchSize;
	UINT64 blasResultSize;

	int blasBuilt;
	int dirty;
	int currentBlasIndex;

	glRaytracingMeshRecord_t()
	{
		handle = 0;
		alive = 0;
		memset(&descCpu, 0, sizeof(descCpu));
		blasScratchSize = 0;
		blasResultSize = 0;
		blasBuilt = 0;
		dirty = 0;
		currentBlasIndex = 0;
	}
};

struct glRaytracingInstanceRecord_t
{
	uint32_t handle;
	int      alive;
	glRaytracingInstanceDesc_t descCpu;
	int dirty;
	int cachedActive;
	D3D12_GPU_VIRTUAL_ADDRESS cachedBlasGpuVA;
	D3D12_RAYTRACING_INSTANCE_DESC cachedDescCpu;

	glRaytracingInstanceRecord_t()
	{
		handle = 0;
		alive = 0;
		memset(&descCpu, 0, sizeof(descCpu));
		dirty = 0;
		cachedActive = 0;
		cachedBlasGpuVA = 0;
		memset(&cachedDescCpu, 0, sizeof(cachedDescCpu));
	}
};

struct glRaytracingSceneUploadBuffer_t
{
	glRaytracingBuffer_t buffer;
	UINT64 capacityBytes;
	D3D12_RAYTRACING_INSTANCE_DESC* mapped;

	glRaytracingSceneUploadBuffer_t()
	{
		capacityBytes = 0;
		mapped = nullptr;
	}
};

struct glRaytracingSceneState_t
{
	std::vector<glRaytracingMeshRecord_t> meshes;
	std::vector<glRaytracingInstanceRecord_t> instances;
	std::vector<int> activeInstanceIndices;
	std::vector<D3D12_RAYTRACING_INSTANCE_DESC> cpuInstanceDescs;

	std::vector<int> meshHandleToIndex;
	std::vector<int> instanceHandleToIndex;

	uint32_t nextMeshHandle;
	uint32_t nextInstanceHandle;

	glRaytracingSceneUploadBuffer_t instanceDescUpload[2];
	glRaytracingBuffer_t tlasScratch;
	glRaytracingBuffer_t tlasResult[2];

	UINT64 tlasScratchSize;
	UINT64 tlasResultSize;

	int initialized;

	UINT activeInstanceCount;
	UINT builtInstanceCount;

	int tlasBuilt;
	int tlasNeedsRebuild;
	int tlasNeedsUpdate;
	int currentTLASIndex;

	glRaytracingSceneState_t()
	{
		nextMeshHandle = 1;
		nextInstanceHandle = 1;
		tlasScratchSize = 0;
		tlasResultSize = 0;
		initialized = 0;

		activeInstanceCount = 0;
		builtInstanceCount = 0;
		tlasBuilt = 0;
		tlasNeedsRebuild = 1;
		tlasNeedsUpdate = 1;
		currentTLASIndex = 0;
	}
};

static glRaytracingSceneState_t g_glRaytracingScene;

void glRaytracingClear(void);

static void glRaytracingEnsureMeshHandleTable(uint32_t handle)
{
	if (handle >= g_glRaytracingScene.meshHandleToIndex.size())
		g_glRaytracingScene.meshHandleToIndex.resize((size_t)handle + 1, -1);
}

static void glRaytracingEnsureInstanceHandleTable(uint32_t handle)
{
	if (handle >= g_glRaytracingScene.instanceHandleToIndex.size())
		g_glRaytracingScene.instanceHandleToIndex.resize((size_t)handle + 1, -1);
}

static glRaytracingBuffer_t* glRaytracingGetMeshCurrentBLAS(glRaytracingMeshRecord_t* mesh)
{
	if (!mesh)
		return nullptr;
	return &mesh->blasResult[mesh->currentBlasIndex & 1];
}

static const glRaytracingBuffer_t* glRaytracingGetMeshCurrentBLASConst(const glRaytracingMeshRecord_t* mesh)
{
	if (!mesh)
		return nullptr;
	return &mesh->blasResult[mesh->currentBlasIndex & 1];
}

static int glRaytracingGetInactiveTLASIndex(void)
{
	return g_glRaytracingScene.currentTLASIndex ^ 1;
}

static glRaytracingSceneUploadBuffer_t* glRaytracingGetBuildInstanceUpload(void)
{
	return &g_glRaytracingScene.instanceDescUpload[glRaytracingGetInactiveTLASIndex()];
}

static glRaytracingBuffer_t* glRaytracingGetCurrentTLASBuffer(void)
{
	return &g_glRaytracingScene.tlasResult[g_glRaytracingScene.currentTLASIndex & 1];
}

static const glRaytracingBuffer_t* glRaytracingGetCurrentTLASBufferConst(void)
{
	return &g_glRaytracingScene.tlasResult[g_glRaytracingScene.currentTLASIndex & 1];
}

static glRaytracingBuffer_t* glRaytracingGetBuildTLASBuffer(void)
{
	return &g_glRaytracingScene.tlasResult[glRaytracingGetInactiveTLASIndex()];
}


static int glRaytracingEnsureTLASBuffers(
	const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* inputs)
{
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
	g_glRaytracingCmd.device->GetRaytracingAccelerationStructurePrebuildInfo(inputs, &prebuild);

	if (prebuild.ResultDataMaxSizeInBytes == 0)
	{
		glRaytracingFatal("TLAS prebuild size is zero");
		return 0;
	}

	const UINT64 requiredScratch = glRaytracingAlignUp(
		prebuild.ScratchDataSizeInBytes,
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);

	const UINT64 requiredResult = glRaytracingAlignUp(
		prebuild.ResultDataMaxSizeInBytes,
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);

	if (!g_glRaytracingScene.tlasScratch.resource ||
		g_glRaytracingScene.tlasScratchSize < requiredScratch)
	{
		g_glRaytracingScene.tlasScratch.resource.Reset();
		g_glRaytracingScene.tlasScratch = glRaytracingCreateBuffer(
			g_glRaytracingCmd.device.Get(),
			requiredScratch,
			D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		if (!g_glRaytracingScene.tlasScratch.resource)
			return 0;

		g_glRaytracingScene.tlasScratchSize = requiredScratch;
	}

	for (int i = 0; i < 2; ++i)
	{
		if (!g_glRaytracingScene.tlasResult[i].resource ||
			g_glRaytracingScene.tlasResultSize < requiredResult)
		{
			g_glRaytracingScene.tlasResult[i].resource.Reset();
			g_glRaytracingScene.tlasResult[i] = glRaytracingCreateBuffer(
				g_glRaytracingCmd.device.Get(),
				requiredResult,
				D3D12_HEAP_TYPE_DEFAULT,
				D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

			if (!g_glRaytracingScene.tlasResult[i].resource)
				return 0;
		}
	}

	g_glRaytracingScene.tlasResultSize = requiredResult;
	return 1;
}

static glRaytracingMeshRecord_t* glRaytracingFindMesh(uint32_t handle)
{
	if (handle == 0 || handle >= g_glRaytracingScene.meshHandleToIndex.size())
		return nullptr;

	const int index = g_glRaytracingScene.meshHandleToIndex[handle];
	if (index < 0 || (size_t)index >= g_glRaytracingScene.meshes.size())
		return nullptr;

	glRaytracingMeshRecord_t& mesh = g_glRaytracingScene.meshes[(size_t)index];
	if (!mesh.alive || mesh.handle != handle)
		return nullptr;

	return &mesh;
}

static const glRaytracingMeshRecord_t* glRaytracingFindMeshConst(uint32_t handle)
{
	if (handle == 0 || handle >= g_glRaytracingScene.meshHandleToIndex.size())
		return nullptr;

	const int index = g_glRaytracingScene.meshHandleToIndex[handle];
	if (index < 0 || (size_t)index >= g_glRaytracingScene.meshes.size())
		return nullptr;

	const glRaytracingMeshRecord_t& mesh = g_glRaytracingScene.meshes[(size_t)index];
	if (!mesh.alive || mesh.handle != handle)
		return nullptr;

	return &mesh;
}

static glRaytracingInstanceRecord_t* glRaytracingFindInstance(uint32_t handle)
{
	if (handle == 0 || handle >= g_glRaytracingScene.instanceHandleToIndex.size())
		return nullptr;

	const int index = g_glRaytracingScene.instanceHandleToIndex[handle];
	if (index < 0 || (size_t)index >= g_glRaytracingScene.instances.size())
		return nullptr;

	glRaytracingInstanceRecord_t& inst = g_glRaytracingScene.instances[(size_t)index];
	if (!inst.alive || inst.handle != handle)
		return nullptr;

	return &inst;
}

static int glRaytracingEnsureMeshScratch(glRaytracingMeshRecord_t* mesh, UINT64 requiredScratch)
{
	if (!mesh)
		return 0;

	requiredScratch = glRaytracingAlignUp(requiredScratch, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);

	if (!mesh->blasScratch.resource || mesh->blasScratchSize < requiredScratch)
	{
		mesh->blasScratch.resource.Reset();
		mesh->blasScratch = glRaytracingCreateBuffer(
			g_glRaytracingCmd.device.Get(),
			requiredScratch,
			D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		if (!mesh->blasScratch.resource)
			return 0;

		mesh->blasScratchSize = requiredScratch;
	}

	return 1;
}

static int glRaytracingEnsureMeshResultBuffers(glRaytracingMeshRecord_t* mesh, UINT64 requiredResult)
{
	if (!mesh)
		return 0;

	requiredResult = glRaytracingAlignUp(requiredResult, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);

	const int resultCount = mesh->descCpu.allowUpdate ? 2 : 1;
	for (int i = 0; i < resultCount; ++i)
	{
		if (!mesh->blasResult[i].resource || mesh->blasResultSize < requiredResult)
		{
			mesh->blasResult[i].resource.Reset();
			mesh->blasResult[i] = glRaytracingCreateBuffer(
				g_glRaytracingCmd.device.Get(),
				requiredResult,
				D3D12_HEAP_TYPE_DEFAULT,
				D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

			if (!mesh->blasResult[i].resource)
				return 0;
		}
	}

	if (!mesh->descCpu.allowUpdate)
		mesh->blasResult[1].resource.Reset();

	mesh->blasResultSize = requiredResult;
	return 1;
}

static inline void glRaytracingBuildInstanceDesc(
	D3D12_RAYTRACING_INSTANCE_DESC* outDesc,
	const glRaytracingInstanceRecord_t& inst,
	D3D12_GPU_VIRTUAL_ADDRESS blasGpuVA)
{
	memcpy(outDesc->Transform, inst.descCpu.transform, sizeof(float) * 12);
	outDesc->InstanceID = inst.descCpu.instanceID;
	outDesc->InstanceMask = (UINT8)(inst.descCpu.mask ? inst.descCpu.mask : 0xFF);
	outDesc->InstanceContributionToHitGroupIndex = 0;
	outDesc->Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
	outDesc->AccelerationStructure = blasGpuVA;
}

static void glRaytracingInvalidateInstanceCache(glRaytracingInstanceRecord_t* inst)
{
	if (!inst)
		return;

	inst->cachedActive = 0;
	inst->cachedBlasGpuVA = 0;
	memset(&inst->cachedDescCpu, 0, sizeof(inst->cachedDescCpu));
}

static int glRaytracingResolveInstanceDesc(
	glRaytracingInstanceRecord_t* inst,
	D3D12_RAYTRACING_INSTANCE_DESC* outDesc,
	D3D12_GPU_VIRTUAL_ADDRESS* outBlasGpuVA)
{
	if (!inst || !inst->alive)
		return 0;

	const glRaytracingMeshRecord_t* mesh = glRaytracingFindMeshConst(inst->descCpu.meshHandle);
	if (!mesh || !mesh->blasBuilt)
		return 0;

	const glRaytracingBuffer_t* blas = glRaytracingGetMeshCurrentBLASConst(mesh);
	if (!blas || !blas->resource || blas->gpuVA == 0)
		return 0;

	if (outDesc)
		glRaytracingBuildInstanceDesc(outDesc, *inst, blas->gpuVA);
	if (outBlasGpuVA)
		*outBlasGpuVA = blas->gpuVA;
	return 1;
}

static int glRaytracingRebuildActiveInstanceCache(void)
{
	g_glRaytracingScene.activeInstanceIndices.clear();
	g_glRaytracingScene.cpuInstanceDescs.clear();
	g_glRaytracingScene.activeInstanceIndices.reserve(g_glRaytracingScene.instances.size());
	g_glRaytracingScene.cpuInstanceDescs.reserve(g_glRaytracingScene.instances.size());

	for (size_t i = 0; i < g_glRaytracingScene.instances.size(); ++i)
	{
		glRaytracingInstanceRecord_t& inst = g_glRaytracingScene.instances[i];
		glRaytracingInvalidateInstanceCache(&inst);

		if (!inst.alive)
			continue;

		D3D12_RAYTRACING_INSTANCE_DESC desc = {};
		D3D12_GPU_VIRTUAL_ADDRESS blasGpuVA = 0;
		if (!glRaytracingResolveInstanceDesc(&inst, &desc, &blasGpuVA))
			continue;

		inst.cachedActive = 1;
		inst.cachedBlasGpuVA = blasGpuVA;
		inst.cachedDescCpu = desc;
		inst.dirty = 0;

		g_glRaytracingScene.activeInstanceIndices.push_back((int)i);
		g_glRaytracingScene.cpuInstanceDescs.push_back(desc);
	}

	g_glRaytracingScene.activeInstanceCount = (UINT)g_glRaytracingScene.cpuInstanceDescs.size();
	return 1;
}

static int glRaytracingRefreshDirtyInstanceCache(void)
{
	for (size_t listIndex = 0; listIndex < g_glRaytracingScene.activeInstanceIndices.size(); ++listIndex)
	{
		const int instIndex = g_glRaytracingScene.activeInstanceIndices[listIndex];
		if (instIndex < 0 || (size_t)instIndex >= g_glRaytracingScene.instances.size())
			return 0;

		glRaytracingInstanceRecord_t& inst = g_glRaytracingScene.instances[(size_t)instIndex];
		if (!inst.alive)
			return 0;

		D3D12_RAYTRACING_INSTANCE_DESC desc = {};
		D3D12_GPU_VIRTUAL_ADDRESS blasGpuVA = 0;
		if (!glRaytracingResolveInstanceDesc(&inst, &desc, &blasGpuVA))
			return 0;

		if (inst.dirty || !inst.cachedActive || inst.cachedBlasGpuVA != blasGpuVA)
		{
			inst.cachedActive = 1;
			inst.cachedBlasGpuVA = blasGpuVA;
			inst.cachedDescCpu = desc;
			g_glRaytracingScene.cpuInstanceDescs[listIndex] = desc;
		}

		inst.dirty = 0;
	}

	g_glRaytracingScene.activeInstanceCount = (UINT)g_glRaytracingScene.cpuInstanceDescs.size();
	return 1;
}

static int glRaytracingEnsureSceneUploadBuffer(UINT64 requiredBytes);
static int glRaytracingUploadCachedInstanceDescs(void)
{
	const UINT activeCount = (UINT)g_glRaytracingScene.cpuInstanceDescs.size();
	const UINT64 instBytes = glRaytracingAlignUp(
		(UINT64)activeCount * (UINT64)sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
		D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);

	if (!glRaytracingEnsureSceneUploadBuffer(instBytes))
		return 0;

	glRaytracingSceneUploadBuffer_t* upload = glRaytracingGetBuildInstanceUpload();
	if (!upload->mapped)
		return 0;

	if (activeCount > 0)
		memcpy(upload->mapped, g_glRaytracingScene.cpuInstanceDescs.data(), (size_t)activeCount * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));

	return 1;
}


static int glRaytracingUploadMeshBuffers(glRaytracingMeshRecord_t* mesh);

static int glRaytracingBuildDirtyMeshesInternal(void)
{
	std::vector<glRaytracingMeshRecord_t*> dirtyMeshes;
	dirtyMeshes.reserve(g_glRaytracingScene.meshes.size());

	for (size_t i = 0; i < g_glRaytracingScene.meshes.size(); ++i)
	{
		glRaytracingMeshRecord_t& mesh = g_glRaytracingScene.meshes[i];
		if (!mesh.alive)
			continue;

		if (!mesh.blasBuilt || mesh.dirty)
			dirtyMeshes.push_back(&mesh);
	}

	if (dirtyMeshes.empty())
		return 1;

	struct glRaytracingMeshBuildInfo_t
	{
		glRaytracingMeshRecord_t* mesh;
		D3D12_RAYTRACING_GEOMETRY_DESC geomDesc;
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs;
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
		ID3D12Resource* barrierResource;
		int newBlasIndex;
	};

	std::vector<glRaytracingMeshBuildInfo_t> builds;
	builds.resize(dirtyMeshes.size());

	for (size_t i = 0; i < dirtyMeshes.size(); ++i)
	{
		glRaytracingMeshRecord_t* mesh = dirtyMeshes[i];
		if (!mesh->vertexBuffer.resource || !mesh->indexBuffer.resource)
		{
			if (!glRaytracingUploadMeshBuffers(mesh))
				return 0;
		}

		glRaytracingMeshBuildInfo_t& info = builds[i];
		memset(&info, 0, sizeof(info));
		info.mesh = mesh;

		info.geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		info.geomDesc.Flags = mesh->descCpu.opaque
			? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
			: D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
		info.geomDesc.Triangles.Transform3x4 = 0;
		info.geomDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		info.geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		info.geomDesc.Triangles.IndexCount = (UINT)mesh->indicesCpu.size();
		info.geomDesc.Triangles.VertexCount = (UINT)mesh->verticesCpu.size();
		info.geomDesc.Triangles.IndexBuffer = mesh->indexBuffer.gpuVA;
		info.geomDesc.Triangles.VertexBuffer.StartAddress = mesh->vertexBuffer.gpuVA;
		info.geomDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(glRaytracingVertex_t);

		info.inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		info.inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		info.inputs.NumDescs = 1;
		info.inputs.pGeometryDescs = &info.geomDesc;
		info.inputs.Flags = mesh->descCpu.allowUpdate
			? (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)
			: D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

		const bool canUpdateInPlace = (mesh->blasBuilt != 0) && (mesh->descCpu.allowUpdate != 0);
		if (canUpdateInPlace)
			info.inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
		g_glRaytracingCmd.device->GetRaytracingAccelerationStructurePrebuildInfo(&info.inputs, &prebuild);
		if (prebuild.ResultDataMaxSizeInBytes == 0)
		{
			glRaytracingFatal("BLAS prebuild size is zero");
			return 0;
		}

		if (!glRaytracingEnsureMeshScratch(mesh, prebuild.ScratchDataSizeInBytes))
			return 0;
		if (!glRaytracingEnsureMeshResultBuffers(mesh, prebuild.ResultDataMaxSizeInBytes))
			return 0;

		const int oldIndex = mesh->currentBlasIndex & 1;
		info.newBlasIndex = (mesh->descCpu.allowUpdate && mesh->blasBuilt) ? (oldIndex ^ 1) : oldIndex;

		info.buildDesc.Inputs = info.inputs;
		info.buildDesc.ScratchAccelerationStructureData = mesh->blasScratch.gpuVA;
		info.buildDesc.DestAccelerationStructureData = mesh->blasResult[info.newBlasIndex].gpuVA;
		info.buildDesc.SourceAccelerationStructureData = 0;

		if (canUpdateInPlace)
			info.buildDesc.SourceAccelerationStructureData = mesh->blasResult[oldIndex].gpuVA;

		info.barrierResource = mesh->blasResult[info.newBlasIndex].resource.Get();
	}

	if (!glRaytracingBeginBlasCmd())
		return 0;

	for (size_t i = 0; i < builds.size(); ++i)
	{
		g_glRaytracingCmd.blasCmdList->BuildRaytracingAccelerationStructure(&builds[i].buildDesc, 0, nullptr);

		D3D12_RESOURCE_BARRIER uav = {};
		uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		uav.UAV.pResource = builds[i].barrierResource;
		g_glRaytracingCmd.blasCmdList->ResourceBarrier(1, &uav);
	}

	const UINT64 blasFenceValue = glRaytracingEndBlasCmd();
	if (!blasFenceValue)
		return 0;

	glRaytracingWaitFenceValue(blasFenceValue);

	for (size_t i = 0; i < builds.size(); ++i)
	{
		glRaytracingMeshRecord_t* mesh = builds[i].mesh;
		mesh->currentBlasIndex = builds[i].newBlasIndex;
		mesh->blasBuilt = 1;
		mesh->dirty = 0;
	}

	g_glRaytracingScene.tlasNeedsRebuild = 1;
	g_glRaytracingScene.tlasNeedsUpdate = 0;
	return 1;
}

static int glRaytracingUploadMeshBuffers(glRaytracingMeshRecord_t* mesh)
{
	if (!mesh)
		return 0;

	if (mesh->verticesCpu.empty() || mesh->indicesCpu.empty())
		return 0;

	const UINT64 vbBytes = UINT64(mesh->verticesCpu.size()) * sizeof(glRaytracingVertex_t);
	const UINT64 ibBytes = UINT64(mesh->indicesCpu.size()) * sizeof(uint32_t);

	mesh->vertexBuffer = glRaytracingCreateBuffer(
		g_glRaytracingCmd.device.Get(),
		vbBytes,
		D3D12_HEAP_TYPE_UPLOAD,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		D3D12_RESOURCE_FLAG_NONE);

	mesh->indexBuffer = glRaytracingCreateBuffer(
		g_glRaytracingCmd.device.Get(),
		ibBytes,
		D3D12_HEAP_TYPE_UPLOAD,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		D3D12_RESOURCE_FLAG_NONE);

	if (!mesh->vertexBuffer.resource || !mesh->indexBuffer.resource)
		return 0;

	glRaytracingMapCopy(mesh->vertexBuffer.resource.Get(), mesh->verticesCpu.data(), (size_t)vbBytes);
	glRaytracingMapCopy(mesh->indexBuffer.resource.Get(), mesh->indicesCpu.data(), (size_t)ibBytes);

	return 1;
}

static int glRaytracingBuildMeshInternal(glRaytracingMeshRecord_t* mesh)
{
	if (!mesh)
		return 0;

	const int oldDirty = mesh->dirty;
	mesh->dirty = 1;
	const int ok = glRaytracingBuildDirtyMeshesInternal();
	if (!ok)
		mesh->dirty = oldDirty;
	return ok;
}

static int glRaytracingEnsureSceneUploadBuffer(UINT64 requiredBytes)
{
	glRaytracingSceneUploadBuffer_t* upload = glRaytracingGetBuildInstanceUpload();

	if (requiredBytes == 0)
		requiredBytes = D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT;

	requiredBytes = glRaytracingAlignUp(
		requiredBytes,
		D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);

	if (upload->buffer.resource &&
		upload->capacityBytes >= requiredBytes &&
		upload->mapped)
	{
		return 1;
	}

	if (upload->buffer.resource && upload->mapped)
		upload->buffer.resource->Unmap(0, nullptr);

	upload->mapped = nullptr;
	upload->buffer.resource.Reset();
	upload->capacityBytes = 0;

	upload->buffer = glRaytracingCreateBuffer(
		g_glRaytracingCmd.device.Get(),
		requiredBytes,
		D3D12_HEAP_TYPE_UPLOAD,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		D3D12_RESOURCE_FLAG_NONE);

	if (!upload->buffer.resource)
		return 0;

	void* mapped = nullptr;
	D3D12_RANGE readRange = {};
	if (FAILED(upload->buffer.resource->Map(0, &readRange, &mapped)) || !mapped)
	{
		upload->buffer.resource.Reset();
		return 0;
	}

	upload->mapped = (D3D12_RAYTRACING_INSTANCE_DESC*)mapped;
	upload->capacityBytes = requiredBytes;
	return 1;
}

struct glRaytracingResolvedInstance_t
{
	const glRaytracingInstanceRecord_t* inst;
	D3D12_GPU_VIRTUAL_ADDRESS blasGpuVA;
};

static int glRaytracingBuildSceneInternal(void)
{
	UINT aliveCount = 0;
	int anyDirty = 0;
	int needsRebuild = g_glRaytracingScene.tlasNeedsRebuild;
	int needsUpdate = g_glRaytracingScene.tlasNeedsUpdate;

	for (size_t i = 0; i < g_glRaytracingScene.instances.size(); ++i)
	{
		const glRaytracingInstanceRecord_t& inst = g_glRaytracingScene.instances[i];
		if (!inst.alive)
			continue;

		++aliveCount;
		if (inst.dirty)
			anyDirty = 1;
	}

	if (aliveCount == 0)
	{
		g_glRaytracingScene.activeInstanceIndices.clear();
		g_glRaytracingScene.cpuInstanceDescs.clear();
		g_glRaytracingScene.activeInstanceCount = 0;
		g_glRaytracingScene.builtInstanceCount = 0;
		g_glRaytracingScene.tlasBuilt = 0;
		g_glRaytracingScene.tlasNeedsRebuild = 0;
		g_glRaytracingScene.tlasNeedsUpdate = 0;
		return 1;
	}

	if (!g_glRaytracingScene.tlasBuilt)
		needsRebuild = 1;

	if ((UINT)g_glRaytracingScene.activeInstanceIndices.size() != g_glRaytracingScene.builtInstanceCount)
		needsRebuild = 1;

	if (needsRebuild)
	{
		if (!glRaytracingRebuildActiveInstanceCache())
			return 0;
	}
	else
	{
		if (!needsUpdate && !anyDirty)
		{
			g_glRaytracingScene.activeInstanceCount = (UINT)g_glRaytracingScene.cpuInstanceDescs.size();
			return 1;
		}

		if (!glRaytracingRefreshDirtyInstanceCache())
		{
			g_glRaytracingScene.tlasNeedsRebuild = 1;
			if (!glRaytracingRebuildActiveInstanceCache())
				return 0;
			needsRebuild = 1;
		}
	}

	const UINT activeCount = (UINT)g_glRaytracingScene.cpuInstanceDescs.size();
	if (activeCount == 0)
	{
		g_glRaytracingScene.activeInstanceCount = 0;
		g_glRaytracingScene.builtInstanceCount = 0;
		g_glRaytracingScene.tlasBuilt = 0;
		g_glRaytracingScene.tlasNeedsRebuild = 0;
		g_glRaytracingScene.tlasNeedsUpdate = 0;
		return 1;
	}

	if (!g_glRaytracingScene.tlasBuilt || activeCount != g_glRaytracingScene.builtInstanceCount)
		needsRebuild = 1;

	if (!glRaytracingUploadCachedInstanceDescs())
		return 0;

	glRaytracingSceneUploadBuffer_t* upload = glRaytracingGetBuildInstanceUpload();

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.NumDescs = activeCount;
	inputs.InstanceDescs = upload->buffer.gpuVA;
	inputs.Flags =
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;

	if (!glRaytracingEnsureTLASBuffers(&inputs))
		return 0;

	glRaytracingBuffer_t* dstTLAS = glRaytracingGetBuildTLASBuffer();
	const glRaytracingBuffer_t* srcTLAS = glRaytracingGetCurrentTLASBufferConst();

	glRaytracingWaitFenceValue(g_glRaytracingCmd.blasLastFenceValue);

	if (!glRaytracingBeginTlasCmd())
		return 0;

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
	buildDesc.Inputs = inputs;
	buildDesc.ScratchAccelerationStructureData = g_glRaytracingScene.tlasScratch.gpuVA;
	buildDesc.DestAccelerationStructureData = dstTLAS->gpuVA;
	buildDesc.SourceAccelerationStructureData = 0;

	if (!needsRebuild && g_glRaytracingScene.tlasBuilt)
	{
		buildDesc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
		buildDesc.SourceAccelerationStructureData = srcTLAS->gpuVA;
	}

	g_glRaytracingCmd.tlasCmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	D3D12_RESOURCE_BARRIER uav = {};
	uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uav.UAV.pResource = dstTLAS->resource.Get();
	g_glRaytracingCmd.tlasCmdList->ResourceBarrier(1, &uav);

	const UINT64 tlasFenceValue = glRaytracingEndTlasCmd();
	if (!tlasFenceValue)
		return 0;

	glRaytracingWaitFenceValue(tlasFenceValue);

	g_glRaytracingScene.currentTLASIndex = glRaytracingGetInactiveTLASIndex();
	g_glRaytracingScene.activeInstanceCount = activeCount;
	g_glRaytracingScene.builtInstanceCount = activeCount;
	g_glRaytracingScene.tlasBuilt = 1;
	g_glRaytracingScene.tlasNeedsRebuild = 0;
	g_glRaytracingScene.tlasNeedsUpdate = 0;

	for (size_t i = 0; i < g_glRaytracingScene.instances.size(); ++i)
	{
		if (g_glRaytracingScene.instances[i].alive)
			g_glRaytracingScene.instances[i].dirty = 0;
	}

	return 1;
}

// ============================================================
// Scene public API
// ============================================================

int glRaytracingInit(void)
{
	if (g_glRaytracingScene.initialized)
		return 1;

	if (!glRaytracingInitCmdContext())
		return 0;

	g_glRaytracingScene.initialized = 1;
	glRaytracingLog("glRaytracingInit ok");
	return 1;
}

void glRaytracingShutdown(void)
{
	if (!g_glRaytracingScene.initialized)
		return;

	glRaytracingClear();
	g_glRaytracingScene = glRaytracingSceneState_t();
	glRaytracingShutdownCmdContext();
}

void glRaytracingClear(void)
{
	g_glRaytracingScene.meshes.clear();
	g_glRaytracingScene.instances.clear();
	g_glRaytracingScene.activeInstanceIndices.clear();
	g_glRaytracingScene.cpuInstanceDescs.clear();
	g_glRaytracingScene.meshHandleToIndex.clear();
	g_glRaytracingScene.instanceHandleToIndex.clear();

	for (int i = 0; i < 2; ++i)
	{
		if (g_glRaytracingScene.instanceDescUpload[i].buffer.resource && g_glRaytracingScene.instanceDescUpload[i].mapped)
			g_glRaytracingScene.instanceDescUpload[i].buffer.resource->Unmap(0, nullptr);

		g_glRaytracingScene.instanceDescUpload[i] = glRaytracingSceneUploadBuffer_t();
		g_glRaytracingScene.tlasResult[i].resource.Reset();
	}

	g_glRaytracingScene.tlasScratch.resource.Reset();
	g_glRaytracingScene.tlasScratchSize = 0;
	g_glRaytracingScene.tlasResultSize = 0;
	g_glRaytracingScene.activeInstanceCount = 0;
	g_glRaytracingScene.builtInstanceCount = 0;
	g_glRaytracingScene.tlasBuilt = 0;
	g_glRaytracingScene.tlasNeedsRebuild = 1;
	g_glRaytracingScene.tlasNeedsUpdate = 1;
	g_glRaytracingScene.currentTLASIndex = 0;
}

glRaytracingMeshHandle_t glRaytracingCreateMesh(const glRaytracingMeshDesc_t* desc)
{
	if (!g_glRaytracingScene.initialized || !desc)
		return 0;

	if (!desc->vertices || !desc->indices || desc->vertexCount == 0 || desc->indexCount == 0)
		return 0;

	glRaytracingMeshRecord_t mesh;
	mesh.handle = g_glRaytracingScene.nextMeshHandle++;
	mesh.alive = 1;
	mesh.descCpu = *desc;
	mesh.verticesCpu.assign(desc->vertices, desc->vertices + desc->vertexCount);
	mesh.indicesCpu.assign(desc->indices, desc->indices + desc->indexCount);
	mesh.descCpu.vertices = nullptr;
	mesh.descCpu.indices = nullptr;
	mesh.dirty = 1;

	g_glRaytracingScene.meshes.push_back(mesh);
	const size_t newIndex = g_glRaytracingScene.meshes.size() - 1;
	glRaytracingEnsureMeshHandleTable(mesh.handle);
	g_glRaytracingScene.meshHandleToIndex[mesh.handle] = (int)newIndex;

	return mesh.handle;
}

int glRaytracingUpdateMesh(glRaytracingMeshHandle_t meshHandle, const glRaytracingMeshDesc_t* desc)
{
	if (!g_glRaytracingScene.initialized || !desc)
		return 0;

	glRaytracingMeshRecord_t* mesh = glRaytracingFindMesh(meshHandle);
	if (!mesh)
		return 0;

	if (!desc->vertices || !desc->indices || desc->vertexCount == 0 || desc->indexCount == 0)
		return 0;

	mesh->descCpu = *desc;
	mesh->verticesCpu.assign(desc->vertices, desc->vertices + desc->vertexCount);
	mesh->indicesCpu.assign(desc->indices, desc->indices + desc->indexCount);
	mesh->descCpu.vertices = nullptr;
	mesh->descCpu.indices = nullptr;

	mesh->vertexBuffer.resource.Reset();
	mesh->indexBuffer.resource.Reset();
	mesh->blasScratch.resource.Reset();
	mesh->blasResult[0].resource.Reset();
	mesh->blasResult[1].resource.Reset();
	mesh->blasBuilt = 0;
	mesh->dirty = 1;
	mesh->currentBlasIndex = 0;

	for (size_t i = 0; i < g_glRaytracingScene.instances.size(); ++i)
	{
		if (g_glRaytracingScene.instances[i].alive &&
			g_glRaytracingScene.instances[i].descCpu.meshHandle == meshHandle)
		{
			glRaytracingInvalidateInstanceCache(&g_glRaytracingScene.instances[i]);
			g_glRaytracingScene.instances[i].dirty = 1;
		}
	}
	g_glRaytracingScene.tlasNeedsRebuild = 1;
	g_glRaytracingScene.tlasNeedsUpdate = 0;

	return 1;
}

void glRaytracingDeleteMesh(glRaytracingMeshHandle_t meshHandle)
{
	glRaytracingMeshRecord_t* mesh = glRaytracingFindMesh(meshHandle);
	if (!mesh)
		return;

	for (size_t i = 0; i < g_glRaytracingScene.instances.size(); ++i)
	{
		if (g_glRaytracingScene.instances[i].alive &&
			g_glRaytracingScene.instances[i].descCpu.meshHandle == meshHandle)
		{
			glRaytracingInvalidateInstanceCache(&g_glRaytracingScene.instances[i]);
			g_glRaytracingScene.instances[i].alive = 0;
			if (g_glRaytracingScene.instances[i].handle < g_glRaytracingScene.instanceHandleToIndex.size())
				g_glRaytracingScene.instanceHandleToIndex[g_glRaytracingScene.instances[i].handle] = -1;
		}
	}

	mesh->alive = 0;

	if (meshHandle < g_glRaytracingScene.meshHandleToIndex.size())
		g_glRaytracingScene.meshHandleToIndex[meshHandle] = -1;

	g_glRaytracingScene.tlasNeedsRebuild = 1;
}

glRaytracingInstanceHandle_t glRaytracingCreateInstance(const glRaytracingInstanceDesc_t* desc)
{
	if (!g_glRaytracingScene.initialized || !desc)
		return 0;

	if (!glRaytracingFindMeshConst(desc->meshHandle))
		return 0;

	glRaytracingInstanceRecord_t inst;
	inst.handle = g_glRaytracingScene.nextInstanceHandle++;
	inst.alive = 1;
	inst.descCpu = *desc;
	inst.dirty = 1;

	g_glRaytracingScene.instances.push_back(inst);
	const size_t newIndex = g_glRaytracingScene.instances.size() - 1;
	glRaytracingEnsureInstanceHandleTable(inst.handle);
	g_glRaytracingScene.instanceHandleToIndex[inst.handle] = (int)newIndex;
	g_glRaytracingScene.tlasNeedsRebuild = 1;
	return inst.handle;
}

int glRaytracingUpdateInstance(glRaytracingInstanceHandle_t instanceHandle, const glRaytracingInstanceDesc_t* desc)
{
	if (!g_glRaytracingScene.initialized || !desc)
		return 0;

	if (!glRaytracingFindMeshConst(desc->meshHandle))
		return 0;

	glRaytracingInstanceRecord_t* inst = glRaytracingFindInstance(instanceHandle);
	if (!inst)
		return 0;

	const uint32_t oldMeshHandle = inst->descCpu.meshHandle;

	inst->descCpu = *desc;
	inst->dirty = 1;

	if (oldMeshHandle != desc->meshHandle)
	{
		glRaytracingInvalidateInstanceCache(inst);
		g_glRaytracingScene.tlasNeedsRebuild = 1;
	}
	else
	{
		g_glRaytracingScene.tlasNeedsUpdate = 1;
	}

	return 1;
}

void glRaytracingDeleteInstance(glRaytracingInstanceHandle_t instanceHandle)
{
	glRaytracingInstanceRecord_t* inst = glRaytracingFindInstance(instanceHandle);
	if (!inst)
		return;

	glRaytracingInvalidateInstanceCache(inst);
	inst->alive = 0;
	if (instanceHandle < g_glRaytracingScene.instanceHandleToIndex.size())
		g_glRaytracingScene.instanceHandleToIndex[instanceHandle] = -1;
	g_glRaytracingScene.tlasNeedsRebuild = 1;
	g_glRaytracingScene.tlasNeedsUpdate = 0;
}

int glRaytracingBuildMesh(glRaytracingMeshHandle_t meshHandle)
{
	if (!g_glRaytracingScene.initialized)
		return 0;

	glRaytracingMeshRecord_t* mesh = glRaytracingFindMesh(meshHandle);
	if (!mesh)
		return 0;

	return glRaytracingBuildMeshInternal(mesh);
}

int glRaytracingBuildAllMeshes(void)
{
	if (!g_glRaytracingScene.initialized)
		return 0;

	return glRaytracingBuildDirtyMeshesInternal();
}

int glRaytracingBuildScene(void)
{
	if (glRaytracingShouldAbortWork())
		return 0;

	if (!g_glRaytracingScene.initialized)
		return 0;

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

	if (!glRaytracingBuildAllMeshes())
		return 0;

	return glRaytracingBuildSceneInternal();
}

ID3D12Resource* glRaytracingGetTopLevelAS(void)
{
	return glRaytracingGetCurrentTLASBuffer()->resource.Get();
}

uint32_t glRaytracingGetMeshCount(void)
{
	uint32_t count = 0;
	for (size_t i = 0; i < g_glRaytracingScene.meshes.size(); ++i)
	{
		if (g_glRaytracingScene.meshes[i].alive)
			++count;
	}
	return count;
}

uint32_t glRaytracingGetInstanceCount(void)
{
	uint32_t count = 0;
	for (size_t i = 0; i < g_glRaytracingScene.instances.size(); ++i)
	{
		if (g_glRaytracingScene.instances[i].alive)
			++count;
	}
	return count;
}

// ============================================================
// Lighting state
// ============================================================

struct glRaytracingLightingConstants_t
{
	float invViewProj[16];
	float invViewMatrix[16];
	float cameraPos[4];
	float ambientColor[4];
	float screenSize[4];
	float normalReconstructZ;
	uint32_t lightCount;
	uint32_t enableSpecular;
	uint32_t enableHalfLambert;
	float shadowBias;
	float exposure;
	float legacyBlend;
	uint32_t debugMode;
	glRaytracingEffectsOptions_t effects;
	uint32_t historyValid;
	uint32_t passMode;
	uint32_t padConstants0;
	uint32_t padConstants1;
};

static_assert(sizeof(glRaytracingEffectsOptions_t) == 256, "DXR Lab effects constants must stay cbuffer-compatible");
static_assert(sizeof(glRaytracingLightingConstants_t) == 480, "DXR Lab lighting constants layout changed");

struct glRaytracingLightingState_t
{
	std::vector<glRaytracingLight_t> cpuLights;
	glRaytracingLightingConstants_t constants;

	ComPtr<ID3D12DescriptorHeap> descriptorHeap;
	UINT descriptorStride;

	glRaytracingBuffer_t constantBuffer;
	glRaytracingBuffer_t resolveConstantBuffer;
	glRaytracingBuffer_t lightBuffer;

	ComPtr<ID3D12Resource> labRawTexture;
	ComPtr<ID3D12Resource> labHistoryTexture;
	D3D12_RESOURCE_STATES labRawState;
	D3D12_RESOURCE_STATES labHistoryState;
	uint32_t labWidth;
	uint32_t labHeight;
	DXGI_FORMAT labFormat;
	int historyValid;
	int havePreviousView;
	float previousView[16];

	ComPtr<ID3D12RootSignature> globalRootSig;
	ComPtr<ID3D12RootSignature> localRootSig;

	ComPtr<ID3D12StateObject> rtStateObject;
	ComPtr<ID3D12StateObjectProperties> rtStateProps;

	glRaytracingBuffer_t raygenTable;
	glRaytracingBuffer_t missTable;
	glRaytracingBuffer_t hitTable;

	bool initialized;

	glRaytracingLightingState_t()
	{
		memset(&constants, 0, sizeof(constants));
		descriptorStride = 0;
		labRawState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		labHistoryState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		labWidth = 0;
		labHeight = 0;
		labFormat = DXGI_FORMAT_UNKNOWN;
		historyValid = 0;
		havePreviousView = 0;
		memset(previousView, 0, sizeof(previousView));
		initialized = false;
	}
};

static glRaytracingLightingState_t g_glRaytracingLighting;
static const UINT GL_RAYTRACING_LAB_DESCRIPTORS_PER_PASS = 9;
static const UINT GL_RAYTRACING_LAB_PASS_COUNT = 2;
static const char* const g_glRaytracingLightingHlslParts[] =
{
R"DXRHLSL(struct Light
{
    float3 position;
    float  radius;
    float3 color;
    float  intensity;
    float3 normal;
    uint   type;
    float3 axisU;
    float  halfWidth;
    float3 axisV;
    float  halfHeight;
    uint   samples;
    uint   twoSided;
    float  persistant;
    float  pad1;
};

struct ShadowPayload
{
    uint hit;
};

cbuffer LightingCB : register(b0)
{
    float4x4 gInvViewProj;
    float4x4 gInvViewMatrix;
    float4   gCameraPos;
    float4   gAmbientColor;
    float4   gScreenSize;
    float    gNormalReconstructZ;
    uint     gLightCount;
    uint     gEnableSpecular;
    uint     gEnableHalfLambert;
    float    gShadowBias;
    float    gExposure;
    float    gLegacyBlend;
    uint     gDebugMode;

    uint     gShadowsEnabled;
    float    gShadowStrength;
    uint     gShadowSamples;
    float    gShadowSoftness;

    float    gShadowMaxDistance;
    uint     gShadowCullMode;
    uint     gContactShadows;
    float    gContactShadowLength;

    uint     gSunEnabled;
    float    gSunIntensity;
    float    gSunAngularRadius;
    uint     gSunSamples;

    float4   gSunDirection;
    float4   gSunColor;

    uint     gDynamicLightsEnabled;
    uint     gDynamicLightShadows;
    uint     gMaxLights;
    float    gDynamicLightIntensityScale;

    float    gDynamicLightRadiusScale;
    uint     gAOEnabled;
    uint     gAOSamples;
    float    gAORadius;

    float    gAOStrength;
    uint     gReflectionsEnabled;
    uint     gReflectionSamples;
    float    gReflectionStrength;

    float    gReflectionMaxDistance;
    float    gReflectionRoughness;
    uint     gGIEnabled;
    uint     gGISamples;

    float    gGIStrength;
    float    gGIMaxDistance;
    uint     gDenoiserEnabled;
    uint     gDenoiserRadius;

    float    gDenoiserStrength;
    float    gDenoiserDepthSigma;
    float    gDenoiserNormalSigma;
    uint     gTemporalEnabled;

    float    gTemporalWeight;
    float    gTemporalClamp;
    float    gTemporalResetThreshold;
    uint     gSkyEnabled;

    float    gSkyStrength;
    uint     gSkySamples;
    float    gSkyMaxDistance;
    uint     gSpecularEnabled;

    float    gSpecularStrength;
    float    gSpecularPower;
    float    gShadowMinVisibility;
    uint     gTonemapMode;

    float    gHDRWhitePoint;
    float    gBloomStrength;
    float    gBloomThreshold;
    float    gSaturation;

    float    gContrast;
    float    gOutputGamma;
    uint     gFrameIndex;
    uint     gDebugEffect;

    uint     gHistoryValid;
    uint     gPassMode;
    uint     gPadConstants0;
    uint     gPadConstants1;
};

StructuredBuffer<Light> gLights : register(t0);
Texture2D<float4>       gAlbedoTex       : register(t1);
Texture2D<float>        gDepthTex        : register(t2);
Texture2D<float4>       gNormalTex       : register(t3);
Texture2D<float4>       gPositionTex     : register(t4);
RaytracingAccelerationStructure gSceneBVH : register(t5);
Texture2D<float4>       gRawLightingTex  : register(t6);
Texture2D<float4>       gHistoryTex      : register(t7);
RWTexture2D<float4>     gOutputTex       : register(u0);

static const uint GL_RAYTRACING_LIGHT_TYPE_POINT = 0;
static const uint GL_RAYTRACING_LIGHT_TYPE_RECT  = 1;
static const uint GEOMETRY_FLAG_SKELETAL = 1;
static const uint GEOMETRY_FLAG_UNLIT    = 2;

float3 LoadScenePosition(uint2 pixel)
{
    return gPositionTex.Load(int3(pixel, 0)).xyz;
}

float4 LoadSceneNormal(uint2 pixel)
{
    return gNormalTex.Load(int3(pixel, 0));
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.hit = 0;
}

[shader("closesthit")]
void ShadowClosestHit(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.hit = 1;
}

uint GetShadowRayFlags()
{
    uint flags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE;
    if (gShadowCullMode == 1)
        flags |= RAY_FLAG_CULL_FRONT_FACING_TRIANGLES;
    else if (gShadowCullMode == 2)
        flags |= RAY_FLAG_CULL_BACK_FACING_TRIANGLES;
    return flags;
}

float TraceVisibility(float3 origin, float3 dir, float maxT)
{
    if (maxT <= 0.001)
        return 1.0;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = normalize(dir);
    ray.TMin = 0.001;
    ray.TMax = maxT;

    ShadowPayload payload;
    payload.hit = 0;

    TraceRay(gSceneBVH, GetShadowRayFlags(), 0xFF, 0, 1, 0, ray, payload);
    return (payload.hit != 0) ? 0.0 : 1.0;
}

float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float SamplingSeed(uint2 pixel, float3 worldPos)
{
    float temporalJitter = (gTemporalEnabled != 0) ? ((float)gFrameIndex * 0.61803398875) : 0.0;
    return Hash12((float2)pixel + worldPos.xy * 0.071 + worldPos.zz * 0.013 + temporalJitter);
}

float2 Hammersley2D(uint i, uint N, float rand)
{
    float e1 = frac((float)i / max((float)N, 1.0) + rand);
    uint bits = i;
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    float e2 = (float)bits * 2.3283064365386963e-10;
    return float2(e1, e2);
}

float2 ConcentricSampleDisk(float2 u)
{
    float2 o = 2.0 * u - 1.0;
    if (abs(o.x) < 1e-6 && abs(o.y) < 1e-6)
        return 0.0;
    float r;
    float theta;
    if (abs(o.x) > abs(o.y))
    {
        r = o.x;
        theta = 0.78539816339 * (o.y / o.x);
    }
    else
    {
        r = o.y;
        theta = 1.57079632679 - 0.78539816339 * (o.x / o.y);
    }
    return r * float2(cos(theta), sin(theta));
}

void BuildOrthonormalBasis(float3 n, out float3 t, out float3 b)
{
    float3 up = (abs(n.z) < 0.999) ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

float3 CosineSampleHemisphere(float2 u)
{
    float2 d = ConcentricSampleDisk(u);
    return float3(d.x, d.y, sqrt(saturate(1.0 - dot(d, d))));
}

float FinalShadowVisibility(float visibility)
{
    visibility = max(visibility, saturate(gShadowMinVisibility));
    return lerp(1.0, visibility, saturate(gShadowStrength));
}

float PointLightShadow(float3 worldPos, float3 N, Light light, float3 toLight, float dist, uint2 pixel)
{
    if (gShadowsEnabled == 0 || gDynamicLightShadows == 0)
        return 1.0;
    if (dist > max(gShadowMaxDistance, 1.0))
        return 1.0;

    uint sampleCount = clamp(gShadowSamples, 1u, 16u);
    float3 centerDir = toLight / max(dist, 1e-5);
    float3 tangent, bitangent;
    BuildOrthonormalBasis(centerDir, tangent, bitangent);
    float areaRadius = max(light.radius * 0.03 * max(gShadowSoftness, 0.0), 0.12);
    float seed = SamplingSeed(pixel, worldPos);
    float visibility = 0.0;

    [loop]
    for (uint s = 0; s < sampleCount; ++s)
    {
)DXRHLSL",
R"DXRHLSL(        float2 disk = (sampleCount == 1) ? float2(0.0, 0.0) : ConcentricSampleDisk(Hammersley2D(s, sampleCount, seed));
        float3 samplePos = light.position + tangent * disk.x * areaRadius + bitangent * disk.y * areaRadius;
        float3 sampleVec = samplePos - worldPos;
        float sampleDist = length(sampleVec);
        float3 sampleDir = sampleVec / max(sampleDist, 1e-5);
        float ndl = saturate(dot(N, sampleDir));
        float bias = lerp(gShadowBias * 3.0, gShadowBias * 0.65, ndl);
        float3 origin = worldPos + N * bias + sampleDir * (gShadowBias * 0.35);
        visibility += TraceVisibility(origin, sampleDir, max(sampleDist - gShadowBias, 0.001));
    }
    visibility /= (float)sampleCount;

    if (gContactShadows != 0 && gContactShadowLength > 0.0)
    {
        float3 origin = worldPos + N * max(gShadowBias * 0.65, 0.002);
        float contact = TraceVisibility(origin, centerDir, min(dist, gContactShadowLength));
        visibility = min(visibility, contact);
    }

    return FinalShadowVisibility(visibility);
}

float RectLightShadow(float3 worldPos, float3 N, Light light, uint2 pixel)
{
    if (gShadowsEnabled == 0 || gDynamicLightShadows == 0)
        return 1.0;

    float centerDistance = length(light.position - worldPos);
    if (centerDistance > max(gShadowMaxDistance, 1.0))
        return 1.0;

    uint sampleCount = clamp(min(max(light.samples, 1u), max(gShadowSamples, 1u)), 1u, 16u);
    float seed = SamplingSeed(pixel, worldPos);
    float visibility = 0.0;

    [loop]
    for (uint s = 0; s < sampleCount; ++s)
    {
        float2 uv = ((sampleCount == 1) ? float2(0.5, 0.5) : Hammersley2D(s, sampleCount, seed)) * 2.0 - 1.0;
        uv *= max(gShadowSoftness, 0.0);
        float3 samplePos = light.position + light.axisU * (uv.x * light.halfWidth) + light.axisV * (uv.y * light.halfHeight);
        float3 toLight = samplePos - worldPos;
        float dist = length(toLight);
        float3 L = toLight / max(dist, 1e-5);
        float emit = (light.twoSided != 0) ? abs(dot(light.normal, -L)) : dot(light.normal, -L);
        if (dot(N, L) <= 0.0 || emit <= 0.0)
            continue;
        float3 origin = worldPos + N * max(gShadowBias, 0.002) + L * (gShadowBias * 0.35);
        visibility += TraceVisibility(origin, L, max(dist - gShadowBias, 0.001));
    }
    visibility /= (float)sampleCount;
    return FinalShadowVisibility(visibility);
}

float SunShadow(float3 worldPos, float3 N, uint2 pixel)
{
    if (gShadowsEnabled == 0 || gSunEnabled == 0)
        return 1.0;

    float3 sunDir = normalize(gSunDirection.xyz);
    uint sampleCount = clamp(gSunSamples, 1u, 16u);
    float3 tangent, bitangent;
    BuildOrthonormalBasis(sunDir, tangent, bitangent);
    float angular = tan(radians(max(gSunAngularRadius, 0.0)));
    float seed = SamplingSeed(pixel, worldPos + sunDir);
    float visibility = 0.0;

    [loop]
    for (uint s = 0; s < sampleCount; ++s)
    {
        float2 disk = (sampleCount == 1) ? float2(0.0, 0.0) : ConcentricSampleDisk(Hammersley2D(s, sampleCount, seed));
        float3 L = normalize(sunDir + tangent * disk.x * angular + bitangent * disk.y * angular);
        float3 origin = worldPos + N * max(gShadowBias, 0.002) + L * (gShadowBias * 0.35);
        visibility += TraceVisibility(origin, L, max(gShadowMaxDistance, 64.0));
    }
    visibility /= (float)sampleCount;

    if (gContactShadows != 0 && gContactShadowLength > 0.0)
    {
        float3 origin = worldPos + N * max(gShadowBias * 0.65, 0.002);
        visibility = min(visibility, TraceVisibility(origin, sunDir, gContactShadowLength));
    }
    return FinalShadowVisibility(visibility);
}

float ComputeAmbientOcclusion(float3 worldPos, float3 N, uint2 pixel)
{
    if (gAOEnabled == 0 || gAOStrength <= 0.0)
        return 1.0;
    uint sampleCount = clamp(gAOSamples, 1u, 32u);
    float3 tangent, bitangent;
    BuildOrthonormalBasis(N, tangent, bitangent);
    float seed = SamplingSeed(pixel, worldPos + N);
    float visibility = 0.0;
    [loop]
    for (uint i = 0; i < sampleCount; ++i)
    {
        float3 h = CosineSampleHemisphere(Hammersley2D(i, sampleCount, seed));
        float3 dir = normalize(tangent * h.x + bitangent * h.y + N * h.z);
        float3 origin = worldPos + N * max(gShadowBias * 0.5, 0.002);
        visibility += TraceVisibility(origin, dir, max(gAORadius, 1.0));
    }
    visibility /= (float)sampleCount;
    visibility = saturate(pow(visibility, 1.35));
    return lerp(1.0, visibility, saturate(gAOStrength));
}

float ComputeSkyVisibility(float3 worldPos, float3 N, uint2 pixel)
{
    if (gSkyEnabled == 0)
        return 0.0;
    uint sampleCount = clamp(gSkySamples, 1u, 16u);
    float3 tangent, bitangent;
    BuildOrthonormalBasis(N, tangent, bitangent);
    float seed = SamplingSeed(pixel, worldPos + N * 3.17);
    float visibility = 0.0;
    [loop]
    for (uint i = 0; i < sampleCount; ++i)
    {
        float3 h = CosineSampleHemisphere(Hammersley2D(i, sampleCount, seed));
        float3 dir = normalize(tangent * h.x + bitangent * h.y + N * h.z);
        visibility += TraceVisibility(worldPos + N * max(gShadowBias, 0.002), dir, max(gSkyMaxDistance, 64.0));
    }
    return visibility / (float)sampleCount;
}

float3 SkyColorForDirection(float3 dir)
{
    float t = saturate(dir.z * 0.5 + 0.5);
    return lerp(float3(0.16, 0.18, 0.22), float3(0.52, 0.57, 0.66), t);
}

float3 ComputeReflection(float3 worldPos, float3 N, float3 V, uint2 pixel)
{
    if (gReflectionsEnabled == 0 || gReflectionStrength <= 0.0)
        return 0.0;
    uint sampleCount = clamp(gReflectionSamples, 1u, 8u);
    float3 R = normalize(reflect(-V, N));
    float3 tangent, bitangent;
    BuildOrthonormalBasis(R, tangent, bitangent);
    float seed = SamplingSeed(pixel, worldPos + R);
    float3 accum = 0.0;
    float roughness = saturate(gReflectionRoughness);
    [loop]
    for (uint i = 0; i < sampleCount; ++i)
    {
        float2 disk = (sampleCount == 1) ? float2(0.0, 0.0) : ConcentricSampleDisk(Hammersley2D(i, sampleCount, seed));
        float3 dir = normalize(R + (tangent * disk.x + bitangent * disk.y) * roughness);
        float visibility = TraceVisibility(worldPos + N * max(gShadowBias, 0.002), dir, max(gReflectionMaxDistance, 16.0));
        accum += SkyColorForDirection(dir) * visibility;
    }
    accum /= (float)sampleCount;
    float fresnel = 0.04 + 0.96 * pow(1.0 - saturate(dot(N, V)), 5.0);
    return accum * (gReflectionStrength * fresnel);
}

float3 ComputeGI(float3 worldPos, float3 N, float3 baseAlbedo, uint2 pixel)
{
    if (gGIEnabled == 0 || gGIStrength <= 0.0)
        return 0.0;
    uint sampleCount = clamp(gGISamples, 1u, 8u);
    float3 tangent, bitangent;
    BuildOrthonormalBasis(N, tangent, bitangent);
    float seed = SamplingSeed(pixel, worldPos + N * 7.31);
    float3 accum = 0.0;
    [loop]
    for (uint i = 0; i < sampleCount; ++i)
    {
        float3 h = CosineSampleHemisphere(Hammersley2D(i, sampleCount, seed));
)DXRHLSL",
R"DXRHLSL(        float3 dir = normalize(tangent * h.x + bitangent * h.y + N * h.z);
        float visibility = TraceVisibility(worldPos + N * max(gShadowBias, 0.002), dir, max(gGIMaxDistance, 8.0));
        accum += (SkyColorForDirection(dir) * 0.55 + gAmbientColor.rgb * 0.45) * visibility;
    }
    return baseAlbedo * (accum / (float)sampleCount) * gGIStrength;
}

float ComputeCavity(uint2 pixel, float3 worldPos, float3 N)
{
    static const int2 taps[12] =
    {
        int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2),
        int2(-2, -2), int2(2, -2), int2(-2, 2), int2(2, 2),
        int2(-4, 0), int2(4, 0), int2(0, -4), int2(0, 4)
    };
    float accum = 0.0;
    float weightSum = 0.0;
    [unroll]
    for (int i = 0; i < 12; ++i)
    {
        int2 sp = int2(pixel) + taps[i];
        if (sp.x < 0 || sp.y < 0 || sp.x >= (int)gScreenSize.x || sp.y >= (int)gScreenSize.y)
            continue;
        float3 samplePos = gPositionTex.Load(int3(sp, 0)).xyz;
        float3 sampleN = normalize(gNormalTex.Load(int3(sp, 0)).xyz);
        float3 d = samplePos - worldPos;
        float distSq = dot(d, d);
        if (distSq > 576.0 || dot(N, sampleN) < 0.65)
            continue;
        float curvature = 1.0 - saturate(dot(N, sampleN));
        float w = 1.0 / (1.0 + distSq * 0.02);
        accum += curvature * w;
        weightSum += w;
    }
    float cavity = (weightSum > 0.0) ? saturate((accum / weightSum) * 2.0) : 0.0;
    return 1.0 - cavity * 0.10;
}

float3 ComputeSpecular(float3 N, float3 V, float3 L, float3 lightColor, float intensity, float attenuation, float shadow, float3 baseAlbedo)
{
    if (gEnableSpecular == 0 || gSpecularEnabled == 0 || gSpecularStrength <= 0.0)
        return 0.0;
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));
    if (NdotL <= 0.0 || NdotV <= 0.0)
        return 0.0;
    float power = clamp(gSpecularPower, 2.0, 256.0);
    float specPow = pow(NdotH, power);
    float3 dielectricF0 = 0.04;
    float metalHint = saturate((max(max(baseAlbedo.r, baseAlbedo.g), baseAlbedo.b) - 0.75) * 1.5);
    float3 F0 = lerp(dielectricF0, baseAlbedo, metalHint);
    float3 fresnel = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    float energyNorm = (power + 8.0) * 0.125;
    return lightColor * (intensity * attenuation * shadow * NdotL) * fresnel * specPow * energyNorm * gSpecularStrength;
}

float3 ApplyOutputPost(float3 color)
{
    color = max(color, 0.0);
    if (gBloomStrength > 0.0)
        color += max(color - gBloomThreshold, 0.0) * gBloomStrength;
    if (gTonemapMode == 1)
    {
        float whitePoint = max(gHDRWhitePoint, 0.1);
        color = (color * (1.0 + color / (whitePoint * whitePoint))) / (1.0 + color);
    }
    else if (gTonemapMode == 2)
    {
        color = saturate((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14));
    }
    float luma = dot(color, float3(0.299, 0.587, 0.114));
    color = lerp(luma.xxx, color, max(gSaturation, 0.0));
    color = (color - 0.5) * max(gContrast, 0.0) + 0.5;
    color = pow(max(color, 0.0), 1.0 / max(gOutputGamma, 0.1));
    return color;
}

bool NeedsResolvePass()
{
    return gDenoiserEnabled != 0 || gTemporalEnabled != 0 || gTonemapMode != 0 ||
        gBloomStrength > 0.0 || abs(gSaturation - 1.0) > 0.001 ||
        abs(gContrast - 1.0) > 0.001 || abs(gOutputGamma - 1.0) > 0.001;
}

float4 ResolveLabPixel(uint2 pixel)
{
    float4 centerSample = gRawLightingTex.Load(int3(pixel, 0));
    float centerDepth = gDepthTex.Load(int3(pixel, 0));
    if (centerDepth <= 0.0 || centerDepth >= 1.0)
        return float4(ApplyOutputPost(centerSample.rgb), centerSample.a);

    float3 centerAlbedo = gAlbedoTex.Load(int3(pixel, 0)).rgb;
    float3 stableBase = centerAlbedo * saturate(gLegacyBlend) * max(gExposure, 0.001);
    float3 currentResidual = centerSample.rgb - stableBase;
    float3 centerNormal = normalize(gNormalTex.Load(int3(pixel, 0)).xyz);
    float3 centerPosition = gPositionTex.Load(int3(pixel, 0)).xyz;

    // Filter only the RT/light residual. The original full-resolution albedo
    // stays at the center pixel, so HD texture packs and fine RTCW details are
    // not blurred by the denoiser.
    if (gDenoiserEnabled != 0 && gDenoiserRadius > 0)
    {
        int radius = min((int)gDenoiserRadius, 3);
        float3 accumResidual = 0.0;
        float weightSum = 0.0;
        [loop]
        for (int y = -3; y <= 3; ++y)
        {
            [loop]
            for (int x = -3; x <= 3; ++x)
            {
                if (abs(x) > radius || abs(y) > radius)
                    continue;
                int2 sp = int2(pixel) + int2(x, y);
                if (sp.x < 0 || sp.y < 0 || sp.x >= (int)gScreenSize.x || sp.y >= (int)gScreenSize.y)
                    continue;
                float sampleDepth = gDepthTex.Load(int3(sp, 0));
                if (sampleDepth <= 0.0 || sampleDepth >= 1.0)
                    continue;
                float3 sampleColor = gRawLightingTex.Load(int3(sp, 0)).rgb;
                float3 sampleAlbedo = gAlbedoTex.Load(int3(sp, 0)).rgb;
                float3 sampleBase = sampleAlbedo * saturate(gLegacyBlend) * max(gExposure, 0.001);
                float3 sampleResidual = sampleColor - sampleBase;
                float3 sampleNormal = normalize(gNormalTex.Load(int3(sp, 0)).xyz);
                float3 samplePosition = gPositionTex.Load(int3(sp, 0)).xyz;
                float spatial = exp(-0.5 * (float)(x * x + y * y) / max((float)(radius * radius), 1.0));
                float depthWeight = exp(-abs(sampleDepth - centerDepth) * max(gDenoiserDepthSigma, 0.0));
                float normalWeight = pow(saturate(dot(centerNormal, sampleNormal)), max(gDenoiserNormalSigma, 1.0));
                float positionWeight = exp(-length(samplePosition - centerPosition) * 0.035);
                float3 delta = sampleResidual - currentResidual;
                float colorWeight = 1.0 / (1.0 + dot(delta, delta) * 3.0);
                float w = spatial * depthWeight * normalWeight * positionWeight * colorWeight;
                accumResidual += sampleResidual * w;
                weightSum += w;
            }
        }
        float3 filteredResidual = (weightSum > 0.0) ? (accumResidual / weightSum) : currentResidual;
        currentResidual = lerp(currentResidual, filteredResidual, saturate(gDenoiserStrength));
    }

    if (gTemporalEnabled != 0 && gHistoryValid != 0)
    {
        float3 historyResidual = gHistoryTex.Load(int3(pixel, 0)).rgb - stableBase;
        float3 minResidual = currentResidual;
        float3 maxResidual = currentResidual;
        [unroll]
        for (int yy = -1; yy <= 1; ++yy)
        {
            [unroll]
            for (int xx = -1; xx <= 1; ++xx)
            {
                int2 sp = clamp(int2(pixel) + int2(xx, yy), int2(0, 0), int2((int)gScreenSize.x - 1, (int)gScreenSize.y - 1));
)DXRHLSL",
R"DXRHLSL(                float3 c = gRawLightingTex.Load(int3(sp, 0)).rgb;
                float3 a = gAlbedoTex.Load(int3(sp, 0)).rgb;
                float3 residual = c - a * saturate(gLegacyBlend) * max(gExposure, 0.001);
                minResidual = min(minResidual, residual);
                maxResidual = max(maxResidual, residual);
            }
        }
        float clampAmount = max(gTemporalClamp, 0.0);
        historyResidual = clamp(historyResidual, minResidual - clampAmount, maxResidual + clampAmount);
        currentResidual = lerp(currentResidual, historyResidual, saturate(gTemporalWeight));
    }

    float3 current = stableBase + currentResidual;
    return float4(ApplyOutputPost(current), centerSample.a);
}

[shader("raygeneration")]
void RayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= (uint)gScreenSize.x || pixel.y >= (uint)gScreenSize.y)
        return;

    if (gPassMode != 0)
    {
        gOutputTex[pixel] = ResolveLabPixel(pixel);
        return;
    }

    float4 albedoSample = gAlbedoTex.Load(int3(pixel, 0));
    float depthSample = gDepthTex.Load(int3(pixel, 0));
    if (depthSample <= 0.0 || depthSample >= 1.0)
    {
        float3 background = NeedsResolvePass() ? albedoSample.rgb : ApplyOutputPost(albedoSample.rgb);
        gOutputTex[pixel] = float4(background, albedoSample.a);
        return;
    }

    float3 baseAlbedo = albedoSample.rgb;
    float3 worldPos = LoadScenePosition(pixel);
    float4 normalSample = LoadSceneNormal(pixel);
    float3 N = normalize(normalSample.xyz);
    float3 V = normalize(gCameraPos.xyz - worldPos);
    float geoFlag = normalSample.w;

    float cavity = ComputeCavity(pixel, worldPos, N);
    float microShadow = lerp(0.90, 1.0, cavity);
    float3 albedo = lerp(baseAlbedo, baseAlbedo * cavity, 0.35) * microShadow;
    float ao = ComputeAmbientOcclusion(worldPos, N, pixel);
    float skyVisibility = ComputeSkyVisibility(worldPos, N, pixel);
    float3 lightingAccum = gAmbientColor.rgb * gAmbientColor.a;
    float3 specularAccum = 0.0;
    float debugShadow = 1.0;

    if (gSkyEnabled != 0)
    {
        float upness = saturate(N.z * 0.5 + 0.5);
        float3 classicSkyColor = float3(0.5, 0.5, 0.5) * (0.35 + 0.65 * upness);
        lightingAccum += classicSkyColor * (gSkyStrength * skyVisibility);
    }

    if (geoFlag == GEOMETRY_FLAG_SKELETAL)
        lightingAccum += 0.15;

    if (gSunEnabled != 0)
    {
        float3 L = normalize(gSunDirection.xyz);
        float ndl = (gEnableHalfLambert != 0) ? saturate((dot(N, L) + 0.25) / 1.25) : saturate(dot(N, L));
        if (ndl > 0.0)
        {
            float shadow = SunShadow(worldPos, N, pixel);
            debugShadow = min(debugShadow, shadow);
            lightingAccum += gSunColor.rgb * (gSunIntensity * ndl * shadow);
            specularAccum += ComputeSpecular(N, V, L, gSunColor.rgb, gSunIntensity, 1.0, shadow, baseAlbedo);
        }
    }

    if (gDynamicLightsEnabled != 0)
    {
        uint lightLimit = min(gLightCount, min(gMaxLights, 256u));
        [loop]
        for (uint i = 0; i < lightLimit; ++i)
        {
            Light light = gLights[i];
            float radius = max(light.radius * max(gDynamicLightRadiusScale, 0.01), 0.01);
            float intensity = light.intensity * max(gDynamicLightIntensityScale, 0.0);

            if (light.type == GL_RAYTRACING_LIGHT_TYPE_POINT)
            {
                float3 toLight = light.position - worldPos;
                float dist = length(toLight);
                float3 L = toLight / max(dist, 1e-5);
                float atten = saturate((radius - dist) / radius);
                atten *= atten;
                float ndl = saturate((dot(N, L) + 0.35) / 1.35);
                float shadow = 1.0;
                if (ndl > 0.0001 && atten > 0.0)
                    shadow = PointLightShadow(worldPos, N, light, toLight, dist, pixel);
                debugShadow = min(debugShadow, shadow);
                lightingAccum += light.color * (intensity * atten * ndl * shadow);
                specularAccum += ComputeSpecular(N, V, L, light.color, intensity, atten, shadow, baseAlbedo);
            }
            else if (light.type == GL_RAYTRACING_LIGHT_TYPE_RECT)
            {
                float3 toCenter = light.position - worldPos;
                float centerDist = length(toCenter);
                float atten = saturate((radius - centerDist) / radius);
                atten = atten * atten * atten * atten;
                float shadow = (atten > 0.0) ? RectLightShadow(worldPos, N, light, pixel) : 1.0;
                debugShadow = min(debugShadow, shadow);

                uint lightSamples = clamp(light.samples, 1u, 16u);
                float3 rectDiffuseAccum = 0.0;
                float3 rectSpecAccum = 0.0;
                float seed = SamplingSeed(pixel, worldPos + light.position);
                [loop]
                for (uint sampleIndex = 0; sampleIndex < lightSamples; ++sampleIndex)
                {
                    float2 xi = (lightSamples == 1) ? float2(0.5, 0.5) : Hammersley2D(sampleIndex, lightSamples, seed);
                    float2 uv = xi * 2.0 - 1.0;
                    float3 samplePos = light.position + light.axisU * (uv.x * light.halfWidth) + light.axisV * (uv.y * light.halfHeight);
                    float3 sampleVec = samplePos - worldPos;
                    float sampleDist = length(sampleVec);
                    float3 L = sampleVec / max(sampleDist, 1e-5);
                    float ndl = saturate(dot(N, L));
                    float face = (light.twoSided != 0) ? abs(dot(light.normal, -L)) : saturate(dot(light.normal, -L));
                    rectDiffuseAccum += light.color * (intensity * ndl * face);
                    rectSpecAccum += ComputeSpecular(N, V, L, light.color, intensity * face, 1.0, 1.0, baseAlbedo);
                }
                rectDiffuseAccum /= (float)lightSamples;
                rectSpecAccum /= (float)lightSamples;
                lightingAccum += clamp(rectDiffuseAccum * atten * shadow, 0.0, 4.0);
                specularAccum += rectSpecAccum * atten * shadow;
            }
        }
    }

    lightingAccum *= ao;
    specularAccum *= lerp(1.0, ao, 0.5);
    if (geoFlag == GEOMETRY_FLAG_SKELETAL)
    {
        lightingAccum *= 1.05;
        specularAccum *= 1.05;
    }
    float3 reflection = ComputeReflection(worldPos, N, V, pixel);
    float3 gi = ComputeGI(worldPos, N, baseAlbedo, pixel);

    float3 finalColor;
    if (geoFlag == GEOMETRY_FLAG_UNLIT)
    {
        finalColor = baseAlbedo;
    }
    else
    {
        float3 rtLitColor = albedo * lightingAccum + specularAccum + reflection + gi;
        rtLitColor = max(rtLitColor, baseAlbedo * 0.15);
        finalColor = lerp(rtLitColor, baseAlbedo, saturate(gLegacyBlend));
        finalColor *= max(gExposure, 0.001);

        if (gDebugMode == 1)
            finalColor = rtLitColor;
        else if (gDebugMode == 2)
)DXRHLSL",
R"DXRHLSL(            finalColor = (ao * skyVisibility).xxx;
        else if (gDebugMode == 3)
            finalColor = baseAlbedo;
        else if (gDebugMode == 4)
            finalColor = lightingAccum / (1.0 + lightingAccum);

        if (gDebugEffect == 1)
            finalColor = debugShadow.xxx;
        else if (gDebugEffect == 2)
            finalColor = ao.xxx;
        else if (gDebugEffect == 3)
            finalColor = (gSunEnabled != 0 ? gSunColor.rgb * gSunIntensity : 0.0);
        else if (gDebugEffect == 4)
            finalColor = reflection;
        else if (gDebugEffect == 5)
            finalColor = gi;
    }

    if (!NeedsResolvePass())
        finalColor = ApplyOutputPost(finalColor);
    gOutputTex[pixel] = float4(finalColor, albedoSample.a);
})DXRHLSL",
};

static std::string glRaytracingBuildLightingHlslSource()
{
	std::string source;
	size_t totalSize = 0;
	for (size_t i = 0; i < _countof(g_glRaytracingLightingHlslParts); ++i)
		totalSize += strlen(g_glRaytracingLightingHlslParts[i]);
	source.reserve(totalSize);
	for (size_t i = 0; i < _countof(g_glRaytracingLightingHlslParts); ++i)
		source.append(g_glRaytracingLightingHlslParts[i]);
	return source;
}

static ComPtr<IDxcBlob> glRaytracingLightingCompileLibrary(const char* src, size_t srcSize)
{
	ComPtr<IDxcUtils> utils;
	ComPtr<IDxcCompiler3> compiler;
	ComPtr<IDxcIncludeHandler> includeHandler;

	HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
	if (FAILED(hr))
	{
		glRaytracingFatal("DxcCreateInstance utils failed 0x%08X", (unsigned)hr);
		return nullptr;
	}

	hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
	if (FAILED(hr))
	{
		glRaytracingFatal("DxcCreateInstance compiler failed 0x%08X", (unsigned)hr);
		return nullptr;
	}

	hr = utils->CreateDefaultIncludeHandler(&includeHandler);
	if (FAILED(hr))
	{
		glRaytracingFatal("CreateDefaultIncludeHandler failed 0x%08X", (unsigned)hr);
		return nullptr;
	}

	DxcBuffer source = {};
	source.Ptr = src;
	source.Size = srcSize;
	source.Encoding = DXC_CP_UTF8;

	const wchar_t* args[] =
	{
		L"-T", L"lib_6_3",
		L"-Zi",
		L"-Qembed_debug",
		L"-O3",
		L"-all_resources_bound"
	};

	ComPtr<IDxcResult> result;
	hr = compiler->Compile(&source, args, _countof(args), includeHandler.Get(), IID_PPV_ARGS(&result));
	if (FAILED(hr))
	{
		glRaytracingFatal("DXC compile failed 0x%08X", (unsigned)hr);
		return nullptr;
	}

	ComPtr<IDxcBlobUtf8> errors;
	result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
	if (errors && errors->GetStringLength() > 0)
	{
		OutputDebugStringA(errors->GetStringPointer());
		OutputDebugStringA("\n");
	}

	HRESULT status = S_OK;
	result->GetStatus(&status);
	if (FAILED(status))
	{
		glRaytracingFatal("DXIL compile status failed 0x%08X", (unsigned)status);
		return nullptr;
	}

	ComPtr<IDxcBlob> dxil;
	result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&dxil), nullptr);
	return dxil;
}

static int glRaytracingLightingCreateDescriptorHeap(void)
{
	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.NumDescriptors = GL_RAYTRACING_LAB_DESCRIPTORS_PER_PASS * GL_RAYTRACING_LAB_PASS_COUNT;
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	GLR_CHECK(g_glRaytracingCmd.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_glRaytracingLighting.descriptorHeap)));
	g_glRaytracingLighting.descriptorStride =
		g_glRaytracingCmd.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	return 1;
}

static int glRaytracingLightingCreateRootSignatures(void)
{
	{
		D3D12_DESCRIPTOR_RANGE ranges[2] = {};

		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = 8;
		ranges[0].BaseShaderRegister = 0;
		ranges[0].RegisterSpace = 0;
		ranges[0].OffsetInDescriptorsFromTableStart = 0;

		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[1].NumDescriptors = 1;
		ranges[1].BaseShaderRegister = 0;
		ranges[1].RegisterSpace = 0;
		ranges[1].OffsetInDescriptorsFromTableStart = 0;

		D3D12_ROOT_PARAMETER params[3] = {};

		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[0].DescriptorTable.NumDescriptorRanges = 1;
		params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[2].Descriptor.ShaderRegister = 0;
		params[2].Descriptor.RegisterSpace = 0;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rsd = {};
		rsd.NumParameters = _countof(params);
		rsd.pParameters = params;
		rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		ComPtr<ID3DBlob> sig;
		ComPtr<ID3DBlob> err;
		GLR_CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
		GLR_CHECK(g_glRaytracingCmd.device->CreateRootSignature(
			0, sig->GetBufferPointer(), sig->GetBufferSize(),
			IID_PPV_ARGS(&g_glRaytracingLighting.globalRootSig)));
	}

	{
		D3D12_ROOT_SIGNATURE_DESC rsd = {};
		rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

		ComPtr<ID3DBlob> sig;
		ComPtr<ID3DBlob> err;
		GLR_CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
		GLR_CHECK(g_glRaytracingCmd.device->CreateRootSignature(
			0, sig->GetBufferPointer(), sig->GetBufferSize(),
			IID_PPV_ARGS(&g_glRaytracingLighting.localRootSig)));
	}

	return 1;
}

static int glRaytracingLightingCreateBuffers(void)
{
	const UINT64 constantsSize = glRaytracingAlignUp(sizeof(glRaytracingLightingConstants_t), 256);
	g_glRaytracingLighting.constantBuffer = glRaytracingCreateBuffer(
		g_glRaytracingCmd.device.Get(),
		constantsSize,
		D3D12_HEAP_TYPE_UPLOAD,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		D3D12_RESOURCE_FLAG_NONE);

	g_glRaytracingLighting.resolveConstantBuffer = glRaytracingCreateBuffer(
		g_glRaytracingCmd.device.Get(),
		constantsSize,
		D3D12_HEAP_TYPE_UPLOAD,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		D3D12_RESOURCE_FLAG_NONE);

	g_glRaytracingLighting.lightBuffer = glRaytracingCreateBuffer(
		g_glRaytracingCmd.device.Get(),
		sizeof(glRaytracingLight_t) * GL_RAYTRACING_MAX_LIGHTS,
		D3D12_HEAP_TYPE_UPLOAD,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		D3D12_RESOURCE_FLAG_NONE);

	return g_glRaytracingLighting.constantBuffer.resource &&
		g_glRaytracingLighting.resolveConstantBuffer.resource &&
		g_glRaytracingLighting.lightBuffer.resource;
}

static void glRaytracingLightingUpdateConstants(void)
{
	glRaytracingLightingConstants_t upload = g_glRaytracingLighting.constants;
	upload.passMode = 0;
	upload.historyValid = g_glRaytracingLighting.historyValid ? 1u : 0u;
	glRaytracingMapCopy(
		g_glRaytracingLighting.constantBuffer.resource.Get(),
		&upload,
		sizeof(upload));
}

static void glRaytracingLightingUpdateResolveConstants(void)
{
	glRaytracingLightingConstants_t upload = g_glRaytracingLighting.constants;
	upload.passMode = 1;
	upload.historyValid = g_glRaytracingLighting.historyValid ? 1u : 0u;
	glRaytracingMapCopy(
		g_glRaytracingLighting.resolveConstantBuffer.resource.Get(),
		&upload,
		sizeof(upload));
}

static void glRaytracingLightingUpdateLights(void)
{
	if (g_glRaytracingLighting.cpuLights.empty())
		return;

	glRaytracingMapCopy(
		g_glRaytracingLighting.lightBuffer.resource.Get(),
		g_glRaytracingLighting.cpuLights.data(),
		g_glRaytracingLighting.cpuLights.size() * sizeof(glRaytracingLight_t));
}

static void glRaytracingLightingCreatePersistentLightSRV(void)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Format = DXGI_FORMAT_UNKNOWN;
	srv.Buffer.FirstElement = 0;
	srv.Buffer.NumElements = GL_RAYTRACING_MAX_LIGHTS;
	srv.Buffer.StructureByteStride = sizeof(glRaytracingLight_t);
	srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

	D3D12_CPU_DESCRIPTOR_HANDLE base = g_glRaytracingLighting.descriptorHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT passIndex = 0; passIndex < GL_RAYTRACING_LAB_PASS_COUNT; ++passIndex)
	{
		const UINT descriptorBase = passIndex * GL_RAYTRACING_LAB_DESCRIPTORS_PER_PASS;
		g_glRaytracingCmd.device->CreateShaderResourceView(
			g_glRaytracingLighting.lightBuffer.resource.Get(),
			&srv,
			glRaytracingOffsetCpu(base, g_glRaytracingLighting.descriptorStride, descriptorBase));
	}
}

static int glRaytracingLightingCreateStateObject(void)
{
	const std::string hlslSource = glRaytracingBuildLightingHlslSource();
	ComPtr<IDxcBlob> dxil = glRaytracingLightingCompileLibrary(hlslSource.c_str(), hlslSource.size());
	if (!dxil)
		return 0;

	D3D12_EXPORT_DESC exports[3] = {};
	exports[0].Name = L"RayGen";
	exports[1].Name = L"ShadowMiss";
	exports[2].Name = L"ShadowClosestHit";

	D3D12_DXIL_LIBRARY_DESC libDesc = {};
	D3D12_SHADER_BYTECODE libBytecode = {};
	libBytecode.pShaderBytecode = dxil->GetBufferPointer();
	libBytecode.BytecodeLength = dxil->GetBufferSize();
	libDesc.DXILLibrary = libBytecode;
	libDesc.NumExports = _countof(exports);
	libDesc.pExports = exports;

	D3D12_HIT_GROUP_DESC hitGroup = {};
	hitGroup.HitGroupExport = L"ShadowHitGroup";
	hitGroup.ClosestHitShaderImport = L"ShadowClosestHit";
	hitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;

	D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
	shaderConfig.MaxPayloadSizeInBytes = sizeof(uint32_t);
	shaderConfig.MaxAttributeSizeInBytes = 8;

	D3D12_GLOBAL_ROOT_SIGNATURE globalRS = {};
	globalRS.pGlobalRootSignature = g_glRaytracingLighting.globalRootSig.Get();

	D3D12_LOCAL_ROOT_SIGNATURE localRS = {};
	localRS.pLocalRootSignature = g_glRaytracingLighting.localRootSig.Get();

	D3D12_STATE_SUBOBJECT subobjects[8] = {};
	UINT sub = 0;

	subobjects[sub].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
	subobjects[sub].pDesc = &libDesc;
	++sub;

	subobjects[sub].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
	subobjects[sub].pDesc = &hitGroup;
	++sub;

	subobjects[sub].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
	subobjects[sub].pDesc = &shaderConfig;
	++sub;

	subobjects[sub].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
	subobjects[sub].pDesc = &globalRS;
	++sub;

	subobjects[sub].Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
	subobjects[sub].pDesc = &localRS;
	++sub;

	LPCWSTR localExports[] = { L"RayGen", L"ShadowMiss", L"ShadowHitGroup" };
	D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assoc = {};
	assoc.pSubobjectToAssociate = &subobjects[4];
	assoc.NumExports = _countof(localExports);
	assoc.pExports = localExports;

	subobjects[sub].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
	subobjects[sub].pDesc = &assoc;
	++sub;

	D3D12_RAYTRACING_PIPELINE_CONFIG pipeConfig = {};
	pipeConfig.MaxTraceRecursionDepth = 1;

	subobjects[sub].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
	subobjects[sub].pDesc = &pipeConfig;
	++sub;

	D3D12_STATE_OBJECT_DESC soDesc = {};
	soDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
	soDesc.NumSubobjects = sub;
	soDesc.pSubobjects = subobjects;

	GLR_CHECK(g_glRaytracingCmd.device->CreateStateObject(&soDesc, IID_PPV_ARGS(&g_glRaytracingLighting.rtStateObject)));
	GLR_CHECK(g_glRaytracingLighting.rtStateObject.As(&g_glRaytracingLighting.rtStateProps));
	return 1;
}

static int glRaytracingLightingCreateShaderTables(void)
{
	void* raygenId = g_glRaytracingLighting.rtStateProps->GetShaderIdentifier(L"RayGen");
	void* missId = g_glRaytracingLighting.rtStateProps->GetShaderIdentifier(L"ShadowMiss");
	void* hitId = g_glRaytracingLighting.rtStateProps->GetShaderIdentifier(L"ShadowHitGroup");

	if (!raygenId || !missId || !hitId)
	{
		glRaytracingFatal("Failed to fetch shader identifiers");
		return 0;
	}

	const UINT shaderIdSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
	const UINT recordSize = (UINT)glRaytracingAlignUp(shaderIdSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);

	g_glRaytracingLighting.raygenTable = glRaytracingCreateBuffer(
		g_glRaytracingCmd.device.Get(),
		recordSize,
		D3D12_HEAP_TYPE_UPLOAD,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		D3D12_RESOURCE_FLAG_NONE);

	g_glRaytracingLighting.missTable = glRaytracingCreateBuffer(
		g_glRaytracingCmd.device.Get(),
		recordSize,
		D3D12_HEAP_TYPE_UPLOAD,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		D3D12_RESOURCE_FLAG_NONE);

	g_glRaytracingLighting.hitTable = glRaytracingCreateBuffer(
		g_glRaytracingCmd.device.Get(),
		recordSize,
		D3D12_HEAP_TYPE_UPLOAD,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		D3D12_RESOURCE_FLAG_NONE);

	if (!g_glRaytracingLighting.raygenTable.resource ||
		!g_glRaytracingLighting.missTable.resource ||
		!g_glRaytracingLighting.hitTable.resource)
	{
		return 0;
	}

	uint8_t temp[256] = {};

	memset(temp, 0, sizeof(temp));
	memcpy(temp, raygenId, shaderIdSize);
	glRaytracingMapCopy(g_glRaytracingLighting.raygenTable.resource.Get(), temp, recordSize);

	memset(temp, 0, sizeof(temp));
	memcpy(temp, missId, shaderIdSize);
	glRaytracingMapCopy(g_glRaytracingLighting.missTable.resource.Get(), temp, recordSize);

	memset(temp, 0, sizeof(temp));
	memcpy(temp, hitId, shaderIdSize);
	glRaytracingMapCopy(g_glRaytracingLighting.hitTable.resource.Get(), temp, recordSize);

	return 1;
}

static void glRaytracingLightingCreatePerPassDescriptors(
	const glRaytracingLightingPassDesc_t* pass,
	UINT descriptorBase,
	ID3D12Resource* outputTexture,
	ID3D12Resource* rawTexture,
	ID3D12Resource* historyTexture)
{
	D3D12_CPU_DESCRIPTOR_HANDLE base = g_glRaytracingLighting.descriptorHeap->GetCPUDescriptorHandleForHeapStart();

	D3D12_SHADER_RESOURCE_VIEW_DESC albedoSrv = {};
	albedoSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	albedoSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	albedoSrv.Format = pass->albedoFormat;
	albedoSrv.Texture2D.MipLevels = 1;
	g_glRaytracingCmd.device->CreateShaderResourceView(
		pass->albedoTexture, &albedoSrv,
		glRaytracingOffsetCpu(base, g_glRaytracingLighting.descriptorStride, descriptorBase + 1));

	D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv = {};
	depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	depthSrv.Format = glRaytracingGetSrvFormatForDepth(pass->depthFormat);
	depthSrv.Texture2D.MipLevels = 1;
	g_glRaytracingCmd.device->CreateShaderResourceView(
		pass->depthTexture, &depthSrv,
		glRaytracingOffsetCpu(base, g_glRaytracingLighting.descriptorStride, descriptorBase + 2));

	D3D12_SHADER_RESOURCE_VIEW_DESC normalSrv = {};
	normalSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	normalSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	normalSrv.Format = pass->normalFormat;
	normalSrv.Texture2D.MipLevels = 1;
	g_glRaytracingCmd.device->CreateShaderResourceView(
		pass->normalTexture, &normalSrv,
		glRaytracingOffsetCpu(base, g_glRaytracingLighting.descriptorStride, descriptorBase + 3));

	D3D12_SHADER_RESOURCE_VIEW_DESC positionSrv = {};
	positionSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	positionSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	positionSrv.Format = pass->positionFormat;
	positionSrv.Texture2D.MipLevels = 1;
	g_glRaytracingCmd.device->CreateShaderResourceView(
		pass->positionTexture, &positionSrv,
		glRaytracingOffsetCpu(base, g_glRaytracingLighting.descriptorStride, descriptorBase + 4));

	D3D12_SHADER_RESOURCE_VIEW_DESC tlasSrv = {};
	tlasSrv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	tlasSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	tlasSrv.RaytracingAccelerationStructure.Location = pass->topLevelAS->GetGPUVirtualAddress();
	g_glRaytracingCmd.device->CreateShaderResourceView(
		nullptr, &tlasSrv,
		glRaytracingOffsetCpu(base, g_glRaytracingLighting.descriptorStride, descriptorBase + 5));

	D3D12_SHADER_RESOURCE_VIEW_DESC lightingSrv = {};
	lightingSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	lightingSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	lightingSrv.Format = pass->outputFormat;
	lightingSrv.Texture2D.MipLevels = 1;
	g_glRaytracingCmd.device->CreateShaderResourceView(
		rawTexture, &lightingSrv,
		glRaytracingOffsetCpu(base, g_glRaytracingLighting.descriptorStride, descriptorBase + 6));
	g_glRaytracingCmd.device->CreateShaderResourceView(
		historyTexture, &lightingSrv,
		glRaytracingOffsetCpu(base, g_glRaytracingLighting.descriptorStride, descriptorBase + 7));

	D3D12_UNORDERED_ACCESS_VIEW_DESC outputUav = {};
	outputUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	outputUav.Format = pass->outputFormat;
	g_glRaytracingCmd.device->CreateUnorderedAccessView(
		outputTexture, nullptr, &outputUav,
		glRaytracingOffsetCpu(base, g_glRaytracingLighting.descriptorStride, descriptorBase + 8));
}

static ComPtr<ID3D12Resource> glRaytracingLightingCreateLabTexture(
	uint32_t width,
	uint32_t height,
	DXGI_FORMAT format,
	D3D12_RESOURCE_FLAGS flags,
	D3D12_RESOURCE_STATES initialState)
{
	ComPtr<ID3D12Resource> texture;
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = flags;

	HRESULT hr = g_glRaytracingCmd.device->CreateCommittedResource(
		&heap,
		D3D12_HEAP_FLAG_NONE,
		&desc,
		initialState,
		nullptr,
		IID_PPV_ARGS(&texture));
	if (FAILED(hr))
	{
		glRaytracingHandleFailure(hr, "CreateCommittedResource(lab texture)", __FILE__, __LINE__);
		texture.Reset();
	}
	return texture;
}

static int glRaytracingLightingEnsureLabTextures(const glRaytracingLightingPassDesc_t* pass)
{
	if (g_glRaytracingLighting.labRawTexture &&
		g_glRaytracingLighting.labHistoryTexture &&
		g_glRaytracingLighting.labWidth == pass->width &&
		g_glRaytracingLighting.labHeight == pass->height &&
		g_glRaytracingLighting.labFormat == pass->outputFormat)
	{
		return 1;
	}

	g_glRaytracingLighting.labRawTexture.Reset();
	g_glRaytracingLighting.labHistoryTexture.Reset();
	g_glRaytracingLighting.labWidth = pass->width;
	g_glRaytracingLighting.labHeight = pass->height;
	g_glRaytracingLighting.labFormat = pass->outputFormat;
	g_glRaytracingLighting.labRawState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	g_glRaytracingLighting.labHistoryState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	g_glRaytracingLighting.historyValid = 0;

	g_glRaytracingLighting.labRawTexture = glRaytracingLightingCreateLabTexture(
		pass->width, pass->height, pass->outputFormat,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		g_glRaytracingLighting.labRawState);
	g_glRaytracingLighting.labHistoryTexture = glRaytracingLightingCreateLabTexture(
		pass->width, pass->height, pass->outputFormat,
		D3D12_RESOURCE_FLAG_NONE,
		g_glRaytracingLighting.labHistoryState);

	return g_glRaytracingLighting.labRawTexture && g_glRaytracingLighting.labHistoryTexture;
}

static glRaytracingEffectsOptions_t glRaytracingLightingDefaultEffectsOptions(void)
{
	glRaytracingEffectsOptions_t o = {};
	o.shadowsEnabled = 1;
	o.shadowStrength = 1.0f;
	o.shadowSamples = 4;
	o.shadowSoftness = 1.0f;
	o.shadowMaxDistance = 4096.0f;
	o.shadowCullMode = 0;
	o.contactShadows = 1;
	o.contactShadowLength = 96.0f;
	o.sunEnabled = 0;
	o.sunIntensity = 1.25f;
	o.sunAngularRadius = 0.35f;
	o.sunSamples = 4;
	o.sunDirection[0] = -0.45f;
	o.sunDirection[1] = 0.25f;
	o.sunDirection[2] = 0.86f;
	o.sunDirection[3] = 0.0f;
	o.sunColor[0] = 1.0f;
	o.sunColor[1] = 0.93f;
	o.sunColor[2] = 0.82f;
	o.sunColor[3] = 1.0f;
	o.dynamicLightsEnabled = 1;
	o.dynamicLightShadows = 1;
	o.maxLights = 16;
	o.dynamicLightIntensityScale = 1.0f;
	o.dynamicLightRadiusScale = 1.0f;
	o.aoEnabled = 0;
	o.aoSamples = 4;
	o.aoRadius = 24.0f;
	o.aoStrength = 0.45f;
	o.reflectionsEnabled = 0;
	o.reflectionSamples = 1;
	o.reflectionStrength = 0.18f;
	o.reflectionMaxDistance = 1536.0f;
	o.reflectionRoughness = 0.35f;
	o.giEnabled = 0;
	o.giSamples = 1;
	o.giStrength = 0.12f;
	o.giMaxDistance = 256.0f;
	o.denoiserEnabled = 0;
	o.denoiserRadius = 1;
	o.denoiserStrength = 0.55f;
	o.denoiserDepthSigma = 160.0f;
	o.denoiserNormalSigma = 24.0f;
	o.temporalEnabled = 0;
	o.temporalWeight = 0.72f;
	o.temporalClamp = 0.12f;
	o.temporalResetThreshold = 0.02f;
	o.skyEnabled = 0;
	o.skyStrength = 0.45f;
	o.skySamples = 2;
	o.skyMaxDistance = 8192.0f;
	o.specularEnabled = 1;
	o.specularStrength = 1.0f;
	o.specularPower = 48.0f;
	o.shadowMinVisibility = 0.04f;
	o.tonemapMode = 0;
	o.hdrWhitePoint = 2.0f;
	o.bloomStrength = 0.0f;
	o.bloomThreshold = 1.1f;
	o.saturation = 1.0f;
	o.contrast = 1.0f;
	o.outputGamma = 1.0f;
	o.frameIndex = 0;
	o.debugEffect = 0;
	return o;
}

// ============================================================
// Lighting public API
// ============================================================

bool glRaytracingLightingInit(void)
{
	if (g_glRaytracingLighting.initialized)
		return true;

	if (!glRaytracingInitCmdContext())
		return false;

	if (!glRaytracingLightingCreateDescriptorHeap())
		return false;

	if (!glRaytracingLightingCreateRootSignatures())
		return false;

	if (!glRaytracingLightingCreateBuffers())
		return false;

	glRaytracingLightingCreatePersistentLightSRV();

	if (!glRaytracingLightingCreateStateObject())
		return false;

	if (!glRaytracingLightingCreateShaderTables())
		return false;

	memset(&g_glRaytracingLighting.constants, 0, sizeof(g_glRaytracingLighting.constants));
	g_glRaytracingLighting.constants.ambientColor[0] = 0.08f;
	g_glRaytracingLighting.constants.ambientColor[1] = 0.08f;
	g_glRaytracingLighting.constants.ambientColor[2] = 0.09f;
	g_glRaytracingLighting.constants.ambientColor[3] = 1.0f;
	g_glRaytracingLighting.constants.enableSpecular = 1;
	g_glRaytracingLighting.constants.enableHalfLambert = 1;
	g_glRaytracingLighting.constants.normalReconstructZ = 1.0f;
	g_glRaytracingLighting.constants.shadowBias = 0.05f;
	g_glRaytracingLighting.constants.exposure = 1.15f;
	g_glRaytracingLighting.constants.legacyBlend = 0.65f;
	g_glRaytracingLighting.constants.debugMode = 0u;
	g_glRaytracingLighting.constants.effects = glRaytracingLightingDefaultEffectsOptions();
	g_glRaytracingLighting.historyValid = 0;
	g_glRaytracingLighting.havePreviousView = 0;

	glRaytracingLightingUpdateConstants();
	glRaytracingLightingUpdateResolveConstants();

	g_glRaytracingLighting.initialized = true;
	glRaytracingLog("glRaytracingLightingInit ok");
	return true;
}

void glRaytracingLightingShutdown(void)
{
	if (!g_glRaytracingLighting.initialized)
		return;

	g_glRaytracingLighting = glRaytracingLightingState_t();
}

bool glRaytracingLightingIsInitialized(void)
{
	return g_glRaytracingLighting.initialized;
}

void glRaytracingLightingClearLights(bool clearPersistant)
{
	if (clearPersistant)
	{
		g_glRaytracingLighting.cpuLights.clear();
	}
	else
	{
		size_t writeIndex = 0;

		for (size_t i = 0; i < g_glRaytracingLighting.cpuLights.size(); ++i)
		{
			if (g_glRaytracingLighting.cpuLights[i].persistant)
			{
				if (writeIndex != i)
				{
					g_glRaytracingLighting.cpuLights[writeIndex] = g_glRaytracingLighting.cpuLights[i];
				}
				++writeIndex;
			}
		}

		g_glRaytracingLighting.cpuLights.resize(writeIndex);
	}

	g_glRaytracingLighting.constants.lightCount =
		(uint32_t)g_glRaytracingLighting.cpuLights.size();

	glRaytracingLightingUpdateConstants();
}

bool glRaytracingLightingAddLight(const glRaytracingLight_t* light)
{
	if (!g_glRaytracingLighting.initialized || !light)
		return false;

	if (g_glRaytracingLighting.cpuLights.size() >= GL_RAYTRACING_MAX_LIGHTS)
		return false;

	g_glRaytracingLighting.cpuLights.push_back(*light);
	g_glRaytracingLighting.constants.lightCount = (uint32_t)g_glRaytracingLighting.cpuLights.size();

	glRaytracingLightingUpdateLights();
	glRaytracingLightingUpdateConstants();
	return true;
}

void glRaytracingLightingSetAmbient(float r, float g, float b, float intensity)
{
	g_glRaytracingLighting.constants.ambientColor[0] = r;
	g_glRaytracingLighting.constants.ambientColor[1] = g;
	g_glRaytracingLighting.constants.ambientColor[2] = b;
	g_glRaytracingLighting.constants.ambientColor[3] = intensity;
	glRaytracingLightingUpdateConstants();
}

void glRaytracingLightingSetCameraPosition(float x, float y, float z)
{
	g_glRaytracingLighting.constants.cameraPos[0] = x;
	g_glRaytracingLighting.constants.cameraPos[1] = y;
	g_glRaytracingLighting.constants.cameraPos[2] = z;
	g_glRaytracingLighting.constants.cameraPos[3] = 1.0f;
	glRaytracingLightingUpdateConstants();
}

void glRaytracingLightingSetInvViewProjMatrix(const float* m16)
{
	if (!m16)
		return;

	memcpy(g_glRaytracingLighting.constants.invViewProj, m16, sizeof(float) * 16);
	glRaytracingLightingUpdateConstants();
}

void glRaytracingLightingSetInvViewMatrix(const float* m16)
{
	if (!m16)
		return;

	if (g_glRaytracingLighting.havePreviousView &&
		g_glRaytracingLighting.constants.effects.temporalEnabled)
	{
		float maxDelta = 0.0f;
		for (int i = 0; i < 16; ++i)
		{
			const float delta = fabsf(m16[i] - g_glRaytracingLighting.previousView[i]);
			if (delta > maxDelta)
				maxDelta = delta;
		}
		if (maxDelta > g_glRaytracingLighting.constants.effects.temporalResetThreshold)
			g_glRaytracingLighting.historyValid = 0;
	}

	memcpy(g_glRaytracingLighting.previousView, m16, sizeof(float) * 16);
	g_glRaytracingLighting.havePreviousView = 1;
	memcpy(g_glRaytracingLighting.constants.invViewMatrix, m16, sizeof(float) * 16);
	glRaytracingLightingUpdateConstants();
}

void glRaytracingLightingSetEffectsOptions(const glRaytracingEffectsOptions_t* options)
{
	if (!options)
		return;

	const uint32_t oldTemporal = g_glRaytracingLighting.constants.effects.temporalEnabled;
	glRaytracingEffectsOptions_t o = *options;

	o.shadowsEnabled = o.shadowsEnabled ? 1u : 0u;
	o.shadowStrength = glRaytracingClamp(o.shadowStrength, 0.0f, 2.0f);
	o.shadowSamples = glRaytracingClamp(o.shadowSamples, 1u, 16u);
	o.shadowSoftness = glRaytracingClamp(o.shadowSoftness, 0.0f, 4.0f);
	o.shadowMaxDistance = glRaytracingClamp(o.shadowMaxDistance, 1.0f, 65536.0f);
	o.shadowCullMode = glRaytracingClamp(o.shadowCullMode, 0u, 2u);
	o.contactShadows = o.contactShadows ? 1u : 0u;
	o.contactShadowLength = glRaytracingClamp(o.contactShadowLength, 0.0f, 4096.0f);
	o.sunEnabled = o.sunEnabled ? 1u : 0u;
	o.sunIntensity = glRaytracingClamp(o.sunIntensity, 0.0f, 32.0f);
	o.sunAngularRadius = glRaytracingClamp(o.sunAngularRadius, 0.0f, 10.0f);
	o.sunSamples = glRaytracingClamp(o.sunSamples, 1u, 16u);
	glRaytracingNormalize3(o.sunDirection[0], o.sunDirection[1], o.sunDirection[2]);
	o.sunDirection[3] = 0.0f;
	o.sunColor[0] = glRaytracingClamp(o.sunColor[0], 0.0f, 8.0f);
	o.sunColor[1] = glRaytracingClamp(o.sunColor[1], 0.0f, 8.0f);
	o.sunColor[2] = glRaytracingClamp(o.sunColor[2], 0.0f, 8.0f);
	o.sunColor[3] = 1.0f;
	o.dynamicLightsEnabled = o.dynamicLightsEnabled ? 1u : 0u;
	o.dynamicLightShadows = o.dynamicLightShadows ? 1u : 0u;
	o.maxLights = glRaytracingClamp(o.maxLights, 0u, (uint32_t)GL_RAYTRACING_MAX_LIGHTS);
	o.dynamicLightIntensityScale = glRaytracingClamp(o.dynamicLightIntensityScale, 0.0f, 16.0f);
	o.dynamicLightRadiusScale = glRaytracingClamp(o.dynamicLightRadiusScale, 0.05f, 8.0f);
	o.aoEnabled = o.aoEnabled ? 1u : 0u;
	o.aoSamples = glRaytracingClamp(o.aoSamples, 1u, 32u);
	o.aoRadius = glRaytracingClamp(o.aoRadius, 1.0f, 2048.0f);
	o.aoStrength = glRaytracingClamp(o.aoStrength, 0.0f, 1.0f);
	o.reflectionsEnabled = o.reflectionsEnabled ? 1u : 0u;
	o.reflectionSamples = glRaytracingClamp(o.reflectionSamples, 1u, 8u);
	o.reflectionStrength = glRaytracingClamp(o.reflectionStrength, 0.0f, 4.0f);
	o.reflectionMaxDistance = glRaytracingClamp(o.reflectionMaxDistance, 1.0f, 65536.0f);
	o.reflectionRoughness = glRaytracingClamp(o.reflectionRoughness, 0.0f, 1.0f);
	o.giEnabled = o.giEnabled ? 1u : 0u;
	o.giSamples = glRaytracingClamp(o.giSamples, 1u, 8u);
	o.giStrength = glRaytracingClamp(o.giStrength, 0.0f, 4.0f);
	o.giMaxDistance = glRaytracingClamp(o.giMaxDistance, 1.0f, 8192.0f);
	o.denoiserEnabled = o.denoiserEnabled ? 1u : 0u;
	o.denoiserRadius = glRaytracingClamp(o.denoiserRadius, 0u, 3u);
	o.denoiserStrength = glRaytracingClamp(o.denoiserStrength, 0.0f, 1.0f);
	o.denoiserDepthSigma = glRaytracingClamp(o.denoiserDepthSigma, 0.0f, 4096.0f);
	o.denoiserNormalSigma = glRaytracingClamp(o.denoiserNormalSigma, 1.0f, 128.0f);
	o.temporalEnabled = o.temporalEnabled ? 1u : 0u;
	o.temporalWeight = glRaytracingClamp(o.temporalWeight, 0.0f, 0.95f);
	o.temporalClamp = glRaytracingClamp(o.temporalClamp, 0.0f, 4.0f);
	o.temporalResetThreshold = glRaytracingClamp(o.temporalResetThreshold, 0.0001f, 2.0f);
	o.skyEnabled = o.skyEnabled ? 1u : 0u;
	o.skyStrength = glRaytracingClamp(o.skyStrength, 0.0f, 8.0f);
	o.skySamples = glRaytracingClamp(o.skySamples, 1u, 16u);
	o.skyMaxDistance = glRaytracingClamp(o.skyMaxDistance, 1.0f, 65536.0f);
	o.specularEnabled = o.specularEnabled ? 1u : 0u;
	o.specularStrength = glRaytracingClamp(o.specularStrength, 0.0f, 8.0f);
	o.specularPower = glRaytracingClamp(o.specularPower, 2.0f, 256.0f);
	o.shadowMinVisibility = glRaytracingClamp(o.shadowMinVisibility, 0.0f, 1.0f);
	o.tonemapMode = glRaytracingClamp(o.tonemapMode, 0u, 2u);
	o.hdrWhitePoint = glRaytracingClamp(o.hdrWhitePoint, 0.1f, 32.0f);
	o.bloomStrength = glRaytracingClamp(o.bloomStrength, 0.0f, 4.0f);
	o.bloomThreshold = glRaytracingClamp(o.bloomThreshold, 0.0f, 32.0f);
	o.saturation = glRaytracingClamp(o.saturation, 0.0f, 4.0f);
	o.contrast = glRaytracingClamp(o.contrast, 0.0f, 4.0f);
	o.outputGamma = glRaytracingClamp(o.outputGamma, 0.1f, 4.0f);
	o.debugEffect = glRaytracingClamp(o.debugEffect, 0u, 5u);

	g_glRaytracingLighting.constants.effects = o;
	if (!o.temporalEnabled || oldTemporal != o.temporalEnabled)
		g_glRaytracingLighting.historyValid = 0;
	glRaytracingLightingUpdateConstants();
	glRaytracingLightingUpdateResolveConstants();
}

void glRaytracingLightingResetHistory(void)
{
	g_glRaytracingLighting.historyValid = 0;
	g_glRaytracingLighting.havePreviousView = 0;
	memset(g_glRaytracingLighting.previousView, 0, sizeof(g_glRaytracingLighting.previousView));
	glRaytracingLightingUpdateConstants();
	glRaytracingLightingUpdateResolveConstants();
}

void glRaytracingLightingSetNormalReconstructSign(float signValue)
{
	g_glRaytracingLighting.constants.normalReconstructZ = signValue;
	glRaytracingLightingUpdateConstants();
}

void glRaytracingLightingEnableSpecular(int enable)
{
	g_glRaytracingLighting.constants.enableSpecular = enable ? 1u : 0u;
	glRaytracingLightingUpdateConstants();
}

void glRaytracingLightingEnableHalfLambert(int enable)
{
	g_glRaytracingLighting.constants.enableHalfLambert = enable ? 1u : 0u;
	glRaytracingLightingUpdateConstants();
}

void glRaytracingLightingSetShadowBias(float bias)
{
	g_glRaytracingLighting.constants.shadowBias = bias;
	glRaytracingLightingUpdateConstants();
}

void glRaytracingLightingSetExposure(float exposure)
{
	g_glRaytracingLighting.constants.exposure = exposure;
	glRaytracingLightingUpdateConstants();
}

void glRaytracingLightingSetLegacyBlend(float legacyBlend)
{
	g_glRaytracingLighting.constants.legacyBlend = legacyBlend;
	glRaytracingLightingUpdateConstants();
}

void glRaytracingLightingSetDebugMode(uint32_t mode)
{
	g_glRaytracingLighting.constants.debugMode = mode;
	glRaytracingLightingUpdateConstants();
}

bool glRaytracingLightingExecute(const glRaytracingLightingPassDesc_t* pass)
{
	if (glRaytracingShouldAbortWork())
		return false;

	if (!g_glRaytracingLighting.initialized || !pass)
		return false;

	// Re-use the previous full-resolution DXR lighting texture on optional
	// interval frames. No low-resolution upscale or block composite is used.
	if (pass->outputTexture != g_glRaytracingCleanVisualLastOutput)
	{
		g_glRaytracingCleanVisualLastOutput = pass->outputTexture;
		g_glRaytracingCleanVisualDispatchFrame = 0;
		g_glRaytracingCleanVisualHasOutput = 0;
		g_glRaytracingLighting.historyValid = 0;
	}
	if (g_glRaytracingCleanVisualHasOutput &&
		g_glRaytracingCleanVisualPerformance.dispatchInterval > 1)
	{
		++g_glRaytracingCleanVisualDispatchFrame;
		if ((g_glRaytracingCleanVisualDispatchFrame % g_glRaytracingCleanVisualPerformance.dispatchInterval) != 0)
			return true;
	}

	if (!pass->albedoTexture ||
		!pass->depthTexture ||
		!pass->normalTexture ||
		!pass->positionTexture ||
		!pass->outputTexture ||
		!pass->topLevelAS)
	{
		return false;
	}

	if (pass->width == 0 || pass->height == 0)
		return false;

	const glRaytracingEffectsOptions_t& effects = g_glRaytracingLighting.constants.effects;
	const bool needsResolve =
		effects.denoiserEnabled != 0 ||
		effects.temporalEnabled != 0 ||
		effects.tonemapMode != 0 ||
		effects.bloomStrength > 0.0f ||
		fabsf(effects.saturation - 1.0f) > 0.001f ||
		fabsf(effects.contrast - 1.0f) > 0.001f ||
		fabsf(effects.outputGamma - 1.0f) > 0.001f;

	if (needsResolve && !glRaytracingLightingEnsureLabTextures(pass))
		return false;

	g_glRaytracingLighting.constants.screenSize[0] = (float)pass->width;
	g_glRaytracingLighting.constants.screenSize[1] = (float)pass->height;
	g_glRaytracingLighting.constants.screenSize[2] = 1.0f / (float)pass->width;
	g_glRaytracingLighting.constants.screenSize[3] = 1.0f / (float)pass->height;
	g_glRaytracingLighting.constants.lightCount =
		(uint32_t)glRaytracingClamp<size_t>(g_glRaytracingLighting.cpuLights.size(), 0, GL_RAYTRACING_MAX_LIGHTS);

	ID3D12Resource* primaryOutput = needsResolve
		? g_glRaytracingLighting.labRawTexture.Get()
		: pass->outputTexture;

	// Wait for the previous DXR command list before rewriting upload buffers or
	// shader-visible descriptors. Performance v2 already waits here to recycle
	// its single command allocator; moving the wait before CPU writes closes a
	// resource-lifetime race without adding another fence wait.
	if (!glRaytracingBeginCmd())
		return false;

	glRaytracingLightingUpdateLights();
	glRaytracingLightingUpdateConstants();
	if (needsResolve)
		glRaytracingLightingUpdateResolveConstants();

	glRaytracingLightingCreatePerPassDescriptors(
		pass,
		0,
		primaryOutput,
		nullptr,
		nullptr);
	if (needsResolve)
	{
		glRaytracingLightingCreatePerPassDescriptors(
			pass,
			GL_RAYTRACING_LAB_DESCRIPTORS_PER_PASS,
			pass->outputTexture,
			g_glRaytracingLighting.labRawTexture.Get(),
			effects.temporalEnabled ? g_glRaytracingLighting.labHistoryTexture.Get() : nullptr);
	}

	if (needsResolve)
	{
		glRaytracingTransition(
			g_glRaytracingCmd.cmdList.Get(),
			g_glRaytracingLighting.labRawTexture.Get(),
			g_glRaytracingLighting.labRawState,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		g_glRaytracingLighting.labRawState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}
	else
	{
		glRaytracingTransition(
			g_glRaytracingCmd.cmdList.Get(),
			pass->outputTexture,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}

	ID3D12DescriptorHeap* heaps[] = { g_glRaytracingLighting.descriptorHeap.Get() };
	g_glRaytracingCmd.cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
	g_glRaytracingCmd.cmdList->SetComputeRootSignature(g_glRaytracingLighting.globalRootSig.Get());
	g_glRaytracingCmd.cmdList->SetPipelineState1(g_glRaytracingLighting.rtStateObject.Get());

	const UINT shaderRecordSize =
		(UINT)glRaytracingAlignUp(
			D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
			D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);

	D3D12_DISPATCH_RAYS_DESC rays = {};
	rays.RayGenerationShaderRecord.StartAddress = g_glRaytracingLighting.raygenTable.gpuVA;
	rays.RayGenerationShaderRecord.SizeInBytes = shaderRecordSize;
	rays.MissShaderTable.StartAddress = g_glRaytracingLighting.missTable.gpuVA;
	rays.MissShaderTable.SizeInBytes = shaderRecordSize;
	rays.MissShaderTable.StrideInBytes = shaderRecordSize;
	rays.HitGroupTable.StartAddress = g_glRaytracingLighting.hitTable.gpuVA;
	rays.HitGroupTable.SizeInBytes = shaderRecordSize;
	rays.HitGroupTable.StrideInBytes = shaderRecordSize;
	rays.Width = pass->width;
	rays.Height = pass->height;
	rays.Depth = 1;

	D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = g_glRaytracingLighting.descriptorHeap->GetGPUDescriptorHandleForHeapStart();
	g_glRaytracingCmd.cmdList->SetComputeRootDescriptorTable(
		0,
		glRaytracingOffsetGpu(gpuBase, g_glRaytracingLighting.descriptorStride, 0));
	g_glRaytracingCmd.cmdList->SetComputeRootDescriptorTable(
		1,
		glRaytracingOffsetGpu(gpuBase, g_glRaytracingLighting.descriptorStride, 8));
	g_glRaytracingCmd.cmdList->SetComputeRootConstantBufferView(
		2,
		g_glRaytracingLighting.constantBuffer.gpuVA);
	g_glRaytracingCmd.cmdList->DispatchRays(&rays);

	D3D12_RESOURCE_BARRIER uav = {};
	uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uav.UAV.pResource = primaryOutput;
	g_glRaytracingCmd.cmdList->ResourceBarrier(1, &uav);

	if (needsResolve)
	{
		glRaytracingTransition(
			g_glRaytracingCmd.cmdList.Get(),
			g_glRaytracingLighting.labRawTexture.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		g_glRaytracingLighting.labRawState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

		glRaytracingTransition(
			g_glRaytracingCmd.cmdList.Get(),
			pass->outputTexture,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		const UINT resolveBase = GL_RAYTRACING_LAB_DESCRIPTORS_PER_PASS;
		g_glRaytracingCmd.cmdList->SetComputeRootDescriptorTable(
			0,
			glRaytracingOffsetGpu(gpuBase, g_glRaytracingLighting.descriptorStride, resolveBase));
		g_glRaytracingCmd.cmdList->SetComputeRootDescriptorTable(
			1,
			glRaytracingOffsetGpu(gpuBase, g_glRaytracingLighting.descriptorStride, resolveBase + 8));
		g_glRaytracingCmd.cmdList->SetComputeRootConstantBufferView(
			2,
			g_glRaytracingLighting.resolveConstantBuffer.gpuVA);
		g_glRaytracingCmd.cmdList->DispatchRays(&rays);

		uav.UAV.pResource = pass->outputTexture;
		g_glRaytracingCmd.cmdList->ResourceBarrier(1, &uav);

		glRaytracingTransition(
			g_glRaytracingCmd.cmdList.Get(),
			pass->outputTexture,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		if (effects.temporalEnabled)
		{
			// Store the un-tonemapped full-resolution raw lighting history. This
			// keeps temporal accumulation in the same color domain on every frame.
			glRaytracingTransition(
				g_glRaytracingCmd.cmdList.Get(),
				g_glRaytracingLighting.labRawTexture.Get(),
				g_glRaytracingLighting.labRawState,
				D3D12_RESOURCE_STATE_COPY_SOURCE);
			g_glRaytracingLighting.labRawState = D3D12_RESOURCE_STATE_COPY_SOURCE;
			glRaytracingTransition(
				g_glRaytracingCmd.cmdList.Get(),
				g_glRaytracingLighting.labHistoryTexture.Get(),
				g_glRaytracingLighting.labHistoryState,
				D3D12_RESOURCE_STATE_COPY_DEST);
			g_glRaytracingLighting.labHistoryState = D3D12_RESOURCE_STATE_COPY_DEST;

			g_glRaytracingCmd.cmdList->CopyResource(
				g_glRaytracingLighting.labHistoryTexture.Get(),
				g_glRaytracingLighting.labRawTexture.Get());

			glRaytracingTransition(
				g_glRaytracingCmd.cmdList.Get(),
				g_glRaytracingLighting.labRawTexture.Get(),
				D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			g_glRaytracingLighting.labRawState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			glRaytracingTransition(
				g_glRaytracingCmd.cmdList.Get(),
				g_glRaytracingLighting.labHistoryTexture.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			g_glRaytracingLighting.labHistoryState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			g_glRaytracingLighting.historyValid = 1;
		}
		else
		{
			g_glRaytracingLighting.historyValid = 0;
		}
	}
	else
	{
		// The shim composites the lighting texture later in glLightScene(). Do not
		// copy directly to the swapchain here; that would race the normal resolve.
		glRaytracingTransition(
			g_glRaytracingCmd.cmdList.Get(),
			pass->outputTexture,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		g_glRaytracingLighting.historyValid = 0;
	}

	if (!glRaytracingEndCmd())
		return false;

	// The DXR and compatibility renderer command lists use the same D3D12
	// queue. Queue submission order provides the GPU dependency; an immediate
	// CPU fence wait only serializes every frame and destroys performance.
	if (!g_glRaytracingCleanVisualPerformance.asyncSubmit)
		glRaytracingWaitFenceValue(g_glRaytracingCmd.cmdLastFenceValue);

	g_glRaytracingCleanVisualHasOutput = 1;
	return true;
}

glRaytracingLight_t glRaytracingLightingMakePointLight(
	float px, float py, float pz,
	float radius,
	float r, float g, float b,
	float intensity)
{
	glRaytracingLight_t l = {};
	l.position.x = px;
	l.position.y = py;
	l.position.z = pz;
	l.radius = radius;

	l.color.x = r;
	l.color.y = g;
	l.color.z = b;
	l.intensity = intensity;

	l.normal.x = 0.0f;
	l.normal.y = 0.0f;
	l.normal.z = 1.0f;
	l.type = GL_RAYTRACING_LIGHT_TYPE_POINT;

	l.axisU.x = 1.0f;
	l.axisU.y = 0.0f;
	l.axisU.z = 0.0f;
	l.halfWidth = 0.0f;

	l.axisV.x = 0.0f;
	l.axisV.y = 1.0f;
	l.axisV.z = 0.0f;
	l.halfHeight = 0.0f;

	l.samples = 1;
	l.twoSided = 0;
	l.persistant = 0.0f;
	l.pad1 = 0.0f;
	return l;
}

glRaytracingLight_t glRaytracingLightingMakeRectLight(
	float px, float py, float pz,
	float nx, float ny, float nz,
	float ux, float uy, float uz,
	float vx, float vy, float vz,
	float halfWidth, float halfHeight,
	float r, float g, float b,
	float intensity,
	uint32_t samples,
	uint32_t twoSided)
{
	glRaytracingLight_t l = {};

	glRaytracingNormalize3(nx, ny, nz);
	glRaytracingNormalize3(ux, uy, uz);
	glRaytracingNormalize3(vx, vy, vz);

	if ((nx == 0.0f && ny == 0.0f && nz == 0.0f) &&
		!((ux == 0.0f && uy == 0.0f && uz == 0.0f) ||
			(vx == 0.0f && vy == 0.0f && vz == 0.0f)))
	{
		glRaytracingCross3(ux, uy, uz, vx, vy, vz, nx, ny, nz);
		glRaytracingNormalize3(nx, ny, nz);
	}

	l.position.x = px;
	l.position.y = py;
	l.position.z = pz;

	// Reuse radius as influence/falloff range for the rect light.
	l.radius = (halfWidth > halfHeight ? halfWidth : halfHeight) * 6.0f;

	l.color.x = r;
	l.color.y = g;
	l.color.z = b;
	l.intensity = intensity;

	l.normal.x = nx;
	l.normal.y = ny;
	l.normal.z = nz;
	l.type = GL_RAYTRACING_LIGHT_TYPE_RECT;

	l.axisU.x = ux;
	l.axisU.y = uy;
	l.axisU.z = uz;
	l.halfWidth = halfWidth;

	l.axisV.x = vx;
	l.axisV.y = vy;
	l.axisV.z = vz;
	l.halfHeight = halfHeight;

	l.samples = samples ? samples : 4u;
	l.twoSided = twoSided ? 1u : 0u;
	l.persistant = 0.0f;
	l.pad1 = 0.0f;

	return l;
}

uint32_t glRaytracingLightingGetLightCount(void)
{
	return (uint32_t)g_glRaytracingLighting.cpuLights.size();
}

