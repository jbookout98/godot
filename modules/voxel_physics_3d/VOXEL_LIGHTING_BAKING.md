# Voxel lighting bake authoring

This feature seeds the existing Voxel Forward DDGI renderer from saved, world-space probe samples. It does not replace the dynamic renderer or bake materials into gameplay code.

## Authoring

1. Enable **Voxel Forward**, voxel indirect lighting, and the **DDGI** backend.
2. Add one **VoxelLightingBake3D** below the level root. Its **Source Root** defaults to its parent. That root must contain the authored geometry, lights, and WorldEnvironment that contribute to the level's starting lighting.
3. Set **Bake Bounds** in the bake node's local coordinates. The baker covers those bounds with overlapping camera windows; it does not use the current editor camera position.
4. Select the node and click **Bake Voxel Lighting** in the 3D toolbar. On the first bake, choose a project-local `.vlight` file. Later bakes reuse the assigned file. Godot's standard progress dialog shows the current view and supports cancellation. A successful bake saves the resource, assigns **Lighting Data**, refreshes the Inspector, and marks the scene unsaved. Save the scene to persist that reference.
5. Leave **Lighting Data** assigned on the node. It is a normal resource dependency and is included with the scene in exports.

The Inspector's **x/y/z** are the minimum corner; **w/h/d** are the region's dimensions, not its maximum corner. For the validation lobby, position `(-12, -0.5, -12)` and size `(24, 6, 24)` cover the room. A `512 × 512 × 512` region is much larger and can exceed the 4,096-view limit even when DDGI is enabled.

Voxel geometry and lights are copied into an isolated world for baking. CharacterBody3D, RigidBody3D, and other non-static PhysicsBody3D subtrees are excluded. The `voxel_bake_excluded` group excludes any other transient subtree. Initially movable objects should be excluded and illuminated by live updates. Do not group permanent geometry with transient objects.

No scripts are run on the copied bake scene. Editor-generated VoxelShapeData must already describe the authored geometry. Bake output is assigned only after all views finish, the source fingerprint remains unchanged, and the resource has been saved successfully. Cancellation or a failed save retains the previous assignment; cancellation does not write partial output.

**Settle Frames** is the minimum per-view warmup, measured in rendered frames. Captures additionally wait for relevant probes to reach a mature state, for placement jobs to finish, and for the shadow-atlas rebuild to finish. **Max Frames Per View** prevents an unconverging bake from running indefinitely. The baker rejects more than 4,096 views or a probe payload over 256 MiB; reduce bounds or probe density when needed. This cap covers serialized samples, not total process memory: capture textures, the isolated world, and indexing also consume memory.

## Runtime behavior

The cache stores versioned probe identities, placements, directional irradiance, visibility moments, and confidence. It maps these samples into the current circular DDGI windows. Runtime GPU uploads occur when resident probe identities need initialization. GPU readback is limited to explicit baking and explicitly requested probe statistics.

Loading is asynchronous after scene setup and native placement. A source check immediately before the first restore rejects authored edits made during startup. Excluded actors can continue moving while the initial windows are seeded. Occupancy builds use a bounded batching delay and publish complete immutable snapshots while queuing newer changes; continuous animation cannot indefinitely postpone initialization. Runtime geometry or lights excluded from the bake cause restored samples to enter the native refresh scheduler. Saved samples provide their starting history; live objects are not permanently lit by an obsolete static solution.

For a loading screen, keep the level rendering behind the overlay and call `request_cache_status()` at a modest interval. `cache_status.ready` means the initial four resident windows have been considered for seeding. It does not mean all unbaked probes, runtime additions, or direct shadows have converged. An absent `ready` key means no cache is registered; `invalidated` means dynamic fallback. Always permit fallback and use a timeout instead of waiting indefinitely. A paused or hidden viewport cannot initialize its lighting.

Missing, stale, unsupported, or corrupt caches fall back to ordinary dynamic lighting. Compatibility checks include a deterministic source fingerprint, probe layout, payload size and checksum, finite samples, and unique probe identities. A changed source scene requires a new bake.

After the initial windows have been seeded, the renderer conservatively disables further seeding after a voxel-world, shading, or lighting revision changes. The first geometry, shading, or local-light edit also requests a lighting refresh across resident cascades, covering indirect effects beyond the edited bounds. Already resident lighting remains owned by the native dynamic updater; it is not overwritten with the original bake. Newly visited regions then initialize dynamically. Regions outside the bake bounds always initialize normally.

Each World3D owns its occupancy, lighting histories, and cache. Viewports sharing a world also share camera-following probe windows; widely separated cameras can increase residency churn. Detached volumes cannot re-register through a shared material's `changed` signal.

Baking reduces work to converge indirect lighting. It does not eliminate scene loading, occupancy construction, probe placement, shader compilation, or GPU allocation/upload costs. Do not assume a frame-rate or startup improvement without measurement.

## API and validation

`bake(save_path = "")` starts asynchronous work and returns an Error. A nonempty path saves the completed `.vlight` before assigning it; an empty path creates in-memory data for tools. The native editor plugin always supplies an external path. Observe `bake_progress(progress)` and `bake_finished(error)`. An empty completion error means success. `cancel_bake()` cancels without publishing partial data. `request_cache_status()` emits `cache_status` with restored probe count, readiness, runtime-input refresh, and invalidation state. `request_cache_status(true)` additionally reads GPU records for explicit diagnostics; do not poll this form in gameplay.

The focused project and its README in `tests/lighting_bake_project` describe bake/load, mutation, lifecycle, file-validation, and export checks. Run GPU checks with the modified engine and a real Vulkan device. Headless parsing alone does not verify capture, restore, or image equivalence.

## Builds and file format

Use both the modified editor and a matching modified export template. A stock Godot export template does not contain these custom classes or the restore shader. The editor UI is compiled only with tools enabled; resource loading, restore, and dynamic updates also compile in export templates.

The dedicated `.vlight` format has a fixed 200-byte header and an uncompressed, checksummed payload. The loader validates version, bounds, spacing, exact file length, and the 256 MiB payload cap before allocating sample storage. Keep caches external in this format; generic embedded `.res`/`.tscn` resources use Godot's generic allocation rules.
