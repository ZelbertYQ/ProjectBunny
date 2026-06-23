# DX12 Mod Support Plan

## Current Goal

Add DirectX 12 mod support in stages, starting with the shortest usable loop:

1. Shader replacement during PSO creation.
2. IA vertex/index buffer replacement.
3. Descriptor/resource replacement.
4. DX11-style command list compatibility where it is safe to map to DX12.

## Current Status

DX12 already has the capture and tracking foundation needed for the workflow:

- F8 frame analysis dumps resources and manifests.
- F9 shader dump writes shader bytecode and disassembly to `ShaderFixes`.
- PSO creation, descriptor creation/copy, root bindings, IA buffers, draw/dispatch, and present are hooked.
- Shader hashes use 64-bit FNV-1a over bytecode and file names follow `<hash>-<stage>.bin`.

- DX12 now has the first small mod runtime layer:

- Basic `[ShaderOverride*]` execution for shader replacement.
- `handling = skip` for VS/PS/CS shader override matches.
- Basic `[Include]` support so shader overrides can be discovered from `Mods`.
- F10 reload for INI and shader replacement PSO cache.
- Lightweight hunting overlay for shader and IA buffer identification.
- First `[TextureOverride*]` runtime matching slice for IA IB/VB hash + `handling = skip`.

DX12 still does not have the full DX11 mod runtime layer:

- No `[Resource*]` loading.
- No `[TextureOverride*]` resource replacement yet.
- No command list execution.

## Progress Log

### 2026-06-23

Completed:

- Added `DX12ModRuntime` as the first DX12-specific mod runtime layer.
- Parse `[ShaderOverride*]` sections from the active `d3dx.ini`.
- Support `hash = <hex>` for shader overrides.
- Look for replacement bytecode at `ShaderFixes\<hash>-<stage>.bin`.
- Apply replacement bytecode before graphics and compute PSO creation.
- Log `DX12ShaderOverrideApplied` when a replacement is used.
- Log `DX12ShaderOverrideMissingReplacement` when a matching override has no replacement file.
- Added `DX12ModRuntime.cpp` to the DirectX12 build.
- Debug x64 build passed via `.\build_debug_x64.ps1`.
- Added shared Core parser files:
  - `src/Core/MigotoShaderOverride.h`
  - `src/Core/MigotoShaderOverride.cpp`
- Moved basic `[ShaderOverride*] hash = ...` parsing into BunnyCore so DX11 and DX12 can converge on shared INI semantics over time.
- Added DX12 F10 hotkey handling.
- F10 now reloads the DX12 mod runtime config and invalidates replacement PSO cache.
- DX12 now records original graphics/compute PSO descriptors with deep-copied shader bytecode.
- `SetPipelineState` now lazily creates and binds a replacement PSO for the current reload generation.
- Replacement PSO creation uses the original D3D12 device function to avoid recursively entering the hook.
- Added `handling = skip` parsing to the shared BunnyCore `[ShaderOverride*]` parser.
- Added shared `MigotoIniLoader` for basic 3Dmigoto include expansion.
- DX12 mod runtime now loads root `d3dx.ini` plus included INI files before parsing shader overrides.
- Supported include forms for DX12 MVP:
  - `[Include] include = SomeFile.ini`
  - `[Include] include_recursive = Mods`
  - relative includes from included INI directories
  - `exclude_recursive = DISABLED*` style prefix filters
  - built-in skip for directories/files starting with `disabled`
- DX12 draw/dispatch now skips matching shader overrides:
  - graphics skip matches original VS or PS hash;
  - compute skip matches original CS hash;
  - replacement PSOs still use the original PSO descriptor for skip matching where available.
- Added DX12 shader hunting hotkeys and overlay display:
  - `Numpad0`: toggle hunting.
  - `Numpad4/5`: previous/next VS.
  - `Numpad1/2`: previous/next PS.
  - `Decimal + Numpad1/2`: previous/next CS.
