# DX12 TextureOverride Command List Trigger

## 2026-06-27

### Symptom

DX12 could load TextureOverride sections and then keep the draw hook hot path active even when no ShaderOverride command list had selected the current draw.

That made TextureOverride data behave like a global automatic IA matcher. In a scene with the DX12 DLL injected, no visible Mod effect, and the overlay in its default state, this design could keep CPU-side draw interception busy enough that GPU utilization stayed near idle.

### DX11 Model

DX11 does not make every loaded TextureOverride globally execute on every draw.

The normal path first matches a ShaderOverride or ShaderRegex against the currently bound shader. The command list owned by that ShaderOverride can then execute `CheckTextureOverride`, and that command performs TextureOverride lookup for the requested target such as `ib`, `vb0`, or `vb1`.

This means Mod authors can bound the expensive TextureOverride search with ShaderOverride hashes or ShaderRegex, and draws outside that selected shader scope do not pay TextureOverride matching cost.

### DX12 Fix

DX12 now follows the same trigger model.

Loaded TextureOverride sections are passive data. They do not make draw hooks perform global IA matching by themselves.

The draw hook enters Mod work only for:

- Present command list replacement
- active ShaderOverride command lists

TextureOverride matching is entered only from explicit command list actions:

- `CheckTextureOverride = ib`
- `CheckTextureOverride = vb0`
- `CheckTextureOverride = vb1`

ShaderOverride command lists still receive the current IA hash state, so `CheckTextureOverride` can find the correct TextureOverride by target hash and draw context. IA state capture is now kept when ShaderOverride is active, not merely because TextureOverride data exists.

### Removed DX12 Design

The old global IA TextureOverride path was removed:

- per-command-list IA TextureOverride candidate flag
- candidate hash probes from IASet hooks
- automatic per-draw `DX12ModPrepareIaReplacement`
- automatic IA replacement duplicate suppression
- related-mesh automatic TextureOverride expansion
- TextureOverride-driven PSO and IA tracking
- TextureOverride-driven draw hot-path activation

PreSkin remains compute-dispatch driven. It is not a generic per-draw TextureOverride matcher. Its binding tracking remains active only when PreSkin UAV probing is needed.

### Runtime Expectations

With only TextureOverride sections loaded and no matching ShaderOverride command list:

- draw hooks should fast-forward
- PSO state tracking should not be enabled just for TextureOverride
- IA binding capture should not run just for TextureOverride
- `DX12BindingBeginFrame` should not run every frame just for TextureOverride

With a matching ShaderOverride command list:

- PSO state is tracked so the ShaderOverride can be selected
- IA state is tracked so `CheckTextureOverride` can resolve `ib` and `vbN`
- only the selected shader scope runs TextureOverride matching

### References

- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-drawindexedinstanced
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setpipelinestate
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-dispatch
