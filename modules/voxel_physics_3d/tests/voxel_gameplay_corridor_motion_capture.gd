extends SceneTree

const MAP_SCENE_PATH := "res://node_3d.tscn"
const CAMERA_START := Vector3(23.9630928, 28.0571594, -30.0)
const CAMERA_ROTATION := Vector3(-4.3623, 179.2699, 0.0)
const DEFAULT_WARMUP_FRAMES := 640
const SAMPLE_FRAMES := 256
# Cross the first LOD-0 recenter threshold at about frame 213, then keep
# recording long enough to catch any reset/copy discontinuity after it.
const TRAVEL_DISTANCE := 3.8
const PRIMARY_SAMPLE_UVS: Array[Vector2] = [
	# Stable interior patches in the representative corridor view. These avoid
	# the sky opening, directly lit foreground floor, and silhouette boundaries.
	Vector2(0.39, 0.50), # Left wall.
	Vector2(0.50, 0.82), # Interior floor.
	Vector2(0.61, 0.50), # Right wall.
	Vector2(0.46, 0.14), # Left ceiling.
	Vector2(0.54, 0.14), # Right ceiling.
]
const DYNAMIC_SAMPLE_UVS: Array[Vector2] = [
	# Observe surfaces away from the placed voxel so geometry replacement is not
	# mistaken for a lighting flash.
	Vector2(0.39, 0.50),
	Vector2(0.61, 0.50),
	Vector2(0.46, 0.14),
	Vector2(0.54, 0.14),
]

var _variant := "final"
var _output_dir := "C:/tmp/voxel_gameplay_corridor"
var _travel_distance := TRAVEL_DISTANCE
var _motion_pattern := "forward"
var _sample_frames := SAMPLE_FRAMES
var _warmup_frames := DEFAULT_WARMUP_FRAMES
var _voxel_gi_debug_override := -1
var _indirect_intensity_override := -1.0
var _diagnostic_suite := false
var _dynamic_edit_test := false
var _suite_actions: Array[Dictionary] = []
var _suite_index := 0
var _suite_wait_remaining := 0
var _frame := 0
var _map: Node3D
var _camera: Camera3D
var _sample_points: Array[Vector3] = []
var _sample_uvs: Array[Vector2] = []
var _rows: PackedStringArray = []
var _previous_origins: Array[Vector3] = []
var _diagnostic_center_cells: Array[Vector3i] = []
var _origin_events := 0
var _dynamic_stage := 0
var _dynamic_stage_frame := 0
var _dynamic_volume: VoxelVolume3D
var _dynamic_cell := Vector3i()
var _dynamic_original_value := 0
var _dynamic_before: Array[float] = []
var _dynamic_previous: Array[float] = []
var _dynamic_max_frame_delta := 0.0
var _dynamic_max_overshoot := 0.0
var _dynamic_max_undershoot := 0.0