- Fixed startup rendering regression caused by hunting being enabled by default.
- DX12 hunting now defaults to off and must be enabled manually with `Numpad0`.
- Skip logs now distinguish `mod_shader_skip` from `hunt_selection`.
- Added first IA buffer hunting path:
  - `Numpad7/8`: previous/next IB binding.
  - `Decimal + Numpad7/8`: previous/next VB binding.
  - selected IB/VB binding hides matching draws, like shader hunting.
  - displayed IA key is `role + slot + GPU VA + size + stride + format + resource pointer`.
- `DX12BindingTracker` now exposes the current command-list IA state for hunting reuse.
- Binding tracking now stays active while DX12 hunting is enabled, not only during F8/F9 capture.
- Investigated a DX12 "game not responding" report from `D:\SSMTCacheFolder\3Dmigoto\ZZMIDX12\d3d12_log.jsonl`.
  - Log showed `DX12ModRuntime` loaded with `shaderOverrides=0`, so no shader replacement or `handling=skip` path was active.
  - Log showed `Present1` continuing through frame 900, so the render path was still returning from Present rather than hard-deadlocking inside the Present hook.
  - Highest-risk runtime feature was the separate topmost overlay window while `safe_mode=true`.
- Tightened the default DX12 safe behavior:
  - DX12 overlay now defaults off in shared runtime config.
  - `safe_mode=true` now suppresses the DX12 overlay even if `overlay=true` is present.
  - `DX12Config` log now includes `overlayStarted` to make this visible in test logs.
  - Mod PSO replacement and `handling=skip` now fast-return when no shader overrides are loaded.
  - Hunting no longer collects PSO hashes on every `SetPipelineState` while hunting is disabled.
- Reworked DX12 hunting after in-game testing showed severe frame drops and over-hiding:
  - `Numpad0` now only enables collection/overlay; it does not hide the default selected shader.
  - VS/PS/CS hiding is armed only after the user presses the corresponding previous/next key.
  - Per-draw `hunt_selection` JSON logging was removed because it can produce thousands of lines per frame.
  - Safe-mode command-list hooks were reduced to the shader hunting/replacement minimum: reset, draw, dispatch, and `SetPipelineState`.
  - DX12 green hunting text now draws through the swap-chain GDI fallback while hunting is enabled, because the separate overlay window is disabled in safe mode.
- Reworked DX12 green text again after the swap-chain GDI fallback did not show in-game:
  - DX12 flip-model swap chains can discard or composite over GDI drawn directly on the game window, so Present-after-GDI is not reliable.
  - `Numpad0` now lazily creates a click-through topmost layered overlay window covering the virtual screen.
  - The overlay uses `WS_EX_NOACTIVATE`, `WS_EX_TRANSPARENT`, `WS_EX_TOOLWINDOW`, and `WS_EX_TOPMOST` so it should not steal focus or mouse input.
  - Closing hunting with `Numpad0` posts `WM_CLOSE` to the overlay window.
  - Swap-chain GDI drawing remains only as a fallback if the overlay window cannot be created.
- Added more DX11-like hunting controls:
  - `Numpad+` resets the active hidden selection without turning hunting off.
  - Live IB/VB hunting is back through a lightweight IA-only tracker that watches only `IASetIndexBuffer` and `IASetVertexBuffers`.
  - `Numpad7/8` selects and hides IB bindings.
  - `Decimal + Numpad7/8` selects and hides VB bindings.
  - Overlay text is now larger and centered at the top of the virtual screen.
- Polished DX12 hunting input/state behavior:
  - Held numpad browse keys now auto-repeat after a short delay, similar to DX11 hunting.
  - IB/VB hunting keys are deduplicated by resolved D3D12 buffer resource when possible, falling back to GPU VA only when resource metadata is unavailable.
  - `Numpad+` reset now visibly returns the overlay to `0/N` selection state for VS/PS/CS/IB/VB.
