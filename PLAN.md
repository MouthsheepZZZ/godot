# LRT: precomputed local transfer and visibility

## Goal and accepted contract

Replace the current OpenMask/six-face transport with RGB local transfer matrices (LTM) and SH4 local visibility. Support exactly two geometry representations, selectable under GeometryInstance3D > GI > LRT: Voxel and Precomputed LTM + LocalVis. Preserve existing light types, sky, external GI, material execution, incremental Trunks, coherent GPU publication, and export.

The reference is `lrt_reference/lrt_ref.pdf`, particularly pp. 22–23 (asset representations/composition), pp. 19–20 (transport), pp. 28–29 (single-bounce matrix), and pp. 31–32 (rotation). Missing numerical details must be decided using the existing test scenes, not by silently restoring OpenMask.

## Settled decisions

- Voxel mode retains configurable target cell size, shared mesh-local geometry, and material-dependent voxel captures. Voxel assets do not store LTM. Runtime Volume construction uses 26 fixed adjacent-Probe cell-footprint queries to produce local transfer and visibility, without DDA or ray traversal. Each footprint is one Volume Probe cell wide, conservatively inverse-transformed to asset coordinates. Occupancy remains center-based. The mesh-local support radius clips neighbor-center offsets; spatial-index bounds include the footprint. It does not create a longer search.
- Precomputed mode traces effective authored triangles, not coarse geometry voxels. Renderer-executed static ShaderMaterial output remains supported, including coverage, normals, albedo, and emission subject to finite material capture resolution.
- Model-local sparse fields store RGB single-bounce 4x4 matrices, LocalVis SH4, and independent emission SH4. FP16 asset coefficients; FP32 composition and GPU arithmetic.
- Geometry and effective coverage determine visibility cache identity. Albedo changes invalidate LTM, alpha/coverage changes also invalidate LocalVis. Compatible instances share immutable fields.
- Project defaults provide local support radius, transfer-field sample spacing, angular sample count, and independent occupancy-band width; node overrides are available. Radius is part of cache identity. Different node radii are explicit local-model approximations.
- Mesh units are authored in world units. No instance transform triggers asset rebaking. Translation/rotation resample and rotate fields. Non-unit/nonuniform scaling is an explicitly accepted approximation; field support scales with the instance.
- Probe occupancy means its center lies in a surface band. Voxel mode uses voxel occupancy. Precomputed mode uses accelerated effective-triangle distance queries with the independently configured band width. Do not fill closed interiors.
- Ordinary trilinear interpolation is intentional; no side/connectivity interpolation metadata. Thin-wall mixing is an accepted risk to measure.
- Compose raw RGB LTMs by addition, LocalVis by projected SH product, emission by addition. Full visibility is `(sqrt(4*pi), 0, 0, 0)`. No per-object infinite-bounce pre-solve; no reflectance-greater-than-one emission.
- Dynamic changes dirty the union of old/new support. Recompose visibility from identity; never divide out old visibility. Unchanged static inputs reuse the existing answer.
- Explicit editor Bake. Missing/stale precomputed dependencies fail clearly at build/export/runtime load. Never silently select Voxel or bake at runtime.
- Full 26-neighbor reference transport first. Remove OpenMask from transport and visibility propagation. Keep sky visibility independent, and retain native lighting and external boundaries. No four-neighbor temporal optimization in this change.
- SH4 is a low-frequency approximation, not exact visibility. Validate leakage, overlap, contact, motion, and energy stability. Revisit the operator if tests fail; do not hide failures with the old gate.

## Numerical contract

Use the existing real SH basis ordered `(constant, x, y, z)`. With column-vector coefficients, a matrix maps incoming SH to reflected outgoing SH. Directional single-bounce contributions use real surface normals and per-hit linear RGB reflectance, not averaged color times a shared matrix. Visibility is the spherical projection of binary effective-surface visibility over the specified finite support.