func _initialize() -> void:
	Engine.max_fps = 0
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	DisplayServer.window_set_size(Vector2i(1280, 720))
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--variant="):
			_variant = argument.trim_prefix("--variant=")
		elif argument.begins_with("--output-dir="):
			_output_dir = argument.trim_prefix("--output-dir=")
		elif argument.begins_with("--travel="):
			_travel_distance = argument.trim_prefix("--travel=").to_float()
		elif argument.begins_with("--motion="):
			_motion_pattern = argument.trim_prefix("--motion=")
		elif argument.begins_with("--sample-frames="):
			_sample_frames = maxi(argument.trim_prefix("--sample-frames=").to_int(), 2)
		elif argument.begins_with("--warmup-frames="):
			_warmup_frames = maxi(argument.trim_prefix("--warmup-frames=").to_int(), 1)
		elif argument.begins_with("--voxel-gi-debug="):
			_voxel_gi_debug_override = argument.trim_prefix("--voxel-gi-debug=").to_int()
		elif argument.begins_with("--indirect-intensity="):
			_indirect_intensity_override = maxf(argument.trim_prefix("--indirect-intensity=").to_float(), 0.0)
		elif argument == "--diagnostic-suite":
			_diagnostic_suite = true
		elif argument == "--dynamic-edit-test":
			_dynamic_edit_test = true
	_sample_uvs.assign(PRIMARY_SAMPLE_UVS)
	for y in [0.15, 0.30, 0.45, 0.60, 0.75, 0.90]:
		for x in [0.10, 0.20, 0.30, 0.40, 0.60, 0.70, 0.80, 0.90]:
			_sample_uvs.append(Vector2(x, y))
	_configure_variant()
	# Keep the caller-selected backend for the dynamic edit path. Forcing the
	# legacy backend here made the gameplay edit test incapable of testing DDGI.
	if _diagnostic_suite:
		_variant = "voxel_gi_gameplay_fixed"
		_configure_variant()
	if _diagnostic_suite:
		_build_diagnostic_suite()
	if _voxel_gi_debug_override >= 0:
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/voxel_gi/debug_mode", _voxel_gi_debug_override)
	if _indirect_intensity_override >= 0.0:
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/intensity", _indirect_intensity_override)
	DirAccess.make_dir_recursive_absolute(_output_dir)
	# Load after SceneTree initialization so project autoload names referenced by
	# the real player scripts are registered before those scripts compile.
	var map_scene := load(MAP_SCENE_PATH) as PackedScene
	if map_scene == null:
		push_error("Unable to load the gameplay map: %s" % MAP_SCENE_PATH)
		quit(6)
		return
	_map = map_scene.instantiate()
	root.add_child(_map)
	call_deferred("_prepare_gameplay_camera")


func _prepare_gameplay_camera() -> void:
	# The real GraphicsSettingsPanel loads user://graphics_settings.cfg from its
	# _ready() and therefore runs after this SceneTree configured the test. Reapply
	# the controlled variant now that every gameplay node has entered the tree so
	# captures cannot silently test the user's saved profile instead.
	_configure_variant()
	if _voxel_gi_debug_override >= 0:
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/voxel_gi/debug_mode", _voxel_gi_debug_override)
	if _indirect_intensity_override >= 0.0:
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/intensity", _indirect_intensity_override)
	var map_cameras := _map.find_children("*", "Camera3D", true, false)
	if map_cameras.is_empty():
		push_error("The gameplay map has no player Camera3D")
		quit(5)
		return
	for existing_camera in map_cameras:
		(existing_camera as Camera3D).current = false
	# node_3d.tscn contains the real character_body_3d.tscn player instance.
	# Reuse its camera settings, but detach it from the live player/helicopter
	# hierarchy so parent motion cannot contaminate paired camera positions.
	_camera = map_cameras[0] as Camera3D
	_camera.reparent(_map, true)
	_camera.fov = 82.0
	_camera.global_position = CAMERA_START
	_camera.global_rotation_degrees = CAMERA_ROTATION
	_camera.current = true
	var header := PackedStringArray(["frame", "camera_x", "camera_y", "camera_z", "camera_pitch", "camera_yaw", "camera_roll", "origin0_x", "origin0_y", "origin0_z", "origin1_x", "origin1_y", "origin1_z", "origin2_x", "origin2_y", "origin2_z"])
	for sample_index in _sample_uvs.size():
		header.append("s%d" % sample_index)
	for sample_index in _sample_uvs.size():
		header.append("u%d" % sample_index)
	_rows.append(",".join(header))
	_previous_origins.resize(3)
	_diagnostic_center_cells.resize(3)
	var voxel_gi_cell_sizes := [0.2, 0.8, 3.2]
	for index in 3:
		_previous_origins[index] = Vector3(INF, INF, INF)
		var cell_size: float = voxel_gi_cell_sizes[index]
		var snap := cell_size * 4.0
		var center := (_camera.global_position / snap).round() * snap
		_diagnostic_center_cells[index] = Vector3i((center / cell_size).round())


