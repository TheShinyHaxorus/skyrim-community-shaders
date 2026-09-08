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
