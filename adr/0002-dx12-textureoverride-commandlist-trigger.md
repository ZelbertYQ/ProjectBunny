# ADR 0002: DX12 TextureOverride Is Triggered By Command Lists

## Status

Accepted

## Date

2026-06-27

## Context

DX12 treated loaded TextureOverride data as a reason to run draw-hook IA matching globally. The draw path entered Mod work when any active TextureOverride existed, IASet hooks maintained TextureOverride candidate flags, and each draw could attempt automatic IA replacement.

DX11 uses a narrower model. ShaderOverride or ShaderRegex first selects a shader scope. TextureOverride lookup is then triggered by explicit command list actions such as `CheckTextureOverride = ib`.

The DX12 global matcher was both slower and less faithful to existing Mod authoring practice. It also created secondary mechanisms such as candidate flags, related-mesh expansion, and duplicate automatic replacement suppression.

## Decision

DX12 TextureOverride sections are passive until a command list explicitly checks them.

Draw hooks enter Mod work only for present replacement and active ShaderOverride command lists. TextureOverride matching is available through command list execution, especially `CheckTextureOverride`, and it uses the current IA state captured for ShaderOverride.

Loaded TextureOverride data alone must not enable global draw matching, PSO tracking, IA binding capture, per-frame binding resets, or automatic replacement dedupe.

PreSkin remains compute-dispatch driven by `match_cs` and UAV/SRV binding state, but its TextureOverride sections are still passive data. PreSkin probing may enable binding capture only when loaded PreSkin TextureOverride candidates are reachable from a ShaderOverride command chain that can execute `CheckTextureOverride`.

Startup logging must distinguish these states:

- `textureOverrides`: loaded passive data
- `shaderTriggeredTextureOverrides`: TextureOverride lookup is reachable from ShaderOverride command execution
- `preSkinCandidates`: loaded PreSkin-capable TextureOverride data exists
- `preSkinProbeEnabled`: PreSkin candidates are both loaded and reachable from ShaderOverride-triggered TextureOverride execution

## Consequences

DX12 now matches the DX11 performance model more closely. Mod packs that only contain TextureOverride sections need a ShaderOverride or ShaderRegex entry that calls `CheckTextureOverride` for the relevant IA targets.

The default injected path is lighter when TextureOverrides are loaded but no shader scope selected them. Loading `match_cs` TextureOverrides by itself no longer enables global compute binding capture, PSO tracking, per-frame binding resets, or resource metadata tracking.

The old fallback-like automatic IA expansion path is removed instead of being bypassed. Future TextureOverride work should extend command-list execution or ShaderOverride matching, not reintroduce a global per-draw matcher.
