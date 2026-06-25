#pragma once

#include <Windows.h>
#include <stdio.h>
#include <wchar.h>

// ---------------------------------------------------------------------------
// DX12 Performance Profiling
// ---------------------------------------------------------------------------
// Mirror of DX11's profiling system.  Records per-operation call counts and
// CPU times using QueryPerformanceCounter, then dumps per-frame summaries to
// the JSON log and/or the overlay.  Toggle with F11.
//
// Usage in hook functions:
//   DX12_PROFILE_SCOPE(DrawInstanced);
//   // ... hook body ...
//
// This increments the DrawInstanced counter and adds the elapsed CPU time to
// the DrawInstanced accumulator.  The SCOPE macro uses RAII so timing stops
// even if the function returns early (including fast-forward).
// When SUMMARY profiling is disabled, the scope only increments the call
// counter. High-resolution timing stays behind F11 because these hooks can run
// thousands of times per frame.
// ---------------------------------------------------------------------------

namespace DX12Profiling {

enum class Mode {
	NONE = 0,
	SUMMARY,          // per-frame aggregate counts + timings → log + overlay
};

struct Counter {
	volatile LONG calls = 0;       // total calls this frame
	volatile LONG64 cpuTicks = 0;  // cumulative QPC ticks this frame
};

// Per-operation counters — one per hooked D3D12 API.
extern Counter gDrawInstanced;
extern Counter gDrawIndexedInstanced;
extern Counter gDispatch;
extern Counter gSetPipelineState;
extern Counter gSetDescriptorHeaps;
extern Counter gSetComputeRootSignature;
extern Counter gSetGraphicsRootSignature;
extern Counter gSetComputeRootDescriptorTable;
extern Counter gSetGraphicsRootDescriptorTable;
extern Counter gSetComputeRootConstantBufferView;
extern Counter gSetGraphicsRootConstantBufferView;
extern Counter gSetComputeRootShaderResourceView;
extern Counter gSetGraphicsRootShaderResourceView;
extern Counter gSetComputeRootUnorderedAccessView;
extern Counter gSetGraphicsRootUnorderedAccessView;
extern Counter gIASetIndexBuffer;
extern Counter gIASetVertexBuffers;
extern Counter gIASetPrimitiveTopology;
extern Counter gResourceBarrier;
extern Counter gExecuteCommandLists;
extern Counter gPresent;
extern Counter gResetCommandList;
extern Counter gCloseCommandList;
extern Counter gIaDrawOverrideChecks;
extern Counter gIaHashStateHits;
extern Counter gIaHashStateMisses;
extern Counter gIaTextureMayHits;
extern Counter gIaTextureMayMisses;
extern Counter gIaPrepareCalls;
extern Counter gIaApplied;
extern Counter gIaCandidateTests;
extern Counter gIaCandidateHits;
extern Counter gIaHuntIaUpdates;
extern Counter gIaReplacementDraws;
extern Counter gIaReplacementIndexedDraws;
extern Counter gIaReplacementDispatches;
extern Counter gPreSkinCsTests;
extern Counter gPreSkinUavTests;
extern Counter gPreSkinUavHits;
extern Counter gPreSkinApplied;
extern Counter gPreSkinDispatchResized;

// Aggregate counters for summary grouping.
extern Counter gAllDraws;        // DrawInstanced + DrawIndexedInstanced
extern Counter gAllDispatches;   // Dispatch
extern Counter gAllBindings;     // all SetRoot* + SetDescriptorHeaps + IASet*
extern Counter gAllCommands;     // sum of everything above
extern Counter gFastForwardHits; // how many calls took the fast-forward path

// Frame-level state.
extern Mode gMode;
extern volatile LONG gFramePresentCount;
extern LARGE_INTEGER gFrameStartTime;
extern LARGE_INTEGER gPerfFrequency;
extern wchar_t gOverlayText[2048];

// Call once at DLL init.
void Init();

// Call at the start of each Present (before the original Present call).
void BeginFrame();

// Call at the end of each Present (after DX12FlushLog etc.).
void EndFrame();

// Toggle profiling on/off (F11).
void Toggle();

// True when profiling is active.
bool IsActive();

void RecordIaDrawOverrideCheck();
void RecordIaHashStateResult(bool hit);
void RecordIaTextureMayMatchResult(bool hit);
void RecordIaPrepareCall();
void RecordIaApplied();
void RecordIaCandidateTest(bool hit);
void RecordIaHuntIaUpdate();
void RecordIaReplacementDraw(bool indexed);
void RecordIaReplacementDispatch();
void RecordPreSkinCsTest(bool hit);
void RecordPreSkinUavTest(bool hit);
void RecordPreSkinApplied();
void RecordPreSkinDispatchResized();

// Lightweight per-frame stats dump to JSON log (counts only, no timing).
// Always runs regardless of profiling mode — gives visibility without F11.
void LogFrameStats();

// Output current frame summary to log + overlay text.
void FlushFrame();

// RAII scope guard for timing a single hook invocation.
class ScopedTimer {
public:
	explicit ScopedTimer(Counter &counter);
	~ScopedTimer();
private:
	Counter &mCounter;
	LARGE_INTEGER mStart;
};

} // namespace DX12Profiling

// Convenience macro: place at the top of a hook function to profile it.
#define DX12_PROFILE_SCOPE(counter) \
	DX12Profiling::ScopedTimer _profile_timer(DX12Profiling::g##counter)

// Fast-forward counter increment (no timing, just count).
#define DX12_PROFILE_FAST_FORWARD() do { \
	InterlockedIncrement(&DX12Profiling::gFastForwardHits.calls); \
} while (0)