func _configure_variant() -> void:
	ProjectSettings.set_setting("rendering/voxel_volume/max_resident_volumes", 1024)
	ProjectSettings.set_setting("rendering/voxel_volume/max_loads_per_frame", 64)
	ProjectSettings.set_setting("rendering/voxel_volume/streaming_distance", 1536.0)
	ProjectSettings.set_setting("rendering/voxel_forward/reflections/enabled", false)
	ProjectSettings.set_setting("game/graphics/atmosphere/cloud_quality", 0)
	ProjectSettings.set_setting("game/graphics/atmosphere/god_ray_quality", 0)
	ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/resolution_scale", 0.5)
	ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/max_distance", 96.0)
	ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/max_steps", 256)
	ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/atlas_resolution", 512)
	ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/max_local_lights", 2)
	ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/soft_shadow_mode", 1)
	ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/soft_shadow_samples", 1)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/backend", 2)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/intensity", 1.0)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/resolution", 32)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/near_cell_size", 0.4)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/far_cell_size", 1.6)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/distant_cell_size", 6.4)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/recenter_hysteresis", 0.75)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/propagation_steps", 4)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/propagation_decay", 0.78)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/dirty_cell_pass_budget_per_frame", 524288)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/temporal_blend_frames", 2)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/voxel_gi/debug_mode", 0)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/spatial_cascade_resolution", 16)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/rays_per_probe", 16)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/probes_per_frame", 32)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/irradiance_hysteresis", 0.98)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/visibility_hysteresis", 0.985)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/self_shadow_bias", 0.3)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 0)
	ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/incremental_atlas_updates", false)
	match _variant:
		"voxel_gi_project_irradiance":
			# Reproduce the project's current large-coverage Voxel GI profile. This
			# specifically guards against accidentally expressing the surface bias in
			# GI-cell units (6.0 m here) instead of voxel units.
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/backend", 1)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/resolution", 64)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/near_cell_size", 6.0)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/far_cell_size", 12.0)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/distant_cell_size", 16.0)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/propagation_steps", 10)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/propagation_decay", 0.05)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/voxel_gi/debug_mode", 11)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
		"voxel_gi":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/backend", 1)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
		"voxel_gi_gameplay_off", "voxel_gi_gameplay_saved", "voxel_gi_gameplay_fixed", "voxel_gi_gameplay_fixed_raw":
			# The real gameplay map and corridor camera with the current saved Ultra
			# profile. Paired variants change only Voxel GI retention/debug state.
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", _variant != "voxel_gi_gameplay_off")
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/backend", 0 if _variant == "voxel_gi_gameplay_off" else 1)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/resolution", 64)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/near_cell_size", 0.2)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/far_cell_size", 0.8)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/distant_cell_size", 3.2)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/propagation_steps", 8)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/propagation_decay", 0.1 if _variant == "voxel_gi_gameplay_saved" else 0.85)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/voxel_gi/debug_mode", 11 if _variant == "voxel_gi_gameplay_fixed_raw" else 0)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
		"direct_no_voxel_shadow":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 15)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", false)
		"direct_shadow":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 15)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
		"ddgi":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 11)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
		"fallback":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 12)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
		"coverage":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 13)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
		"coverage_high_hysteresis":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 13)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/irradiance_hysteresis", 0.99)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
		"lod":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 14)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
		"confidence":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 16)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
		"in_front":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 17)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
		"depth_visibility":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 18)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
		"final":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
		"final_clouds":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
			ProjectSettings.set_setting("game/graphics/atmosphere/cloud_quality", 2)
			ProjectSettings.set_setting("game/graphics/atmosphere/god_ray_quality", 2)
		"final_clouds_high_hysteresis":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/irradiance_hysteresis", 0.99)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/visibility_hysteresis", 0.99)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
			ProjectSettings.set_setting("game/graphics/atmosphere/cloud_quality", 2)
			ProjectSettings.set_setting("game/graphics/atmosphere/god_ray_quality", 2)
		"direct_shadow_clouds":
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
			ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 15)
			ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
			ProjectSettings.set_setting("game/graphics/atmosphere/cloud_quality", 2)
			ProjectSettings.set_setting("game/graphics/atmosphere/god_ray_quality", 2)
		_:
			push_error("Unknown corridor diagnostic variant: %s" % _variant)
			quit(2)


