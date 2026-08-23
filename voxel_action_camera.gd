extends Camera3D

enum ShapeMode {
	SPHERE,
	CUBOID,
	SURFACE,
}

enum VoxelAction {
	REPLACE,
	ADD,
	CLEAR,
}

enum PlacementMode {
	MERGED,
	ATTACHED,
	INSET,
}

@export_category("Voxel Action")
@export var shape_mode: ShapeMode = ShapeMode.SPHERE
@export var voxel_action: VoxelAction = VoxelAction.CLEAR
@export var placement_mode: PlacementMode = PlacementMode.MERGED

@export_category("Shape Settings")
@export_range(1, 32, 1) var radius_voxels := 4
@export var cuboid_size_voxels := Vector3i(8, 8, 8)

@export_category("Raycast")
@export var ray_distance := 256.0
@export_flags_3d_physics var collision_mask := 0xFFFFFFFF

@export_category("Material")
@export_range(1, 255, 1) var selected_material := 1

@export_category("Shape Preview")
@export var draw_shape_preview := true
@export var draw_preview_outline := true
@export var preview_outline_color := Color(1.0, 0.85, 0.1, 0.95)
@export var draw_preview_fill := false
@export var preview_fill_color := Color(1.0, 0.65, 0.05, 0.2)
@export var use_preview_texture := false
@export var preview_texture: Texture2D
@export var preview_xray := false
@export_range(0.0, 0.1, 0.001) var preview_expansion := 0.01
@export_range(1.0, 60.0, 1.0) var preview_updates_per_second := 30.0
@export_range(0.0, 0.5, 0.01) var preview_settle_time := 0.02
@export var hide_preview_while_moving := true
@export_range(1000, 500000, 1000) var max_preview_voxels := 50000

const UNIQUE_META := "_voxel_action_data_is_unique"
const FACE_DIRECTIONS: Array[Vector3i] = [
	Vector3i.LEFT,
	Vector3i.RIGHT,
	Vector3i.DOWN,
	Vector3i.UP,
	Vector3i.FORWARD,
	Vector3i.BACK,
]
const CUBE_CORNERS: Array[Vector3i] = [
	Vector3i(0, 0, 0),
	Vector3i(1, 0, 0),
	Vector3i(1, 1, 0),
	Vector3i(0, 1, 0),
	Vector3i(0, 0, 1),
	Vector3i(1, 0, 1),
	Vector3i(1, 1, 1),
	Vector3i(0, 1, 1),
]
const FACE_CORNERS := [
	[0, 4, 7, 3],
	[1, 2, 6, 5],
	[0, 1, 5, 4],
	[3, 7, 6, 2],
	[0, 3, 2, 1],
	[4, 5, 6, 7],
]
const QUAD_EDGES := [Vector2i(0, 1), Vector2i(1, 2), Vector2i(2, 3), Vector2i(3, 0)]

var _shape_preview: MeshInstance3D
var _preview_signature := ""
var _preview_candidate_signature := ""
var _preview_candidate_since_msec := 0
var _preview_limit_reported := false
var _preview_update_time := 0.0
var _player_exclusions: Array[RID] = []
var _sphere_query_shape := SphereShape3D.new()
var _box_query_shape := BoxShape3D.new()
var _fill_material := StandardMaterial3D.new()
var _outline_material := StandardMaterial3D.new()


func _ready() -> void:
	_shape_preview = _create_preview_instance("VoxelShapePreview")
	_player_exclusions = _build_player_exclusions()


func _physics_process(delta: float) -> void:
	_preview_update_time += delta
	var update_interval := 1.0 / maxf(preview_updates_per_second, 1.0)
	if _preview_signature.is_empty() or _preview_update_time >= update_interval:
		_preview_update_time = 0.0
		_update_shape_preview()


func _input(event: InputEvent) -> void:
	if not event is InputEventMouseButton or not event.pressed:
		return

	match event.button_index:
		MOUSE_BUTTON_LEFT:
			select_material_at_crosshair()
		MOUSE_BUTTON_RIGHT:
			apply_action_at_crosshair()


## Samples the palette index of the solid voxel under the crosshair.
func select_material_at_crosshair() -> void:
	var hit := _raycast_from_crosshair()
	if hit.is_empty():
		return

	var body := hit.get("collider") as CollisionObject3D
	var volume := _find_voxel_volume(body)
	if volume == null or volume.voxel_data == null:
		return

	var hit_cell := _get_hit_cell(volume, hit.position, hit.normal)
	if not _is_inside(hit_cell, volume.voxel_data.dimensions):
		return

	var material := volume.get_voxel(hit_cell)
	if material != 0:
		selected_material = material
		_preview_signature = ""


## Applies the selected action and shape at the voxel under the crosshair.
func apply_action_at_crosshair() -> void:
	var hit := _raycast_from_crosshair()
	if hit.is_empty():
		return

	var hit_body := hit.get("collider") as CollisionObject3D
	var hit_volume := _find_voxel_volume(hit_body)
	if hit_volume == null or hit_volume.voxel_data == null:
		return

	var hit_position: Vector3 = hit.position
	var hit_normal: Vector3 = hit.normal.normalized()
	var hit_cell := _get_hit_cell(hit_volume, hit_position, hit_normal)
	if not _is_inside(hit_cell, hit_volume.voxel_data.dimensions):
		return

	if shape_mode == ShapeMode.SURFACE:
		_apply_surface_action(hit_volume, hit_body, hit_cell)
		_preview_signature = ""
		return

	_apply_geometric_action(hit_volume, hit_body, hit_cell, hit_position, hit_normal)
	_preview_signature = ""


