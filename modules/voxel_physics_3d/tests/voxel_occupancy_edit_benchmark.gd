extends SceneTree

const WARMUP_FRAMES := 360
const SAMPLE_FRAMES := 360
const LONG_SAMPLE_FRAMES := 3600
const EDIT_INTERVAL_FRAMES := 18
# Captured construction/destruction commits at most five voxels per ordinary
# gameplay action. Keep this benchmark representative; use a separate bulk
# import benchmark for 16^3 world-authoring mutations.
const EDIT_SIZE := Vector3i(5, 1, 1)
const ASYNC_DRAIN_FRAMES := 180

var _frame := 0
var _world: Node3D
var _camera: Camera3D
var _target: VoxelVolume3D
var _positions: Array[Vector3i] = []
var _solid_values := PackedByteArray()
var _empty_values := PackedByteArray()
var _next_edit_solid := true
var _last_frame_usec := 0
var _frame_times_usec: Array[int] = []
var _edit_call_times_usec: Array[int] = []
var _edit_count := 0
var _sample_frames := SAMPLE_FRAMES
var _force_rebuild := false
var _indirect_enabled := true


func _initialize() -> void:
	Engine.max_fps = 0
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	var disable_indirect := "--disable-indirect" in (OS.get_cmdline_args() + OS.get_cmdline_user_args())
	_indirect_enabled = not disable_indirect
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", _indirect_enabled)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/backend", 1)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/voxel_gi/debug_mode", 0)
	var arguments := OS.get_cmdline_args() + OS.get_cmdline_user_args()
	if "--long-run" in arguments:
		_sample_frames = LONG_SAMPLE_FRAMES
	if "--disable-occupancy-consumers" in arguments:
		ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", false)
		ProjectSettings.set_setting("rendering/voxel_forward/reflections/enabled", false)
	_force_rebuild = "--force-rebuild" in arguments
	print("VOXEL_OCCUPANCY_EDIT_MODE sample_frames=%d consumers_enabled=%s indirect_enabled=%s" % [
		_sample_frames,
		not "--disable-occupancy-consumers" in arguments,
		not disable_indirect,
	])
	call_deferred("_start_offline_world")


func _start_offline_world() -> void:
	var session_manager := root.get_node_or_null("SessionManager")
	if session_manager == null:
		push_error("VOXEL_OCCUPANCY_EDIT_ERROR: SessionManager autoload is unavailable")
		quit(2)
		return
	session_manager.call("start_offline", "__voxel_occupancy_edit_benchmark__", false)


func _process(_delta: float) -> bool:
	if _world == null:
		_try_prepare_world()
		return false
	_frame += 1
	if _force_rebuild and _frame == WARMUP_FRAMES + 60:
		ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", false)
		ProjectSettings.set_setting("rendering/voxel_forward/reflections/enabled", false)
		print("VOXEL_OCCUPANCY_EDIT_FORCE_REBUILD consumers_disabled")
	if _force_rebuild and _frame == WARMUP_FRAMES + 90:
		ProjectSettings.set_setting("rendering/voxel_forward/reflections/enabled", true)
		print("VOXEL_OCCUPANCY_EDIT_FORCE_REBUILD consumers_reenabled")
	if _frame == WARMUP_FRAMES:
		_last_frame_usec = Time.get_ticks_usec()
		print("VOXEL_OCCUPANCY_EDIT_READY target=%s dimensions=%s resident=%s" % [
			_target.get_path(),
			_target.voxel_data.dimensions,
			_target.is_streaming_resident(),
		])
		return false
	if _frame > WARMUP_FRAMES and _frame <= WARMUP_FRAMES + _sample_frames:
		var now := Time.get_ticks_usec()
		_frame_times_usec.push_back(now - _last_frame_usec)
		_last_frame_usec = now
		var rebuild_pause := _force_rebuild and _frame >= WARMUP_FRAMES + 60 and _frame < WARMUP_FRAMES + 180
		if not rebuild_pause and (_frame - WARMUP_FRAMES) % EDIT_INTERVAL_FRAMES == 0:
			_apply_edit()
	if _frame == WARMUP_FRAMES + _sample_frames:
		_finish_benchmark()
	if _frame == WARMUP_FRAMES + _sample_frames + ASYNC_DRAIN_FRAMES:
		quit(0)
	return false


