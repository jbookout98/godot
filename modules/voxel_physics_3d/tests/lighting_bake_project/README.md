# Voxel lighting bake regression project

Run these commands from this directory with the modified Godot editor binary (`$engine` below). Use a real Vulkan GPU and the Voxel Forward renderer. Run GPU processes serially for comparable results.

```powershell
& $engine --path . --rendering-method voxel_forward --disable-vsync --max-fps 60 -- --mode=bake
& $engine --path . --rendering-method voxel_forward --disable-vsync --max-fps 60 -- --mode=load
& $engine --headless --path . --script res://file_validation.gd
```

`bake` creates `fixture_lighting.vlight` and `export_fixture.tscn`. Each following mode must run in a fresh process. Successful runs print `LIGHTING_BAKE_TEST_OK` and exit zero. GPU images go to `user://lighting_MODE_FRAMES.png`.

| Modes | Coverage |
| --- | --- |
| `unit`, `repeat`, `save_failure` | Signature change, immediate cancellation, cancellation retaining the previous resource, failed-save retention, rebake |
| `load`, `unbaked` | Matched starting scene, restore statistics and screenshot |
| `mutate`, `construct`, `move`, `detach` | Destruction, construction, transform change, detached shared-material signal |
| `light`, `local_light`, `emission`, `early_light` | Directional/local/emissive changes, source edit while startup is pending |
| `runtime_inputs`, `runtime_sun`, `moving_startup` | Initially present excluded actor/local light or directional light; warm-start refresh and continuously moving startup |
| `camera`, `views`, `worlds`, `switch` | Out-of-bounds/cascade traversal, shared viewport, independent world, scene reload |
| `stale`, `corrupt`, `version` | Safe fallback from incompatible data |

The mutation modes also accept `unbaked_` prefixes for image comparisons. `unbaked_runtime_inputs` and `unbaked_runtime_sun` are corresponding dynamic references. `--frames=N` controls the initial measurement window (default 360). Probe statistics are requested after that timed window and involve an explicit GPU readback. `ready` describes cache application, not convergence of the complete rendered image.

`file_validation.gd` checks truncated and oversized files, unsupported versions, nonfinite layout, checksum corruption, trailing bytes, duplicate logical cells, and a missing external cache dependency. It requires the generated valid fixture first.

For export, point the **Lighting Validation** preset's custom debug template to your matching engine build. Temporarily set the main scene to `res://export_fixture.tscn`, export that preset, and restore the main scene to `res://main.tscn`. Launch the exported executable with its adjacent PCK. Success prints `EXPORTED_LIGHTING_OK`. The assigned `.vlight` is a resource dependency and must appear in the pack.

The small room deliberately uses pre-authored material resources. Test scripts create geometry and lights to exercise renderer behavior; they do not create gameplay materials.

`editor_smoke.gd` is a temporary **EditorPlugin**, not a SceneTree script. Run `python run_editor_smoke.py /absolute/path/to/modified/godot.exe` after `bake` has generated the export fixture. The runner enables the plugin in this test project, opens a normal editor, tests first bake/save/Inspector assignment/rebake/cancellation, then starts another editor to verify persistence. It restores project settings and removes its temporary plugin afterward.