Rigid rotation applies `D T D^-1` to each RGB matrix and `D v` to visibility/emission. Full visibility is the product identity. Matrix accumulation and visibility composition are separate operations. Deterministic instance order avoids nondeterministic repeated SH truncation.

The current propagation follows the reference p. 20 masked-SH accumulation: `G = (1/26) * sum(product(L_neighbor, reverse(V_neighbor)))`, `H = product(G, V_receiver)`, then `L_next = H + T*H + Source`. Gather averages complete SH4 coefficients directly; there is no directional evaluation/reprojection, per-hop directional low-pass, sender spectral scaling or receiver L2 normalization. Fixed weights retain open-boundary losses instead of renormalizing available neighbors. The reference does not specify Gather normalization; equal averaging is the explicitly selected implementation convention. The existing identity-through-air term `H` is retained by agreement while the local matrix definition is audited separately. Four-neighbor time slicing is deferred.

Geometry-only sky uses the same coefficient averaging and raw sender/receiver Triple Products; off-grid sky is full visibility. External GI retains crossed-face coverage blending and enters as isotropic SH `(radiance/C0, 0, 0, 0)` before receiver masking. Source injection, native-light/sky projection, local reflection/emission matrices and surface Gather are unchanged in this alignment. Native incoming light receives LocalVis before one reflection; already-visible sky bypasses that second mask. Source remains additive. Coefficient averaging preserves uniform empty-air SH4 fields but raw projected local operators can still produce signed values; scene-specific stability evidence is recorded below.

## Implementation sequence

### 1. Mathematical core and reference tests

- [x] Add full RGB transfer/visibility records and directional integration utilities.
- [x] Implement SH rotation, trilinear sampling with identity outside support, and deterministic field composition.
- [ ] Revalidate SH identity/product, matrix/channel integration, rotation, interpolation, and empty/overlapping contributions against the current implementation (no new unit-test files).

### 2. Asset representation and baking

- [x] Add sparse model-local transfer fields, shared geometry-query acceleration, and FP16 serialization.
- [x] Bake visibility and RGB transfer from effective triangles/material output; integrate independent emission.
- [x] Separate dependency identities for effective coverage, albedo/normal, emission/side, and bake specification.
- [x] Add cancellation, progress, explicit invalidation, immutable sharing, and versioned persistence.

### 3. Inspector, settings, and export

- [x] Add representation enum and mode-dependent overrides under GI > LRT, including supported instanced geometry.
- [x] Expose support radius, precomputed spatial/angular precision, occupancy band, Bake/status/errors.
- [x] Keep Voxel controls and geometry/material sharing intact.
- [x] Implement dependency validation and packaging; reject stale/missing precomputed assets without fallback. Export/reload acceptance remains below.

### 4. Volume composition and occupancy

- [x] Route each input to fixed-neighbor Voxel queries or precomputed sampling.
- [x] Use mode-specific occupancy and compose RGB LTM/LocalVis/emission per Probe.
- [x] Include mode/specification/material/transform changes in spatial-index signatures.
- [x] Retain dirty-Trunk, recycled CPU-bank, cancellation, and atomic GPU-bank publication paths. Motion/reuse acceptance remains below.

The packed LocalField builder now consumes the mixed sampling kernel and updates RGB records, LocalVis, emission and occupancy together. Static Voxel occupancy uses the same covered-cell semantics as Dynamic Voxel occupancy; it no longer rejects open-sheet voxel bands through an exact triangle/interior query.

### 5. GPU pipeline

- [x] Update local-record, patch, source, diagnostic, and boundary layouts coherently.
- [x] Replace six-face/OpenMask propagation with 26-neighbor SH transport.
- [x] Replace directional-bitset sky solving with SH LocalVis transport and projection.
- [x] Update native injection and emission handling; retain projector/area/spot/omni/directional light paths. Render acceptance remains below.
- [x] Remove obsolete OpenMask buffers, formats, shader substitutions, cache fields, diagnostics and unused six-face direction helpers.

