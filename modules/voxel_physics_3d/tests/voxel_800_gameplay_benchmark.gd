extends SceneTree

const WARMUP_FRAMES := 360
const SAMPLE_FRAMES := 600
const REQUIRED_RESIDENT_VOLUMES := 515

var _mode := "moving"
var _variant := "current"
var _frame := 0
var _world: Node3D
var _camera: Camera3D
var _anchor_position := Vector3.ZERO
var _scene_center := Vector3.ZERO
var _scene_radius := 1.0
var _volumes: Array[VoxelVolume3D] = []
var _sample_start_usec := 0
var _last_frame_usec := 0
var _frame_times_usec: Array[int] = []
var _process_time_sum := 0.0
var _physics_time_sum := 0.0
var _draw_call_sum := 0.0
var _object_sum := 0.0
var _minimum_objects := INF
var _maximum_objects := 0.0
var _resident_volumes_at_ready := 0
var _visible_volume_centers_at_ready := 0
var _validation_spot: SpotLight3D


func _initialize() -> void:
	Engine.max_fps = 0
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_EXCLUSIVE_FULLSCREEN)
	DisplayServer.window_set_size(Vector2i(1920, 1080))
	_apply_medium_settings()
	_mode = OS.get_environment("VOXEL_800_BENCH_MODE")
	if _mode.is_empty():
		_mode = "moving"
	_variant = OS.get_environment("VOXEL_800_BENCH_VARIANT")
	if _variant.is_empty():
		_variant = "current"
	_apply_variant_settings()
	if OS.get_environment("VOXEL_800_BENCH_DDGI_DEBUG") == "on":
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 6)
	call_deferred("_start_offline_world")


func _apply_variant_settings() -> void:
	if _variant == "legacy":
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/backend", 1)
	elif _variant == "ddgi":
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/backend", 2)
	elif _variant == "no_shadow":
		ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", false)
	elif _variant == "no_local_voxel_shadows":
		ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/max_local_lights", 0)
	elif _variant == "no_indirect":
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", false)
	elif _variant == "no_reflections":
		ProjectSettings.set_setting("rendering/voxel_forward/reflections/enabled", false)
	elif _variant == "base_only":
		ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", false)
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", false)
		ProjectSettings.set_setting("rendering/voxel_forward/reflections/enabled", false)


func _apply_medium_settings() -> void:
	# Pin the benchmark to the same cohesive Medium profile used by the player
	# settings screen. Without this, a saved editor/Ultra atmosphere profile can
	# make nominally identical runs differ by several times their frame cost.
	var values := {
		"rendering/scaling_3d/scale": 1.0,
		"rendering/voxel_volume/max_resident_volumes": 515,
		"rendering/voxel_volume/max_loads_per_frame": 4,
		"rendering/voxel_volume/shadow_proxy/enabled": false,
		"rendering/voxel_forward/shadow_mask/enabled": true,
		"rendering/voxel_forward/shadow_mask/resolution_scale": 0.5,
		"rendering/voxel_forward/shadow_mask/max_distance": 112.0,
		"rendering/voxel_forward/shadow_mask/max_steps": 256,
		"rendering/voxel_forward/shadow_mask/atlas_resolution": 512,
		"rendering/voxel_forward/shadow_mask/max_local_lights": 2,
		"rendering/voxel_forward/shadow_mask/soft_shadow_mode": 1,
		"rendering/voxel_forward/shadow_mask/soft_shadow_samples": 1,
		"rendering/voxel_forward/reflections/enabled": false,
		"rendering/voxel_forward/reflections/resolution_scale": 0.35,
		"rendering/voxel_forward/reflections/max_distance": 64.0,
		"rendering/voxel_forward/reflections/max_steps": 256,
		"rendering/voxel_forward/reflections/grid_resolution": 48,
		"rendering/voxel_forward/indirect_light/resolution": 32,
		"rendering/voxel_forward/indirect_light/near_cell_size": 0.4,
		"rendering/voxel_forward/indirect_light/far_cell_size": 1.6,
		"rendering/voxel_forward/indirect_light/distant_cell_size": 6.4,
		"rendering/voxel_forward/indirect_light/recenter_hysteresis": 0.75,
		"rendering/voxel_forward/indirect_light/propagation_steps": 4,
		"rendering/voxel_forward/indirect_light/propagation_decay": 0.85,
		"rendering/voxel_forward/indirect_light/dirty_cell_pass_budget_per_frame": 524288,
		"rendering/voxel_forward/indirect_light/temporal_blend_frames": 2,
		"rendering/voxel_forward/indirect_light/ddgi/spatial_cascade_resolution": 16,
		"rendering/voxel_forward/indirect_light/ddgi/rays_per_probe": 16,
		"rendering/voxel_forward/indirect_light/ddgi/probes_per_frame": 32,
		"rendering/voxel_forward/indirect_light/ddgi/self_shadow_bias": 0.3,
		"rendering/voxel_forward/indirect_light/ddgi/irradiance_hysteresis": 0.98,
		"rendering/voxel_forward/indirect_light/ddgi/visibility_hysteresis": 0.985,
		"rendering/voxel_forward/indirect_light/intensity": 1.0,
		"rendering/voxel_forward/indirect_light/ddgi/lod_transition": 0.15,
		"rendering/voxel_forward/indirect_light/ddgi/debug_mode": 0,
		"game/graphics/atmosphere/cloud_quality": 2,
		"game/graphics/atmosphere/cloud_distance": 640.0,
		"game/graphics/atmosphere/cloud_density": 0.5,
		"game/graphics/atmosphere/god_ray_quality": 2,
	}
	for key: String in values:
		ProjectSettings.set_setting(key, values[key])
	if OS.get_environment("VOXEL_800_BENCH_CLOUDS") == "off":
		ProjectSettings.set_setting("game/graphics/atmosphere/cloud_quality", 0)
		ProjectSettings.set_setting("game/graphics/atmosphere/god_ray_quality", 0)


