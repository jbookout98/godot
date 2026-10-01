extends SceneTree

# Exercises the failure path that previously reset every DDGI probe after one
# occupancy revision. The edited floor patch is behind the camera, so a large
# image-wide luminance change is an invalidation flash rather than scene change.

# At 32 scheduled probes per frame, four 16^3 cascades need substantially more
# than 700 frames to reach steady state. Measuring during initial convergence
# mistakes normal settling for an edit-triggered lighting pulse.
const WARMUP_FRAMES := 2500
const SAMPLE_FRAMES := 160
const OUTPUT_DIRECTORY := "C:/tmp"

var _world: Node3D
var _camera: Camera3D
var _material: VoxelMaterial
var _floor: VoxelVolume3D


func _initialize() -> void:
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/backend", 2)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/spatial_cascade_resolution", 16)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/rays_per_probe", 16)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/probes_per_frame", 32)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/view_bias", 0.0)
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	DisplayServer.window_set_size(Vector2i(1280, 720))
	call_deferred("_run")


func _run() -> void:
	_build_corridor()
	for _frame in WARMUP_FRAMES:
		await process_frame
	var before_image := root.get_texture().get_image()
	var before := _center_luminance(before_image)
	before_image.save_png("%s/voxel_ddgi_edit_before.png" % OUTPUT_DIRECTORY)

	var positions: Array[Vector3i] = []
	for z in range(88, 92):
		for x in range(10, 14):
			positions.push_back(Vector3i(x, 0, z))
	var empty_values := PackedByteArray()
	empty_values.resize(positions.size())
	empty_values.fill(0)
	var edit_started := Time.get_ticks_usec()
	var changed := _floor.apply_voxel_edits(positions, empty_values)
	var edit_ms := float(Time.get_ticks_usec() - edit_started) / 1000.0

	var minimum := INF
	var maximum_delta := 0.0
	var previous := before
	var first_delta := 0.0
	for frame in SAMPLE_FRAMES:
		await process_frame
		var value := _center_luminance(root.get_texture().get_image())
		if frame == 0:
			first_delta = absf(value - before)
		minimum = minf(minimum, value)
		maximum_delta = maxf(maximum_delta, absf(value - previous))
		previous = value
		if frame == 0 or frame == 15 or frame == SAMPLE_FRAMES - 1:
			root.get_texture().get_image().save_png("%s/voxel_ddgi_edit_%03d.png" % [OUTPUT_DIRECTORY, frame])
	var minimum_ratio := minimum / maxf(before, 0.0001)
	print("DDGI_EDIT_STABILITY changed=%d edit_ms=%.3f before=%.6f first_delta=%.6f max_delta=%.6f minimum_ratio=%.6f final=%.6f" % [
		changed, edit_ms, before, first_delta, maximum_delta, minimum_ratio, previous,
	])
	var stable := changed == positions.size() and first_delta <= 0.01 and maximum_delta <= 0.015 and minimum_ratio >= 0.85
	quit(0 if stable else 1)


func _center_luminance(image: Image) -> float:
	var begin := Vector2i(image.get_width() * 3 / 10, image.get_height() * 3 / 10)
	var end := Vector2i(image.get_width() * 7 / 10, image.get_height() * 8 / 10)
	var total := 0.0
	var count := 0
	for y in range(begin.y, end.y, 12):
		for x in range(begin.x, end.x, 12):
			total += image.get_pixel(x, y).get_luminance()
			count += 1
	return total / float(maxi(count, 1))


func _build_corridor() -> void:
	_material = load("res://voxel_materials/shared_voxel_material.tres") as VoxelMaterial
	_material.ambient_occlusion_enabled = false
	_material.shading_mode = VoxelMaterial.SHADING_MODE_PBR
	_world = Node3D.new()
	root.add_child(_world)

	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.34, 0.48, 0.72)
	environment.background_energy_multiplier = 1.0
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.50, 0.53, 0.60)
	environment.ambient_light_energy = 0.7
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	_world.add_child(world_environment)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-55.0, -35.0, 0.0)
	sun.light_energy = 1.5
	sun.shadow_enabled = true
	_world.add_child(sun)

	_floor = _add_solid_volume(Vector3i(24, 1, 96), Vector3(-1.2, 0.0, -4.8))
	_add_solid_volume(Vector3i(24, 1, 96), Vector3(-1.2, 2.4, -4.8))
	_add_solid_volume(Vector3i(1, 24, 96), Vector3(-1.2, 0.0, -4.8))
	_add_solid_volume(Vector3i(1, 24, 96), Vector3(1.1, 0.0, -4.8))
	_add_solid_volume(Vector3i(24, 24, 1), Vector3(-1.2, 0.0, -4.8))

	_camera = Camera3D.new()
	_camera.fov = 70.0
	_camera.near = 0.05
	_camera.far = 256.0
	_camera.position = Vector3(0.0, 1.2, 3.6)
	_world.add_child(_camera)
	_camera.look_at(_camera.position + Vector3(0.0, 0.0, -4.0), Vector3.UP)
	_camera.current = true


func _add_solid_volume(dimensions: Vector3i, origin: Vector3) -> VoxelVolume3D:
	var data := VoxelShapeData.new()
	data.dimensions = dimensions
	data.voxel_size = 0.1
	var dense := PackedByteArray()
	dense.resize(dimensions.x * dimensions.y * dimensions.z)
	dense.fill(1)
	data.voxel_data = dense
	var volume := VoxelVolume3D.new()
	volume.voxel_data = data
	volume.voxel_material = _material
	volume.streaming_mode = 1
	volume.position = origin
	_world.add_child(volume)
	return volume
