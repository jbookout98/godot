extends SceneTree

# Deterministic rendered regression for Directional Voxel GI light removal.
# Run once with --mode=presealed and once with --mode=runtime. Both modes use
# identical geometry, camera, light, exposure, and Medium GI settings. Runtime
# closes the opening through VoxelVolume3D.apply_voxel_edits().

const OUTPUT_DIRECTORY := "C:/tmp"
const DIMENSIONS := Vector3i(48, 32, 48)
const VOXEL_SIZE := 0.1
const OPENING_X := Vector2i(20, 27)
const OPENING_Y := Vector2i(10, 19)
const SETTLE_FRAMES := 360
const EDIT_OBSERVATION_FRAMES := 180

var _mode := "runtime"
var _outside_view := false
var _capture_frames := false
var _propagation_decay := 0.78
var _propagation_steps := 4
var _debug_mode := 0
var _world: Node3D
var _volume: VoxelVolume3D
var _camera: Camera3D
var _voxel_material: VoxelMaterial


func _initialize() -> void:
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--mode="):
			_mode = argument.trim_prefix("--mode=")
		elif argument == "--outside":
			_outside_view = true
		elif argument == "--capture-frames":
			_capture_frames = true
		elif argument.begins_with("--decay="):
			_propagation_decay = float(argument.trim_prefix("--decay="))
		elif argument.begins_with("--steps="):
			_propagation_steps = int(argument.trim_prefix("--steps="))
		elif argument.begins_with("--debug-mode="):
			_debug_mode = int(argument.trim_prefix("--debug-mode="))
	if _mode != "runtime" and _mode != "presealed" and _mode != "open":
		_fail("unknown mode %s" % _mode, 2)
		return
	_configure_renderer()
	call_deferred("_start")


func _start() -> void:
	_create_world(_mode == "presealed")
	await _run()


func _configure_renderer() -> void:
	Engine.max_fps = 0
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	DisplayServer.window_set_size(Vector2i(1280, 720))
	ProjectSettings.set_setting("rendering/voxel_forward/reflections/enabled", false)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/backend", 1)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/resolution", 32)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/near_cell_size", 0.4)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/far_cell_size", 1.6)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/distant_cell_size", 6.4)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/propagation_steps", _propagation_steps)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/propagation_decay", _propagation_decay)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/invalidation_epsilon", 0.01)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/dirty_updates_enabled", true)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/temporal_updates_enabled", true)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/dispatch_budget_per_frame", 1)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/temporal_blend_enabled", true)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/temporal_blend_frames", 2)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/voxel_gi/debug_mode", _debug_mode)
	ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/enabled", true)
	ProjectSettings.set_setting("rendering/voxel_forward/shadow_mask/max_distance", 64.0)
	ProjectSettings.set_setting("game/graphics/atmosphere/cloud_quality", 0)
	ProjectSettings.set_setting("game/graphics/atmosphere/god_ray_quality", 0)


func _create_world(sealed: bool) -> void:
	_voxel_material = _create_fixture_material()
	_world = Node3D.new()
	_world.name = "DirectionalGISealedRoom"
	root.add_child(_world)

	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.02, 0.025, 0.035)
	environment.background_energy_multiplier = 1.0
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	environment.volumetric_fog_enabled = false
	environment.adjustment_enabled = false
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	_world.add_child(world_environment)

	var sun := DirectionalLight3D.new()
	# Rays travel from the +Z opening into the room and down toward the floor.
	sun.rotation_degrees = Vector3(-25.0, 0.0, 0.0)
	sun.light_color = Color(1.0, 0.91, 0.74)
	sun.light_energy = 2.0
	sun.shadow_enabled = true
	_world.add_child(sun)

	# Separate slabs make every interior face an ordinary outward-facing surface,
	# matching the authored gameplay corridors. The +Z wall is the mutable target.
	_add_solid_volume(Vector3i(48, 1, 48), Vector3(-2.4, 0.0, -2.4))
	_add_solid_volume(Vector3i(48, 1, 48), Vector3(-2.4, 2.3, -2.4))
	_add_solid_volume(Vector3i(1, 24, 48), Vector3(-2.0, 0.0, -2.4))
	_add_solid_volume(Vector3i(1, 24, 48), Vector3(1.9, 0.0, -2.4))
	_add_solid_volume(Vector3i(48, 24, 1), Vector3(-2.4, 0.0, -2.0))

	var dimensions := Vector3i(48, 24, 1)
	var data := VoxelShapeData.new()
	data.dimensions = dimensions
	data.voxel_size = VOXEL_SIZE
	var dense := PackedByteArray()
	dense.resize(dimensions.x * dimensions.y)
	for y in dimensions.y:
		for x in dimensions.x:
			var in_opening := x >= OPENING_X.x and x <= OPENING_X.y and y >= OPENING_Y.x and y <= OPENING_Y.y
			dense[x + y * dimensions.x] = 1 if sealed or not in_opening else 0
	data.voxel_data = dense

	_volume = VoxelVolume3D.new()
	_volume.name = "Room"
	_volume.voxel_data = data
	_volume.voxel_material = _voxel_material
	_volume.streaming_mode = 1
	_volume.position = Vector3(-2.4, 0.0, 1.9)
	_world.add_child(_volume)

	_camera = Camera3D.new()
	_camera.name = "InteriorCamera"
	_camera.fov = 78.0
	_camera.near = 0.05
	_camera.far = 128.0
	_world.add_child(_camera)
	if _outside_view:
		_camera.position = Vector3(0.0, 1.4, 5.0)
		_camera.look_at(Vector3(0.0, 1.0, 0.0), Vector3.UP)
	else:
		_camera.position = Vector3(0.0, 1.15, 0.45)
		_camera.look_at(Vector3(0.0, 1.15, -1.8), Vector3.UP)
	_camera.current = true
	print("VOXEL_GI_FIXTURE wall_voxels=%d camera=%s" % [dense.count(1), str(_camera.position)])