func _start_offline_world() -> void:
	var session_manager := root.get_node_or_null("SessionManager")
	if session_manager == null:
		push_error("VOXEL_800_ERROR: SessionManager autoload is unavailable")
		quit(2)
		return
	session_manager.call("start_offline", "__voxel_800_benchmark__", false)


func _process(_delta: float) -> bool:
	if _world == null:
		_try_prepare_world()
		return false

	_frame += 1
	_update_camera()
	_update_validation_spot()
	if _frame == WARMUP_FRAMES:
		_print_ready_state()
		_sample_start_usec = Time.get_ticks_usec()
		_last_frame_usec = _sample_start_usec
		return false

	if _frame > WARMUP_FRAMES and _frame <= WARMUP_FRAMES + SAMPLE_FRAMES:
		var now := Time.get_ticks_usec()
		_frame_times_usec.push_back(now - _last_frame_usec)
		_last_frame_usec = now
		_process_time_sum += Performance.get_monitor(Performance.TIME_PROCESS)
		_physics_time_sum += Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS)
		var drawn_objects := Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME)
		_object_sum += drawn_objects
		_minimum_objects = minf(_minimum_objects, drawn_objects)
		_maximum_objects = maxf(_maximum_objects, drawn_objects)
		_draw_call_sum += Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)

	if _frame == WARMUP_FRAMES + SAMPLE_FRAMES:
		_finish_benchmark()
	return false


