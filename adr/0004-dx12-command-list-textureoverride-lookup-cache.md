# ADR 0004: DX12 Caches Command-List TextureOverride IA Lookups

## Status

Accepted

## Date

2026-06-27

## Context

DX12 now matches DX11's trigger model: TextureOverride sections are passive until a selected ShaderOverride or executable no-pattern ShaderRegex command list executes `CheckTextureOverride`.

The KeFUY Mod uses a broad no-pattern ShaderRegex over common VS and PS models, then checks `ib`, `vb0`, and `vb1`. After PSO section deduplication, the same section no longer ran twice per graphics PSO, but the runtime could still become unresponsive because every matching draw recomputed all three IA TextureOverride lookups.

DX11 keeps TextureOverride lookup inside the command-list path and uses hash-based filtering before draw-context checks. DX12 needed the same low-cost repeated lookup behavior without returning to the removed global IA matcher.

## Decision

DX12 caches command-list-triggered IA TextureOverride lookup results.

The cache key includes:

- Mod reload generation
- PreSkin active generation
- target kind, shader stage, and slot
- current IA hash
- draw context fields

The cache value stores a matched IA candidate index or a no-match result. It does not retain COM objects or loaded resource ownership. The cache is bounded and cleared on Mod reload.

Descriptor targets are not included in this IA cache. They depend on descriptor table and root descriptor state, so they require a separate binding-serial cache if a future Mod needs that optimization.

## Consequences

Broad ShaderRegex scopes can execute `CheckTextureOverride = ib/vb0/vb1` repeatedly without scanning the same TextureOverride candidate lists on every draw.

`match_cs` dependencies remain correct because PreSkin active generation invalidates earlier no-match results when a producer dispatch activates a section later in the frame.

The cache preserves the accepted architecture: TextureOverride data remains passive, and lookup only happens from selected command-list execution.

Command-list execution must still follow the snapshot model from ADR 0005. Cached lookup results are metadata only and must not justify holding `gModLock` across resource preparation or command-list recording.
