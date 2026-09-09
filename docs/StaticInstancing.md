# Static Instancing

Coalesces runs of identical static shadow-caster draws into one instanced draw. A Community
Shaders feature (Bottled fork), branch `feature/static-instancing`.

## Why

Measured on the user's machine (9950X3D, 4090, 4K, AE 1.6.1170, 2026-09-06): the render thread
spends 17 to 21 ms of a 24 to 27 ms frame submitting about 10,700 draws. The Utility pass
(shadow casters plus depth) is the largest slice at 4,800 to 6,500 draws costing 1.45 to 1.57 us
each. Every one of those draws runs `BSUtilityShader::SetupGeometry` (671 instructions, two Map,
four Unmap, four SetConstantBuffers) and `Renderer::DrawTriShape` (dirty-state flush,
IASetIndexBuffer, IASetVertexBuffers, DrawIndexed). Clones of the same NIF share their GPU
buffers, so identical placed meshes can be drawn once with N instances.

Verified against the decrypted 1.6.1170 and 1.5.97 executables:

| Function | SE | AE | Notes |
|---|---|---|---|
| BSBatchRenderer::RenderPassImmediately | 100854 | 107644 | static, (pass, technique, alphaTest, renderFlags) |
| BSBatchRenderer::RenderBatches | 100852 | 107642 | loop over passGroupNext, calls the above at +0x29E / +0x28F |
| Renderer::DrawTriShape | 75477 | 77263 | SetDirtyStates, IASetIndexBuffer, IASetVertexBuffers, DrawIndexed |
| Renderer::DrawInstancedTriShape | 75479 | 77265 | grass only, single caller |
| Renderer::SetDirtyStates | 75580 | 77386 | |

## Design

### Hooks

1. Detour `RenderPassImmediately` at its entry. The two existing call-site thunks in this repo
   (Light Limit Fix, Terrain Blending) and Engine Fixes' entry detour keep running because they
   sit upstream of this detour in the chain, and deferred passes are replayed through `func`,
   which is the downstream chain.
2. Detour `RenderBatches` only to maintain a depth counter. Deferral is allowed only while the
   counter is non-zero, so passes rendered through `RenderPersistentPassList` or the accumulator's
   own loops are untouched.
3. Detour `DrawTriShape`. When a batch is pending, replicate the engine's body and issue
   `DrawIndexedInstanced` with the batch's instance count instead of `DrawIndexed`.

### Eligibility

A pass is deferred when all of these hold:

- `pass->shader->shaderType == Utility`
- the technique has `RenderShadowmap` set and none of `Skinned`, `LodLandscape`, `TreeAnim`,
  `RenderNormal`, `RenderShadowmapPb`, `RenderDepth`, any shadow-mask bit
- `geometry->GetType() == kTriShape` exactly, no skin instance, renderer data present
- shader property is a `BSLightingShaderProperty` without `kMultiTextureLandscape`
- alpha-tested passes only when the setting allows them (the key then carries the diffuse
  texture, alpha threshold and flags)

Depth pre-pass techniques are excluded on purpose. Terrain Blending sequences those passes
statefully and reordering them would break its depth trick. Shadow-map passes are depth-only, so
order inside a group does not matter.

### Batching

Key = (rendererData, technique, alphaTest, renderFlags, material identity). Buckets keep
insertion order. At flush, singles are replayed in original order through `func`; buckets with
two or more passes get their world matrices written into a per-frame dynamic structured buffer,
then the representative pass is replayed through `func` with a pending record. The engine sets
up technique, material and per-geometry state for it as usual; the `DrawTriShape` detour turns
the final draw into the instanced one.

Flush points: the last pass of a group (`passGroupNext == nullptr`), which is before the engine's
`EndPass` resets cull mode and alpha-to-coverage, and a safety flush when `RenderBatches`
returns with anything still pending.

### GPU side

- `StructuredBuffer<float3x4> InstanceWorlds : register(t9)` in the Utility vertex shader,
  rows = [R*s | t] in world units. The shader subtracts `FrameBuffer::CameraPosAdjust` itself,
  the same way the skinned path does, so no engine matrix convention has to be matched.
- `cbuffer StaticInstancingCB : register(b13) { uint InstanceBase; uint InstanceEnabled; }`.
  `InstanceEnabled` must equal a magic value. After each instanced draw a zeroed constant
  buffer is rebound so later vanilla draws take the normal path. b13 and t9 are unused by the
  game and by every other feature in this repo.
- Instance id is `InstanceBase + SV_InstanceID` because D3D11 does not add
  `StartInstanceLocation` to `SV_InstanceID`.
- Compiled under the feature define `STATIC_INSTANCING`, which the Utility define generator now
  appends like the other shader types already do.

### Instrumentation

Per frame, per Utility shadow technique: eligible draws, unique keys, draws that would collapse,
draws actually collapsed, instanced draws issued, and exclusion counts by reason. Shown in the
feature's settings panel and returned by devbench `communityshaders.inspect kind=instancing`.
The performance overlay's per-type draw count and cost per draw give the frame-level effect.