### 6. Acceptance

- [x] Compile the engine and changed shaders; run affected existing validation paths (no new unit tests).
- [ ] Exercise Voxel-only, precomputed-only, and mixed scenes; compare coverage and RGB reflection.
- [x] Exercise translation, rotation, accepted scaling approximation, removal, overlap, and static reuse (functional checks; visual quality remains below).
- [ ] Check alpha, normal detail, material override/sharing, self-emission, and missing/stale caches.
- [ ] Check thin walls, corners, contact, visibility outside support, repeated product, and long-run energy.
- [x] Inspect actual scene renders through Godot MCP; capture representative results. Visual acceptance failed; see findings below.
- [ ] Validate export and reload with precomputed dependencies; document memory/build/runtime costs.
- [x] Update class docs and remove stale transport/occupancy terminology.

## Current review and remaining acceptance

- Engine compilation succeeded with `platform=windows target=editor dev_build=yes debug_symbols=no`. Actual Vulkan RenderingDevice shader compilation, local-field publication, native-light injection and sky convergence were exercised through Godot MCP.
- Precomputed manifests and FP16 transfer format are version 2. Local volume cache format is 25, build fingerprint version 42. Precomputed bake-model identity is version 3 (serialized manifest and FP16 layout remain version 2); existing precomputed fields require explicit rebaking because reflection/emission projection now uses the full SH basis. Existing precomputed assets require explicit Bake; existing Volume data requires rebuild/save. No legacy fallback is retained.
- FP16 visibility normalizes coefficients by sqrt(4*pi) so both identity and zero lattice nodes round-trip exactly. Raw albedo/normal and emission/side dependencies have separate manifest identities; their coefficients still share one material payload. Trunk invalidation independently hashes the loaded reflection/emission coefficients.
- Review corrected Static Voxel occupancy, analytic occupied-Probe LocalVis, native receiver masking, directional-input energy normalization and a forced-false external-GI setter. Voxel construction now uses fixed neighbor-cell footprint queries instead of runtime DDA. Vulkan runtime checks pass; visual quality is not yet accepted.
- Godot MCP is connected. Cornell Box runtime validation used 53,621 Probes at spacing 0.14. Temporary editor-bake bridge and representation metadata were removed after validation; the authored Voxel scene was restored.
- Clarified with the user: Voxel is the generic fast runtime representation and must not store asset LTM. Keep voxel coverage/material assets and construct Volume LTM from fixed 26-neighbor queries, as in reference p. 23. The obsolete runtime segment/DDA path has been removed. Precomputed fields remain the high-detail alternative.

## Earlier MCP validation findings (before propagation correction)

