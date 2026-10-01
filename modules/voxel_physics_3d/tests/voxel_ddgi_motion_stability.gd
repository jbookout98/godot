extends SceneTree

const WARMUP_FRAMES := 700
const STATIONARY_SAMPLE_FRAMES := 128
const MOTION_SAMPLE_FRAMES := 180
const OUTPUT_DIRECTORY := "C:/tmp"

var _world: Node3D
var _camera: Camera3D
var _material: VoxelMaterial


func _initialize() -> void:
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", true)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/backend", 2)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/spatial_cascade_resolution", 16)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/rays_per_probe", 16)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/probes_per_frame", 32)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/self_shadow_bias", 0.3)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 0)
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	DisplayServer.window_set_size(Vector2i(1280, 720))
	call_deferred("_run")


func _run() -> void:
	_build_corridor()
	for _frame in WARMUP_FRAMES:
		await process_frame
	_capture("stationary_start")
	var stationary := await _sample_phase(STATIONARY_SAMPLE_FRAMES, false)
	_capture("stationary_end")
	var motion := await _sample_phase(MOTION_SAMPLE_FRAMES, true)
	_capture("motion_end")
	print("DDGI_STABILITY stationary_range=%.6f stationary_max_delta=%.6f motion_range=%.6f motion_max_delta=%.6f" % [
		stationary.range,
		stationary.max_delta,
		motion.range,
		motion.max_delta,
	])
	# A translating camera legitimately changes the projected geometry, so the
	# motion bound is looser. These gates reject flashes and grid-aligned steps,
	# not ordinary gradual lighting changes.
	var stable: bool = float(stationary.range) <= 0.025 and float(stationary.max_delta) <= 0.008
	stable = stable and float(motion.max_delta) <= 0.025
	quit(0 if stable else 1)


func _sample_phase(frame_count: int, move_camera: bool) -> Dictionary:
	var minimum := INF
	var maximum := -INF
	var max_delta := 0.0
	var previous := -1.0
	for frame in frame_count:
		if move_camera:
			var progress := float(frame) / float(maxi(frame_count - 1, 1))
			_camera.position = Vector3(sin(progress * TAU) * 0.03, 1.2, lerpf(3.6, 2.4, progress))
			_camera.look_at(_camera.position + Vector3(0.0, 0.0, -4.0), Vector3.UP)
		await process_frame
		var value := _center_luminance(root.get_texture().get_image())
		minimum = minf(minimum, value)
		maximum = maxf(maximum, value)
		if previous >= 0.0:
			max_delta = maxf(max_delta, absf(value - previous))
		previous = value
	return { "range": maximum - minimum, "max_delta": max_delta }


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


func _capture(suffix: String) -> void:
	root.get_texture().get_image().save_png("%s/voxel_ddgi_corridor_%s.png" % [OUTPUT_DIRECTORY, suffix])


func _build_corridor() -> void:
	_material = load("res://voxel_materials/shared_voxel_material.tres") as VoxelMaterial
	_material.ambient_occlusion_enabled = false
	_material.shading_mode = VoxelMaterial.SHADING_MODE_UNLIT
	_material.shading_mode = VoxelMaterial.SHADING_MODE_PBR
	_world = Node3D.new()
	_world.name = "DdgiMotionCorridor"
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

	_add_solid_volume(Vector3i(24, 1, 96), Vector3(-1.2, 0.0, -4.8))
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


func _add_solid_volume(dimensions: Vector3i, origin: Vector3) -> void:
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
