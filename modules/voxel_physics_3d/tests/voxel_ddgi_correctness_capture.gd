extends SceneTree

const OUTPUT_DIRECTORY := "C:/tmp"
const FIXTURE_WARMUP_FRAMES := 240

enum Fixture {
	THIN_WALL,
	CORRIDOR_CORNER,
	SOLID_AND_EMPTY_CELL,
	OPEN_SKY,
}

var _world: Node3D
var _camera: Camera3D
var _fixture_root: Node3D
var _voxel_material: VoxelMaterial
var _variant := "ddgi"
var _corner_ao_debug := false


func _initialize() -> void:
	if "--legacy" in OS.get_cmdline_user_args():
		_variant = "legacy"
	elif "--off" in OS.get_cmdline_user_args():
		_variant = "off"
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/enabled", _variant != "off")
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/backend", 1 if _variant == "legacy" else 2)
	_corner_ao_debug = "--corner-ao" in OS.get_cmdline_user_args()
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/voxel_gi/debug_mode", 12 if _corner_ao_debug else 0)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/spatial_cascade_resolution", 16)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/rays_per_probe", 16)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/self_shadow_bias", 0.3)
	var debug_mode := 0
	if "--probe-debug" in OS.get_cmdline_user_args():
		debug_mode = 7
	elif "--contribution-debug" in OS.get_cmdline_user_args():
		debug_mode = 11
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/ddgi/debug_mode", debug_mode)
	ProjectSettings.set_setting("rendering/voxel_forward/indirect_light/intensity", 8.0 if "--exaggerate" in OS.get_cmdline_user_args() else 1.0)
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	DisplayServer.window_set_size(Vector2i(1280, 720))
	call_deferred("_run")


func _run() -> void:
	_create_world()
	for fixture in Fixture.values():
		await _build_fixture(fixture)
		for _frame in FIXTURE_WARMUP_FRAMES:
			await process_frame
		if not _capture_fixture(fixture, _variant):
			return
	quit(0)


func _capture_fixture(fixture: Fixture, suffix: String) -> bool:
	var output_path := "%s/voxel_ddgi_fixture_%s_%s%s.png" % [OUTPUT_DIRECTORY, Fixture.keys()[fixture].to_lower(), suffix, "_corner_ao" if _corner_ao_debug else ""]
	var image := root.get_texture().get_image()
	var error := image.save_png(output_path)
	if error != OK:
		push_error("DDGI_FIXTURE_FAILED fixture=%s error=%s" % [Fixture.keys()[fixture], error_string(error)])
		quit(1)
		return false
	print("DDGI_FIXTURE_CAPTURED fixture=%s enabled=%s backend=%d path=%s" % [
		Fixture.keys()[fixture],
		str(ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/enabled", false)),
		int(ProjectSettings.get_setting("rendering/voxel_forward/indirect_light/backend", -1)),
		output_path,
	])
	return true


func _create_world() -> void:
	_voxel_material = load("res://voxel_materials/shared_voxel_material.tres") as VoxelMaterial
	# The regular fixtures isolate GI visibility. The optional corner-AO view
	# instead renders the exact topology factor on the same deterministic geometry.
	_voxel_material.ambient_occlusion_enabled = _corner_ao_debug
	# Rebuild after the test selects its runtime lighting backend so this fixture
	# also catches feature-gating regressions in cached VoxelMaterial shaders.
	_voxel_material.shading_mode = VoxelMaterial.SHADING_MODE_UNLIT
	_voxel_material.shading_mode = VoxelMaterial.SHADING_MODE_PBR
	_world = Node3D.new()
	_world.name = "DdgiCorrectnessFixtures"
	root.add_child(_world)
	_fixture_root = Node3D.new()
	_fixture_root.name = "Fixture"
	_world.add_child(_fixture_root)

	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.34, 0.48, 0.72)
	environment.background_energy_multiplier = 1.0
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.04, 0.05, 0.07)
	environment.ambient_light_energy = 0.15
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	_world.add_child(world_environment)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-55.0, -35.0, 0.0)
	sun.light_energy = 1.5
	sun.shadow_enabled = true
	_world.add_child(sun)

	_camera = Camera3D.new()
	_camera.fov = 70.0
	_camera.far = 256.0
	_camera.current = true
	_world.add_child(_camera)