## Phases

1. Instrumentation only (feature loaded, coalescing off). Read the collapse ratio.
2. Coalescer on, non-alpha-tested casters. Validate shadows against a screenshot with the
   feature off. Debug toggle "instance singles" routes every eligible draw through the instanced
   path with one instance, which validates the matrix convention independently of batching.
3. Alpha-tested casters.
4. Lighting pass (separate design: per-instance PreviousWorld, fade alpha, light selection).

## Test protocol

Same spot, frame generation at 2X or off, overlay open:

1. Feature off: record Utility draw count and cost per draw.
2. Phase 1 on: histogram numbers, no frame change expected.
3. Phase 2 on: Utility draw count should fall by the collapsible count; frame time should fall by
   about 1.5 us per removed draw. Compare shadow screenshots.

## Rollback

Feature toggle in the menu, or delete the `Static Instancing` feature folder. No engine state is
modified outside the three detours.

## Results, 2026-09-08 (BottleRim2_3, AE 1.6.1170, pine forest near Falkreath, 9950X3D + 4090)

Measured live through devbench (`tools/static_instancing_probe.py`, plus an A/B script cycling
the settings, 2 cycles x 18 one-second samples per configuration, performance overlay open).

| Configuration | Utility draws | Draws removed | Batches | Utility submit ms | Frame ms |
|---|---|---|---|---|---|
| Off | 2,977 | 0 | 0 | 4.43 | 19.1 |
| Shadow maps only | 2,914 | 76 | 48 | 4.22 | 17.6 |
| Plus depth pre-pass | 2,624 | 373 | 152 | 4.14 | 18.4 |
| Plus alpha-tested casters | 2,556 | 483 | 200 | 4.16 | 18.3 |

Findings:

- The technique id passed to RenderPassImmediately is the raw Utility descriptor plus 0x2B
  (BSUtilityShader::SetupTechnique decompile). The first build tested the wrong value and
  matched nothing.
- Per-batch overhead matters. With a Map/Unmap pair for the matrices and another for a
  per-batch constant buffer, 260 batches per frame cancelled most of the saving (0.15 ms for
  690 removed draws). One Map per flush and pre-created SRVs per 64-instance block brought it
  to about 0.3 ms for 480 removed draws.
- The removable cost per duplicate draw is about 0.6 us, not the 1.4 us average cost per
  Utility draw. Duplicates sit inside a material run, so they never paid for the technique and
  material switches that make the average high.
- Blue flicker on some meshes appeared with an early key. Adding the shader-property flags and
  the pass state (accumulation hint, LOD mode, extra param) to the key, and excluding passes in
  the fade range (accumulation hint 10, which sets a per-object stencil reference), removed it.
  Shadows were unchanged in instance-singles mode by inspection.
- Of roughly 3,000 Utility passes per frame, 1,300 to 1,500 are eligible; the rest are skinned
  (about 400), wind-animated trees (about 530, per-geometry TreeParams), non-TriShape geometry
  types (about 590, mostly mesh-LOD shapes), landscape, and a few dozen custom-render passes.
  Duplicates are 35 to 45 percent of the eligible set.

Ceiling in this scene for the Utility pass is therefore about 0.5 ms. Further gains need the
Lighting pass (3,700 draws at about 2 us), TreeParams in the instance record for trees, and
mesh-LOD shapes keyed on their LOD level.

## Depth pre-pass: retired (2026-09-08, later the same evening)

Instancing the depth pre-pass produced white-blue, dithered, flickering meshes. Two captures
settled the cause: with the feature switched off and the modified shaders installed the artifact
was present; with the feature off and the original shaders installed it was gone. The main pass
depth-tests against the pre-pass for equality, and the original Utility and Lighting vertex
shaders only match bit for bit because they compile from unchanged source. Any change to the
pre-pass permutation's code shape, even one that leaves the vanilla expression intact or adds
`precise`, let the compiler emit different arithmetic on some meshes.

Consequence: `STATIC_INSTANCING_VS` is defined only for RENDER_SHADOWMAP permutations (not
parabolic, not RENDER_DEPTH), so pre-pass permutations compile from source that is byte-identical
to vanilla. Shadow maps have no equality test, so no code-shape change there can cause this.
The `IncludeDepthPrepass` setting is retired and ignored.

Also added: wind-animated trees in shadow maps (per-instance TreeParams computed with the exact
formula from BSUtilityShader::SetupGeometry: timer * 6, the shadow scene node wind value at
+0x304, distance-dampened amplitude via the engine's fast inverse square root, leaf frequency),
BSMeshLODTriShape geometry (LOD mode is in the batch key; the representative draw supplies the
index range), and per-frame counters for the Lighting pass: static TriShape passes, unique
(renderer data, material, technique, flags, light counts) keys, and the collapsible count.
