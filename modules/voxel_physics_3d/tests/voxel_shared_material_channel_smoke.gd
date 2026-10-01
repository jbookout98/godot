extends SceneTree


func _initialize() -> void:
	var material := load("res://voxel_materials/shared_voxel_material.tres") as VoxelMaterial
	if material == null:
		push_error("Shared voxel material did not load")
		quit(1)
		return
	if material.material_texture != null:
		push_error("Specularity lookup is still bound as a combined material/emission texture")
		quit(1)
		return
	if material.specularity_texture == null:
		push_error("Shared voxel material has no explicit specularity texture")
		quit(1)
		return
	print("VOXEL_SHARED_MATERIAL_CHANNEL_SMOKE_OK")
	quit(0)