func _add_solid_volume(dimensions: Vector3i, position: Vector3) -> void:
	var data := VoxelShapeData.new()
	data.dimensions = dimensions
	data.voxel_size = VOXEL_SIZE
	var dense := PackedByteArray()
	dense.resize(dimensions.x * dimensions.y * dimensions.z)
	dense.fill(1)
	data.voxel_data = dense
	var volume := VoxelVolume3D.new()
	volume.voxel_data = data
	volume.voxel_material = _voxel_material
	volume.streaming_mode = 1
	volume.position = position
	_world.add_child(volume)


func _create_fixture_material() -> VoxelMaterial:
	var palette := Image.create(256, 1, false, Image.FORMAT_RGBA8)
	palette.fill(Color.BLACK)
	palette.set_pixel(1, 0, Color(0.74, 0.63, 0.48, 1.0))
	var properties := Image.create(256, 1, false, Image.FORMAT_RGBA8)
	properties.fill(Color(0.5, 0.0, 0.0, 0.0))
	var material := VoxelMaterial.new()
	material.palette_texture = ImageTexture.create_from_image(palette)
	material.material_texture = ImageTexture.create_from_image(properties)
	material.ambient_occlusion_enabled = false
	return material


func _run() -> void:
	for _frame in SETTLE_FRAMES:
		await process_frame
	var before_image := root.get_texture().get_image()
	var before_luminance := _mean_luminance(before_image)
	if _mode == "open":
		var open_output_path := "%s/voxel_gi_open_decay_%03d_debug_%02d.png" % [OUTPUT_DIRECTORY, roundi(_propagation_decay * 100.0), _debug_mode]
		var open_save_error := before_image.save_png(open_output_path)
		if open_save_error != OK:
			_fail("could not save %s: %s" % [open_output_path, error_string(open_save_error)], 9)
			return
		print("VOXEL_GI_OPEN_OK decay=%.2f steps=%d debug=%d luminance=%.6f output=%s" % [_propagation_decay, _propagation_steps, _debug_mode, before_luminance, open_output_path])
		quit(0)
		return
	var maximum_frame_delta := 0.0
	var changed_voxels := 0
	var sealing_luminance_trace: Array[float] = []
	if _mode == "runtime":
		var positions: Array[Vector3i] = []
		for y in range(OPENING_Y.x, OPENING_Y.y + 1):
			for x in range(OPENING_X.x, OPENING_X.y + 1):
				positions.push_back(Vector3i(x, y, 0))
		var values := PackedByteArray()
		values.resize(positions.size())
		values.fill(1)
		changed_voxels = _volume.apply_voxel_edits(positions, values)
		var previous_luminance := before_luminance
		for _frame in EDIT_OBSERVATION_FRAMES:
			await process_frame
			var frame_image := root.get_texture().get_image()
			var frame_luminance := _mean_luminance(frame_image)
			sealing_luminance_trace.push_back(frame_luminance)
			maximum_frame_delta = maxf(maximum_frame_delta, absf(frame_luminance - previous_luminance))
			previous_luminance = frame_luminance
			if _capture_frames:
				DirAccess.make_dir_recursive_absolute("%s/voxel_gi_sealing_frames" % OUTPUT_DIRECTORY)
				frame_image.save_png("%s/voxel_gi_sealing_frames/frame_%03d.png" % [OUTPUT_DIRECTORY, _frame])
	else:
		for _frame in EDIT_OBSERVATION_FRAMES:
			await process_frame

	var final_image := root.get_texture().get_image()
	var final_luminance := _mean_luminance(final_image)
	var wall_luminances := await _measure_wall_luminances()
	var output_path := "%s/voxel_gi_sealed_room_%s.png" % [OUTPUT_DIRECTORY, _mode]
	var save_error := final_image.save_png(output_path)
	if save_error != OK:
		_fail("could not save %s: %s" % [output_path, error_string(save_error)], 3)
		return

	var reopened_luminance := -1.0
	var reopened_voxels := 0
	if _mode == "runtime":
		var reopening_positions: Array[Vector3i] = []
		for y in range(OPENING_Y.x, OPENING_Y.y + 1):
			for x in range(OPENING_X.x, OPENING_X.y + 1):
				reopening_positions.push_back(Vector3i(x, y, 0))
		var reopening_values := PackedByteArray()
		reopening_values.resize(reopening_positions.size())
		reopening_values.fill(0)
		reopened_voxels = _volume.apply_voxel_edits(reopening_positions, reopening_values)
		for _frame in EDIT_OBSERVATION_FRAMES:
			await process_frame
		var reopened_image := root.get_texture().get_image()
		reopened_luminance = _mean_luminance(reopened_image)
		reopened_image.save_png("%s/voxel_gi_sealed_room_runtime_reopened.png" % OUTPUT_DIRECTORY)

	var metadata := {
		"mode": _mode,
		"before_luminance": before_luminance,
		"final_luminance": final_luminance,
		"maximum_consecutive_frame_luminance_delta": maximum_frame_delta,
		"changed_voxels": changed_voxels,
		"reopened_voxels": reopened_voxels,
		"reopened_luminance": reopened_luminance,
		"wall_luminances": wall_luminances,
		"sealing_luminance_trace": sealing_luminance_trace,
		"settle_frames": SETTLE_FRAMES,
		"observation_frames": EDIT_OBSERVATION_FRAMES,
		"camera_position": [_camera.position.x, _camera.position.y, _camera.position.z],
	}
	var metadata_path := output_path.get_basename() + ".json"
	var file := FileAccess.open(metadata_path, FileAccess.WRITE)
	if file == null:
		_fail("could not write %s" % metadata_path, 4)
		return
	file.store_string(JSON.stringify(metadata, "\t"))
	if _mode == "runtime":
		var reference_path := "%s/voxel_gi_sealed_room_presealed.json" % OUTPUT_DIRECTORY
		if not FileAccess.file_exists(reference_path):
			_fail("run --mode=presealed before --mode=runtime", 5)
			return
		var reference_data: Variant = JSON.parse_string(FileAccess.get_file_as_string(reference_path))
		if not reference_data is Dictionary:
			_fail("invalid presealed reference metadata", 6)
			return
		var reference_luminance := float(reference_data.get("final_luminance", 0.0))
		var relative_error := absf(final_luminance - reference_luminance) / maxf(reference_luminance, 0.0001)
		if relative_error > 0.02:
			_fail("runtime-sealed luminance differs from presealed by %.2f%%" % (relative_error * 100.0), 7)
			return
		var reopen_error := absf(reopened_luminance - before_luminance) / maxf(before_luminance, 0.0001)
		if reopen_error > 0.05:
			_fail("reopened luminance differs from original opening by %.2f%%" % (reopen_error * 100.0), 8)
			return
	print("VOXEL_GI_SEALED_ROOM_OK mode=%s before=%.6f final=%.6f max_frame_delta=%.6f changed=%d output=%s" % [
		_mode, before_luminance, final_luminance, maximum_frame_delta, changed_voxels, output_path,
	])
	quit(0)