func _apply_geometric_action(
	hit_volume: VoxelVolume3D,
	hit_body: CollisionObject3D,
	hit_cell: Vector3i,
	hit_position: Vector3,
	hit_normal: Vector3
) -> void:
	var geometry := _get_geometric_parameters(
		hit_volume,
		hit_cell,
		hit_position,
		hit_normal
	)
	var center: Vector3 = geometry.center
	var world_radius: float = geometry.world_radius
	var world_size: Vector3 = geometry.world_size
	var half_extents: Vector3 = geometry.half_extents
	var shape_basis: Basis = geometry.shape_basis

	var bodies := _get_overlapping_bodies(
		center,
		hit_body,
		world_radius,
		world_size,
		shape_basis
	)

	for body in bodies.values():
		var volume := _find_voxel_volume(body)
		if volume == null or volume.voxel_data == null:
			continue

		var positions := _collect_geometric_positions(
			volume,
			center,
			world_radius,
			half_extents,
			shape_basis
		)
		_apply_positions(volume, body, positions)


func _get_geometric_parameters(
	hit_volume: VoxelVolume3D,
	hit_cell: Vector3i,
	hit_position: Vector3,
	hit_normal: Vector3
) -> Dictionary:
	var voxel_size: float = hit_volume.voxel_data.voxel_size
	var merged_center := hit_volume.voxel_to_world(hit_cell, true)
	var center := merged_center
	var shape_basis := hit_volume.global_transform.basis.orthonormalized()
	var world_radius := float(radius_voxels) * voxel_size
	var world_size := Vector3(cuboid_size_voxels.max(Vector3i.ONE)) * voxel_size
	var half_extents := world_size * 0.5

	if placement_mode != PlacementMode.MERGED:
		var direction := 1.0 if placement_mode == PlacementMode.ATTACHED else -1.0
		var local_normal := shape_basis.inverse() * hit_normal
		var grid_normal := Vector3.ZERO
		var dominant_axis := local_normal.abs().max_axis_index()
		grid_normal[dominant_axis] = signf(local_normal[dominant_axis])
		var world_grid_normal := (shape_basis * grid_normal).normalized()
		var surface_center := merged_center + world_grid_normal * voxel_size * 0.5
		var support_distance := world_radius
		if shape_mode == ShapeMode.CUBOID:
			support_distance = half_extents[dominant_axis]
		center = surface_center + world_grid_normal * support_distance * direction

	return {
		"center": center,
		"world_radius": world_radius,
		"world_size": world_size,
		"half_extents": half_extents,
		"shape_basis": shape_basis,
	}


func _get_overlapping_bodies(
	center: Vector3,
	hit_body: CollisionObject3D,
	world_radius: float,
	world_size: Vector3,
	shape_basis: Basis
) -> Dictionary:
	var query := PhysicsShapeQueryParameters3D.new()
	if shape_mode == ShapeMode.SPHERE:
		_sphere_query_shape.radius = world_radius
		query.shape = _sphere_query_shape
	else:
		_box_query_shape.size = world_size
		query.shape = _box_query_shape

	var query_basis := Basis.IDENTITY if shape_mode == ShapeMode.SPHERE else shape_basis
	query.transform = Transform3D(query_basis, center)
	query.collision_mask = collision_mask
	query.collide_with_bodies = true
	query.collide_with_areas = false

	var bodies := {}
	bodies[hit_body.get_instance_id()] = hit_body

	var state := get_world_3d().direct_space_state
	for result in state.intersect_shape(query, 256):
		var body := result.get("collider") as CollisionObject3D
		if body != null:
			bodies[body.get_instance_id()] = body

	return bodies


func _collect_geometric_positions(
	volume: VoxelVolume3D,
	center: Vector3,
	world_radius: float,
	half_extents: Vector3,
	shape_basis: Basis,
	respect_action := true
) -> Array[Vector3i]:
	var data: VoxelShapeData = volume.voxel_data
	var dimensions := data.dimensions
	var voxel_size: float = data.voxel_size
	var dense_voxels: PackedByteArray = data.get_voxel_data()
	var volume_transform := volume.global_transform
	var volume_basis := volume_transform.basis
	var volume_origin := volume_transform.origin
	var inverse_shape_basis := shape_basis.inverse()
	var center_cell := volume.world_to_voxel(center)
	if (
		shape_mode == ShapeMode.SPHERE
		and volume_basis.get_scale().is_equal_approx(Vector3.ONE)
	):
		var local_sphere_center := (
			volume.to_local(center) / voxel_size - Vector3.ONE * 0.5
		)
		return _collect_sphere_scanlines(
			dimensions,
			dense_voxels,
			local_sphere_center,
			world_radius / voxel_size,
			respect_action
		)
	var bounding_radius := world_radius
	if shape_mode == ShapeMode.CUBOID:
		bounding_radius = half_extents.length()
	var cell_radius := ceili(bounding_radius / voxel_size) + 2

	var minimum := Vector3i(
		maxi(0, center_cell.x - cell_radius),
		maxi(0, center_cell.y - cell_radius),
		maxi(0, center_cell.z - cell_radius)
	)
	var maximum := Vector3i(
		mini(dimensions.x - 1, center_cell.x + cell_radius),
		mini(dimensions.y - 1, center_cell.y + cell_radius),
		mini(dimensions.z - 1, center_cell.z + cell_radius)
	)

	var radius_squared := world_radius * world_radius
	var positions: Array[Vector3i] = []
	for z in range(minimum.z, maximum.z + 1):
		for y in range(minimum.y, maximum.y + 1):
			for x in range(minimum.x, maximum.x + 1):
				var cell := Vector3i(x, y, z)
				var voxel_index := x + y * dimensions.x + z * dimensions.x * dimensions.y
				var current_material := dense_voxels[voxel_index]
				if respect_action and not _action_accepts(current_material):
					continue

				var local_center := (Vector3(cell) + Vector3.ONE * 0.5) * voxel_size
				var offset := volume_basis * local_center + volume_origin - center
				var is_inside := offset.length_squared() <= radius_squared
				if shape_mode == ShapeMode.CUBOID:
					offset = inverse_shape_basis * offset
					is_inside = (
						absf(offset.x) <= half_extents.x
						and absf(offset.y) <= half_extents.y
						and absf(offset.z) <= half_extents.z
					)

				if is_inside:
					positions.append(cell)

	return positions