func _process(_delta: float) -> bool:
	if _camera == null:
		return false
	_frame += 1
	if _diagnostic_suite:
		_process_diagnostic_suite()
		return false
	if _dynamic_edit_test:
		_process_dynamic_edit_test()
		return false
	if _frame == _warmup_frames:
		_capture_world_samples()
	if _frame < _warmup_frames or _sample_points.size() != _sample_uvs.size():
		return false
	var sample_frame := _frame - _warmup_frames
	if sample_frame >= _sample_frames:
		_finish()
		return false
	var progress := float(sample_frame) / float(_sample_frames - 1)
	if _motion_pattern == "pingpong":
		progress = 1.0 - abs(progress * 2.0 - 1.0)
	_camera.global_position = CAMERA_START + Vector3(0.0, 0.0, _travel_distance * progress)
	_camera.global_rotation_degrees = CAMERA_ROTATION
	_record_frame(sample_frame)
	return false


func _build_diagnostic_suite() -> void:
	_suite_actions = [
		{"name": "final_decay_085", "debug": 0, "wait": 0},
		{"name": "lobe_pos_x", "debug": 5, "wait": 2},
		{"name": "lobe_neg_x", "debug": 6, "wait": 2},
		{"name": "lobe_pos_y", "debug": 7, "wait": 2},
		{"name": "lobe_neg_y", "debug": 8, "wait": 2},
		{"name": "lobe_pos_z", "debug": 9, "wait": 2},
		{"name": "lobe_neg_z", "debug": 10, "wait": 2},
		{"name": "raw_decay_085", "debug": 11, "wait": 2},
		{"name": "gather_visibility", "debug": 14, "wait": 2},
		{"name": "fallback_weight", "debug": 16, "wait": 2},
		{"name": "shaded_decay_085", "debug": 15, "wait": 2},
		{"name": "voxel_gi_off", "backend": 0, "debug": 0, "wait": 2},
		{"name": "raw_decay_055", "backend": 1, "decay": 0.55, "debug": 11, "wait": 60},
		{"name": "shaded_decay_055", "debug": 15, "wait": 2},
		{"name": "raw_decay_010", "decay": 0.10, "debug": 11, "wait": 60},
		{"name": "shaded_decay_010", "debug": 15, "wait": 2},
		{"name": "raw_decay_085_restored", "decay": 0.85, "debug": 11, "wait": 60},
		{"name": "shaded_decay_085_restored", "debug": 15, "wait": 2},
	]


