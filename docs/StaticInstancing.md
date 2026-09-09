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

## Results, build 13 (shadow maps only, trees, mesh-LOD, alpha-tested), 2026-09-08 late

Validation: singles mode with every eligible caster type instanced renders identically to the
feature-off frame (capture pair `v2-singles` / `v2-off`). Same spot, overlay open, 3 cycles x 10
samples per configuration:

| Configuration | Utility draws | Removed | Batches | Utility submit ms | Frame ms |
|---|---|---|---|---|---|
| Off | 3,134 | 0 | 0 | 4.99 | 22.3 |
| Shadow static | 3,056 | 80 | 49 | 4.98 | 22.6 |
| + mesh-LOD | 3,065 | 82 | 50 | 4.97 | 22.2 |
| + alpha-tested | 3,051 | 103 | 62 | 5.01 | 22.7 |
| + trees | 3,005 | 160 | 84 | 4.97 | 22.5 |

Counters at that spot: 3,169 Utility passes per frame, of which 897 shadow-map and 2,272 depth
pre-pass; 524 eligible shadow casters, 342 unique keys, 182 collapsible. Lighting pass: 2,920
passes, 1,493 static TriShapes, 1,039 unique keys, 454 collapsible (30 percent); a further 1,262
passes are other geometry types (mostly mesh-LOD shapes) and 165 skinned.

Reading: with the pre-pass excluded the shadow-map duplicate pool is small at this spot (160
draws, about 0.1 ms) and the frame-time effect is inside noise. The pre-pass holds 2,272 of the
3,169 Utility passes, which is why the second pre-pass attempt below matters.

## System profile, 30 s, feature off, overlay open (WPR CPU sampling)

Render thread by module: CommunityShaders.dll 29.4 percent, SkyrimSE.exe 22.1, ntdll 11.5,
d3d11 10.5, ntoskrnl 4.7, nvwgf2umx 4.7, vcruntime 4.4, EngineFixes 1.5,
NativeMeshLightFlickerFix 1.5. Two other threads spin in ntdll (one is the FSR3 frame generation
pacing thread). Engine functions on the render thread are diffuse: batch draw dispatch,
BSWaterShader::SetupGeometry, culling, RenderPassImmediately, NiTransform::Multiply,
BSLightingShader::SetupGeometry, BSStringPool::Release, SetDirtyStates, hkp3AxisSweep, each
under 5 percent. Conclusion: the plugin's own per-draw work is the largest reachable CPU item on
the render thread; driver cost is small; instancing tops out around 1 to 1.5 ms.

## Depth pre-pass, second attempt

Design that does not depend on compiler behaviour: while `IncludeDepthPrepass` was on at load,
Utility RENDER_DEPTH permutations compile with STATIC_INSTANCING_PREPASS, every pre-pass draw
adds a four-ulp NDC depth bias from b13.z, and the feature replaces the engine's 40 `kTestEqual`
depth-stencil states (BSGraphics table, AE id 411362, [depthMode][40 stencil modes]) with
less-or-equal clones. The main pass then passes wherever its depth is at or in front of the
biased pre-pass depth, so instanced and vanilla draws never have to agree bit for bit. Changing
the option requires a restart and a shader recompile.

## Results, build 17 (pre-pass v2 live), 2026-09-08 23:15

Depth-state swap: the engine fills its table on first use, so the swap runs from Prepass each
frame until it finds the 40 Equal states (found on the second frame; DepthFunc histogram over
the 280 populated states: never 40, less 40, equal 40, lessEqual 80, greaterEqual 40, always 40).
The bias is armed only after a swap succeeded, and the biased constant buffer is re-bound at
every batch group; before that fix the feature-off path left pre-pass draws unbiased and the
white dithered meshes came back.

Validation: singles mode (every eligible draw instanced alone, pre-pass included) and feature
off both render correctly (captures `v2-singles` / `v2-off`).