- Reworked live DX12 IB/VB hunting to use hash-based selection instead of resource pointer/GPU VA display:
  - IB/VB visited sets now store 32-bit buffer view hashes.
  - overlay now shows only `IB/VB` hash values, not byte/VA/resource details.
  - selected IB/VB hiding compares by hash, so selection is no longer tied to a transient resource pointer or GPU virtual address.
  - current live hash is a lightweight resource-view hash built from D3D12 buffer desc plus view offset/size/heap type; it intentionally excludes resource pointer and GPU VA.
  - `Numpad9` copies the most recently selected VS/PS/CS/IB/VB hash to the clipboard.
- Made F8 output searchable by the same live hunting hash:
  - `summary\BuffersDX12.csv` now includes `hunt_hash`.
  - `BindIA` rows in the frame-analysis JSON manifest now include `hunt_hash`.
  - `hunt_hash` uses the same `DX12HashBufferResourceView()` path as the hunting overlay and `Numpad9` copy.
  - F8 deduped file `hash` remains the readback/content hash; use `hunt_hash` when matching against green-text hunting.
- Tightened IB/VB de-duplication to match DX11's hunting behavior more closely:
  - live IB/VB browsing uses a resource-level hash, not a view-range hash;
  - the hash excludes transient resource pointer, GPU VA, view offset, and view size;
  - F8 `BuffersDX12.csv` de-duplicates IA rows by `role + hunt_hash`, while retaining slot/stride/format as sample context.
- Restricted DX12 hunting hotkeys to hunting mode:
  - `Numpad0` remains the only hunting key active while green text is off;
  - reset/copy/browse keys are ignored until `Numpad0` enables hunting;
  - repeat-key state is cleared while hunting is off to avoid stale held-key repeats.
- Made `Numpad0` behave as a pause/resume for hunting effects:
  - turning hunting off disables shader/IB/VB hiding immediately;
  - selected hashes and armed categories are preserved while hunting is off;
  - turning hunting on again restores the previous selection and hiding state.

Build artifact copied by the project script:

- `D:\Dev\ssmt4\src-tauri\resources\DX12\d3d12.dll`

Still open:

- PSO stream replacement for `ID3D12Device2::CreatePipelineState`.
- Runtime reload is manual F10 only; no file watcher yet.
- Explicit replacement path keys.
- ShaderRegex and HLSL text compile.
- Buffer and descriptor replacement.
- Release tracking for cached original/replacement PSOs.
- True content CRC display for live IB/VB hunting. Current live IA hunting uses stable hash semantics, but the live hash is still metadata/view-based to avoid GPU stalls; content hashes are still available through F8 dump/readback output.
- Lightweight on-demand IA tracker for live IB/VB hunting without enabling the full F8 binding tracker.
- Full DX11 include semantics:
  - conditional include evaluation is not implemented yet;
  - namespace-specific variable/resource resolution is not implemented yet;
  - user_config late override is not implemented yet.

## Stage 1: Shader Replacement MVP

Implement a small DX12 runtime parser for `[ShaderOverride*]` sections.

Supported first:

- `hash = <16 hex digits>`
- `handling = skip`
- Automatic replacement files:
  - `ShaderFixes\<hash>-vs.bin`
  - `ShaderFixes\<hash>-ps.bin`
  - `ShaderFixes\<hash>-cs.bin`
- Replacement applies before `CreateGraphicsPipelineState` and `CreateComputePipelineState`.

Not supported yet:

- `ID3D12Device2::CreatePipelineState` stream replacement.
- `ShaderRegex`.
- HLSL compile from `.txt`.
- Command lists.

## Stage 2: ShaderOverride Draw/Dispatch Handling

Add runtime decisions around draw/dispatch:

- Track current PSO shader hashes.
- Support `handling = skip` for draw/dispatch.
- Add simple pre/post execution hooks after the command-list subset exists.

Current `handling = skip` syntax:

```ini
[ShaderOverrideHideExample]
hash = 0123456789abcdef
handling = skip
```

DX12 behavior:

- Graphics draw skip triggers when the current original PSO has a matching VS or PS hash.
- Compute dispatch skip triggers when the current original PSO has a matching CS hash.
- Press F10 after editing `d3dx.ini`.
- Stream-created PSOs are not covered yet.

## Stage 2.5: Hunting Support

