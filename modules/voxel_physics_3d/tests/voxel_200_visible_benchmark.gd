extends SceneTree

const MIN_VISIBLE_VOLUMES := 200
const TARGET_VISIBLE_VOLUMES := 220
const CAMERA_CALIBRATION_START_FRAME := 120
const CAMERA_CALIBRATION_FRAMES := 24
const SETTLE_FRAMES := 360
const SAMPLE_FRAMES := 600

var _scene_root: Node
var _camera: Camera3D
var _volumes: Array[VoxelVolume3D] = []
var _frame := 0
var _sample_start_usec := 0
var _frame_times_usec: Array[int] = []
var _last_frame_usec := 0
var _draw_call_sum := 0.0
var _object_sum := 0.0
var _variant := "current"
var _scene_center := Vector3.ZERO
var _view_direction := Vector3.ZERO
var _minimum_view_distance := 0.0
var _maximum_view_distance := 0.0


func _initialize() -> void:
	Engine.max_fps = 0
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	ProjectSettings.set_setting("rendering/voxel_volume/max_loads_per_frame", 64)
	_variant = OS.get_environment("VOXEL_BENCH_VARIANT")
	if _variant.is_empty():
		_variant = "current"
	if _variant == "no_shadow":
		ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", false)
	elif _variant == "no_indirect":
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", false)
	elif _variant == "no_reflections":
		ProjectSettings.set_setting("rendering/voxel_forward/reflections/enabled", false)
	elif _variant == "exact_hit_position":
		ProjectSettings.set_setting("rendering/voxel_forward/architectural_hit_buffer/exact_position_enabled", true)
	elif _variant == "no_outline":
		ProjectSettings.set_setting("rendering/voxel_forward/outline/enabled", false)
	elif _variant == "base_only":
		ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", false)
		ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", false)
		ProjectSettings.set_setting("rendering/voxel_forward/reflections/enabled", false)
		ProjectSettings.set_setting("rendering/voxel_forward/outline/enabled", false)
	if _variant == "no_ao":
		var benchmark_material := load("res://voxel_materials/shared_voxel_material.tres") as VoxelMaterial
		if benchmark_material != null:
			benchmark_material.ambient_occlusion_enabled = false

	var packed_scene := load("res://node_3d.tscn") as PackedScene
	if packed_scene == null:
		push_error("BENCHMARK_ERROR: could not load res://node_3d.tscn")
		quit(2)
		return

	_scene_root = packed_scene.instantiate()
	root.add_child(_scene_root)
	call_deferred("_prepare_benchmark_view")


func _prepare_benchmark_view() -> void:
	for node in _scene_root.find_children("*", "VoxelVolume3D", true, false):
		_volumes.push_back(node as VoxelVolume3D)
	if _volumes.size() < MIN_VISIBLE_VOLUMES:
		push_error("BENCHMARK_ERROR: scene has only %d voxel volumes" % _volumes.size())
		quit(3)
		return

	var minimum := Vector3(INF, INF, INF)
	var maximum := Vector3(-INF, -INF, -INF)
	for volume in _volumes:
		var center := volume.global_transform * volume.get_aabb().get_center()
		minimum = minimum.min(center)
		maximum = maximum.max(center)

	_scene_center = (minimum + maximum) * 0.5
	var scene_extent := maximum - minimum
	var view_distance := maxf(scene_extent.x, maxf(scene_extent.y, scene_extent.z)) * 1.35
	_minimum_view_distance = view_distance * 0.04
	_maximum_view_distance = view_distance
	_view_direction = Vector3(1.0, 0.45, 1.0).normalized()
	_camera = Camera3D.new()
	_camera.name = "VoxelBenchmarkCamera"
	_camera.fov = 85.0
	_camera.near = 0.05
	_camera.far = maxf(4096.0, view_distance * 4.0)
	_scene_root.add_child(_camera)
	_set_camera_distance(view_distance)
	_camera.current = true

	print("BENCHMARK_SETUP variant=%s total_volumes=%d resolution=%dx%d" % [
		_variant,
		_volumes.size(),
		DisplayServer.window_get_size().x,
		DisplayServer.window_get_size().y,
	])


func _process(_delta: float) -> bool:
	if _camera == null:
		return false

	_frame += 1
	if _frame > CAMERA_CALIBRATION_START_FRAME and _frame <= CAMERA_CALIBRATION_START_FRAME + CAMERA_CALIBRATION_FRAMES:
		var test_distance := (_minimum_view_distance + _maximum_view_distance) * 0.5
		_set_camera_distance(test_distance)
		var visible_at_distance := _count_visible_resident_centers()
		if visible_at_distance >= TARGET_VISIBLE_VOLUMES:
			_maximum_view_distance = test_distance
		else:
			_minimum_view_distance = test_distance
		if _frame == CAMERA_CALIBRATION_START_FRAME + CAMERA_CALIBRATION_FRAMES:
			_set_camera_distance(_maximum_view_distance)
		return false

	if _frame == SETTLE_FRAMES:
		var resident := 0
		for volume in _volumes:
			if volume.is_streaming_resident():
				resident += 1
		var visible := _count_visible_resident_centers()
		print("BENCHMARK_READY resident=%d visible_centers=%d camera_distance=%.3f" % [resident, visible, _maximum_view_distance])
		if visible < MIN_VISIBLE_VOLUMES:
			push_error("BENCHMARK_ERROR: only %d resident volume centers are visible" % visible)
			quit(4)
			return false
		_sample_start_usec = Time.get_ticks_usec()
		_last_frame_usec = _sample_start_usec
		return false

	if _frame > SETTLE_FRAMES and _frame <= SETTLE_FRAMES + SAMPLE_FRAMES:
		var now := Time.get_ticks_usec()
		_frame_times_usec.push_back(now - _last_frame_usec)
		_last_frame_usec = now
		_draw_call_sum += Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)
		_object_sum += Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME)

	if _frame == SETTLE_FRAMES + SAMPLE_FRAMES:
		_finish_benchmark()
	return false


func _set_camera_distance(distance: float) -> void:
	_camera.global_position = _scene_center + _view_direction * distance
	_camera.look_at(_scene_center, Vector3.UP)


func _count_visible_resident_centers() -> int:
	var visible := 0
	for volume in _volumes:
		if not volume.is_streaming_resident():
			continue
		var center := volume.global_transform * volume.get_aabb().get_center()
		if _camera.is_position_in_frustum(center):
			visible += 1
	return visible


func _finish_benchmark() -> void:
	var elapsed_usec := Time.get_ticks_usec() - _sample_start_usec
	var sorted_times := _frame_times_usec.duplicate()
	sorted_times.sort()
	var average_fps := float(SAMPLE_FRAMES) * 1_000_000.0 / float(elapsed_usec)
	var average_ms := float(elapsed_usec) / float(SAMPLE_FRAMES) / 1000.0
	var median_ms := float(sorted_times[sorted_times.size() / 2]) / 1000.0
	var p95_ms := float(sorted_times[int(float(sorted_times.size() - 1) * 0.95)]) / 1000.0
	print("BENCHMARK_RESULT average_fps=%.2f average_ms=%.3f median_ms=%.3f p95_ms=%.3f average_draw_calls=%.1f average_objects=%.1f" % [
		average_fps,
		average_ms,
		median_ms,
		p95_ms,
		_draw_call_sum / SAMPLE_FRAMES,
		_object_sum / SAMPLE_FRAMES,
	])
	quit(0 if average_fps > 80.0 else 1)