func _build_fixture(fixture: Fixture) -> void:
	for child in _fixture_root.get_children():
		child.queue_free()
	await process_frame
	match fixture:
		Fixture.THIN_WALL:
			_add_voxel_volume(Vector3i(48, 16, 1), Vector3.ZERO, _floor_and_wall.bind(true))
			_add_voxel_volume(Vector3i(48, 1, 32), Vector3(0, -0.1, -1.5), _solid_voxel)
			_camera.position = Vector3(2.4, 0.8, 2.8)
			_camera.look_at(Vector3(2.4, 0.8, 0.0), Vector3.UP)
		Fixture.CORRIDOR_CORNER:
			if _corner_ao_debug:
				_add_voxel_volume(Vector3i(48, 18, 48), Vector3(-2.4, -0.1, -2.4), _l_corner_voxel)
			else:
				_add_voxel_volume(Vector3i(48, 1, 48), Vector3(-2.4, -0.1, -2.4), _solid_voxel)
				_add_voxel_volume(Vector3i(1, 18, 48), Vector3.ZERO, _solid_voxel)
				_add_voxel_volume(Vector3i(24, 18, 1), Vector3.ZERO, _solid_voxel)
			_camera.position = Vector3(1.5, 0.8, 2.0)
			_camera.look_at(Vector3(0.0, 0.7, 0.0), Vector3.UP)
		Fixture.SOLID_AND_EMPTY_CELL:
			_add_voxel_volume(Vector3i(32, 12, 12), Vector3(-3.2, 0.0, 0.0), _solid_voxel)
			_add_voxel_volume(Vector3i(64, 1, 32), Vector3(-3.2, -0.1, -1.0), _solid_voxel)
			_camera.position = Vector3(4.5, 2.0, 5.0)
			_camera.look_at(Vector3(-0.8, 0.8, 0.0), Vector3.UP)
		Fixture.OPEN_SKY:
			_add_voxel_volume(Vector3i(64, 1, 64), Vector3(-3.2, -0.1, -3.2), _solid_voxel)
			_add_voxel_volume(Vector3i(8, 14, 8), Vector3(-0.4, 0.0, -0.4), _solid_voxel)
			_camera.position = Vector3(3.5, 2.4, 4.5)
			_camera.look_at(Vector3.ZERO, Vector3.UP)


func _add_voxel_volume(dimensions: Vector3i, origin: Vector3, occupancy: Callable) -> void:
	var data := VoxelShapeData.new()
	data.dimensions = dimensions
	data.voxel_size = 0.1
	var dense := PackedByteArray()
	dense.resize(dimensions.x * dimensions.y * dimensions.z)
	for z in dimensions.z:
		for y in dimensions.y:
			for x in dimensions.x:
				var index := x + y * dimensions.x + z * dimensions.x * dimensions.y
				dense[index] = 1 if occupancy.call(Vector3i(x, y, z), dimensions) else 0
	data.voxel_data = dense
	var volume := VoxelVolume3D.new()
	volume.voxel_data = data
	volume.voxel_material = _voxel_material
	volume.streaming_mode = 1
	volume.position = origin
	_fixture_root.add_child(volume)


func _solid_voxel(_position: Vector3i, _dimensions: Vector3i) -> bool:
	return true


func _l_corner_voxel(position: Vector3i, _dimensions: Vector3i) -> bool:
	return position.y == 0 or position.x == 24 or position.z == 24


func _floor_and_wall(position: Vector3i, dimensions: Vector3i, include_wall: bool) -> bool:
	return position.y == 0 or (include_wall and position.z == dimensions.z / 2)