func _collect_sphere_scanlines(
	dimensions: Vector3i,
	dense_voxels: PackedByteArray,
	center: Vector3,
	radius: float,
	respect_action: bool
) -> Array[Vector3i]:
	var positions: Array[Vector3i] = []
	var radius_squared := radius * radius
	var minimum_z := maxi(0, ceili(center.z - radius))
	var maximum_z := mini(dimensions.z - 1, floori(center.z + radius))
	var plane_size := dimensions.x * dimensions.y

	for z in range(minimum_z, maximum_z + 1):
		var delta_z := float(z) - center.z
		var remaining_yz := radius_squared - delta_z * delta_z
		if remaining_yz < 0.0:
			continue
		var y_radius := sqrt(remaining_yz)
		var minimum_y := maxi(0, ceili(center.y - y_radius))
		var maximum_y := mini(dimensions.y - 1, floori(center.y + y_radius))

		for y in range(minimum_y, maximum_y + 1):
			var delta_y := float(y) - center.y
			var remaining_x := remaining_yz - delta_y * delta_y
			if remaining_x < 0.0:
				continue
			var x_radius := sqrt(remaining_x)
			var minimum_x := maxi(0, ceili(center.x - x_radius))
			var maximum_x := mini(dimensions.x - 1, floori(center.x + x_radius))

			for x in range(minimum_x, maximum_x + 1):
				var voxel_index := x + y * dimensions.x + z * plane_size
				if (
					respect_action
					and not _action_accepts(dense_voxels[voxel_index])
				):
					continue
				positions.append(Vector3i(x, y, z))

	return positions


func _apply_surface_action(
	volume: VoxelVolume3D,
	body: CollisionObject3D,
	hit_cell: Vector3i
) -> void:
	var positions := _collect_surface_positions(volume, hit_cell)
	_apply_positions(volume, body, positions)


func _collect_surface_positions(
	volume: VoxelVolume3D,
	hit_cell: Vector3i
) -> Array[Vector3i]:
	var target_material := volume.get_voxel(hit_cell)
	if target_material == 0:
		return []

	var surface_cells := _collect_connected_surface(
		volume,
		hit_cell,
		target_material
	)
	var positions: Array[Vector3i] = []

	if voxel_action == VoxelAction.ADD:
		var additions := {}
		var radius_squared := radius_voxels * radius_voxels
		var dimensions: Vector3i = volume.voxel_data.dimensions
		for surface_cell in surface_cells:
			for direction in FACE_DIRECTIONS:
				var neighbor := surface_cell + direction
				if (
					_is_inside(neighbor, dimensions)
					and volume.get_voxel(neighbor) == 0
					and _cell_distance_squared(neighbor, hit_cell) <= radius_squared
				):
					additions[neighbor] = true
		positions.assign(additions.keys())
	else:
		positions = surface_cells

	return positions


func _collect_connected_surface(
	volume: VoxelVolume3D,
	start_cell: Vector3i,
	target_material: int
) -> Array[Vector3i]:
	var dimensions: Vector3i = volume.voxel_data.dimensions
	if not _is_surface_voxel(volume, start_cell, dimensions):
		return []

	var radius_squared := radius_voxels * radius_voxels
	var visited := {start_cell: true}
	var queue: Array[Vector3i] = [start_cell]
	var surface_cells: Array[Vector3i] = []
	var read_index := 0

	while read_index < queue.size():
		var cell := queue[read_index]
		read_index += 1
		surface_cells.append(cell)

		for direction in FACE_DIRECTIONS:
			var neighbor := cell + direction
			if (
				visited.has(neighbor)
				or not _is_inside(neighbor, dimensions)
				or _cell_distance_squared(neighbor, start_cell) > radius_squared
				or volume.get_voxel(neighbor) != target_material
				or not _is_surface_voxel(volume, neighbor, dimensions)
			):
				continue

			visited[neighbor] = true
			queue.append(neighbor)

	return surface_cells


func _is_surface_voxel(
	volume: VoxelVolume3D,
	cell: Vector3i,
	dimensions: Vector3i
) -> bool:
	for direction in FACE_DIRECTIONS:
		var neighbor := cell + direction
		if not _is_inside(neighbor, dimensions) or volume.get_voxel(neighbor) == 0:
			return true
	return false