Implemented:

- Shader hunting overlay and selected-shader draw/dispatch skip.
- IA buffer hunting overlay and selected-IB/VB draw skip.
- IB/VB hunting display and skip now use 8-digit hash values rather than GPU VA/resource pointer text.
- `Numpad9` copies the most recently selected hash:
  - shader selections copy 16 hex digits;
  - IB/VB selections copy 8 hex digits.
- Hunting defaults to off. This is important because the selected shader/IA binding intentionally hides matching draws.
- Basic keyboard loop:
  - `Numpad0`: hunting on/off. All other hunting keys below require hunting to be on.
  - `Numpad+`: reset active selection to `0/N`.
  - `Numpad9`: copy selected hash.
  - `Numpad4/5`: VS previous/next.
  - `Numpad1/2`: PS previous/next.
  - `Decimal + Numpad1/2`: CS previous/next.
  - `Numpad7/8`: IB previous/next.
  - `Decimal + Numpad7/8`: VB previous/next.

Current limitation:

- Live IB/VB hunting does not perform GPU readback content hashing per draw yet.
- The current live IB/VB hash is a stable resource-level hash derived from tracked D3D12 metadata, not the final DX11-style content CRC.
- This avoids the large stalls that would happen if every draw synchronously copied GPU buffers back to CPU memory.
- To find the selected green-text IB/VB after F8, search for the copied value in `summary\BuffersDX12.csv` under `hunt_hash`, or in `BindIA` manifest rows under `hunt_hash`.
- For actual buffer content hashes and dump files, use the F8 `hash`/deduped file name plus dumped `.buf` files.
- Hunting keys are hardcoded, not read from `d3dx.ini`.

Next planned work:

- Add an asynchronous IA buffer hash cache:
  - queue selected/visible IB/VB ranges for readback from the command queue;
  - compute CRC32C from bytes using the same family of logic as F8 resource dump;
  - cache by resource plus view range and reuse across frames;
  - update the overlay from metadata hash to content hash once readback completes;
  - keep the draw hot path non-blocking.
- Once live content hashes are available, use the same hash source for `[TextureOverride]` buffer matching and generated mod replacement.

Regression note:

- 2026-06-23: Startup/loading screen rendering was broken because hunting was accidentally enabled by default. Logs showed `shaderOverrides=0` and many `DX12DrawSkipped` / `DX12DispatchSkipped`, proving this was hunting skip rather than INI mod skip or shader replacement. Fixed by defaulting hunting to off and splitting skip log reasons into `hunt_selection` and `mod_shader_skip`.

## Include / Mods Loading

Implemented for DX12 shader overrides:

- The root `d3dx.ini` is loaded first.
- `[Include] include = path\file.ini` loads an explicit file.
- `[Include] include_recursive = Mods` recursively loads every `.ini` under `Mods`.
- INI files are processed in deterministic case-insensitive sorted order.
- Included files may contain their own `[Include]` sections; relative paths resolve from the including file's directory.
- Duplicate include detection prevents infinite loops.
- `exclude_recursive` supports exact names and simple prefix wildcard patterns like `DISABLED*`.
- Disabled paths are skipped when their name starts with `disabled` or `DISABLED`.

Current DX12 support:

- This loader only feeds the current DX12 parser. Today that means `[ShaderOverride*] hash` and `handling = skip`.
- `[TextureOverride*] hash` with `handling = skip` is parsed and applied to live IA IB/VB hashes.
- Complex DX11 namespacing, conditional include logic, command lists, resources, and full texture/buffer replacement still need separate parser/runtime work.
- Included mods that rely only on shader override hash replacement should be visible now.
- Included mods that hide an IA buffer by `TextureOverride hash + handling = skip` should be visible after F10 reload.

## Stage 3: IA Buffer Replacement MVP

Add `[Resource*]` binary buffer loading and `[TextureOverride*]` matching for IA buffers:

- Implemented first slice:
  - `[TextureOverride*] hash = <8hex>`
  - `handling = skip`
  - matches current DX12 IA IB/VB hunting hash.
  - F10 reload refreshes the map.
