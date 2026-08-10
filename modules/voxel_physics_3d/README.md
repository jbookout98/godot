# Voxel Physics 3D

`voxel_physics_3d` is a Godot engine module that combines a voxel-aware 3D
physics backend with sparse voxel storage and GPU ray-marched voxel rendering.
It is a fork of GodotPhysics3D, so it replaces rather than supplements the
stock `godot_physics_3d` module.

## Building

The stock and voxel physics modules contain classes with the same internal
names and cannot be linked into one executable. Disable the stock module and
explicitly enable this module in every clean and build command:

```powershell
py -m SCons platform=windows target=editor module_godot_physics_3d_enabled=no module_voxel_physics_3d_enabled=yes --clean
py -m SCons platform=windows target=editor module_godot_physics_3d_enabled=no module_voxel_physics_3d_enabled=yes -j2
```

Use `-j1` if MSVC reports `C1060: compiler is out of heap space`. The resulting
editor is `bin/godot.windows.editor.x86_64.exe`. Add `dev_build=yes` only when
working on engine diagnostics; the `.dev` executable can be substantially
slower and is not suitable for measuring game performance.

After launching the custom editor, select **VoxelPhysics3D** as the 3D physics
engine if it is not already selected. A build made without the module flags
does not register `VoxelVolume3D`, `VoxelShape3D`, `VoxelShapeData`, or
`VoxelMaterial`.

## Quick start

Create one `VoxelShapeData` resource and share it between a visual volume and
a collision shape:

```gdscript
var data := VoxelShapeData.new()
data.dimensions = Vector3i(32, 16, 32)
data.voxel_size = 0.25
data.fill_voxel_region(Vector3i(0, 0, 0), Vector3i(32, 2, 32), 1)

var volume := VoxelVolume3D.new()
volume.voxel_data = data
volume.voxel_material = VoxelMaterial.new()
add_child(volume)

var body := StaticBody3D.new()
var collision := CollisionShape3D.new()
var shape := VoxelShape3D.new()
shape.voxel_data = data
collision.shape = shape
body.add_child(collision)
add_child(body)
```

Palette index `0` means empty. Indices `1..255` are occupied and address the
same entry in every visualization lookup texture.

## Public API

### VoxelShapeData

`VoxelShapeData` is the authoritative voxel resource. It owns dimensions,
voxel size, sparse occupancy and palette indices, visualization textures, and
optional collision feature shapes.

Voxel coordinates are zero-based and use X-major linear indexing:

```text
index = x + y * dimensions.x + z * dimensions.x * dimensions.y
```

The valid coordinate range is `[0, dimensions)` on every axis. Changing
`dimensions` clears the previous voxel contents. `voxel_size` must be greater
than zero and is measured in Godot world units.

The preferred editing methods are:

- `set_voxel()` for an isolated change.
- `apply_voxel_edits()` for coordinate-based batches.
- `apply_voxel_edits_by_index()` when the caller already has linear indices.
- `fill_voxel_region()` for an axis-aligned block.
- `begin_edit()` and `end_edit()` around many custom operations so listeners
  receive one combined dirty region and one revision increment.

Batch methods return the number of values that actually changed. Out-of-range
entries in batch arrays are skipped. A direct out-of-range `set_voxel()` is an
error. Palette values must be in `0..255`.

Every flushed voxel edit increments `revision`, emits
`voxels_changed(position, size, revision)`, and emits the standard Resource
`changed` notification. `position` and `size` describe the inclusive union of
all modified cells in that edit transaction.

Storage uses `VoxelBrickStorage`, which divides the volume into 8 × 8 × 8
bricks. A brick is empty, uniform, or mixed. Only mixed bricks allocate a
512-byte payload. `sparse_brick_data` is the serialized representation.
`voxel_data` and `solid_voxels` materialize dense arrays and exist for scripting
and compatibility; avoid them in hot paths or for very large volumes.

Surface topology is derived from occupied neighbors. The six mask bits map to
`+X`, `-X`, `+Y`, `-Y`, `+Z`, and `-Z`. A solid cell with one exposed side is a
face, two exposed sides is an edge, and three or more exposed sides is a
corner. The legacy topology setters validate old resource data but topology is
not stored independently.

`palette_texture` provides 256 color entries. `metallic_texture`,
`transparency_texture`, `specularity_texture`, and `emission_texture` are
matching 256-entry grayscale lookups. Their values are inverted: white is 0,
black is 1, and gray is `1 - value`. Roughness is `1 - specularity`.
Transparency 0 is opaque and 1 is fully transparent. The older packed
`material_texture` remains a fallback for roughness, metallic, and emission.

### VoxelVolume3D

`VoxelVolume3D` renders a `VoxelShapeData` resource. It creates a 36-vertex box
proxy and ray marches the sparse voxel textures in the fragment shader. The
node origin is the minimum corner of voxel `(0, 0, 0)`; the visual AABB is
`dimensions * voxel_size`.

