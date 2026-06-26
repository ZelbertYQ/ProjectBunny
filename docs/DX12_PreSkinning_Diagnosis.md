# DX12 PreSkinning Diagnosis

## 2026-06-27

### Symptom

The runtime log showed IA texture override candidates matching `dffb91a3`, but IA replacement was suppressed with `inactive_preskin_dependency`.

Frame stats also showed:

- `iaCandidateHits` was non-zero
- `iaApplied` stayed zero
- `preSkinCsTests` was non-zero
- `preSkinUavHits` and `preSkinApplied` stayed zero

The repeated probe for CS `93db774c5ca9a3ea` reported `no_compute_srvs` and `uavs:0`.

### Cause

The PreSkin dispatch path used the lightweight direct binding tracker when normal frame-analysis binding capture was disabled.

Compute root tables, root descriptors, constants, and descriptor heaps were written into that tracker, but the active pipeline state was only synchronized when full binding capture was enabled. Dispatch still knew the compute shader from `DX12CommandListRuntimeState`, while `DX12BindingGetCurrentComputeSrvs` and `DX12BindingGetCurrentComputeUavs` could not derive the PSO root signature from the lightweight tracker snapshot.

Without the PSO/root signature, descriptor tables could not be expanded into SRV/UAV bindings, so match_cs input hashing never saw `cs-t0` and `cs-t1`.

### Fix

When PreSkinning needs UAV probing, `DX12CommandListCapturePipelineState` now also stores the active PSO into `DX12BindingTracker`.

Binding event logging remains gated by full binding capture, so this does not turn the hot path into frame-analysis logging.

### Verification

After building, retest the same Mod and check `D:\SSMTCacheFolder\3Dmigoto\ZZMIDX12\d3d12_log.jsonl`.

The expected direction is:

- `DX12PreSkinDispatchProbe` should no longer repeatedly report `no_compute_srvs` for CS `93db774c5ca9a3ea`
- `DX12PreSkinMatchCsProbe` should show SRV input hashes for the configured `match_cs_t0_hash` and `match_cs_t1_hash`
- `preSkinUavHits` or `preSkinApplied` should increase when the matching dispatch is reached
- IA replacement for `dffb91a3` should no longer be suppressed by `inactive_preskin_dependency`

### Follow-up Risk

`DX12ModRuntimeIaRuntime.cpp` still contains section-id fallback paths such as `fallbackSeen` and `fallbackConfig`. They were not the root cause of this PreSkin failure, but they are special fallback logic and should be replaced with a single section identity model.

`DX12BindingTracker.cpp` used to be over 2000 lines and mixed live binding state, event recording, and frame-analysis export. It should remain split by responsibility so future PreSkin fixes do not reintroduce hidden coupling.

### Second Failure

The next runtime log proved that PreSkinning was not globally dead. The first frames showed successful `DX12IaReplacement` entries for `dffb91a3`, including the PreSkin position buffer bound to `vb0`, the texcoord buffer bound to `vb1`, the replacement index buffer, and the replacement draw.

Later frames still produced `inactive_preskin_dependency` and `preSkinApplied:0`. The later PreSkin probes for CS `93db774c5ca9a3ea` returned to `no_compute_srvs`, sometimes while still seeing one UAV.

### Second Cause

PreSkin descriptor patching runs under `DX12InternalReplayScope`. During the patch, the real command list receives temporary descriptor heaps and root descriptor tables, while the DX12 runtime and binding tracker mirrors intentionally do not observe those hook calls.

Restore used the mirror state to decide whether the real descriptor heap needed to be restored. Since the mirror still described the original heap, restore could skip the real `SetDescriptorHeaps` call even though the real command list was still using the temporary PreSkin heap. The root tables were restored, but the heap/table mirror and the actual command list state could diverge after the temporary patch.

Once the command list continued recording, `DX12BindingGetCurrentComputeSrvs` could no longer reliably expand the game SRV tables for later PreSkin dispatches. That produced the later `no_compute_srvs` probes and caused the active PreSkin dependency to become inactive again.

### Second Fix

`DX12ModRestorePreSkinningUavReplacement` now always restores the real descriptor heaps after a PreSkin patch and then synchronizes the DX12 runtime and binding tracker mirrors to the restored heap and root table state.

The `post_dispatch` log now includes the texture override section and present number so the next test can directly correlate PreSkin output with IA replacement in the same frame sequence.

### Third Failure

The latest runtime log again showed `DX12PreSkinDispatchProbe` with `reason:"no_compute_srvs"` for CS `93db774c5ca9a3ea`. Unlike the first failure, many probes had `uavs:1`, so the command list state was only partially visible.

Frame stats kept reporting non-zero `preSkinCsTests` and `preSkinUavTests`, while `preSkinUavHits`, `preSkinApplied`, and IA replacement all stayed zero. IA replacement was still suppressed by `inactive_preskin_dependency`.

### Third Cause

The previous fixes kept PreSkin alive through a special direct compute-binding tracker path while normal binding capture stayed disabled outside FrameAnalysis and ShaderDump.

`DX12HotPathUpdate` still set `gDX12HotPathSkipBindings` from heavy capture needs only. During normal gameplay with PreSkin enabled, many root binding hooks could stay in the fast path. Some fast paths wrote directly into `DX12BindingTracker`, but that special path did not share the same top-level tracking semantics as the normal capture path.

This split design let PSO, descriptor heaps, root signatures, tables, descriptors, constants, and runtime serials drift by hook and by frame. The visible symptom was exactly a partial state snapshot: the CS hash and sometimes the UAV table were visible, but SRV descriptor tables were not reliably expandable.

### Third Fix

PreSkin UAV probing is now treated as a normal reason to track command-list binding state.

`DX12CommandListCaptureShouldTrackBindings` includes `DX12ModNeedsPreSkinningUavProbe`, and `DX12HotPathUpdate` keeps binding hooks active when PreSkin needs them. `DX12CommandListCaptureShouldTrackPsoState` also includes PreSkin, so Dispatch CS lookup and binding expansion use the same runtime state model.

The special direct PreSkin compute-binding branch was removed from command-list hooks. Descriptor heaps, compute root signatures, descriptor tables, root descriptors, and root constants now enter `DX12BindingTracker` through the same capture path used by FrameAnalysis, while binding event recording remains gated by FrameAnalysis and ShaderDump.

### Third Verification

After rebuilding, retest the same Mod and check `D:\SSMTCacheFolder\3Dmigoto\ZZMIDX12\d3d12_log.jsonl`.

The expected direction is:

- `DX12PreSkinDispatchProbe` should stop repeating `no_compute_srvs` for configured PreSkin CS `93db774c5ca9a3ea`
- `DX12PreSkinMatchCsProbe` should show `cs-t0` and `cs-t1` hash checks for `dbc98878` and `bfa79855`
- `preSkinUavHits` and `preSkinApplied` should become non-zero on matching frames
- IA replacement for `dffb91a3` should stop reporting `inactive_preskin_dependency`

### Third Follow-up Risk

`DX12BindingTracker.cpp` was split into live tracking, frame export, resource export, and shared private helpers after this fix. The split keeps each cpp below 1000 lines and makes future binding-state changes easier to review without mixing runtime capture with frame-analysis output.

The remaining section-id fallback paths in `DX12ModRuntimeIaRuntime.cpp` are still architectural debt. They were not changed here because the reproduced failure was in command-list binding state tracking, not texture override identity resolution.