func _apply_positions(
	volume: VoxelVolume3D,
	body: CollisionObject3D,
	positions: Array[Vector3i]
) -> void:
	if positions.is_empty():
		return

	var new_material := 0 if voxel_action == VoxelAction.CLEAR else selected_material
	var new_values := PackedByteArray()
	new_values.resize(positions.size())
	new_values.fill(new_material)
	if not volume.has_meta(UNIQUE_META):
		_make_volume_unique_with_edits(volume, body, positions, new_values)
		return
	volume.apply_voxel_edits(positions, new_values)


func _action_accepts(current_material: int) -> bool:
	match voxel_action:
		VoxelAction.ADD:
			return current_material == 0
		VoxelAction.REPLACE:
			return current_material != 0 and current_material != selected_material
		VoxelAction.CLEAR:
			return current_material != 0
	return false


func _get_hit_cell(
	volume: VoxelVolume3D,
	hit_position: Vector3,
	hit_normal: Vector3
) -> Vector3i:
	var voxel_size: float = volume.voxel_data.voxel_size
	return volume.world_to_voxel(hit_position - hit_normal * voxel_size * 0.5)


func _raycast_from_crosshair() -> Dictionary:
	# A camera-forward ray is the captured-mouse crosshair ray. Using the camera
	# transform also works correctly with embedded and stretched viewports.
	var ray_origin := global_position
	var ray_direction := -global_transform.basis.z.normalized()

	var query := PhysicsRayQueryParameters3D.create(
		ray_origin,
		ray_origin + ray_direction * ray_distance
	)
	query.collision_mask = collision_mask
	query.collide_with_bodies = true
	query.collide_with_areas = false
	query.exclude = _player_exclusions

	return get_world_3d().direct_space_state.intersect_ray(query)


func _cell_distance_squared(a: Vector3i, b: Vector3i) -> int:
	var difference := a - b
	return (
		difference.x * difference.x
		+ difference.y * difference.y
		+ difference.z * difference.z
	)


func _is_inside(cell: Vector3i, dimensions: Vector3i) -> bool:
	return (
		cell.x >= 0
		and cell.y >= 0
		and cell.z >= 0
		and cell.x < dimensions.x
		and cell.y < dimensions.y
		and cell.z < dimensions.z
	)


func _create_preview_instance(instance_name: String) -> MeshInstance3D:
	var instance := MeshInstance3D.new()
	instance.name = instance_name
	instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	instance.top_level = true
	instance.visible = false
	add_child(instance)
	instance.global_transform = Transform3D.IDENTITY
	return instance


func _update_shape_preview() -> void:
	if not draw_shape_preview:
		_hide_shape_preview()
		_preview_signature = "disabled"
		_preview_candidate_signature = ""
		return

	var hit := _raycast_from_crosshair()
	if hit.is_empty():
		_hide_shape_preview()
		_preview_signature = "no_hit"
		_preview_candidate_signature = ""
		return

	var hit_body := hit.get("collider") as CollisionObject3D
	var hit_volume := _find_voxel_volume(hit_body)
	if hit_volume == null or hit_volume.voxel_data == null:
		_hide_shape_preview()
		_preview_signature = "no_voxel_volume"
		_preview_candidate_signature = ""
		return

	var hit_position: Vector3 = hit.position
	var hit_normal: Vector3 = hit.normal.normalized()
	var hit_cell := _get_hit_cell(hit_volume, hit_position, hit_normal)
	if not _is_inside(hit_cell, hit_volume.voxel_data.dimensions):
		_hide_shape_preview()
		_preview_signature = "outside_volume"
		_preview_candidate_signature = ""
		return

	var signature := _make_preview_signature(
		hit_body,
		hit_volume,
		hit_cell,
		hit_position,
		hit_normal
	)
	if signature == _preview_signature:
		if _shape_preview.mesh != null:
			_shape_preview.visible = true
		return

	var now_msec := Time.get_ticks_msec()
	if signature != _preview_candidate_signature:
		_preview_candidate_signature = signature
		_preview_candidate_since_msec = now_msec
		if hide_preview_while_moving:
			_hide_shape_preview()
		if preview_settle_time > 0.0:
			return
	if (
		preview_settle_time > 0.0
		and now_msec - _preview_candidate_since_msec
		< roundi(preview_settle_time * 1000.0)
	):
		return

	var chunks := _collect_preview_chunks(
		hit_body,
		hit_volume,
		hit_cell,
		hit_position,
		hit_normal
	)
	_build_preview_meshes(chunks)
	_preview_signature = signature


func _make_preview_signature(
	hit_body: CollisionObject3D,
	hit_volume: VoxelVolume3D,
	hit_cell: Vector3i,
	hit_position: Vector3,
	hit_normal: Vector3
) -> String:
	var texture_id := 0
	if preview_texture != null:
		texture_id = preview_texture.get_instance_id()

	var grid_normal := Vector3i.ZERO
	if shape_mode != ShapeMode.SURFACE and placement_mode != PlacementMode.MERGED:
		var shape_basis := hit_volume.global_transform.basis.orthonormalized()
		var local_normal := shape_basis.inverse() * hit_normal
		var dominant_axis := local_normal.abs().max_axis_index()
		grid_normal[dominant_axis] = 1 if local_normal[dominant_axis] >= 0.0 else -1

	return str(hash([
		hit_body.get_instance_id(),
		hit_cell,
		grid_normal,
		shape_mode,
		voxel_action,
		placement_mode,
		radius_voxels,
		cuboid_size_voxels,
		selected_material,
		draw_preview_outline,
		preview_outline_color,
		draw_preview_fill,
		preview_fill_color,
		use_preview_texture,
		texture_id,
		preview_xray,
		preview_expansion,
		max_preview_voxels,
	]))