func _measure_wall_luminances() -> Dictionary:
	var center := Vector3(0.0, 1.15, 0.0)
	var targets := {
		"positive_x": Vector3(1.9, 1.15, 0.0),
		"negative_x": Vector3(-1.9, 1.15, 0.0),
		"positive_y": Vector3(0.0, 2.3, 0.0),
		"negative_y": Vector3(0.0, 0.0, 0.0),
		"positive_z": Vector3(0.0, 1.15, 1.9),
		"negative_z": Vector3(0.0, 1.15, -2.0),
	}
	var result := {}
	_camera.position = center
	for label: String in targets:
		_camera.look_at(targets[label], Vector3.FORWARD if label == "positive_y" or label == "negative_y" else Vector3.UP)
		for _frame in 4:
			await process_frame
		result[label] = _mean_center_luminance(root.get_texture().get_image())
	_camera.position = Vector3(0.0, 1.15, 0.45)
	_camera.look_at(Vector3(0.0, 1.15, -1.8), Vector3.UP)
	return result


func _mean_center_luminance(source: Image) -> float:
	var image := source.duplicate()
	image.resize(160, 90, Image.INTERPOLATE_BILINEAR)
	var sum := 0.0
	var count := 0
	for y in range(25, 65):
		for x in range(50, 110):
			var color: Color = image.get_pixel(x, y).srgb_to_linear()
			sum += color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
			count += 1
	return sum / float(maxi(count, 1))


func _variant_vector(value: Variant) -> Array:
	if value is Vector4:
		return [value.x, value.y, value.z, value.w]
	if value is Vector3:
		return [value.x, value.y, value.z]
	return []


func _mean_luminance(source: Image) -> float:
	var image := source.duplicate()
	image.resize(160, 90, Image.INTERPOLATE_BILINEAR)
	var sum := 0.0
	var count := 0
	# The opening is behind the camera. This crop therefore observes only stable
	# interior surfaces and doubles as the outside-dirty-screen flash metric.
	for y in range(8, 82):
		for x in range(12, 148):
			var color: Color = image.get_pixel(x, y).srgb_to_linear()
			sum += color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
			count += 1
	return sum / float(maxi(count, 1))

func _fail(message: String, code: int) -> void:
	push_error("VOXEL_GI_SEALED_ROOM_FAILED: %s" % message)
	quit(code)