- Point-only neighbor queries missed captured surface bands between Probe centers: only 974 Probes had local transfer and large shadowed regions were pure black. Conservative one-cell footprint resampling increased contributing Probes to 27,505 without changing center-based occupancy (190 occupied Probes). Full local build measured 148.394 ms, sampling 130.597 ms, in this unoptimized developer build; the prior point-only build measured 76.999 ms. These are single-run build measurements, not frame-time or release-performance guarantees.
- Voxel translation plus rotation updated 60 of 144 Trunks, reused the CPU snapshot and GPU history, and took 83.481 ms for local construction. Hiding the block updated 48 Trunks. Restore reproduced matrices, occupancy and LocalVis exactly. Unchanged inputs performed no new geometry build over 30 observed frames. Runtime voxel capture submissions remained zero.
- Explicit editor Bake of ShortBlock succeeded in 715.941 ms and produced three validated dependencies (visibility, material transfer, triangle coverage). Mixed input runtime build succeeded in 156.192 ms. A precomputed-only contributor set (other meshes receive-only) built in 37.198 ms. Rotation, nonuniform scale and mirroring retained valid assets without capture or rebake.
- Missing manifest, stale radius and a temporarily missing visibility file failed explicitly. Restoring the file and metadata restored ready status. Runtime Bake was rejected. No Voxel fallback was selected.
- Static-scene radiance remained finite after 512 extra propagation passes: RGB peak coefficients changed from 80.62387/79.18225/76.72364 to 80.63932/79.19799/76.73766; mean DC changed from 0.386664/0.393959/0.357079 to 0.399255/0.406767/0.368563. This checks finite bounded behavior for that run, not complete convergence. Overlap recomposition restored LocalVis exactly afterward; its radiance sample was taken before deferred Source work had settled and is not energy-validation evidence.
- **Visual acceptance failed on this earlier candidate:** shadow contrast remained too strong, color bleeding was weak, and low-frequency shadow boundaries remained visible in the indirect-only view. Footprint resampling fixed the missing contributors but did not resolve propagation. The direction-loss investigation and correction below supersede this candidate's transport kernel.
- Archived MCP screenshots: [before](lrt_reference/validation/cornell_voxel_before.png), [Voxel after](lrt_reference/validation/cornell_voxel_after.png), [mixed](lrt_reference/validation/cornell_mixed.png), and [indirect only](lrt_reference/validation/cornell_indirect_only.png).
- Alpha/normal-detail variants, all native-light types, external GI with an active provider, complete numerical-core reference checks and actual export/reload remain unvalidated. The export collector is editor-only; a runtime `game_eval` instantiation failed and does not validate packaging. Resume export validation in the editor.

## Earlier propagation direction-loss correction (superseded by reference alignment)

- Reproduced through the production Vulkan pipeline in an empty standalone Volume: uniform input SH `(4, 0.5, 0.25, 0.125)` became `(4.00000095, 0.16666661, 0.08333333, 0.04166663)` after one hop. The former positive-lobe reconstruction incorrectly multiplied every directional coefficient by 1/3 per hop, turning transport into excessive local diffusion. Source, reflection and emission projection contained the same extra low-pass.
- Replaced transport reconstruction with `Y(d)` and aligned native-light, sky, reflection and emission projections with that basis. Sender product bounds and composed receiver bounds now use coefficient L2, compatible with orthogonal reconstruction; old L-infinity bounds assumed a positive gather and were not suitable for the corrected kernel. No brightness multiplier, project lighting change or increased production iteration budget was used.
- The same GPU uniform-field check now returns `(4.00000095, 0.49999997, 0.25000003, 0.12500004)`. An isolated empty-air impulse's squared coefficient energy decreased from 16.328125 to 2.51201975 after one hop, 0.02930827 after 8, and 0.00000427 after 32 with open boundaries; all samples were finite. This verifies coefficient-energy contraction, not pointwise positivity of truncated SH.
- Validated the authored `combined.tscn` with Global, Carriage, Warm and Cool Volumes. All four publish local fields and complete sky visibility with no Volume errors. Before extra diagnostic iterations, the WarmVolume red DC at world `(8, 2.1, -2)` increased from 0.02067857 to 0.11455711, and at `(8, 2.1, 0)` from 0.02238112 to 0.13253942. Production budget settings and authored lights are unchanged; differing sky/source publication timing yielded 76 old versus 83 new total WarmVolume passes.
- Isolated emission by temporarily disabling local sky, external GI and Sun indirect energy, waiting for Source publication, clearing radiance history, then running 64 passes. WarmVolume samples at world x=8, y=2.1 returned red DC 0.19379278 at z=-4.1, 0.05517730 at z=-2, 0.01650906 at z=0, and 0.00614574 at z=2. All four sampled Probes had zero local Source. The last sample is approximately 6.5 m from the panel: it is propagated emission rather than local injection or sky fill. Temporary settings were restored; a fresh normal run was used for final validation.
- In that final normal run, 512 extra diagnostic passes changed total squared RGB coefficient energy by less than 0.8% in every Volume: Global 945.99243→946.93591, Carriage 68003.85939→68010.63852, Warm 11800.95410→11843.16642, Cool 16358.98106→16488.33365. Peak absolute coefficients stayed essentially fixed and there were no NaN/Inf values. Each Volume retained one geometry build.
- Inspected and archived the ordinary-budget combined-scene renders: [before](lrt_reference/validation/propagation_before.png), [after](lrt_reference/validation/propagation_after.png), [carriage after](lrt_reference/validation/propagation_carriage_after.png). The gallery's formerly black walls now receive distributed indirect light. Deep carriage compartments remain dark; this correction establishes transport beyond bright-source neighborhoods, not full visual acceptance of every compartment or all earlier checklist items.
- Engine and changed shaders compiled successfully. Runtime checks used Vulkan and the Godot MCP bridge; no unit-test files or runtime trace path were added.