Clean machine, same spot, 3 cycles x 10 samples:

| Configuration | Utility draws | Removed | Batches | Utility submit ms | Frame ms |
|---|---|---|---|---|---|
| Off | 3,127 | 0 | 0 | 5.22 | 22.0 |
| Shadow + pre-pass, static | 2,714 | 421 | 202 | 5.05 | 21.9 |
| + mesh-LOD | 2,735 | 419 | 200 | 5.10 | 22.1 |
| + alpha-tested | 2,625 | 537 | 251 | 5.05 | 22.3 |
| + trees | 2,584 | 605 | 276 | 5.02 | 21.9 |

Counters: 1,512 eligible, 918 unique keys, 594 collapsible (39 percent), all collapsed. The
remaining exclusions are 627 BSSubIndexTriShape passes (segmented meshes), 510 skinned, and
shadow-mask or parabolic techniques. Lighting: 1,459 static, 1,031 unique, 428 collapsible.

Reading: correct and stable, 605 draws removed per frame, about 0.2 ms of Utility submission
recovered, frame time inside noise. Per removed draw the net saving is about 0.3 us after the
deferral bookkeeping. This is the practical ceiling of draw coalescing for the Utility pass at
this location; the Lighting pass would add a similar amount. The system profile puts the
plugin's own per-draw work at 20 to 30 percent of the render thread, which is the next target.

## Frame Annotations and caster culling, measured live (build 17, overlay open)

| State | Frame ms | All shader types submit ms | Utility ms | Lighting ms |
|---|---|---|---|---|
| Frame Annotations on | 16.75 | 12.2 | 4.18 | 4.90 |
| Frame Annotations off | 15.3 | 8.2 | 3.26 | 3.23 |

The advanced setting "Frame Annotations" (SettingsUser.json) makes the plugin format two strings
and open a GPU debug event for every draw. With it off, draw submission drops by 4 ms per frame
and the frame by about 1.4 ms (the rest is absorbed by pacing). It is meant for RenderDoc
captures and should be off in play.

Caster culling by world-bound radius (feature setting, shadow maps only, actors never culled):

| Radius | Utility draws | Culled | Utility ms |
|---|---|---|---|
| 0 | 3,088 | 0 | 3.23 |
| 20 | 2,976 | 147 | 3.13 |
| 40 | 2,955 | 202 | 3.18 |
| 80 | 2,802 | 389 | 3.17 |

Removing small casters saves little here (they are cheap draws inside material runs), so 40 is
kept as a harmless default and it is not a lever.

## Pre-pass v2, retired

With the feature disabled and only the bias plus less-or-equal states active, foliage and some
meshes still flickered (bisect phase A). Some consumer of the depth buffer does not tolerate the
bias. The option is off in the shipped settings; shadow maps only is the configuration.

## Main-pass (Lighting) instancing, plan

CS patches BSLightingShader::SetupGeometry to write the lighting constants in world space, so
instances of one mesh differ only in World, PreviousWorld and MaterialData (envmap mask, specular
LOD fade, alpha). Design:

- A separate compiled permutation for instanced draws: descriptor bit 31 adds the define
  STATIC_INSTANCING_LIGHTING; vanilla permutations stay byte-identical. The DrawTriShape detour
  binds the instanced VS/PS for the representative; if the cache has not compiled them yet the
  bucket falls back to individual vanilla draws.
- Instance record: World rows, PreviousWorld rows (previousWorld minus previousPosAdjust, or the
  current world when renderFlags bit 4 is set), MaterialData. The PS receives MaterialData
  through a nointerpolation VS output.