func _collect_preview_chunks(
	hit_body: CollisionObject3D,
	hit_volume: VoxelVolume3D,
	hit_cell: Vector3i,
	hit_position: Vector3,
	hit_normal: Vector3
) -> Array[Dictionary]:
	var chunks: Array[Dictionary] = []
	if shape_mode == ShapeMode.SURFACE:
		var surface_volumes: Array[VoxelVolume3D] = [hit_volume]
		var surface_positions := _collect_surface_positions(hit_volume, hit_cell)
		var surface_result := {
			"positions": surface_positions,
			"face_masks": {},
		}
		if voxel_action == VoxelAction.CLEAR:
			surface_result = _collect_exposed_surface(
				hit_volume,
				surface_positions,
				surface_volumes
			)
		chunks.append({
			"volume": hit_volume,
			"positions": surface_result.positions,
			"face_masks": surface_result.face_masks,
			"empty_neighbor_faces": voxel_action == VoxelAction.CLEAR,
			"neighbor_volumes": surface_volumes,
		})
		return chunks

	var geometry := _get_geometric_parameters(
		hit_volume,
		hit_cell,
		hit_position,
		hit_normal
	)
	var bodies := _get_overlapping_bodies(
		geometry.center,
		hit_body,
		geometry.world_radius,
		geometry.world_size,
		geometry.shape_basis
	)
	var neighbor_volumes: Array[VoxelVolume3D] = []
	for body in bodies.values():
		var nearby_volume := _find_voxel_volume(body)
		if nearby_volume != null and nearby_volume.voxel_data != null:
			if not neighbor_volumes.has(nearby_volume):
				neighbor_volumes.append(nearby_volume)

	if voxel_action == VoxelAction.CLEAR:
		for body in bodies.values():
			var volume := _find_voxel_volume(body)
			if volume == null or volume.voxel_data == null:
				continue
			var clear_positions := _collect_geometric_positions(
				volume,
				geometry.center,
				geometry.world_radius,
				geometry.half_extents,
				geometry.shape_basis
			)
			var clear_surface := _collect_exposed_surface(
				volume,
				clear_positions,
				neighbor_volumes
			)
			chunks.append({
				"volume": volume,
				"positions": clear_surface.positions,
				"face_masks": clear_surface.face_masks,
				"empty_neighbor_faces": true,
				"neighbor_volumes": neighbor_volumes,
			})
		return chunks

	var brush_cells := _collect_grid_aligned_brush_cells(hit_volume, geometry)
	chunks.append({
		"volume": hit_volume,
		"positions": _filter_empty_preview_cells(
			hit_volume,
			brush_cells,
			neighbor_volumes
		),
		"empty_neighbor_faces": false,
		"neighbor_volumes": neighbor_volumes,
	})
	return chunks


func _collect_exposed_surface(
	volume: VoxelVolume3D,
	positions: Array[Vector3i],
	neighbor_volumes: Array[VoxelVolume3D]
) -> Dictionary:
	var data: VoxelShapeData = volume.voxel_data
	var dimensions: Vector3i = data.dimensions
	var dense_voxels: PackedByteArray = data.get_voxel_data()
	var voxel_size: float = data.voxel_size
	var surface_positions: Array[Vector3i] = []
	var face_masks := {}
	var row_size := dimensions.x
	var plane_size := dimensions.x * dimensions.y

	for cell in positions:
		var face_mask := 0
		var voxel_index := cell.x + cell.y * row_size + cell.z * plane_size

		if cell.x > 0:
			if dense_voxels[voxel_index - 1] == 0:
				face_mask |= 1 << 0
		elif not _outside_neighbor_is_solid(
			volume, cell + Vector3i.LEFT, voxel_size, neighbor_volumes
		):
			face_mask |= 1 << 0

		if cell.x + 1 < dimensions.x:
			if dense_voxels[voxel_index + 1] == 0:
				face_mask |= 1 << 1
		elif not _outside_neighbor_is_solid(
			volume, cell + Vector3i.RIGHT, voxel_size, neighbor_volumes
		):
			face_mask |= 1 << 1

		if cell.y > 0:
			if dense_voxels[voxel_index - row_size] == 0:
				face_mask |= 1 << 2
		elif not _outside_neighbor_is_solid(
			volume, cell + Vector3i.DOWN, voxel_size, neighbor_volumes
		):
			face_mask |= 1 << 2

		if cell.y + 1 < dimensions.y:
			if dense_voxels[voxel_index + row_size] == 0:
				face_mask |= 1 << 3
		elif not _outside_neighbor_is_solid(
			volume, cell + Vector3i.UP, voxel_size, neighbor_volumes
		):
			face_mask |= 1 << 3

		if cell.z > 0:
			if dense_voxels[voxel_index - plane_size] == 0:
				face_mask |= 1 << 4
		elif not _outside_neighbor_is_solid(
			volume, cell + Vector3i.FORWARD, voxel_size, neighbor_volumes
		):
			face_mask |= 1 << 4

		if cell.z + 1 < dimensions.z:
			if dense_voxels[voxel_index + plane_size] == 0:
				face_mask |= 1 << 5
		elif not _outside_neighbor_is_solid(
			volume, cell + Vector3i.BACK, voxel_size, neighbor_volumes
		):
			face_mask |= 1 << 5

		if face_mask != 0:
			surface_positions.append(cell)
			face_masks[cell] = face_mask

	return {
		"positions": surface_positions,
		"face_masks": face_masks,
	}