## Directly lit surface black-patch diagnosis

- Reproduced the reported carriage floor patches through Godot MCP using `CarriageNearWall`. Normal rendering retains bright native direct sunlight; LRT Lighting debug removes direct lighting and reveals black patches at the sunlight footprints. This is an indirect-field defect, not native direct light being multiplied by LRT shadowing.
- GPU field readback in the authored scene found 89 Carriage Source red DC coefficients below -1e-6 and 3,422 radiance red DC coefficients below -1e-6. Upward cosine evaluation divided by PI was negative at 4,846 Source Probes and 14,502 radiance Probes; minima were -0.565286 and -0.764561 respectively. These are whole-volume diagnostic counts, not all visible floor pixels.
- Causal display check: temporarily zeroed only the three directional coefficients in each published RGB radiance buffer, preserving DC, lights, geometry, sky and exposure. Black sunlight-footprint patches became bright patches. All 12 buffers were restored from their original bytes immediately after capture. DC-only is a diagnostic, not an accepted fix.
- `integrate_transfer_direction()` projects reflected directions with full `Y(d)`. A single directional contribution evaluated by the Gather cosine kernel gives a factor proportional to `1 + 2*dot(d,n)`, which is negative when `dot(d,n) < -0.5`. Reflection from floor geometry below a Probe therefore has a negative upper-hemisphere tail. Projected LocalVis products and full-basis propagation also admit signed values. The diagnosed candidate's coefficient-L2 contraction bound constrained coefficient energy but did not preserve nonnegative radiance; that bound is removed in the reference-alignment stage below. `lrt_screen_gather.glsl` clamps the final RGB irradiance to zero, making the negative lobes visible as black patches; it does not remove their earlier cancellation.
- The DC-only experiment identifies directional cancellation in the current field, not which construction/propagation stage first introduces the problematic values. After comparing the PDF, the user selected propagation/operator alignment first; that work is recorded below. Local matrix and final reconstruction alignment remain separate. No engine source or authored project files were changed during the diagnosis itself.
- Archived MCP captures: [normal direct + indirect](lrt_reference/validation/direct_dark_near.png), [LRT indirect only](lrt_reference/validation/direct_dark_indirect.png), [DC-only diagnostic](lrt_reference/validation/direct_dark_dc_diagnostic.png), and [Sun indirect disabled](lrt_reference/validation/direct_dark_no_sun_indirect.png).

## Reference-aligned neighbor operator and energy handling (current candidate)

