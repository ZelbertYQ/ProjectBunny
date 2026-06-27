# DX12 ShaderRegex Command List Trigger

## 2026-06-27

### Symptom

The KeFUY Mod needed a ShaderRegex trigger with no shader pattern:

```ini
[ShaderRegex_KeFUY_Trigger]
shader_model = vs_5_0 ps_5_0
CheckTextureOverride = ib
CheckTextureOverride = vb0
CheckTextureOverride = vb1
```

DX12 had ShaderOverride hash matching, but no no-pattern ShaderRegex matching. After TextureOverride was made passive, a Mod without ShaderOverride hashes had no selected shader scope to execute `CheckTextureOverride`, so the TextureOverride data correctly stayed inactive.

### DX11 Model

DX11 parses the ShaderRegex main section for `shader_model`, command-list actions, and linked command lists. If the group has no pattern sections, DX11 links those command lists directly to the matching shader hash by shader model and skips shader disassembly.

At draw time, DX11 then runs the linked commands through the normal ShaderOverride path. A single ShaderRegex section can list multiple shader models, and each matching shader stage can independently select the command list.

### DX12 Fix

DX12 now parses ShaderRegex main sections into runtime configs and records shader models from DXBC or DXIL bytecode at PSO creation time.

When a graphics PSO is selected for a draw, DX12 checks the VS and PS model strings against no-pattern ShaderRegex configs. When a compute PSO is selected for dispatch, DX12 checks the CS model string. Matching ShaderRegex configs are converted into transient ShaderOverride-style command configs and executed through the existing command-list executor.

Pattern-backed ShaderRegex groups are not executed in DX12 yet. DX12 recognizes them and keeps them out of the executable ShaderRegex activation path so unsupported shader patching does not turn on draw-hook work.

### CheckTextureOverride Targets

The target parser now accepts DX11-style explicit shader-stage bindings:

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

Non-IA targets are valid for `CheckTextureOverride` lookup. They are not accepted as resource assignment targets in DX12 command lists yet.

DX12 resolves descriptor targets from the command-list binding tracker. It uses the active graphics or compute root signature, descriptor range type, shader visibility, shader register, and register space to reconstruct the current CBV/SRV/UAV binding for the requested target. Descriptor tables and root descriptors are both covered. The bound descriptor or root descriptor resource view is then hashed and looked up in the passive TextureOverride map.

### Performance Boundary

Descriptor binding tracking is enabled only when a ShaderOverride or executable no-pattern ShaderRegex command chain can reach a descriptor-target `CheckTextureOverride`. IA-only checks still use IA hash state and do not require descriptor tracking.

This keeps the default path aligned with DX11: loaded TextureOverride data is passive, ShaderOverride or ShaderRegex selects the shader scope, and only the selected command list performs the TextureOverride lookup.

### 2026-06-27 Namespace Dot Parsing Fix

The first KeFUY retest still logged `shaderRegexes:0` even though `KeFUY.ini` contained `[ShaderRegex_KeFUY_Trigger]`.

The loaded section name was namespaced through the include path, which contains `KeFUY.ini`. DX12 split ShaderRegex groups at the first dot after the `ShaderRegex` prefix, so it treated the dot in `.ini` as a ShaderRegex subsection separator. The main section was therefore marked pattern-backed, `shader_model` was never parsed, and the group was erased.

DX12 now mirrors the DX11 rule: namespaced section text can contain dots, so ShaderRegex subsection splitting only happens after the namespace. In the current DX12 namespaced section format, that means only dots after the final namespace separator can create `.Pattern`, `.Replace`, or `.InsertDeclarations` style sub-sections.

### 2026-06-27 PSO Section Deduplication

The next KeFUY retest parsed ShaderRegex successfully, but the runtime became unresponsive. The log showed the same ShaderRegex section executing twice for almost every graphics draw:

- `matches:2`
- both entries were `ShaderRegex\Mods\SSMTGeneratedMod\KeFUY\KeFUY.ini\_KeFUY_Trigger`
- the first match came from `vs_6_0`
- the second match came from `ps_6_0`

The Mod's no-pattern ShaderRegex intentionally listed both VS and PS models so it could select a broad DX11-style command-list scope. In DX12, graphics draws execute against a selected PSO. Microsoft documents `SetPipelineState` as setting all shaders for the GPU pipeline, and the graphics PSO descriptor contains separate VS and PS shader bytecode fields. That means the DX12 draw-scope command-list match must be unique by ShaderRegex section within the active graphics PSO, not duplicated once per shader stage.

DX12 now deduplicates no-pattern ShaderRegex transient configs by section for each PSO match cache. If the same section matches VS and PS in one graphics PSO, its command list runs once for that draw. Compute PSOs still match CS once.

The PSO match cache was also corrected so shared locking only reads existing cache entries. Cache misses are built and stored under the exclusive Mod lock. Shared-lock writes to `gShaderOverridePsoMatchCache` were unsafe because SRW shared mode allows concurrent readers, while `unordered_map` mutation requires exclusive ownership.

Hot-path ShaderRegex and ShaderOverride diagnostic logs are capped and remain behind `MIGOTO_DX12_DIAGNOSTIC_LOGS=1`, so diagnosis can be enabled deliberately without turning every draw into unbounded JSON queue work.

### References

- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_graphics_pipeline_state_desc
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_compute_pipeline_state_desc
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setpipelinestate
- https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signatures