- Depth: instanced objects cannot pass the main pass's equality test against a vanilla pre-pass.
  Build 19 simply skipped them in the pre-pass and drew them test-and-write in the main pass.
  That was wrong in a way the flicker bisect did not catch: terrain blending and screen-space
  shadows consume the pre-pass depth before the main pass, and a rock missing from it turns
  see-through and casts a hard-edged wrong shadow (seen 2026-09-09 00:10 with InstanceSingles
  on, which pulls large close rocks into the set).
  Build 20 draws them into the pre-pass instead, as one instanced draw per bucket through a
  dedicated Utility permutation (descriptor bit 31 adds STATIC_INSTANCING_DEPTH; vanilla
  pre-pass permutations stay byte-identical). That permutation adds 4 ulps of NDC depth, and
  the main pass draws those objects test-and-write, so the two shaders never need to agree bit
  for bit. On a shader miss the bucket replays vanilla and its members are not marked, so the
  main pass draws them vanilla against exact depth. The pre-pass draw count saving is the same
  as before: one instanced draw replaces the bucket.
- Exclusions: skinned, projected UV, landscape, tree animation, fading, decals, additive or
  blended alpha, any pass with shadow lights.

## Results, build 20/21 (main pass on), 2026-09-09 00:27 to 00:56

Visual: every phase clean on build 20, including InstanceSingles with the main pass on (the
configuration that exposed the build 19 hole). Counters per frame: 147 instanced main-pass draws
replacing 400 Lighting draws, 150 instanced pre-pass draws replacing 456, no shader misses.

Timing (overlay open, alternating configs, 15 samples each):

| config            | frame ms      | Lighting type ms |
|-------------------|---------------|------------------|
| shadow maps only  | 18.14 / 17.89 / 15.46 | 4.61 / 4.54 / 3.50 |
| plus main pass    | 18.22 / 17.73 / 15.60 | 4.73 / 4.64 / 3.66 |
| feature off       | 14.99 (drifted) | 3.41 |

The main pass removes about 850 draws a frame and buys nothing measurable; the per-type timer
says the Lighting pass is 0.1 ms slower with it on. Build 21's counters explain why: my code is
cheap (flush 0.55 ms including the engine's replayed draws, instance writes 0.04 ms, instanced
draws 0.13 ms, 32 depth-mode changes), so the engine's own per-draw cost in these passes must be
near 0.5 us, the same figure the shadow-map result implied. Draw elimination has hit its floor
on this CPU. The main pass stays available (it is clean) but should not be sold as a win.

## Render-thread profile, build 21, 2026-09-09 01:00 (WPR CPU sampling, 30 s, overlay open)

Thread 54168 (render), 50.5k samples. By module: SkyrimSE.exe 27%, ntdll 19%, CommunityShaders
14%, d3d11 11%, kernel 6%, NVIDIA driver 6%, third-party SKSE plugins about 5%.

Where the ntdll time goes: RtlQueryPerformanceCounter is 13.5% of the thread, and the stacks put
92% of it inside amd_fidelityfx_framegeneration_dx12.dll under DX12SwapChain::Present, called
from HDRDisplay::RunPresentChainWithHDR. That is FSR frame generation pacing its interpolated
frame by spinning; about 1.9 ms of every frame on this thread is that wait, not submission. This
applies to this branch's build only: it carries no DLSS-G, so Upscaling falls back to FSR frame
generation. The main-tree build runs DLSS-G with Reflex pacing and measures 1 to 2 ms of wait at
the same Present without a spin (per the rebuild chat).

Community Shaders items on the thread, and what build 22 does about them:

- WeatherManager::UpdateFeatures 1.6%: State::Draw called it on every draw call whenever the
  CS Editor feature is loaded. Now once per frame (State::weatherFrameChecker).
- StaticInstancing 2.0% (OnRenderPass 1.3%, BatchKey hashing 0.5%, heap allocations from the
  per-flush bucket vectors and per-frame unordered containers). The containers are now
  stamped instead of cleared (nodes persist), and buckets chain their passes through the
  deferred array instead of owning a vector.
- SnowCover::BSLightingShader_Setup about 0.8%: NiAVObject::HasAnimation walks extra data with
  _stricmp on every pass, plus a name hash. Now cached per geometry for 30 frames, keyed on the
  reference as well.