func _outside_neighbor_is_solid(
	volume: VoxelVolume3D,
	neighbor: Vector3i,
	voxel_size: float,
	neighbor_volumes: Array[VoxelVolume3D]
) -> bool:
	var neighbor_world := volume.to_global(
		(Vector3(neighbor) + Vector3.ONE * 0.5) * voxel_size
	)
	return _is_world_position_solid(neighbor_world, neighbor_volumes)


func _filter_empty_preview_cells(
	grid_volume: VoxelVolume3D,
	positions: Array[Vector3i],
	neighbor_volumes: Array[VoxelVolume3D]
) -> Array[Vector3i]:
	var voxel_size: float = grid_volume.voxel_data.voxel_size
	var empty_positions: Array[Vector3i] = []
	for cell in positions:
		var world_center := grid_volume.to_global(
			(Vector3(cell) + Vector3.ONE * 0.5) * voxel_size
		)
		if not _is_world_position_solid(world_center, neighbor_volumes):
			empty_positions.append(cell)
	return empty_positions


func _is_world_position_solid(
	world_position: Vector3,
	volumes: Array[VoxelVolume3D]
) -> bool:
	for volume in volumes:
		if volume == null or volume.voxel_data == null:
			continue
		var cell := volume.world_to_voxel(world_position)
		if (
			_is_inside(cell, volume.voxel_data.dimensions)
			and volume.get_voxel(cell) != 0
		):
			return true
	return false


func _collect_grid_aligned_brush_cells(
	volume: VoxelVolume3D,
	geometry: Dictionary
) -> Array[Vector3i]:
	var voxel_size: float = volume.voxel_data.voxel_size
	var center: Vector3 = geometry.center
	var half_extents: Vector3 = geometry.half_extents
	var shape_basis: Basis = geometry.shape_basis
	var world_radius: float = geometry.world_radius
	var local_center := volume.to_local(center) / voxel_size - Vector3.ONE * 0.5
	var bounding_radius := world_radius
	if shape_mode == ShapeMode.CUBOID:
		bounding_radius = half_extents.length()
	var cell_radius := ceili(bounding_radius / voxel_size) + 1
	var center_cell := Vector3i(
		roundi(local_center.x),
		roundi(local_center.y),
		roundi(local_center.z)
	)
	var radius_squared := world_radius * world_radius
	var positions: Array[Vector3i] = []
	var inverse_shape_basis := shape_basis.inverse()
	var volume_transform := volume.global_transform
	var volume_basis := volume_transform.basis
	var volume_origin := volume_transform.origin
	var x_step := volume_basis.x * voxel_size
	var minimum_x := center_cell.x - cell_radius
	var maximum_x := center_cell.x + cell_radius

	for z in range(center_cell.z - cell_radius, center_cell.z + cell_radius + 1):
		for y in range(center_cell.y - cell_radius, center_cell.y + cell_radius + 1):
			var cell_center := (
				volume_basis * (
					(Vector3(minimum_x, y, z) + Vector3.ONE * 0.5) * voxel_size
				)
				+ volume_origin
			)
			for x in range(minimum_x, maximum_x + 1):
				var cell := Vector3i(x, y, z)
				var offset := cell_center - center
				var is_inside := offset.length_squared() <= radius_squared
				if shape_mode == ShapeMode.CUBOID:
					offset = inverse_shape_basis * offset
					is_inside = (
						absf(offset.x) <= half_extents.x
						and absf(offset.y) <= half_extents.y
						and absf(offset.z) <= half_extents.z
					)
				if is_inside:
					positions.append(cell)
				cell_center += x_step

	return positions