The authored `VoxelMaterial` is lightweight configuration. Each volume creates
a private runtime material containing its shader and GPU textures. This avoids
serializing generated 3D textures into scenes and prevents volumes that share
configuration from sharing mutable GPU bindings.

Coordinate conversion helpers use the node transform:

- `local_to_voxel()` floors local coordinates by `voxel_size`.
- `world_to_voxel()` first transforms the point to node-local space.
- `voxel_to_local()` and `voxel_to_world()` return the cell center by default;
  pass `false` to return its minimum corner.

Resources are reference-counted and may be shared by several nodes.
`make_voxel_data_unique()` deep-duplicates the current resource before local
editing.

Streaming modes are:

- `STREAMING_AUTOMATIC`: residency depends on camera distance and the global
  resident-volume budget.
- `STREAMING_ALWAYS_RESIDENT`: the GPU textures and proxy remain resident.
- `STREAMING_MANUAL`: the manager leaves `streaming_resident` under caller
  control.

Automatic volumes begin nonresident so loading a scene does not construct GPU
textures that the first residency pass would immediately discard. The initial
streaming pass runs on the first process frame; subsequent passes run every 12
process frames. A per-volume
`streaming_distance` of zero uses
`rendering/voxel_volume/streaming_distance`. Eligible volumes are sorted by
camera distance and capped by
`rendering/voxel_volume/max_resident_volumes`. A volume without an active
camera remains resident. Streaming releases rendering textures and geometry;
the CPU voxel resource remains available.

### VoxelShape3D

`VoxelShape3D` adapts `VoxelShapeData` to a `Shape3D` RID with custom physics
shape type. Assign it to a `CollisionShape3D`. Resource changes are forwarded
to the active physics server, so edits update collision without replacing the
shape resource. Debug geometry displays the volume bounds, not every occupied
cell.

### VoxelMaterial

`VoxelMaterial` selects unlit or PBR ray-marched rendering and controls the
emission multiplier. The runtime shader is generated lazily by
`VoxelVolume3D`; application code normally changes only `shading_mode` and
`emission_energy`.

## Physics architecture

The files named `godot_*` are the in-tree GodotPhysics3D fork. Area, body,
broad-phase, constraint, joint, space, stepping, and direct-state behavior
remain compatible with the stock backend. Voxel-specific integration points
are intentionally narrow:

1. `VoxelShape3D` submits `VoxelShapeData` through a custom physics shape RID.
2. `GodotPhysicsServer3D` constructs `GodotVoxelShape3D` for that custom type.
3. `GodotCollisionSolver3D` routes pairs containing a voxel shape to
   `GodotVoxelCollisionSolver3D`.
4. The voxel solver gathers candidate contacts from occupied voxel boxes and
   feature proxies, reduces them to a manifold, and returns voxel indices in
   the callback feature-index fields.
5. `GodotBodyPair3D` stores those indices, reports voxel coordinates through
   direct body state, and uses the indices to persist warm-start impulses.

Voxel-to-convex collision and swept-distance queries use cached exposed-surface
cells. This keeps `CharacterBody3D.move_and_slide()` and long `SpringArm3D`
casts from testing every filled interior voxel. If a query range contains no
surface cells, occupied cells remain as an embedded-shape fallback.
Voxel-to-concave collision uses the concave shape's culling callback.
Voxel-to-voxel collision tests edges against edges, then corners from each
shape against the other shape's occupied volume. Faces and edges are not
tested against the occupied volume. A seen set prevents duplicate ownership
of one voxel pair. Raw generation stops at 256 contacts for edge-edge and for
each corner-volume direction, as in the reference narrowphase, so pathological
overlaps cannot continue running proxy SAT tests after that pass has enough
input. Separate budgets prevent edge contacts from starving corner contacts.
The final manifold is selected from the bounded combined set for penetration
and spatial spread.

Edge membership is cached as a 512-bit mask in each 8³ physics brick. The
edge-edge inner loop reads this mask directly instead of reclassifying the
target voxel and querying its six neighbors for every nearby edge candidate.

Manifold reduction is deterministic. It chooses the deepest candidate first,
then maximizes minimum squared separation for spatial coverage. Penetration and
squared separation are compared as separate lexicographic keys rather than
added together. Equivalent candidates are resolved by voxel indices, normal,
and midpoint so traversal order cannot randomly change the selected manifold.

The body-pair manifold holds up to eight contacts because voxel volumes can
position-based recycling and cached impulses to warm-start the next solver
step. Voxel contacts additionally match the nearest unused support point with
the same voxel pair and a compatible normal. A matched contact uses the newly
detected local anchors and normal while retaining its accumulated impulses,
preventing stale cached geometry from suppressing a newly penetrating point.
One-to-one matching prevents several support points from collapsing into one
cache entry. Voxel indices remain attached to contacts for reporting and
diagnostics.

## Rendering architecture

`VoxelVolume3D` converts CPU brick storage into two GPU textures:

- The brick-directory 3D texture has one RGBA8 texel per logical brick. Empty
  bricks use code 0, uniform bricks use code 1 with the palette value in alpha,
  and mixed bricks store an atlas slot plus 2.