- State::Debug (overlay measurement) about 0.5% while the overlay is open: QPC plus a mutex per
  draw. Only present while measuring; left alone.

Third-party per-draw hooks on the same thread, for the modlist owner: NativeMeshLightFlickerFix
1.9%, EngineFixes 1.6% (its SetupGeometry wrapper appears in every Lighting stack, plus
GetCurrentThreadId per pass), UnderwaterNG and HorizonFix inside the water and lighting setup
chains, FPWeaponAdjust and ImprovedCameraSE 0.5% each. About 0.6 ms a frame between them.

## Results, build 22, 2026-09-09 01:36 (same spot, overlay open, 15 samples per config)

| config            | frame ms              | Utility ms | Lighting ms |
|-------------------|-----------------------|------------|-------------|
| shadow maps only  | 14.96 / 15.84 / 14.56 | 3.34       | 3.17        |
| plus main pass    | 15.03 / 15.57         | 3.36-3.40  | 3.18-3.19   |
| feature off       | 15.36                 | 3.22       | 3.14        |

Against build 21 at the same spot (Utility 3.41-4.27, Lighting 3.50-4.73) the per-type submission
timers dropped by 0.3 to 0.5 ms each; frame time is still dominated by run-to-run drift, so the
three per-draw fixes are confirmed by the timers, not by the frame average. Static Instancing's
own counters: flush 0.44 ms (was 0.55), writes 0.04 ms, instanced draws 0.13 ms, 30 depth-mode
changes, no allocations in steady state. Visual check clean with Snow Cover's cache in place.

Shipped configuration (SettingsUser.json): Enabled, alpha-tested, trees, mesh-LOD, main pass on,
caster culling below 40 units, Frame Annotations off.

## Hang analysis, 2026-09-09 02:27 (combined build, C:/tmp/hangs/SkyrimSE-hang-20260909-022727.txt)

Second freeze on an action (the first was 01:46 on build 22). Captured with Tools/Hang-Stacks.ps1
while the process was still alive:

- Main thread: Main::Draw -> BSShadowDirectionalLight -> BSBatchRenderer::RenderBatches ->
  (Static Instancing / Light Limit Fix / Terrain Blending / Engine Fixes hooks) ->
  RenderPassImmediately +0x1a7 -> BSDynamicTriShape::LockDynamicData (ID 70950, lock at
  geometry+0x168) -> BSSpinLock::Lock spinning with Sleep(1). The geometry is
  "00DZB_HumanBeard003" under BSFaceGenNiNodeSkinned in skeleton.nif, a face morph mesh.
- The lock: owner thread 0x105bc, count 1. That thread is an engine job-pool worker
  (ID 68445 thread start, ID 69344 wait) sitting idle at its work-queue wait. So a job that ran
  on it locked the beard's morph data and finished without unlocking; the render thread hangs
  the next time it draws the face. All 510 threads are parked in waits; nothing is running.
- Dead-stack residue on that worker: engine culling/accumulation (BSGeometryListCullingProcess::
  AppendNonAccum, ID 107784 which calls ShadowSceneNode::GetShadowCasterLightArrayEntry and
  BSLightingShaderProperty), with NativeMeshLightFlickerFix, EngineFixes, MCMHelper,
  PerkEntryPointExtender and AudioOcclusion frames in the same window. Not conclusive for one
  module.
- Community Shaders code appears on the render thread only (normal hook chain) and Static
  Instancing never classifies or defers dynamic tri-shapes (skinned and face geometry are
  excluded), so the leak did not come from this plugin. The trigger is an action (attack) that
  starts a facial expression, which queues the face-morph job.

First experiment for the modlist: disable Native Mesh Light Flicker Fix (it hooks the same
accumulation path on job threads and duplicates Light Limit Fix), then reproduce the attack.
