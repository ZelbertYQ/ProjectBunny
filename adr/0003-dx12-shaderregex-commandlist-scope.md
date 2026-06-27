# ADR 0003: DX12 ShaderRegex Selects Command List Scope By Shader Model

## Status

Accepted

## Date

2026-06-27

## Context

TextureOverride sections are now passive until a command list explicitly executes `CheckTextureOverride`. That matches DX11 and removes the old global IA matcher, but it exposed a missing DX12 compatibility path: existing Mods can use no-pattern ShaderRegex sections to select the command-list scope by shader model instead of by exact ShaderOverride hash.

The required Mod syntax is:

```ini
[ShaderRegex_KeFUY_Trigger]
shader_model = vs_5_0 ps_5_0
CheckTextureOverride = ib
CheckTextureOverride = vb0
CheckTextureOverride = vb1
```

DX11 links no-pattern ShaderRegex groups to matching shader hashes without shader disassembly. Pattern-backed ShaderRegex groups are a shader patching feature and are a separate mechanism.

D3D12 stores shader bytecode in graphics and compute PSO descriptions, and `SetPipelineState` selects the active PSO for later draws or dispatches. Descriptor targets such as `ps-t1` or `vs-cb0` must be reconstructed through the active root signature, descriptor ranges, shader visibility, register, and space.

## Decision

DX12 supports no-pattern ShaderRegex as a command-list trigger scope.

At PSO creation time, DX12 records VS, PS, and CS shader models from DXBC or DXIL bytecode. At draw or dispatch time, the active PSO is matched against executable no-pattern ShaderRegex configs. Matches are executed through the existing ShaderOverride command-list executor, preserving the same passive TextureOverride model.

Pattern-backed ShaderRegex groups are parsed as non-executable in DX12 until shader patching is implemented. They do not activate ShaderOverride hot-path predicates by themselves.

`CheckTextureOverride` now supports DX11-style targets for IA and explicit shader-stage CBV/SRV/UAV bindings:

- `ib`
- `vbN`
- `vs-cbN`
- `ps-cbN`
- `cs-cbN`
- `vs-tN`
- `ps-tN`
- `cs-tN`
- `ps-uN`
- `cs-uN`

Bare CBV syntax such as `cb0` is intentionally not accepted because DX11 ResourceCopyTarget syntax carries the shader stage.

Descriptor binding tracking is enabled only when a reachable command chain contains descriptor-target `CheckTextureOverride`. This keeps IA-only Mods from paying descriptor tracking cost.

## Consequences

Existing DX11-style no-pattern ShaderRegex trigger Mods can select TextureOverride checks in DX12 without adding exact ShaderOverride hashes.

The implementation stays inside the existing command-list execution model instead of adding a new global TextureOverride lookup path.

The current scope covers VS, PS, and CS shader model matching. HS, DS, and GS model matching are not part of this change because the immediate DX12 runtime only had draw and dispatch command-list selection for VS, PS, and CS.

Namespaced ShaderRegex section parsing must ignore dots that belong to include namespaces or file names such as `KeFUY.ini`. Only dots in the section suffix after the namespace separator are valid ShaderRegex subsection separators.