- `vb0 = ResourceName`
- `vb1 = ResourceName`
- `ib = ResourceName`
- Static `.buf/.vb/.ib` file backed resources.
- Draw-context matching such as `match_index_count` and `match_vertex_count`.

## Stage 4: Descriptor Replacement

Replace SRV/UAV/CBV resources by patching descriptor tables or descriptor heaps:

- Use existing descriptor metadata from `DX12ResourceTracker`.
- Create views for replacement resources.
- Patch root descriptor tables before recording commands.

## Stage 5: DX12 Command List Subset

Implement only the DX11 command-list features needed by generated ZZMI-style mods first:

- `run = CommandListX`
- `pre` and `post`
- `checktextureoverride = ib/vb0`
- simple resource copy
- variables/key toggles after the core path is stable

## Test Loop

Primary sample paths:

- F8 dump: `D:\SSMTCacheFolder\3Dmigoto\ZZMIDX12\FrameAnalysis-2026-06-22-232521`
- Extracted workspace: `D:\SSMTCacheFolder\WorkSpace\ZZMIDX12\XiDe`

Initial manual shader replacement test:

1. Use F9 or F8 output to identify a shader hash.
2. Place a replacement bytecode file at `ShaderFixes\<hash>-vs.bin`, `ShaderFixes\<hash>-ps.bin`, or `ShaderFixes\<hash>-cs.bin`.
3. Add or confirm a matching `[ShaderOverride*]` section with `hash = <hash>`.
4. Launch the DX12 target and verify log entries with `DX12ShaderOverrideApplied`.

Manual `handling = skip` test:

1. Use the hunting overlay or F9/F8 output to identify a VS/PS/CS hash.
2. Add a section like:

```ini
[ShaderOverrideDX12HideTest]
hash = 0123456789abcdef
handling = skip
```

3. Press F10 in-game.
4. Confirm the target draw/dispatch disappears.
5. Confirm the log contains `DX12DrawSkipped` or `DX12DispatchSkipped`.

Manual Mods include test:

1. Confirm root `d3dx.ini` contains:

```ini
[Include]
include_recursive = Mods
exclude_recursive = DISABLED*
```

2. Create or use a mod INI under `Mods\SomeMod\mod.ini`:

```ini
[ShaderOverrideDX12IncludedTest]
hash = 0123456789abcdef
handling = skip
```

3. Press F10 in-game.
4. Confirm `d3d12_log.jsonl` contains `DX12ModRuntime` with `iniFiles` greater than 1.
5. Confirm there are `DX12ModRuntimeIni` log rows for the root `d3dx.ini` and the mod INI.
6. Confirm the override takes effect.

Manual hunting test:

1. Launch the target with overlay enabled.
2. Move the scene so the target object is visible.
3. Use `Numpad4/5`, `Numpad1/2`, or `Decimal + Numpad1/2` to cycle VS/PS/CS.
4. When the selected shader is part of a draw/dispatch, that draw/dispatch is skipped and should visually disappear.
5. Use `Numpad7/8` to cycle IB and `Decimal + Numpad7/8` to cycle VB.
6. When the selected IA binding is part of a draw, matching draws should disappear.
7. Press `Numpad9` after selecting VS/PS/CS/IB/VB and paste into a text editor to confirm the copied hash.
8. Press F8 and search the copied IB/VB hash in `summary\BuffersDX12.csv` column `hunt_hash`.

## DXIL Shader Editing Toolchain

Most DX12 shaders are DXIL. The DX12 dump writes two useful files:

- `<hash>-<stage>.bin`: original shader container bytecode.
- `<hash>-<stage>.asm`: DXIL disassembly text generated through `dxcompiler.dll`.

Important limitation:

- DXIL disassembly is not a practical editable source format for this project.
- The existing DX11 assembler path is for DXBC-style assembly, not DXIL round-trip assembly.
- For DXIL, use the `.asm` file for inspection only, then write or patch HLSL and compile it to a replacement `.bin`.

Recommended DXIL replacement workflow:

