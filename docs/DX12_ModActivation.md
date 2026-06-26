# DX12 Mod Activation

## 2026-06-27

### Symptom

The runtime log loaded the ZZMIDX12 config successfully:

- `textureOverrides:4`
- `resources:4`
- `commandLists:3`
- included `Mods\SSMTGeneratedMod\KeFUY\KeFUY.ini`

The same log also reported `safeMode:true`. Every frame kept these counters at zero:

- `iaOverrideChecks`
- `iaCandidateTests`
- `preSkinCsTests`
- `preSkinUavTests`
- `preSkinApplied`

This proves the current failure was not a PreSkin hash miss. The Mod never entered the active DX12 runtime path.

### Cause

`RuntimeConfig::dx12SafeMode` defaulted to `true`. The active Mod predicates correctly honor safe mode, so any config without `[DX12] safe_mode = 0` loaded Mod data but disabled TextureOverride, ShaderOverride, PreSkin probing, IA replacement, and present runtime effects.

That default is not compatible with DX11 Mod behavior. DX11 TextureOverride lookup finds sections by hash and draw context and then executes their command lists; it does not require a separate runtime activation key for ordinary Mod effects.

### Fix

`RuntimeConfig::dx12SafeMode` now defaults to `false`. Explicit `[DX12] safe_mode = 1` or `[System] dx12_safe_mode = 1` still disables DX12 Mod work for performance diagnosis and pure-forward testing.

`DX12ModRuntime` load logging now reports both loaded counts and active state:

- `safeMode`
- `activeShaderOverrides`
- `activeTextureOverrides`
- `preSkinCandidates`
- `preSkinProbeEnabled`
- `presentRuntimeEffect`

If safe mode disables loaded Mod work, `DX12ModRuntimeDisabled` is logged with reason `safe_mode`.

### Verification Signal

After rebuilding and launching without an explicit safe mode setting, the startup log should show:

- `DX12Config.safeMode:false`
- `DX12ModRuntime.activeTextureOverrides:true`
- `DX12ModRuntime.preSkinCandidates:true`
- `DX12ModRuntime.preSkinProbeEnabled:true`

During gameplay for the KeFUY test Mod, the next expected signal is that `preSkinCsTests` becomes non-zero. If it stays zero, investigate hook flow or CS hash observation before changing any Mod ini.

### References

- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-dispatch
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setdescriptorheaps
- https://learn.microsoft.com/en-us/windows/win32/direct3d12/setting-descriptor-heaps