- The L8 mixed-brick atlas tightly packs only mixed 8³ payloads. Empty and
  uniform bricks consume no atlas payload.

The generated shader intersects the camera ray with the proxy AABB, traverses
voxel cells, samples the palette-indexed lookups, and writes depth and lighting
data. Hardware back-face culling selects one proxy layer instead of comparing
overlapping proxy fragments in the shader. At a shared edge, simultaneous DDA
entry crossings prefer an exposed volume face over an internal neighbor wall,
so discarding the wall cannot also remove the exterior sample. Brick and voxel
coordinates remain integer during DDA traversal. Each
crossing time is recalculated from its integer grid plane instead of accumulated
with repeated floating-point additions, and the final depth is snapped to the
exact face plane. Axes parallel to the ray are excluded from crossing selection
so they cannot consume the traversal limit without moving. This keeps adjacent
proxy triangles and touching volumes from disagreeing at shared boundaries.
Touching, grid-aligned resident volumes exchange six cached occupancy slices.
The shader rejects a volume-local boundary wall when the neighboring voxel is
solid, preventing internal walls from entering camera and shadow depth. The
slices update only when adjacency, residency, transforms, or voxel revisions
change; they are not rebuilt by the periodic steady-state residency ranking.
Identical material variants also share their generated Shader resource to avoid
per-volume pipeline compilation stalls. All voxel and lookup textures use
nearest sampling without mipmaps. PBR mode supplies albedo, roughness, specularity,
metallic, emission, and a stable light vertex at the hit cell. A transparency
map selects the transparent depth-prepass shader variant; opaque volumes keep
the opaque variant. Unlit mode adds the spatial shader `unshaded` render mode.

## Source map

- `voxel_shape_data.*`: resource API, edit transactions, topology derivation,
  textures, and feature-shape configuration.
- `voxel_brick_storage.*`: sparse 8³ brick representation, normalization,
  dense conversion, serialization, revisions, and dirty flags.
- `voxel_volume_3d.*`: visual node, GPU texture construction, coordinate
  conversion, and residency transitions.
- `voxel_material.*`: generated ray-march shader and authored shading options.
- `voxel_shape_3d.*`: public `Shape3D` adapter.
- `godot_voxel_shape_3d.*`: physics-backend representation and feature proxies.
- `godot_voxel_collision_solver_3d.*`: voxel pair routing, contact generation,
  distance queries, and manifold reduction.
- `voxel_contact_3d.h`: shared voxel contact/feature metadata types.
- `gjk_epa.*`: convex distance and penetration support used by voxel queries.
- `godot_collision_solver_3d_sat.*`: separating-axis collision calculations.
- `godot_collision_solver_3d.*`: top-level shape-pair dispatch.
- `godot_body_pair_3d.*`: rigid contact manifold, warm starting, reporting,
  bias correction, friction, bounce, and CCD.
- `godot_area_pair_3d.*`: body/area and area/area overlap constraints.
- `godot_body_3d.*`, `godot_area_3d.*`, `godot_collision_object_3d.*`: backend
  objects and shape ownership.
- `godot_shape_3d.*`: stock primitive/concave shape implementations used by
  the backend and as voxel feature proxies.
- `godot_broad_phase_3d*`: broad-phase interfaces and BVH implementation.
- `godot_space_3d.*`: space state, query state, pair management, and settings.
- `godot_step_3d.*`: island generation and iterative physics stepping.
- `godot_physics_server_3d.*`: PhysicsServer3D API implementation and RID
  ownership.
- `godot_body_direct_state_3d.*`: script-facing direct body state, including
  voxel contact reporting.
- `godot_soft_body_3d.*`: soft-body backend integration.
- `godot_constraint_3d.h`, `godot_joint_3d.h`, and `joints/`: constraint base
  types and all standard GodotPhysics3D joint solvers.
- `voxel_volume_streaming_manager.*`: camera-distance residency service.
- `register_types.*`: physics backend, class, project-setting, and singleton
  registration.
- `config.py` and `SCsub`: module compatibility checks and SCons sources.

## Debugging

Use **Visible Collision Shapes** to inspect contact behavior and the physics
debug-contact facilities to inspect manifold points. Alternation between
different voxel IDs, distant points, or normals on opposite sides indicates
contact-generation instability. Warm starting affects solver forces; it does
not make generated debug-contact positions persistent.

If the classes disappear from the editor:

1. Confirm the executable came from this source tree.
2. Rebuild with `module_godot_physics_3d_enabled=no` and
   `module_voxel_physics_3d_enabled=yes`.
3. Clean with the same module flags before rebuilding after switching physics
   backends.
4. Confirm `VoxelPhysics3D` is the selected 3D physics engine.

## Contribution rules

Keep script-visible declarations and `doc_classes/*.xml` synchronized. Preserve
the meaning of palette index zero, X-major indexing, the six topology mask
bits, and the 8³ brick serialization format unless a migration path is added.
Changes to callback feature indices must preserve voxel identity because body
pair persistence and contact reporting depend on them.