- User selected: retain all 26 neighbors for this stage, align coefficient Gather and remove extra L2 scaling first; defer four-neighbor scheduling. Keep the current identity-through-air term pending a separate matrix-definition audit. Reference pp. 19–20 specify masked neighbor SH accumulation, receiver LocalVis and local reflection; the PDF does not publish normalization or the complete four-neighbor/dither implementation. Equal averaging is our declared choice, not a recovered author formula.
- RGB and geometry-only sky now directly average masked SH4 coefficients. Removed neighbor-direction sampling/reprojection, sender spectral normalization, composed receiver normalization, transport-weight calculation/storage/use and its update counters. The existing 256-byte record layout retains a reserved zero vec4 at plane 12, keeping emission at planes 13–15. Local cache format 25 / build fingerprint 42 invalidate old derived Volume records. Mesh-local precomputed coefficients are unchanged and do not need rebaking for this step; existing Volume data needs rebuild/save.
- Engine build passed with `platform=windows target=editor dev_build=yes debug_symbols=no`; the current Python build environment required installing `zstandard`. Production Vulkan dispatches of both changed shaders ran through Godot MCP. No unit-test files, runtime traces, new configuration switches or engine glue changes were added.
- Standalone production-GPU checks on a 5x5x5 empty grid: uniform `(4, .5, .25, .125)` became `(4.00000191, .50000024, .25000012, .12500006)` at the interior receiver. A single populated neighbor produced `(.15384619, .01923077, .00961539, .00480769)`, matching input/26 without directional reprojection; its original cell became zero. With sender/receiver mask `(3, .7, -.5, .25)` (spectral norm 1.09899), the result `(2.60934544, .34382430, .18924527, .08442668)` matched the raw double Triple Product CPU calculation to less than 1e-6, confirming removal of the extra mask scaling.
- Authored `combined.tscn` published all four Volumes and completed sky work without Volume errors. The ordinary production RGB budgets were 74/74/84/87 passes for Global/Carriage/Warm/Cool. Two extra diagnostic batches of 512 passes gave the following total squared RGB coefficient energies (arbitrary coefficient units, not physical radiant energy); all values were finite. The last batch changed each RGB channel by less than 0.002%, with essentially fixed peaks.

| Volume | Ordinary budget | +512 passes | +1024 passes |
| --- | ---: | ---: | ---: |
| Global | 30390.28604 | 30953.19757 | 30953.19756 |
| Carriage | 815228.63013 | 818228.70222 | 818228.70227 |
| Warm | 108205.82812 | 113613.17593 | 113613.76726 |
| Cool | 115676.00369 | 124620.27662 | 124622.22876 |

- Restarted the game before final captures so they show ordinary-budget output, not the extra diagnostic iterations. Inspected [normal](lrt_reference/validation/sh_average_carriage_normal.png) and [LRT indirect only](lrt_reference/validation/sh_average_carriage_indirect.png). Floor sunlight footprints now receive bright indirect patches instead of the former black patches. Large dark wall/ceiling regions remain; complete visual acceptance is still open. This stage does not establish local-matrix or receiver-reconstruction equivalence to the reference.
- Next independent stage: audit the reference's `SampleBasis` / `GetSH2PIDivDFT(-SampleDir)` convention against the current normal-based local reflection matrix, then audit final receiver reconstruction. External GI with an active provider remains unvalidated.

## Dark-wall investigation at the user's updated voxel settings

- Read the current project through MCP rather than restoring previous precision: effective voxel cell size 0.125 m, Carriage Probe spacing 0.2 m, surface normal bias 0.1 m. Carriage has 9,240 occupied Probes. All four Volumes publish without errors. No engine source, project precision, lighting or authored scene properties were changed in this investigation.
- Captured the actual depth/normal inputs with a temporary read-only GPU compute dispatch, then reconstructed world position and normal using the same matrices as production Gather. Replayed its eight-Probe interpolation on CPU from published fields. The sampled wall normals face the carriage interior; they are not flipped. The helper shader and its resources existed only in memory and were freed after readback.
- Right-wall pixel (1050,430), world approximately (-1.6812,2.3356,7.4948): 60.58% of interpolation weight is on occupied zero Probes. At the ordinary budget the interpolated red DC contribution is 0.104045, but cosine evaluation yields 0.013774: directional terms cancel approximately 86.8% of that DC contribution. Zeroing only radiance directional coefficients makes this wall brighter; all 12 original RGB buffers were restored afterward. These are separate occupancy-interpolation and directional-cancellation effects.
- Left-wall pixel (40,430), world approximately (1.6812,2.2455,10.9386): occupied weight is zero, DC contribution 0.001982 and raw red receiver value 0.001612. DC-only display leaves this region dark: the field itself has insufficient energy there. After 512 extra RGB passes the receiver value is still only 0.002417. The issue is not resolved by longer iteration.
- A cross-section at world y=2.3028, z=10.9000 after a history reset and 512 ordinary-operator passes shows red DC 0.302164 at x=-1.06, 0.105235 at x=-0.06, 0.024228 at x=0.94 and 0.004262 at x=1.54. The first three locations have identity LocalVis and no local Source; the last is in the left-wall support band. The current coefficient-averaging transport spreads by spatial diffusion while nearby geometry removes energy; preserving SH direction coefficients alone does not provide directional spatial streaming.
- Decisive independent production-GPU check: a white (RGB reflectance 1) flat box from y=-0.2 to 0, a 0.2 m Probe grid, no sky/external/native/emission Source, initial unit isotropic radiance in every air Probe, zero in occupied Probes. At an interior receiver y=0.3, one hop reduces red DC from 1 to 0.469078; y=0.5 drops from 1 to 0.880178 despite having no occupied immediate neighbors. Both receivers' 26-neighbor stencils are entirely inside the Volume. Local-record and GPU buffers are valid. CPU decomposition of the same input matches the GPU result:

| Stage at y=0.3 | Red DC contribution |
| --- | ---: |
| Initial uniform air | 1.000000 |
| Fixed neighbor average (9 occupied neighbors) | 0.653846 |
| After sender LocalVis masking | 0.547337 |
| After receiver LocalVis masking | 0.306432 |
| Added local reflection | 0.162646 |
| Final | 0.469078 |

- This identifies a boundary-energy closure problem in the current combination of fixed masked-neighbor averaging, zero occupied samples and the retained identity-plus-reflection operator. The white reflecting boundary fails to preserve a uniform physical incident field; it is not sufficient to tune exposure or voxel precision. The PDF's omitted transport details and the retained local-matrix/identity convention must be reconciled before attributing this behavior to the original author.
- Counterexample to a tempting workaround: replacing transport LocalVis with identity in air while retaining the existing reflection matrices and Source caused red coefficient peak to reach 5.83e17 in 64 passes. That ablation changes the operator's energy balance and is not valid visual evidence that removing visibility fixes darkness. Restored the original GPU visibility bytes, cleared RGB history and recomputed the original operator; peak returned to 8.88567. Do not remove visibility as a fix.
- Receiver-only diagnostic: temporarily changing runtime normal bias from 0.1 to 0.3 m removes occupied interpolation weights at the sampled right/back wall and increases right-wall red receiver value from about 0.0139 to 0.0704. Left-wall value rises only from about 0.0024 to 0.0068; it remains weak. Restored bias to 0.1 without saving settings. Moving samples is not a correction to the transport or negative lobes.
- Evidence: [ordinary-budget baseline](lrt_reference/validation/wall_dark_baseline.png), [DC-only receiver diagnostic](lrt_reference/validation/wall_dark_dc_only.png), [receiver-bias diagnostic](lrt_reference/validation/wall_dark_bias_diagnostic.png). The identity-mask capture is an invalid, divergent diagnostic and must not be treated as a successful lighting comparison.
- Recommended correction order: close the propagation energy budget at reflecting/occupied boundaries (including sender/receiver mask semantics and the retained identity term), then align reflected-direction reconstruction and receiver sampling. A fix must preserve empty-air transport, avoid the measured flat-white-boundary loss, remain stable with visibility and Source, and reduce the actual wall artifacts without a brightness multiplier. Four-neighbor scheduling remains deferred.

## Local baseline checkpoint

- At the user's request, reverted the subsequent wall-fix attempt and retained the coefficient-average baseline: local cache 25, build fingerprint 42, precomputed bake model 3. The experimental response bounds, wrapped kernel, boundary extension, receiver limiter and two-dispatch incident-field pipeline are not included.
- Dark-wall findings above remain unresolved and are the starting point for later optimization. This checkpoint is committed locally only; no push is requested.