func _try_prepare_world() -> void:
	var current := current_scene as Node3D
	if current == null or not current.is_in_group("network_world"):
		return
	_world = current
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", _indirect_enabled)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/backend", 1)
	var minimum := Vector3(INF, INF, INF)
	var maximum := Vector3(-INF, -INF, -INF)
	for node in _world.find_children("*", "VoxelVolume3D", true, false):
		var volume := node as VoxelVolume3D
		if volume == null or volume.voxel_data == null:
			continue
		var center := volume.global_transform * volume.get_aabb().get_center()
		minimum = minimum.min(center)
		maximum = maximum.max(center)
		var dimensions: Vector3i = volume.voxel_data.dimensions
		if (
			_target == null
			and dimensions.x >= EDIT_SIZE.x
			and dimensions.y >= EDIT_SIZE.y
			and dimensions.z >= EDIT_SIZE.z
		):
			_target = volume
	if _target == null:
		push_error("VOXEL_OCCUPANCY_EDIT_ERROR: no suitable voxel volume")
		quit(3)
		return
	for node in _world.find_children("*", "Camera3D", true, false):
		(node as Camera3D).current = false
	_camera = Camera3D.new()
	_camera.name = "VoxelOccupancyEditBenchmarkCamera"
	_camera.fov = 75.0
	_camera.near = 0.05
	_camera.far = 4096.0
	_world.add_child(_camera)
	var scene_center := (minimum + maximum) * 0.5
	_camera.global_position = scene_center + Vector3(0.0, 20.0, 90.0)
	_camera.look_at(scene_center, Vector3.UP)
	_camera.current = true
	_prepare_edit_data()


func _prepare_edit_data() -> void:
	var dimensions: Vector3i = _target.voxel_data.dimensions
	var minimum := Vector3i(
		(dimensions.x - EDIT_SIZE.x) / 2,
		(dimensions.y - EDIT_SIZE.y) / 2,
		(dimensions.z - EDIT_SIZE.z) / 2
	)
	_positions.resize(EDIT_SIZE.x * EDIT_SIZE.y * EDIT_SIZE.z)
	var write_index := 0
	for z in range(EDIT_SIZE.z):
		for y in range(EDIT_SIZE.y):
			for x in range(EDIT_SIZE.x):
				_positions[write_index] = minimum + Vector3i(x, y, z)
				write_index += 1
	_solid_values.resize(_positions.size())
	_solid_values.fill(1)
	_empty_values.resize(_positions.size())
	_empty_values.fill(0)


func _apply_edit() -> void:
	var values := _solid_values if _next_edit_solid else _empty_values
	var started_usec := Time.get_ticks_usec()
	var changed := _target.apply_voxel_edits(_positions, values)
	_edit_call_times_usec.push_back(Time.get_ticks_usec() - started_usec)
	_next_edit_solid = not _next_edit_solid
	_edit_count += 1
	print("VOXEL_OCCUPANCY_EDIT operation=%d changed=%d" % [_edit_count, changed])


func _finish_benchmark() -> void:
	var frame_percentiles := _calculate_percentiles(_frame_times_usec)
	var edit_percentiles := _calculate_percentiles(_edit_call_times_usec)
	print("VOXEL_OCCUPANCY_EDIT_RESULT edits=%d median_ms=%.3f p95_ms=%.3f p99_ms=%.3f max_ms=%.3f edit_call_median_ms=%.3f edit_call_p95_ms=%.3f edit_call_p99_ms=%.3f edit_call_max_ms=%.3f" % [
		_edit_count,
		frame_percentiles.x,
		frame_percentiles.y,
		frame_percentiles.z,
		frame_percentiles.w,
		edit_percentiles.x,
		edit_percentiles.y,
		edit_percentiles.z,
		edit_percentiles.w,
	])


func _calculate_percentiles(samples: Array[int]) -> Vector4:
	var sorted := samples.duplicate()
	sorted.sort()
	var count := sorted.size()
	var median_ms := float(sorted[count / 2]) / 1000.0
	var p95_ms := float(sorted[int(float(count - 1) * 0.95)]) / 1000.0
	var p99_ms := float(sorted[int(float(count - 1) * 0.99)]) / 1000.0
	var maximum_ms := float(sorted[count - 1]) / 1000.0
	return Vector4(median_ms, p95_ms, p99_ms, maximum_ms)