func _process_dynamic_edit_test() -> void:
	if _frame < _warmup_frames:
		return
	if _dynamic_volume == null:
		if not _begin_dynamic_edit_test():
			quit(7)
		return
	var values := _capture_primary_luminance()
	for index in values.size():
		_dynamic_max_frame_delta = maxf(_dynamic_max_frame_delta, absf(values[index] - _dynamic_previous[index]))
		_dynamic_max_overshoot = maxf(_dynamic_max_overshoot, values[index] - _dynamic_before[index])
		_dynamic_max_undershoot = maxf(_dynamic_max_undershoot, _dynamic_before[index] - values[index])
	_dynamic_previous = values
	_dynamic_stage_frame += 1
	if _dynamic_stage_frame < 120:
		return
	if _dynamic_stage == 0:
		root.get_texture().get_image().save_png("%s/dynamic_placed.png" % _output_dir)
		var removed := _dynamic_volume.apply_voxel_edits([_dynamic_cell], PackedByteArray([0]))
		if removed != 1:
			push_error("Gameplay dynamic GI test could not remove placed voxel")
			quit(8)
			return
		_dynamic_stage = 1
		_dynamic_stage_frame = 0
		return
	root.get_texture().get_image().save_png("%s/dynamic_removed.png" % _output_dir)
	var restore_error := 0.0
	for index in values.size():
		restore_error = maxf(restore_error, absf(values[index] - _dynamic_before[index]))
	print("CORRIDOR_DYNAMIC_EDIT_OK cell=%s value=%d max_frame_delta=%.6f max_overshoot=%.6f max_undershoot=%.6f restore_error=%.6f output=%s" % [
		_dynamic_cell, _dynamic_original_value, _dynamic_max_frame_delta, _dynamic_max_overshoot,
		_dynamic_max_undershoot, restore_error, _output_dir,
	])
	quit(0 if _dynamic_max_frame_delta <= 0.10 and _dynamic_max_overshoot <= 0.10 and restore_error <= 0.03 else 9)


func _begin_dynamic_edit_test() -> bool:
	# Place into the air immediately above a real floor hit in the enclosure.
	# This exercises both placement and removal without punching a skylight into
	# the test room, which would be a legitimate large lighting change.
	var screen := Vector2(root.size) * Vector2(0.50, 0.78)
	var origin := _camera.project_ray_origin(screen)
	var direction := _camera.project_ray_normal(screen)
	var hit := _camera.get_world_3d().direct_space_state.intersect_ray(PhysicsRayQueryParameters3D.create(origin, origin + direction * 256.0))
	if hit.is_empty():
		push_error("Gameplay dynamic GI test found no target surface")
		return false
	var collider := hit.get("collider") as Node
	while collider != null and not collider is VoxelVolume3D:
		collider = collider.get_parent()
	var candidates: Array[Node] = []
	if collider is VoxelVolume3D:
		candidates.append(collider)
	else:
		# Voxel collision bodies can be batched outside their source volume's node
		# hierarchy. Resolve those hits from the world-space point instead.
		candidates.assign(_map.find_children("*", "VoxelVolume3D", true, false))
	for candidate in candidates:
		var volume := candidate as VoxelVolume3D
		if volume == null or volume.voxel_data == null:
			continue
		var voxel_size: float = volume.voxel_data.voxel_size
		var local_normal: Vector3 = (volume.global_transform.basis.inverse() * (hit.normal as Vector3)).normalized()
		var inside_position: Vector3 = volume.to_local(hit.position) - local_normal * voxel_size * 0.25
		var outside_position: Vector3 = volume.to_local(hit.position) + local_normal * voxel_size * 0.25
		var source_cell := Vector3i(floor(inside_position / voxel_size))
		var cell := Vector3i(floor(outside_position / voxel_size))
		var value := volume.get_voxel(source_cell)
		if value == 0:
			continue
		if volume.get_voxel(cell) != 0:
			continue
		_dynamic_volume = volume
		_dynamic_cell = cell
		_dynamic_original_value = value
		break
	if _dynamic_volume == null:
		push_error("Gameplay dynamic GI target did not resolve to an occupied VoxelVolume3D cell")
		return false
	_dynamic_volume.make_voxel_data_unique()
	if _dynamic_original_value == 0:
		push_error("Gameplay dynamic GI target resolved to an empty voxel at %s" % _dynamic_cell)
		return false
	_dynamic_before = _capture_primary_luminance()
	_dynamic_previous = _dynamic_before.duplicate()
	root.get_texture().get_image().save_png("%s/dynamic_before.png" % _output_dir)
	var placed := _dynamic_volume.apply_voxel_edits([_dynamic_cell], PackedByteArray([_dynamic_original_value]))
	if placed != 1:
		push_error("Gameplay dynamic GI test could not place target voxel")
		return false
	return true


