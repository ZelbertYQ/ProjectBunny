# ADR 0001: DX12 Safe Mode Is Opt-In

## Status

Accepted

## Date

2026-06-27

## Context

DX12 safe mode is useful for performance diagnosis because it turns the runtime into a near pure-forwarding path. A previous performance fix made active Mod predicates honor safe mode consistently.

The latest Mod failure showed the opposite risk: safe mode defaulted to enabled, so configs that did not explicitly opt out loaded TextureOverride and Resource data but never executed IA replacement or PreSkin probing. The log showed loaded Mod counts while all Mod and PreSkin runtime counters stayed at zero.

DX11-compatible Mod files should work when included. The compatibility model should not require every existing DX11-style Mod pack to add a DX12-only activation key before ordinary TextureOverride effects run.

## Decision

DX12 safe mode defaults to off.

Safe mode remains available only through explicit configuration:

- `[DX12] safe_mode = 1`
- `[System] dx12_safe_mode = 1`

Runtime load logs must distinguish loaded data from active runtime work. When safe mode disables loaded Mod work, the runtime emits a `DX12ModRuntimeDisabled` log row with reason `safe_mode`.

## Consequences

DX12 now follows the DX11 Mod author expectation: loaded TextureOverrides are active by default.

Pure-forward performance tests must explicitly enable safe mode. That is a good tradeoff because a diagnostic mode should not silently become the default behavior for Mod playback.

When `preSkinCsTests` is zero, the first check is `safeMode` and `preSkinProbeEnabled` in the startup log. Only after those are active should debugging move to CS hashes, descriptor tables, or SRV/UAV matching.
