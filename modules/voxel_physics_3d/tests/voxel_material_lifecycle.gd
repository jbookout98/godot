extends SceneTree
## Run with a rendering-capable editor build. Tests are not launched by this file's creation.

var failed := false

func _initialize() -> void:
	call_deferred("run")

func check(condition: bool, message: String) -> void:
	if not condition:
		failed = true
		push_error("VOXEL_MATERIAL_LIFECYCLE: " + message)

func lookup(color: Color) -> ImageTexture:
	var image := Image.create_empty(256, 1, false, Image.FORMAT_RGBA8)
	image.fill(color)
	return ImageTexture.create_from_image(image)

func surface_material(volume: VoxelVolume3D) -> RID:
	var mesh := volume.get_base()
	check(RenderingServer.mesh_get_surface_count(mesh) == 1, "visible volume lost its surface")
	if RenderingServer.mesh_get_surface_count(mesh) != 1:
		return RID()
	var material := RenderingServer.mesh_surface_get_material(mesh, 0)
	check(material.is_valid(), "surface has no material")
	if material.is_valid():
		# A syntactically valid RID can still refer to an already-freed material.
		var energy: Variant = RenderingServer.material_get_param(material, &"emission_energy")
		check(energy != null and is_equal_approx(float(energy), 3.0), "surface references a freed or incorrect material")
	return material

func run() -> void:
	var scene := Node3D.new()
	root.add_child(scene)
	var data := VoxelShapeData.new()
	data.dimensions = Vector3i(2, 2, 2)
	data.voxel_size = 0.5
	data.fill_voxel_region(Vector3i.ZERO, data.dimensions, 1)
	var material := VoxelMaterial.new()
	material.palette_texture = lookup(Color(1.0, 0.2, 0.1))
	material.emission_energy = 3.0
	var volumes: Array[VoxelVolume3D] = []
	for index in 2:
		var volume := VoxelVolume3D.new()
		volume.name = "Volume%d" % index
		volume.streaming_mode = VoxelVolume3D.STREAMING_ALWAYS_RESIDENT
		volume.voxel_data = data
		volume.voxel_material = material
		scene.add_child(volume)
		volume.owner = scene
		volumes.append(volume)
	var initial := surface_material(volumes[0])
	check(initial == surface_material(volumes[1]), "shared volumes did not use the same runtime material")
	for color: Color in [Color.BLACK, Color.WHITE, Color(0.5, 0.5, 0.5)]:
		material.emission_texture = lookup(color)
		await process_frame
		var replacement := surface_material(volumes[0])
		check(replacement != initial, "changing texture identity did not replace the runtime material")
		check(replacement == surface_material(volumes[1]), "shared material replacement left one volume behind")
		initial = replacement
	material.emission_texture = null
	await process_frame
	for volume in volumes:
		surface_material(volume)
	material.emission_texture = lookup(Color.BLACK)
	var packed := PackedScene.new()
	check(packed.pack(scene) == OK, "could not pack emissive scene")
	var save_path := "user://voxel_material_lifecycle.tscn"
	check(ResourceSaver.save(packed, save_path) == OK, "could not save emissive scene")
	scene.free()
	await process_frame
	var loaded := ResourceLoader.load(save_path, "PackedScene", ResourceLoader.CACHE_MODE_IGNORE) as PackedScene
	check(loaded != null, "saved emissive scene did not load")
	if loaded != null:
		var restored := loaded.instantiate()
		root.add_child(restored)
		await process_frame
		for child in restored.get_children():
			var volume := child as VoxelVolume3D
			check(volume.get_voxel(Vector3i.ZERO) == 1, "reload lost voxel occupancy")
			check(volume.voxel_material.emission_texture != null, "reload lost emission texture")
			surface_material(volume)
		restored.free()
	DirAccess.remove_absolute(save_path)
	print("VOXEL_MATERIAL_LIFECYCLE_", "FAILED" if failed else "OK")
	quit(1 if failed else 0)
