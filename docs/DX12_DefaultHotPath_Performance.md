# DX12 Default Hot Path Performance

## 2026-06-27

### Symptom

With the DX12 DLL injected, safe mode enabled, overlay disabled, and no visible Mod work active, GPU utilization stayed around 5 percent while DX11 could keep the same scene near 95 percent.

The latest runtime log was already quiet after diagnostic JSON logging became opt-in. It showed only startup records:

- `safeMode:true`
- `overlay:false`
- `textureOverrides:4`
- no per-frame hook calls
- no `DX12FrameStats`

This means default disk logging was no longer the current bottleneck. The remaining default path still treated loaded TextureOverride data as active runtime work even when safe mode was enabled.

### Causes

DX12 safe mode disabled present replacement, but several TextureOverride, ShaderOverride, PreSkin, IA, and command-list runtime predicates still read raw loaded-state flags such as `gHasTextureOverrides` and `gHasShaderOverrides`.

That made the default safe-mode path keep entering Mod matching, IA candidate checks, PreSkin probes, shader override checks, command-list state capture, and resource metadata paths even though the runtime should behave like pure forwarding.

Profiling counters also ran `InterlockedIncrement` from hook paths even when profiling and diagnostic logging were disabled. Microsoft documents `InterlockedIncrement` as a full memory barrier, so using it per hook call is not a free counter in a high-frequency path.

Resource and descriptor creation hooks recorded metadata by default. Microsoft documents `CreateCommittedResource` as creating both a resource and an implicit heap, and descriptor heap creation returns a real heap object. The hook should forward these calls in the default path without adding metadata work unless FrameAnalysis, ShaderDump, or PreSkin needs it.

### Fix

Active Mod predicates now honor safe mode consistently:

- `DX12ModHasActiveShaderOverrides`
- `DX12ModHasActiveTextureOverrides`
- `DX12ModHasAnyActiveOverrides`
- `DX12ModNeedsPresentReplacement`
- `DX12ModNeedsPreSkinningUavProbe`
- `DX12ModHasActivePreSkinTextureOverrides`
- `DX12ModShouldProbePreSkinningForCs`

TextureOverride, ShaderOverride, PreSkin, IA replacement, and command-list runtime entry points now use those active predicates instead of raw loaded-state flags.

Shader bytecode replacement now exits before hashing when shader overrides are not active.

Profiling counters now collect only when diagnostic logging is enabled or F11 summary profiling is active. Default hook forwarding no longer pays per-hook interlocked counter cost.

Resource metadata tracking now starts disabled and is turned on by `DX12HotPathUpdate` only for heavy tracking or PreSkin needs. Resource creation hooks still allow Mod buffer-desc adjustment before forwarding, but metadata recording after successful creation is gated by active need.

`BunnyDX12RuntimeInitialize` now calls `DX12HotPathUpdate` after config and Mod loading, so the startup flags reflect the loaded runtime instead of the compile-time defaults.

### Verification

`build_debug_x64.ps1` succeeded and copied the DX12 DLL to:

`D:\SSMTCacheFolder\3Dmigoto\ZZMIDX12\d3d12.dll`

Static checks on touched source files found:

- no code comments
- no garbled text markers
- no UTF-8 BOM

Retest the same default safe-mode scene. The expected direction is:

- the log should remain startup-only unless `MIGOTO_DX12_DIAGNOSTIC_LOGS=1` is set
- safe mode with loaded but inactive TextureOverrides should keep `gDX12HotPathSkipAll=1`
- binding capture should stay skipped unless FrameAnalysis, ShaderDump, Hunt, or PreSkin requires it
- resource metadata recording should stay off unless heavy tracking or PreSkin requires it
- GPU utilization should rise if the remaining bottleneck was default hook-side CPU pressure

### Follow-up Risk

Several fallback designs remain and should be replaced by single resolution models:

- original-function lookup helpers still accept static fallback function pointers
- `DX12ModRuntimeIaRuntime.cpp` still has section-id fallback paths such as `fallbackSeen` and `fallbackConfig`
- resource copy fallback in FrameAnalysis may be valid as a diagnostic recovery path, but it should remain outside default gameplay hot paths

If GPU utilization remains low after this change, the next hypothesis should test command-list original lookup, hook coverage, and forwarding overhead under safe mode with diagnostics disabled.

### Debug Runtime Check Failure

The next test crashed with MSVC runtime check failure:

`Stack around the variable '_profile_timer' was corrupted.`

The failing variable is created by `DX12_PROFILE_SCOPE`. The prior change added a `bool` member to `DX12Profiling::ScopedTimer`, which changed the stack object size used by every hook translation unit. A clean rebuild fixes that class of mismatch, but the safer design is to avoid changing this hot-path object's layout at all.

`ScopedTimer` now uses the existing `mStart` sentinel to represent inactive collection. The class layout is back to the old two-field shape: counter reference plus `LARGE_INTEGER`.

This also documents a build-system hazard: header-only layout changes in hook hot-path RAII objects must be treated as clean-rebuild changes. The safer architectural rule is to avoid adding state to these stack objects unless the benefit is essential.

### Debug Logging Policy