func _build_preview_meshes(chunks: Array[Dictionary]) -> void:
	var voxel_count := 0
	for chunk in chunks:
		var positions: Array[Vector3i] = chunk.positions
		voxel_count += positions.size()

	if voxel_count == 0:
		_clear_preview_meshes()
		return
	if voxel_count > max_preview_voxels:
		_clear_preview_meshes()
		if not _preview_limit_reported:
			push_warning(
				"Voxel shape preview skipped: %d voxels exceeds the %d voxel limit."
				% [voxel_count, max_preview_voxels]
			)
			_preview_limit_reported = true
		return
	_preview_limit_reported = false

	if not draw_preview_outline and not draw_preview_fill:
		_clear_preview_meshes()
		return

	var fill_vertices := PackedVector3Array()
	var fill_uvs := PackedVector2Array()
	var fill_indices := PackedInt32Array()
	var feature_edges := {}
	var neighbor_volumes: Array[VoxelVolume3D] = []
	for chunk in chunks:
		var chunk_neighbors: Array = chunk.get("neighbor_volumes", [chunk.volume])
		for neighbor in chunk_neighbors:
			var neighbor_volume := neighbor as VoxelVolume3D
			if neighbor_volume != null and not neighbor_volumes.has(neighbor_volume):
				neighbor_volumes.append(neighbor_volume)
	for chunk in chunks:
		_append_exterior_geometry(
			chunk.volume,
			chunk.positions,
			chunk.get("empty_neighbor_faces", false),
			chunk.get("face_masks", {}),
			neighbor_volumes,
			fill_vertices,
			fill_uvs,
			fill_indices,
			feature_edges
		)

	var outline_vertices := _collect_feature_edge_vertices(feature_edges)
	if fill_vertices.is_empty() and outline_vertices.is_empty():
		_clear_preview_meshes()
		return

	var mesh := ArrayMesh.new()
	if draw_preview_fill and not fill_vertices.is_empty():
		var fill_arrays := []
		fill_arrays.resize(Mesh.ARRAY_MAX)
		fill_arrays[Mesh.ARRAY_VERTEX] = fill_vertices
		fill_arrays[Mesh.ARRAY_TEX_UV] = fill_uvs
		fill_arrays[Mesh.ARRAY_INDEX] = fill_indices
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, fill_arrays)
		mesh.surface_set_material(mesh.get_surface_count() - 1, _create_fill_material())

	if draw_preview_outline and not outline_vertices.is_empty():
		var outline_arrays := []
		outline_arrays.resize(Mesh.ARRAY_MAX)
		outline_arrays[Mesh.ARRAY_VERTEX] = outline_vertices
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, outline_arrays)
		mesh.surface_set_material(mesh.get_surface_count() - 1, _create_outline_material())

	_shape_preview.mesh = mesh
	_shape_preview.visible = true


func _append_exterior_geometry(
	volume: VoxelVolume3D,
	positions: Array[Vector3i],
	empty_neighbor_faces: bool,
	face_masks: Dictionary,
	neighbor_volumes: Array[VoxelVolume3D],
	fill_vertices: PackedVector3Array,
	fill_uvs: PackedVector2Array,
	fill_indices: PackedInt32Array,
	feature_edges: Dictionary
) -> void:
	var selected_cells := {}
	if face_masks.is_empty():
		for cell in positions:
			selected_cells[cell] = true

	var voxel_size: float = volume.voxel_data.voxel_size
	var expansion := voxel_size * preview_expansion
	for cell in positions:
		for face_index in range(FACE_DIRECTIONS.size()):
			var direction := FACE_DIRECTIONS[face_index]
			var neighbor := cell + direction
			if not face_masks.is_empty():
				var face_mask: int = face_masks.get(cell, 0)
				if (face_mask & (1 << face_index)) == 0:
					continue
			elif empty_neighbor_faces:
				var neighbor_world := volume.to_global(
					(Vector3(neighbor) + Vector3.ONE * 0.5) * voxel_size
				)
				if _is_world_position_solid(neighbor_world, neighbor_volumes):
					continue
			elif selected_cells.has(neighbor):
				continue

			var face_world := PackedVector3Array()
			face_world.resize(4)
			for corner_index in range(4):
				var grid_corner := cell + CUBE_CORNERS[FACE_CORNERS[face_index][corner_index]]
				var local_corner := Vector3(grid_corner) * voxel_size
				face_world[corner_index] = volume.to_global(local_corner)
			var world_normal := (
				volume.global_transform.basis * Vector3(direction)
			).normalized()

			if draw_preview_fill:
				var expanded_face := PackedVector3Array()
				expanded_face.resize(4)
				for corner_index in range(4):
					expanded_face[corner_index] = (
						face_world[corner_index] + world_normal * expansion
					)
				_append_fill_face(expanded_face, fill_vertices, fill_uvs, fill_indices)

			if draw_preview_outline:
				for edge in QUAD_EDGES:
					_record_feature_edge(
						feature_edges,
						face_world[edge.x],
						face_world[edge.y],
						world_normal,
						expansion
					)


func _append_fill_face(
	face_world: PackedVector3Array,
	vertices: PackedVector3Array,
	uvs: PackedVector2Array,
	indices: PackedInt32Array
) -> void:
	const FACE_UVS := [Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1)]
	var first_vertex := vertices.size()
	for index in range(4):
		vertices.append(face_world[index])
		uvs.append(FACE_UVS[index])
	indices.append(first_vertex)
	indices.append(first_vertex + 1)
	indices.append(first_vertex + 2)
	indices.append(first_vertex)
	indices.append(first_vertex + 2)
	indices.append(first_vertex + 3)


func _record_feature_edge(
	edges: Dictionary,
	point_a: Vector3,
	point_b: Vector3,
	normal: Vector3,
	expansion: float
) -> void:
	var snapped_a := point_a.snapped(Vector3.ONE * 0.0001)
	var snapped_b := point_b.snapped(Vector3.ONE * 0.0001)
	var hash_a: int = hash(snapped_a)
	var hash_b: int = hash(snapped_b)
	var key := Vector2i(mini(hash_a, hash_b), maxi(hash_a, hash_b))
	var record: Dictionary = edges.get(key, {
		"a": point_a,
		"b": point_b,
		"normals": {},
		"face_count": 0,
		"expansion": expansion,
	})
	var normals: Dictionary = record.normals
	normals[hash(normal.snapped(Vector3.ONE * 0.001))] = normal
	record.normals = normals
	record.face_count = int(record.face_count) + 1
	record.expansion = maxf(record.expansion, expansion)
	edges[key] = record