1. Identify the shader from F9/F8 output:
   - `ShaderFixes\<hash>-<stage>.bin`
   - `ShaderFixes\<hash>-<stage>.asm`
   - `ShaderUsage.txt`
   - `pso_resource_summary.csv`
2. Inspect the `.asm` to learn:
   - stage: `vs`, `ps`, or `cs`
   - shader model, commonly `*_6_0` or newer
   - resource registers such as `t#`, `s#`, `b#`, `u#`
   - input/output semantics
3. Write an HLSL replacement that keeps the same stage contract:
   - same stage profile
   - compatible input/output semantics
   - compatible resource registers and spaces
   - `main` entry point unless the compile command says otherwise
4. Compile with Microsoft DXC:
   - `dxc.exe -T ps_6_0 -E main -Fo ShaderFixes\<hash>-ps.bin my_shader.hlsl`
   - `dxc.exe -T vs_6_0 -E main -Fo ShaderFixes\<hash>-vs.bin my_shader.hlsl`
   - `dxc.exe -T cs_6_0 -E main -Fo ShaderFixes\<hash>-cs.bin my_shader.hlsl`
5. Add a matching INI section:
   - `[ShaderOverrideName]`
   - `hash = <hash>`
6. Launch or reload once DX12 F10 reload exists.

For quick tests, use a tiny replacement shader that preserves the signature and returns a visible constant color for PS, or offsets position slightly for VS. For serious mods, prefer source-level HLSL edits over binary patching.

Useful tool locations:

- Windows SDK DXC is often under `C:\Program Files (x86)\Windows Kits\10\bin\<sdk>\x64\dxc.exe`.
- This project currently loads `dxcompiler.dll` from the Windows SDK or process search path for DXIL disassembly.

## DX12 F10 Reload Plan

DX11 F10 reload swaps independent shader objects. DX12 is different: shaders are baked into pipeline state objects, so reload must work at the PSO level.

Implemented approach:

1. F10 reloads `d3dx.ini` and rescans `ShaderFixes`.
2. Replacement bytecode cache is invalidated.
3. Existing PSOs are not mutated in-place.
4. When the game binds an original PSO through `SetPipelineState`, DX12 runtime lazily creates or reuses a replacement PSO with the current replacement bytecode.
5. The command list receives the replacement PSO instead of the original PSO.

This should support live iteration without restarting the game, but it requires storing original PSO creation descriptors and adding an original-PSO to replacement-PSO cache.

Current implementation notes:

- Graphics and compute PSO descriptors are stored for non-stream creation APIs.
- Shader bytecode is deep-copied because game-provided descriptor memory may be temporary.
- Root signatures and devices are retained with `AddRef`.
- Replacement PSOs are recreated after every F10 generation if a matching override exists.
- The first `SetPipelineState` after F10 may pay the PSO creation cost.

Risks:

- `ID3D12Device2::CreatePipelineState` stream descriptors need separate descriptor capture and rebuild logic.
- Some engines create many PSOs lazily, so reload must be thread-safe and avoid creating duplicate replacement PSOs.
- Replacement HLSL must remain compatible with the original root signature and pipeline layout.

Manual F10 test:

1. Launch target with the new DX12 DLL.
2. Confirm `d3d12_log.jsonl` contains `DX12ModRuntime` with `status=loaded`.
3. Add or edit a `[ShaderOverride*] hash = ...` section.
4. Compile a DXIL replacement to `ShaderFixes\<hash>-<stage>.bin`.
5. Press F10 in-game.
6. Confirm log contains:
   - `InputHotkey` with `F10`
   - `DX12ModRuntimeReload`
   - `DX12ModRuntime` with an incremented `generation`
   - `DX12ShaderOverrideApplied`
   - `DX12ReplacementPsoCreate`

## Open Questions

- Whether TheHerta4 will emit explicit replacement paths or rely on `<hash>-<stage>.bin`.
- Whether shader replacement files will be DXBC/DXIL `.bin` only at first.
- Which exact TextureOverride syntax TheHerta4 emits for DX12 buffer mods.