func _capture_primary_luminance() -> Array[float]:
	var image := root.get_texture().get_image()
	var values: Array[float] = []
	var viewport_size := Vector2(image.get_size())
	for uv in DYNAMIC_SAMPLE_UVS:
		values.append(_sample_image_luminance(image, uv * viewport_size))
	return values


func _process_diagnostic_suite() -> void:
	if _frame < _warmup_frames:
		return
	if _sample_points.is_empty():
		_capture_world_samples()
		_apply_suite_action()
		return
	if _suite_wait_remaining > 0:
		_suite_wait_remaining -= 1
		return
	_capture_suite_action()
	_suite_index += 1
	if _suite_index >= _suite_actions.size():
		print("CORRIDOR_DIAGNOSTIC_SUITE_OK captures=%d output=%s" % [_suite_actions.size(), _output_dir])
		quit(0)
		return
	_apply_suite_action()


func _apply_suite_action() -> void:
	var action := _suite_actions[_suite_index]
	if action.has("backend"):
		var backend := int(action.backend)
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", backend != 0)
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/backend", backend)
	if action.has("decay"):
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/propagation_decay", float(action.decay))
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/voxel_gi/debug_mode", int(action.debug))
	_suite_wait_remaining = int(action.wait)


func _capture_suite_action() -> void:
	var action := _suite_actions[_suite_index]
	var image := root.get_texture().get_image()
	if image == null:
		push_error("Viewport image unavailable during diagnostic suite")
		quit(4)
		return
	var values: Array[float] = []
	var viewport_size := Vector2(image.get_size())
	for uv in PRIMARY_SAMPLE_UVS:
		values.append(_sample_image_luminance(image, uv * viewport_size))
	var output_path := "%s/%02d_%s.png" % [_output_dir, _suite_index, action.name]
	image.save_png(output_path)
	print("CORRIDOR_DIAGNOSTIC name=%s backend=%s decay=%s debug=%s left_wall=%.6f floor=%.6f right_wall=%.6f left_ceiling=%.6f right_ceiling=%.6f output=%s" % [
		action.name,
		ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/backend"),
		ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/propagation_decay"),
		ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/voxel_gi/debug_mode"),
		values[0], values[1], values[2], values[3], values[4], output_path,
	])


func _capture_world_samples() -> void:
	var space := _camera.get_world_3d().direct_space_state
	var viewport_size := Vector2(root.size)
	for uv in _sample_uvs:
		var screen := uv * viewport_size
		var origin := _camera.project_ray_origin(screen)
		var direction := _camera.project_ray_normal(screen)
		var query := PhysicsRayQueryParameters3D.create(origin, origin + direction * 256.0)
		var hit := space.intersect_ray(query)
		if hit.is_empty():
			push_error("No corridor surface at diagnostic UV %s" % uv)
			quit(3)
			return
		_sample_points.append(hit.position as Vector3)
	var representative := _sample_points.slice(0, mini(4, _sample_points.size()))
	print("CORRIDOR_SETTINGS variant=%s enabled=%s backend=%s resolution=%s cells=%s/%s/%s steps=%s decay=%s intensity=%s debug=%s" % [
		_variant,
		ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/enabled"),
		ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/backend"),
		ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/resolution"),
		ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/near_cell_size"),
		ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/far_cell_size"),
		ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/distant_cell_size"),
		ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/propagation_steps"),
		ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/propagation_decay"),
		ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/intensity"),
		ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/voxel_gi/debug_mode"),
	])
	print("CORRIDOR_WORLD_SAMPLES count=%d representative=%s" % [_sample_points.size(), representative])