DX12 diagnostic logging is controlled by `MIGOTO_DX12_DIAGNOSTIC_LOGS=1`.

Startup, reload, and error-level logs remain available without the switch. Hook-call logs, `DX12FrameStats`, ShaderRegex match logs, and per-draw ShaderOverride command-list logs are diagnostic data and must not run by default.

DEBUG logging still uses the background log queue instead of synchronous hook-side disk writes. The queue capacity is larger in DEBUG so full hook logging is less likely to drop lines during diagnosis, but hot-path debug logs are also capped where they can repeat every draw.

### ShaderRegex Hot Path Failure

The KeFUY ShaderRegex retest produced a 62 MB log and then became unresponsive. The log showed nearly every graphics draw executing the same ShaderRegex command list twice:

- `DX12ShaderOverrideCommandList.matches:2`
- both matched sections were identical
- one entry came from the VS model match
- one entry came from the PS model match

This doubled every `CheckTextureOverride = ib/vb0/vb1` command-list lookup on an intentionally broad no-pattern ShaderRegex. It also multiplied diagnostic JSON formatting and queue pressure.

DX12 now deduplicates no-pattern ShaderRegex configs by section within one PSO match cache, so a graphics PSO whose VS and PS both match the same ShaderRegex section executes that section once per draw.

The PSO match cache also no longer writes under an SRW shared lock. Cache hits can be read under shared lock, but cache misses are built and stored under the exclusive Mod lock.

The remaining risk is broad ShaderRegex scope itself: if a Mod lists common models such as `vs_6_0 ps_6_0`, many draws will still run one command-list check. DX11 has resource-hash caching optimized for this style. If GPU utilization remains low after deduplication, the next fix should be a clean command-list TextureOverride lookup cache keyed by current IA binding hash state and draw context, not a global TextureOverride fallback path.

### Command List TextureOverride Lookup Cache

The next KeFUY retest produced a small startup-only log and still became unresponsive. That ruled out default JSON logging as the active bottleneck. The loaded runtime state was:

- `shaderRegexes:1`
- `activeShaderOverrides:true`
- `shaderTriggeredTextureOverrides:true`
- `descriptorTextureOverrideTriggers:false`
- `preSkinProbeEnabled:true`

The Mod's no-pattern ShaderRegex intentionally matched common VS and PS models and executed three IA checks:

- `CheckTextureOverride = ib`
- `CheckTextureOverride = vb0`
- `CheckTextureOverride = vb1`

DX12 had PSO-level ShaderRegex deduplication, but each matching draw still recomputed the three TextureOverride lookups. DX11's `CheckTextureOverride` path first selects a command-list scope and then finds TextureOverride sections by resource hash and draw context. DX12 now keeps that trigger model but caches IA-target lookup results inside the command-list TextureOverride path.

The cache key contains:

- Mod reload generation
- PreSkin active generation
- target kind, shader stage, and slot
- current IA hash
- draw context fields

The value stores only a candidate index or a no-match result. It does not hold COM references, loaded resource ownership, descriptor heap objects, or command-list objects. The cache is cleared on Mod reload and bounded to avoid unbounded growth.

PreSkin active generation invalidates cached misses when a `match_cs` dependency becomes active later in the frame. This is required because a TextureOverride section that was intentionally inactive before its producer dispatch may become valid after PreSkin writes its output.

This fix reduces CPU work for broad ShaderRegex scopes without reintroducing the removed global IA TextureOverride matcher.

### Command List Executor Snapshot Fix

The next unresponsive retest showed the lookup cache was not enough. Broad ShaderRegex execution made the command-list path hot, while the executor still depended on global Mod containers during execution.

The command-list executor now follows a snapshot-then-execute rule:

- command-list configs are copied under a short shared `gModLock`
- compiled command-list and TextureOverride plans are copied under a short shared `gModLock`
- ShaderOverride and ShaderRegex PSO matches are copied to value snapshots before execution
- IA TextureOverride lookup cache reads and writes are protected by `gModLock`
- matched TextureOverride configs are copied out before resource view materialization
- descriptor/resource creation, `ResourceBarrier`, GPU virtual-address queries, and command-list recording run outside `gModLock`

The Present command-list path also stopped executing under `gModLock`. The one-per-frame Present marker now uses an atomic compare/exchange instead of holding the Mod lock around command-list execution.

The PreSkin path had a second lock-order risk: IA TextureOverride matching could enter `gModLock` and then read active PreSkin state through `gPreSkinLock`. BeginFrame and explicit `match_cs` blocking had similar nested-lock shapes. DX12 now snapshots active PreSkin sections before the Mod lock is taken, and resource release is moved outside locks.

This keeps the DX12 design aligned with the project rule that locks move metadata only. Driver-facing work, COM release, and GPU resource preparation do not run inside the global configuration lock.

`build_debug_x64.ps1` succeeded after this fix and copied the DX12 DLL to:

- `D:\Dev\ssmt4\src-tauri\resources\DX12\d3d12.dll`
- `D:\SSMTCacheFolder\3Dmigoto\ZZMIDX12\d3d12.dll`

### References

- https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-interlockedincrement
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createcommittedresource
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createdescriptorheap
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_graphics_pipeline_state_desc
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setpipelinestate
