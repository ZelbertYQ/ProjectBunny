# ADR 0005: DX12 Command-List Execution Uses Snapshots

## Status

Accepted

## Date

2026-06-27

## Context

DX12 moved TextureOverride lookup back into ShaderOverride and no-pattern ShaderRegex command-list execution to match DX11. The KeFUY Mod then used a deliberately broad ShaderRegex scope over common graphics shader models and executed three IA checks on many draws:

- `CheckTextureOverride = ib`
- `CheckTextureOverride = vb0`
- `CheckTextureOverride = vb1`

After PSO section deduplication and IA lookup caching, the runtime could still become unresponsive. The remaining issue was command-list execution scope. The executor no longer held `gModLock` for the whole top-level call in every path, but it still read global containers and compiled plan maps during execution. DX12 command lists may be recorded concurrently, so naked `unordered_map` access and mixed lock ownership were unsafe in a hot path.

There was also a lock-order risk between `gModLock` and `gPreSkinLock`. IA TextureOverride matching, BeginFrame cleanup, and explicit `match_cs` blocking could read active PreSkin state while the Mod lock was already held.

D3D12 command-list recording, resource view creation, descriptor creation, GPU virtual-address queries, barriers, and COM release can enter runtime or driver code. These operations should not run inside a global configuration lock.

## Decision

DX12 command-list execution uses snapshot-then-execute.

Under short `gModLock` reads, the runtime copies:

- command-list configs
- compiled command-list plans
- compiled TextureOverride plans
- matched ShaderOverride and ShaderRegex configs
- matched TextureOverride configs
- IA TextureOverride cache entries

After the snapshot is made, command-list execution proceeds without holding `gModLock`. Resource view materialization, loaded resource preparation, descriptor and resource work, `ResourceBarrier`, GPU virtual-address queries, and D3D12 command-list recording happen outside the global Mod lock.

Present command-list execution follows the same rule. The per-frame Present command-list marker is atomic state, not a reason to execute under `gModLock`.

Active PreSkin section state is snapshotted before entering Mod-lock-sensitive paths. Explicit `match_cs` blocking receives that snapshot instead of acquiring `gPreSkinLock` while `gModLock` is held. COM resources removed from PreSkin active maps are moved into a local release list and released after locks are dropped.

## Consequences

Broad ShaderRegex scopes can exercise the command-list path without racing global containers or holding the configuration lock across driver-facing work.

The global Mod lock is now a metadata protection boundary, not an execution boundary. Future changes must not read or mutate global `unordered_map` containers from command-list execution without a lock or a value snapshot.

The active PreSkin generation remains part of the IA lookup cache key, so cached misses are invalidated when `match_cs` output becomes active later in the frame.

This does not remove every fallback design in the DX12 runtime. Original-function fallback pointers and path fallback handling remain separate architectural debt and should be replaced by single resolution models later.