func _record_frame(sample_frame: int) -> void:
	var image := root.get_texture().get_image()
	if image == null:
		push_error("Viewport image unavailable")
		quit(4)
		return
	var origins: Array[Vector3] = []
	var voxel_gi_cell_sizes := [0.2, 0.8, 3.2]
	for cascade in 3:
		# Mirror the Voxel GI deadband without querying renderer globals (which is
		# editor-only and stalls rendering). This records actual Voxel GI cascade
		# ownership rather than the unrelated four-level DDGI probe layout.
		var cell_size: float = voxel_gi_cell_sizes[cascade]
		var snap := cell_size * 4.0
		var half_extent := cell_size * 32.0
		var threshold := maxf(snap, half_extent * 0.75)
		var center := Vector3(_diagnostic_center_cells[cascade]) * cell_size
		for axis in 3:
			var offset: float = _camera.global_position[axis] - center[axis]
			if offset > threshold:
				center[axis] += ceilf((offset - threshold) / snap) * snap
			elif offset < -threshold:
				center[axis] -= ceilf((-threshold - offset) / snap) * snap
		_diagnostic_center_cells[cascade] = Vector3i((center / cell_size).round())
		var origin := center - Vector3(half_extent, half_extent, half_extent)
		origins.append(origin)
		if _previous_origins[cascade].x != INF and not origin.is_equal_approx(_previous_origins[cascade]):
			_origin_events += 1
			print("CORRIDOR_ORIGIN_EVENT variant=%s frame=%d cascade=%d old=%s new=%s camera=%s" % [_variant, sample_frame, cascade, _previous_origins[cascade], origin, _camera.global_position])
		_previous_origins[cascade] = origin
	var values: Array[float] = []
	for point in _sample_points:
		if _camera.is_position_behind(point):
			values.append(-1.0)
			continue
		values.append(_sample_image_luminance(image, _camera.unproject_position(point)))
	var fields: PackedStringArray = PackedStringArray([
		str(sample_frame), str(_camera.global_position.x), str(_camera.global_position.y), str(_camera.global_position.z),
		str(_camera.global_rotation_degrees.x), str(_camera.global_rotation_degrees.y), str(_camera.global_rotation_degrees.z),
	])
	for origin in origins:
		fields.append(str(origin.x)); fields.append(str(origin.y)); fields.append(str(origin.z))
	for value in values:
		fields.append(str(value))
	var viewport_size := Vector2(image.get_size())
	for uv in _sample_uvs:
		fields.append(str(_sample_image_luminance(image, uv * viewport_size)))
	_rows.append(",".join(fields))
	if sample_frame in [0, 63, 127, 191, 208, 209, 210, 211, 212, 213, 214, 215, 223, 239, 255] || (_motion_pattern == "pingpong" && (sample_frame % 8 == 0 || (_sample_frames - 1 - sample_frame) % 8 == 0)):
		image.save_png("%s/%s_%03d.png" % [_output_dir, _variant, sample_frame])


func _sample_image_luminance(image: Image, pixel: Vector2) -> float:
	var base := Vector2i(floori(pixel.x), floori(pixel.y))
	if base.x < 0 or base.y < 0 or base.x + 1 >= image.get_width() or base.y + 1 >= image.get_height():
		return -1.0
	var fraction := pixel - Vector2(base)
	var top := lerpf(image.get_pixelv(base).get_luminance(), image.get_pixel(base.x + 1, base.y).get_luminance(), fraction.x)
	var bottom := lerpf(image.get_pixel(base.x, base.y + 1).get_luminance(), image.get_pixel(base.x + 1, base.y + 1).get_luminance(), fraction.x)
	return lerpf(top, bottom, fraction.y)


func _finish() -> void:
	var path := "%s/%s.csv" % [_output_dir, _variant]
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file != null:
		file.store_string("\n".join(_rows))
	print("CORRIDOR_CAPTURE_OK variant=%s samples=%d origin_events=%d csv=%s" % [_variant, _sample_frames, _origin_events, path])
	quit(0)