func _collect_feature_edge_vertices(edges: Dictionary) -> PackedVector3Array:
	var line_groups := {}
	for record in edges.values():
		var normals: Dictionary = record.normals
		if normals.size() < 2 and int(record.face_count) > 1:
			continue
		var normal_sum := Vector3.ZERO
		for normal in normals.values():
			normal_sum += normal
		var offset := Vector3.ZERO
		if not normal_sum.is_zero_approx():
			offset = normal_sum.normalized() * float(record.expansion)
		var point_a: Vector3 = record.a + offset
		var point_b: Vector3 = record.b + offset
		var direction := (point_b - point_a).normalized()
		if _direction_needs_flip(direction):
			direction = -direction
		var line_origin := point_a - direction * point_a.dot(direction)
		var key := Vector2i(
			hash(direction.snapped(Vector3.ONE * 0.0001)),
			hash(line_origin.snapped(Vector3.ONE * 0.0001))
		)
		var group: Dictionary = line_groups.get(key, {
			"direction": direction,
			"origin": line_origin,
			"intervals": [],
		})
		var intervals: Array = group.intervals
		var distance_a := point_a.dot(direction)
		var distance_b := point_b.dot(direction)
		intervals.append(Vector2(
			minf(distance_a, distance_b),
			maxf(distance_a, distance_b)
		))
		group.intervals = intervals
		line_groups[key] = group

	var vertices := PackedVector3Array()
	for group in line_groups.values():
		var intervals: Array = group.intervals
		intervals.sort_custom(func(a: Vector2, b: Vector2) -> bool: return a.x < b.x)
		var merged_start: float = intervals[0].x
		var merged_end: float = intervals[0].y
		for index in range(1, intervals.size()):
			var interval: Vector2 = intervals[index]
			if interval.x <= merged_end + 0.0002:
				merged_end = maxf(merged_end, interval.y)
			else:
				_append_merged_line(vertices, group, merged_start, merged_end)
				merged_start = interval.x
				merged_end = interval.y
		_append_merged_line(vertices, group, merged_start, merged_end)
	return vertices


func _direction_needs_flip(direction: Vector3) -> bool:
	if not is_zero_approx(direction.x):
		return direction.x < 0.0
	if not is_zero_approx(direction.y):
		return direction.y < 0.0
	return direction.z < 0.0


func _append_merged_line(
	vertices: PackedVector3Array,
	group: Dictionary,
	start_distance: float,
	end_distance: float
) -> void:
	var origin: Vector3 = group.origin
	var direction: Vector3 = group.direction
	vertices.append(origin + direction * start_distance)
	vertices.append(origin + direction * end_distance)


func _create_fill_material() -> StandardMaterial3D:
	var material := _fill_material
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.albedo_color = preview_fill_color
	material.no_depth_test = preview_xray
	material.depth_draw_mode = BaseMaterial3D.DEPTH_DRAW_ALWAYS
	material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	if use_preview_texture and preview_texture != null:
		material.albedo_texture = preview_texture
	return material


func _create_outline_material() -> StandardMaterial3D:
	var material := _outline_material
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_color = preview_outline_color
	material.no_depth_test = preview_xray
	return material


func _hide_shape_preview() -> void:
	if _shape_preview != null:
		_shape_preview.visible = false


func _clear_preview_meshes() -> void:
	if _shape_preview != null:
		_shape_preview.mesh = null
		_shape_preview.visible = false


func _make_volume_unique_with_edits(
	volume: VoxelVolume3D,
	body: CollisionObject3D,
	positions: Array[Vector3i],
	new_values: PackedByteArray
) -> void:
	var previous_data: VoxelShapeData = volume.voxel_data
	if previous_data == null:
		return

	var unique_data := previous_data.duplicate(true) as VoxelShapeData
	if unique_data == null:
		return

	# Apply the first local edit before attaching the copy. The volume therefore
	# observes one assignment/rebuild containing the final edited voxel data.
	unique_data.apply_voxel_edits(positions, new_values)
	volume.voxel_data = unique_data

	_sync_collision_data(body, previous_data, unique_data)
	if body.has_meta("voxel_data"):
		body.set_meta("voxel_data", unique_data)

	volume.set_meta(UNIQUE_META, true)


func _sync_collision_data(
	node: Node,
	previous_data: VoxelShapeData,
	unique_data: VoxelShapeData
) -> void:
	if node is CollisionShape3D:
		var collision := node as CollisionShape3D
		if collision.shape is VoxelShape3D:
			var old_shape := collision.shape as VoxelShape3D
			if old_shape.voxel_data == previous_data:
				var new_shape := old_shape.duplicate(false) as VoxelShape3D
				new_shape.voxel_data = unique_data
				collision.shape = new_shape

	for child in node.get_children():
		_sync_collision_data(child, previous_data, unique_data)


func _find_voxel_volume(node: Node) -> VoxelVolume3D:
	if node == null:
		return null
	if node is VoxelVolume3D:
		return node

	for child in node.get_children():
		var result := _find_voxel_volume(child)
		if result != null:
			return result
	return null


func _build_player_exclusions() -> Array[RID]:
	var exclusions: Array[RID] = []
	var node: Node = get_parent()
	while node != null:
		if node is CollisionObject3D:
			exclusions.append((node as CollisionObject3D).get_rid())
			break
		node = node.get_parent()
	return exclusions
