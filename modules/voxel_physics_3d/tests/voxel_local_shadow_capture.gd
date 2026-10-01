extends SceneTree

const SETTLE_FRAMES := 420

var _frame := 0
var _world: Node3D
var _volumes: Array[VoxelVolume3D] = []


func _initialize() -> void:
	Engine.max_fps = 0
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	if OS.get_environment("VOXEL_LOCAL_CAPTURE_VARIANT") == "no_local":
		ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/max_local_lights", 0)
	call_deferred("_start_offline_world")


func _start_offline_world() -> void:
	var session_manager := root.get_node_or_null("SessionManager")
	if session_manager == null:
		push_error("VOXEL_LOCAL_CAPTURE_ERROR: SessionManager autoload is unavailable")
		quit(2)
		return
	session_manager.call("start_offline", "__voxel_local_shadow_capture__", false)


func _process(_delta: float) -> bool:
	if _world == null:
		_prepare_world()
		return false
	_frame += 1
	if OS.get_environment("VOXEL_LOCAL_CAPTURE_VARIANT") == "destroy" and _frame == 240:
		_apply_runtime_voxel_edit()
	if _frame < SETTLE_FRAMES:
		return false
	var output_path := OS.get_environment("VOXEL_LOCAL_CAPTURE_OUTPUT")
	if output_path.is_empty():
		push_error("VOXEL_LOCAL_CAPTURE_ERROR: VOXEL_LOCAL_CAPTURE_OUTPUT is empty")
		quit(2)
		return false
	var error := root.get_texture().get_image().save_png(output_path)
	print("VOXEL_LOCAL_CAPTURE_RESULT path=%s error=%s" % [output_path, error_string(error)])
	quit(0 if error == OK else 2)
	return false


func _prepare_world() -> void:
	var current := current_scene as Node3D
	if current == null or not current.is_in_group("network_world"):
		return
	_world = current
	for node in _world.find_children("*", "Camera3D", true, false):
		(node as Camera3D).current = false
	for node in _world.find_children("*", "VoxelVolume3D", true, false):
		_volumes.push_back(node as VoxelVolume3D)
	var minimum := Vector3(INF, INF, INF)
	var maximum := Vector3(-INF, -INF, -INF)
	for volume in _volumes:
		var center := volume.global_transform * volume.get_aabb().get_center()
		minimum = minimum.min(center)
		maximum = maximum.max(center)
	var scene_center := (minimum + maximum) * 0.5
	var marker := _world.get_node_or_null("PlayerMarker") as Marker3D
	var camera := Camera3D.new()
	camera.fov = 75.0
	camera.near = 0.05
	camera.far = 4096.0
	_world.add_child(camera)
	camera.global_position = marker.global_position + Vector3(0.0, 1.6, 0.0) if marker != null else _world.global_position
	camera.look_at(scene_center, Vector3.UP)
	camera.current = true


func _apply_runtime_voxel_edit() -> void:
	var world_state := _world.find_child("VoxelWorldState", true, false)
	if world_state == null:
		push_error("VOXEL_LOCAL_CAPTURE_ERROR: VoxelWorldState is unavailable")
		return
	for volume in _volumes:
		if volume.voxel_data == null or not volume.is_streaming_resident():
			continue
		var dimensions := volume.voxel_data.dimensions
		var dense := volume.voxel_data.get_voxel_data()
		for index in dense.size():
			if dense[index] == 0:
				continue
			var x := index % dimensions.x
			var yz := index / dimensions.x
			var cell := Vector3i(x, yz % dimensions.y, yz / dimensions.y)
			var changed: int = world_state.call("apply_validated_brush", {
				"position": volume.voxel_to_world(cell, true),
				"shape": 1,
				"action": 0,
				"radius": 2,
			})
			print("VOXEL_LOCAL_CAPTURE_DIRTY_EDIT changed=%d" % changed)
			return
	push_error("VOXEL_LOCAL_CAPTURE_ERROR: No resident occupied voxel was found")
