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

`DX12BindingTracker.cpp` is over 2000 lines and mixes live binding state, event recording, and frame-analysis export. It should be split by responsibility after the PreSkin fix is validated.