func _try_prepare_world() -> void:
	var current := current_scene as Node3D
	if current == null or not current.is_in_group("network_world"):
		return
	# The menu/settings autoload applies the saved player preset while the world
	# loads. Reassert the complete controlled Medium profile, not only its backend,
	# so a benchmark cannot silently inherit Ultra cell sizes or saved decay.
	_apply_medium_settings()
	_apply_variant_settings()
	_world = current
	for node in _world.find_children("*", "VoxelVolume3D", true, false):
		_volumes.push_back(node as VoxelVolume3D)
	var minimum := Vector3(INF, INF, INF)
	var maximum := Vector3(-INF, -INF, -INF)
	for volume in _volumes:
		var volume_aabb := volume.get_aabb()
		var center := volume.global_transform * volume_aabb.get_center()
		minimum = minimum.min(center)
		maximum = maximum.max(center)
	_scene_center = (minimum + maximum) * 0.5
	_scene_radius = maximum.distance_to(minimum) * 0.5
	for node in _world.find_children("*", "Camera3D", true, false):
		(node as Camera3D).current = false
	# Place the benchmark camera outside the complete volume-center bounds. The
	# former PlayerMarker view sat inside the level and culled most of the stated
	# 800-object workload, making its name and acceptance gate misleading.
	_anchor_position = _scene_center + Vector3(0.0, 0.0, -maxf(_scene_radius * 2.25, 64.0))
	_camera = Camera3D.new()
	_camera.name = "Voxel800BenchmarkCamera"
	_camera.fov = 75.0
	_camera.near = 0.05
	_camera.far = 4096.0
	_world.add_child(_camera)
	_camera.global_position = _anchor_position
	_camera.look_at(_scene_center, Vector3.UP)
	_camera.current = true
	if _variant == "spot_validation":
		_validation_spot = SpotLight3D.new()
		_validation_spot.name = "Voxel800ValidationSpot"
		_validation_spot.light_energy = 4.0
		_validation_spot.spot_range = 128.0
		_validation_spot.spot_angle = 55.0
		_validation_spot.shadow_enabled = true
		_world.add_child(_validation_spot)
		_validation_spot.global_position = _anchor_position + Vector3(0.0, 8.0, 0.0)
		_validation_spot.look_at(_scene_center, Vector3.UP)
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	print("VOXEL_800_SETUP mode=%s variant=%s indirect_backend=%d resolution=%dx%d volumes=%d configured_loads_per_frame=%d camera_anchor=%s scene_center=%s" % [
		_mode,
		_variant,
		int(ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/backend", -1)),
		DisplayServer.window_get_size().x,
		DisplayServer.window_get_size().y,
		_volumes.size(),
		int(ProjectSettings.get_setting("rendering/voxel_volume/max_loads_per_frame", -1)),
		_anchor_position,
		_scene_center,
	])


func _update_camera() -> void:
	if _camera == null:
		return
	if _mode == "stationary":
		_camera.global_position = _anchor_position
		_camera.look_at(_scene_center, Vector3.UP)
		return
	var phase := float(_frame) * 0.0125
	var offset := Vector3(sin(phase) * 2.5, sin(phase * 0.47) * 0.45, cos(phase) * 2.5)
	_camera.global_position = _anchor_position + offset
	var target_offset := Vector3(sin(phase * 0.63) * 8.0, sin(phase * 0.37) * 3.0, cos(phase * 0.41) * 8.0)
	_camera.look_at(_scene_center + target_offset, Vector3.UP)


func _update_validation_spot() -> void:
	if _validation_spot == null:
		return
	var phase := float(_frame) * 0.01
	_validation_spot.global_position = _anchor_position + Vector3(sin(phase) * 3.0, 8.0, cos(phase) * 3.0)
	_validation_spot.look_at(_scene_center, Vector3.UP)
	if _frame == WARMUP_FRAMES + 180:
		_validation_spot.shadow_enabled = false
		print("VOXEL_800_SPOT shadow_enabled=false")
	elif _frame == WARMUP_FRAMES + 300:
		_validation_spot.shadow_enabled = true
		print("VOXEL_800_SPOT shadow_enabled=true")


func _print_ready_state() -> void:
	var resident := 0
	var visible_centers := 0
	for volume in _volumes:
		if not volume.is_streaming_resident():
			continue
		resident += 1
		var center := volume.global_transform * volume.get_aabb().get_center()
		if _camera.is_position_in_frustum(center):
			visible_centers += 1
	_resident_volumes_at_ready = resident
	_visible_volume_centers_at_ready = visible_centers
	print("VOXEL_800_READY mode=%s variant=%s resident=%d visible_centers=%d drawn_objects=%.0f draw_calls=%.0f" % [
		_mode,
		_variant,
		resident,
		visible_centers,
		Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME),
		Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME),
	])


func _finish_benchmark() -> void:
	var elapsed_usec := Time.get_ticks_usec() - _sample_start_usec
	var sorted_times := _frame_times_usec.duplicate()
	sorted_times.sort()
	var average_fps := float(SAMPLE_FRAMES) * 1_000_000.0 / float(elapsed_usec)
	var average_ms := float(elapsed_usec) / float(SAMPLE_FRAMES) / 1000.0
	var median_ms := float(sorted_times[sorted_times.size() / 2]) / 1000.0
	var p95_ms := float(sorted_times[int(float(sorted_times.size() - 1) * 0.95)]) / 1000.0
	var p99_ms := float(sorted_times[int(float(sorted_times.size() - 1) * 0.99)]) / 1000.0
	var average_objects := _object_sum / SAMPLE_FRAMES
	root.get_texture().get_image().save_png("C:/tmp/voxel_800_%s_%s.png" % [_variant, _mode])
	print("VOXEL_800_RESULT mode=%s variant=%s average_fps=%.2f average_ms=%.3f median_ms=%.3f p95_ms=%.3f p99_ms=%.3f process_ms=%.3f physics_ms=%.3f average_objects=%.1f min_objects=%.0f max_objects=%.0f average_draw_calls=%.1f" % [
		_mode,
		_variant,
		average_fps,
		average_ms,
		median_ms,
		p95_ms,
		p99_ms,
		_process_time_sum / SAMPLE_FRAMES * 1000.0,
		_physics_time_sum / SAMPLE_FRAMES * 1000.0,
		average_objects,
		_minimum_objects,
		_maximum_objects,
		_draw_call_sum / SAMPLE_FRAMES,
	])
	# Object draw counts are renderer implementation details and changed when the
	# voxel batches were consolidated. The workload contract is the actual one:
	# all 515 gameplay volumes must be resident and their centers visible.
	var object_target_met := _resident_volumes_at_ready >= REQUIRED_RESIDENT_VOLUMES and _visible_volume_centers_at_ready >= REQUIRED_RESIDENT_VOLUMES
	var performance_target_met := average_fps >= 120.0 and p95_ms <= 10.0
	quit(0 if object_target_met and performance_target_met else 1)
