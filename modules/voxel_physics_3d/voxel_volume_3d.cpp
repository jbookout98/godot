#include "voxel_volume_3d.h"

#include "core/object/class_db.h"
#include "servers/rendering_server.h"
#include "voxel_volume_streaming_manager.h"

void VoxelVolume3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_voxel_data", "data"), &VoxelVolume3D::set_voxel_data);
	ClassDB::bind_method(D_METHOD("get_voxel_data"), &VoxelVolume3D::get_voxel_data);
	ClassDB::bind_method(D_METHOD("set_voxel_material", "material"), &VoxelVolume3D::set_voxel_material);
	ClassDB::bind_method(D_METHOD("get_voxel_material"), &VoxelVolume3D::get_voxel_material);
	ClassDB::bind_method(D_METHOD("get_voxel", "position"), &VoxelVolume3D::get_voxel);
	ClassDB::bind_method(D_METHOD("set_voxel", "position", "palette_index"), &VoxelVolume3D::set_voxel);
	ClassDB::bind_method(D_METHOD("apply_voxel_edits", "positions", "palette_indices"), &VoxelVolume3D::apply_voxel_edits);
	ClassDB::bind_method(D_METHOD("apply_voxel_edits_by_index", "indices", "palette_indices"), &VoxelVolume3D::apply_voxel_edits_by_index);
	ClassDB::bind_method(D_METHOD("fill_voxel_region", "position", "size", "palette_index"), &VoxelVolume3D::fill_voxel_region);
	ClassDB::bind_method(D_METHOD("local_to_voxel", "local_position"), &VoxelVolume3D::local_to_voxel);
	ClassDB::bind_method(D_METHOD("world_to_voxel", "world_position"), &VoxelVolume3D::world_to_voxel);
	ClassDB::bind_method(D_METHOD("voxel_to_local", "voxel", "center"), &VoxelVolume3D::voxel_to_local, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("voxel_to_world", "voxel", "center"), &VoxelVolume3D::voxel_to_world, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("make_voxel_data_unique"), &VoxelVolume3D::make_voxel_data_unique);
	ClassDB::bind_method(D_METHOD("set_streaming_mode", "mode"), &VoxelVolume3D::set_streaming_mode);
	ClassDB::bind_method(D_METHOD("get_streaming_mode"), &VoxelVolume3D::get_streaming_mode);
	ClassDB::bind_method(D_METHOD("set_streaming_distance", "distance"), &VoxelVolume3D::set_streaming_distance);
	ClassDB::bind_method(D_METHOD("get_streaming_distance"), &VoxelVolume3D::get_streaming_distance);
	ClassDB::bind_method(D_METHOD("set_streaming_resident", "resident"), &VoxelVolume3D::set_streaming_resident);
	ClassDB::bind_method(D_METHOD("is_streaming_resident"), &VoxelVolume3D::is_streaming_resident);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "voxel_data", PROPERTY_HINT_RESOURCE_TYPE, "VoxelShapeData"), "set_voxel_data", "get_voxel_data");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "voxel_material", PROPERTY_HINT_RESOURCE_TYPE, "VoxelMaterial"), "set_voxel_material", "get_voxel_material");
	ADD_GROUP("Streaming", "streaming_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "streaming_mode", PROPERTY_HINT_ENUM, "Automatic,Always Resident,Manual"), "set_streaming_mode", "get_streaming_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "streaming_distance", PROPERTY_HINT_RANGE, "0,100000,1,or_greater,suffix:m"), "set_streaming_distance", "get_streaming_distance");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "streaming_resident"), "set_streaming_resident", "is_streaming_resident");
	BIND_ENUM_CONSTANT(STREAMING_AUTOMATIC);
	BIND_ENUM_CONSTANT(STREAMING_ALWAYS_RESIDENT);
	BIND_ENUM_CONSTANT(STREAMING_MANUAL);
}

Ref<ImageTexture3D> VoxelVolume3D::_create_texture_3d(
		const PackedByteArray &p_bytes,
		const Vector3i &p_dimensions,
		Image::Format p_format,
		int p_bytes_per_pixel) const {
	if (p_dimensions.x <= 0 || p_dimensions.y <= 0 || p_dimensions.z <= 0 ||
			p_bytes.size() != p_dimensions.x * p_dimensions.y * p_dimensions.z * p_bytes_per_pixel) {
		return Ref<ImageTexture3D>();
	}

	Vector<Ref<Image>> slices;
	slices.resize(p_dimensions.z);
	const int slice_size = p_dimensions.x * p_dimensions.y * p_bytes_per_pixel;
	for (int z = 0; z < p_dimensions.z; z++) {
		PackedByteArray slice;
		slice.resize(slice_size);
		for (int i = 0; i < slice_size; i++) {
			slice.set(i, p_bytes[z * slice_size + i]);
		}
		slices.set(z, Image::create_from_data(
				p_dimensions.x, p_dimensions.y, false, p_format, slice));
	}

	Ref<ImageTexture3D> texture;
	texture.instantiate();
	if (texture->create(
				p_format,
				p_dimensions.x,
				p_dimensions.y,
				p_dimensions.z,
				false,
				slices) != OK) {
		return Ref<ImageTexture3D>();
	}
	return texture;
}

void VoxelVolume3D::_ensure_fallback_textures() {
	if (fallback_palette.is_null()) {
		Ref<Image> image = Image::create_empty(256, 1, false, Image::FORMAT_RGBA8);
		image->fill(Color(1, 1, 1, 1));
		fallback_palette = ImageTexture::create_from_image(image);
	}
	if (fallback_material.is_null()) {
		Ref<Image> image = Image::create_empty(256, 1, false, Image::FORMAT_RGBA8);
		image->fill(Color(1, 0, 0, 1));
		fallback_material = ImageTexture::create_from_image(image);
	}
	if (fallback_neighbor_face_texture.is_null()) {
		PackedByteArray empty_faces;
		empty_faces.resize(NEIGHBOR_FACE_COUNT);
		empty_faces.fill(0);
		fallback_neighbor_face_texture = _create_texture_3d(
				empty_faces, Vector3i(1, 1, NEIGHBOR_FACE_COUNT), Image::FORMAT_L8, 1);
	}
}

void VoxelVolume3D::_rebuild_volume_textures() {
	mixed_brick_atlas.unref();
	brick_directory_texture.unref();
	if (!streaming_resident) {
		RenderingServer::get_singleton()->mesh_clear(procedural_surface);
		return;
	}
	if (voxel_data.is_null()) {
		rendered_revision = UINT64_MAX;
		local_aabb = AABB();
		_rebuild_procedural_surface();
		return;
	}

	const Vector3i dimensions = voxel_data->get_dimensions();
	const VoxelBrickStorage &storage = voxel_data->get_brick_storage();
	const Vector3i brick_dimensions = storage.get_brick_dimensions();
	int mixed_count = 0;
	for (int index = 0; index < storage.get_brick_count(); index++) {
		mixed_count += storage.get_brick(index).type == VoxelBrickStorage::BRICK_MIXED ? 1 : 0;
	}
	int atlas_side = 1;
	while (atlas_side * atlas_side * atlas_side < mixed_count) {
		atlas_side++;
	}
	atlas_brick_dimensions = Vector3i(atlas_side, atlas_side,
			MAX(1, (mixed_count + atlas_side * atlas_side - 1) / (atlas_side * atlas_side)));
	const Vector3i atlas_dimensions = atlas_brick_dimensions * VoxelBrickStorage::BRICK_SIZE;
	PackedByteArray atlas_bytes;
	atlas_bytes.resize(atlas_dimensions.x * atlas_dimensions.y * atlas_dimensions.z);
	atlas_bytes.fill(0);
	PackedByteArray directory_bytes;
	directory_bytes.resize(storage.get_brick_count() * 4);
	directory_bytes.fill(0);

	int mixed_slot = 0;
	for (int brick_index = 0; brick_index < storage.get_brick_count(); brick_index++) {
		const VoxelBrickStorage::Brick &brick = storage.get_brick(brick_index);
		uint32_t directory_code = 0;
		if (brick.type == VoxelBrickStorage::BRICK_UNIFORM) {
			directory_code = 1;
			directory_bytes.set(brick_index * 4 + 3, brick.uniform_value);
		} else if (brick.type == VoxelBrickStorage::BRICK_MIXED) {
			directory_code = uint32_t(mixed_slot + 2);
			const Vector3i atlas_brick(
					mixed_slot % atlas_brick_dimensions.x,
					(mixed_slot / atlas_brick_dimensions.x) % atlas_brick_dimensions.y,
					mixed_slot / (atlas_brick_dimensions.x * atlas_brick_dimensions.y));
			const Vector3i atlas_origin = atlas_brick * VoxelBrickStorage::BRICK_SIZE;
			for (int z = 0; z < VoxelBrickStorage::BRICK_SIZE; z++) {
				for (int y = 0; y < VoxelBrickStorage::BRICK_SIZE; y++) {
					for (int x = 0; x < VoxelBrickStorage::BRICK_SIZE; x++) {
						const int local_index = x + y * VoxelBrickStorage::BRICK_SIZE + z * VoxelBrickStorage::BRICK_SIZE * VoxelBrickStorage::BRICK_SIZE;
						const Vector3i atlas_position = atlas_origin + Vector3i(x, y, z);
						const int atlas_index = atlas_position.x + atlas_position.y * atlas_dimensions.x + atlas_position.z * atlas_dimensions.x * atlas_dimensions.y;
						atlas_bytes.set(atlas_index, brick.mixed_values[local_index]);
					}
				}
			}
			mixed_slot++;
		}
		directory_bytes.set(brick_index * 4 + 0, uint8_t(directory_code & 0xFF));
		directory_bytes.set(brick_index * 4 + 1, uint8_t((directory_code >> 8) & 0xFF));
		directory_bytes.set(brick_index * 4 + 2, uint8_t((directory_code >> 16) & 0xFF));
	}
	mixed_brick_atlas = _create_texture_3d(atlas_bytes, atlas_dimensions, Image::FORMAT_L8, 1);
	brick_directory_texture = _create_texture_3d(directory_bytes, brick_dimensions, Image::FORMAT_RGBA8, 4);
	rendered_revision = voxel_data->get_revision();

	const Vector3 size = Vector3(dimensions) * voxel_data->get_voxel_size();
	// Match VoxelShape3D: voxel (0,0,0) begins at the node origin.
	local_aabb = AABB(Vector3(), size);
	_rebuild_procedural_surface();
	_update_material_bindings();
	update_gizmos();
}

void VoxelVolume3D::_rebuild_procedural_surface() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	rendering_server->mesh_clear(procedural_surface);
	if (voxel_data.is_null()) {
		return;
	}

	RenderingServer::SurfaceData surface;
	surface.format = RenderingServer::ARRAY_FLAG_USES_EMPTY_VERTEX_ARRAY |
			RenderingServer::ARRAY_FLAG_FORMAT_CURRENT_VERSION;
	surface.primitive = RenderingServer::PRIMITIVE_TRIANGLES;
	surface.vertex_count = 36;
	surface.aabb = local_aabb;
	rendering_server->mesh_add_surface(procedural_surface, surface);
	if (runtime_material.is_valid()) {
		rendering_server->mesh_surface_set_material(
				procedural_surface, 0, runtime_material->get_rid());
	}
}

void VoxelVolume3D::_update_material_bindings() {
	if (voxel_data.is_null() || runtime_material.is_null()) {
		return;
	}
	_ensure_fallback_textures();
	const bool has_metallic = voxel_data->get_metallic_texture().is_valid();
	const bool has_transparency = voxel_data->get_transparency_texture().is_valid();
	const bool has_specularity = voxel_data->get_specularity_texture().is_valid();
	const bool has_emission = voxel_data->get_emission_texture().is_valid();
	runtime_material->set_transparency_enabled(has_transparency);
	const Vector3i dimensions = voxel_data->get_dimensions();
	const Vector3i brick_dimensions(
			(dimensions.x + 7) / 8,
			(dimensions.y + 7) / 8,
			(dimensions.z + 7) / 8);
	runtime_material->set_shader_parameter("u_voxels", mixed_brick_atlas);
	runtime_material->set_shader_parameter("u_bricks", brick_directory_texture);
	runtime_material->set_shader_parameter(
			"u_neighbor_faces",
			neighbor_face_texture.is_valid() ? neighbor_face_texture : fallback_neighbor_face_texture);
	runtime_material->set_shader_parameter("u_neighbor_mask", neighbor_mask);
	runtime_material->set_shader_parameter(
			"u_palette",
			voxel_data->get_palette_texture().is_valid() ?
					voxel_data->get_palette_texture() : fallback_palette);
	runtime_material->set_shader_parameter(
			"u_material",
			voxel_data->get_material_texture().is_valid() ?
					voxel_data->get_material_texture() : fallback_material);
	runtime_material->set_shader_parameter("u_metallic", has_metallic ? voxel_data->get_metallic_texture() : fallback_material);
	runtime_material->set_shader_parameter("u_specularity", has_specularity ? voxel_data->get_specularity_texture() : fallback_material);
	runtime_material->set_shader_parameter("u_emission", has_emission ? voxel_data->get_emission_texture() : fallback_material);
	runtime_material->set_shader_parameter("u_has_metallic", has_metallic);
	runtime_material->set_shader_parameter("u_has_specularity", has_specularity);
	runtime_material->set_shader_parameter("u_has_emission", has_emission);
	if (has_transparency) runtime_material->set_shader_parameter("u_transparency", voxel_data->get_transparency_texture());
	runtime_material->set_shader_parameter("u_volume_dims", dimensions);
	runtime_material->set_shader_parameter("u_brick_dims", brick_dimensions);
	runtime_material->set_shader_parameter("u_atlas_brick_dims", atlas_brick_dimensions);
	runtime_material->set_shader_parameter(
			"u_volume_size", Vector3(dimensions) * voxel_data->get_voxel_size());
	runtime_material->set_shader_parameter("u_voxel_size", voxel_data->get_voxel_size());
}

void VoxelVolume3D::_rebuild_runtime_material() {
	runtime_material.instantiate();
	if (voxel_material.is_valid()) {
		runtime_material->set_shading_mode(voxel_material->get_shading_mode());
		runtime_material->set_emission_energy(voxel_material->get_emission_energy());
	}
	runtime_material->ensure_shader();
}

void VoxelVolume3D::_voxel_data_changed() {
	if (VoxelVolumeStreamingManager::get_singleton() != nullptr) {
		VoxelVolumeStreamingManager::get_singleton()->mark_neighbors_dirty();
	}
	if (voxel_data.is_null()) {
		_rebuild_volume_textures();
		return;
	}
	// Voxel mutations arrive through voxels_changed first. A plain changed
	// notification means palette, material binding, voxel size, or feature data.
	if (voxel_data->get_revision() != rendered_revision) {
		return;
	}
	local_aabb = AABB(Vector3(), Vector3(voxel_data->get_dimensions()) * voxel_data->get_voxel_size());
	_rebuild_procedural_surface();
	_update_material_bindings();
	update_gizmos();
}

void VoxelVolume3D::_voxel_data_voxels_changed(const Vector3i &p_position, const Vector3i &p_size, int64_t p_revision) {
	if (streaming_resident) {
		_rebuild_volume_textures();
	}
	if (VoxelVolumeStreamingManager::get_singleton() != nullptr) {
		VoxelVolumeStreamingManager::get_singleton()->mark_neighbors_dirty();
	}
}

void VoxelVolume3D::_voxel_material_changed() {
	_rebuild_runtime_material();
	_rebuild_procedural_surface();
	_update_material_bindings();
}

void VoxelVolume3D::set_voxel_data(const Ref<VoxelShapeData> &p_data) {
	if (voxel_data == p_data) return;
	if (voxel_data.is_valid()) {
		voxel_data->disconnect_changed(callable_mp(this, &VoxelVolume3D::_voxel_data_changed));
		voxel_data->disconnect(SNAME("voxels_changed"), callable_mp(this, &VoxelVolume3D::_voxel_data_voxels_changed));
	}
	voxel_data = p_data;
	if (voxel_data.is_valid()) {
		voxel_data->connect_changed(callable_mp(this, &VoxelVolume3D::_voxel_data_changed));
		voxel_data->connect(SNAME("voxels_changed"), callable_mp(this, &VoxelVolume3D::_voxel_data_voxels_changed));
	}
	_rebuild_volume_textures();
	if (VoxelVolumeStreamingManager::get_singleton() != nullptr) {
		VoxelVolumeStreamingManager::get_singleton()->mark_neighbors_dirty();
	}
}

Ref<VoxelShapeData> VoxelVolume3D::get_voxel_data() const { return voxel_data; }

int VoxelVolume3D::get_voxel(const Vector3i &p_position) const {
	return voxel_data.is_valid() ? voxel_data->get_voxel(p_position) : 0;
}

bool VoxelVolume3D::set_voxel(const Vector3i &p_position, int p_palette_index) {
	ERR_FAIL_COND_V_MSG(voxel_data.is_null(), false, "VoxelVolume3D has no VoxelShapeData.");
	return voxel_data->set_voxel(p_position, p_palette_index);
}

int VoxelVolume3D::apply_voxel_edits(const Array &p_positions, const PackedByteArray &p_palette_indices) {
	ERR_FAIL_COND_V_MSG(voxel_data.is_null(), 0, "VoxelVolume3D has no VoxelShapeData.");
	return voxel_data->apply_voxel_edits(p_positions, p_palette_indices);
}

int VoxelVolume3D::apply_voxel_edits_by_index(const PackedInt32Array &p_indices, const PackedByteArray &p_palette_indices) {
	ERR_FAIL_COND_V_MSG(voxel_data.is_null(), 0, "VoxelVolume3D has no VoxelShapeData.");
	return voxel_data->apply_voxel_edits_by_index(p_indices, p_palette_indices);
}

int VoxelVolume3D::fill_voxel_region(const Vector3i &p_position, const Vector3i &p_size, int p_palette_index) {
	ERR_FAIL_COND_V_MSG(voxel_data.is_null(), 0, "VoxelVolume3D has no VoxelShapeData.");
	return voxel_data->fill_voxel_region(p_position, p_size, p_palette_index);
}

Vector3i VoxelVolume3D::local_to_voxel(const Vector3 &p_local_position) const {
	ERR_FAIL_COND_V_MSG(voxel_data.is_null(), Vector3i(), "VoxelVolume3D has no VoxelShapeData.");
	const real_t size = voxel_data->get_voxel_size();
	return Vector3i(Math::floor(p_local_position.x / size), Math::floor(p_local_position.y / size), Math::floor(p_local_position.z / size));
}

Vector3i VoxelVolume3D::world_to_voxel(const Vector3 &p_world_position) const {
	return local_to_voxel(to_local(p_world_position));
}

Vector3 VoxelVolume3D::voxel_to_local(const Vector3i &p_voxel, bool p_center) const {
	ERR_FAIL_COND_V_MSG(voxel_data.is_null(), Vector3(), "VoxelVolume3D has no VoxelShapeData.");
	return (Vector3(p_voxel) + (p_center ? Vector3(0.5, 0.5, 0.5) : Vector3())) * voxel_data->get_voxel_size();
}

Vector3 VoxelVolume3D::voxel_to_world(const Vector3i &p_voxel, bool p_center) const {
	return to_global(voxel_to_local(p_voxel, p_center));
}

void VoxelVolume3D::make_voxel_data_unique() {
	if (voxel_data.is_null()) {
		return;
	}
	Ref<Resource> duplicated = voxel_data->duplicate(true);
	Ref<VoxelShapeData> unique_data = duplicated;
	set_voxel_data(unique_data);
}

void VoxelVolume3D::set_streaming_mode(StreamingMode p_mode) {
	ERR_FAIL_INDEX(p_mode, 3);
	if (streaming_mode == p_mode) {
		return;
	}
	streaming_mode = p_mode;
	if (streaming_mode == STREAMING_ALWAYS_RESIDENT) {
		set_streaming_resident(true);
	}
}

VoxelVolume3D::StreamingMode VoxelVolume3D::get_streaming_mode() const { return streaming_mode; }

void VoxelVolume3D::set_streaming_distance(real_t p_distance) {
	streaming_distance = MAX(real_t(0.0), p_distance);
}

real_t VoxelVolume3D::get_streaming_distance() const { return streaming_distance; }

void VoxelVolume3D::set_streaming_resident(bool p_resident) {
	if (streaming_mode == STREAMING_ALWAYS_RESIDENT) {
		p_resident = true;
	}
	if (streaming_resident == p_resident) {
		return;
	}
	streaming_resident = p_resident;
	if (streaming_resident) {
		_rebuild_volume_textures();
	} else {
		mixed_brick_atlas.unref();
		brick_directory_texture.unref();
		RenderingServer::get_singleton()->mesh_clear(procedural_surface);
	}
	if (VoxelVolumeStreamingManager::get_singleton() != nullptr) {
		VoxelVolumeStreamingManager::get_singleton()->mark_neighbors_dirty();
	}
}

bool VoxelVolume3D::is_streaming_resident() const { return streaming_resident; }

void VoxelVolume3D::update_neighbor_faces(VoxelVolume3D *const p_neighbors[NEIGHBOR_FACE_COUNT]) {
	const uint64_t self_revision = voxel_data.is_valid() ? voxel_data->get_revision() : UINT64_MAX;
	const Vector3i self_dimensions = voxel_data.is_valid() ? voxel_data->get_dimensions() : Vector3i();
	ObjectID new_ids[NEIGHBOR_FACE_COUNT];
	uint64_t new_revisions[NEIGHBOR_FACE_COUNT];
	bool unchanged = neighbor_self_revision == self_revision && neighbor_self_dimensions == self_dimensions;
	for (int face = 0; face < NEIGHBOR_FACE_COUNT; face++) {
		VoxelVolume3D *neighbor = p_neighbors[face];
		new_ids[face] = neighbor != nullptr ? neighbor->get_instance_id() : ObjectID();
		new_revisions[face] = neighbor != nullptr && neighbor->get_voxel_data().is_valid() ?
				neighbor->get_voxel_data()->get_revision() : UINT64_MAX;
		unchanged = unchanged && neighbor_ids[face] == new_ids[face] && neighbor_revisions[face] == new_revisions[face];
	}
	if (unchanged) {
		return;
	}

	neighbor_self_revision = self_revision;
	neighbor_self_dimensions = self_dimensions;
	neighbor_mask = 0;
	neighbor_face_texture.unref();
	for (int face = 0; face < NEIGHBOR_FACE_COUNT; face++) {
		neighbor_ids[face] = new_ids[face];
		neighbor_revisions[face] = new_revisions[face];
	}
	if (voxel_data.is_null() || !streaming_resident) {
		_update_material_bindings();
		return;
	}

	const Vector3i dimensions = voxel_data->get_dimensions();
	const int side = MAX(dimensions.x, MAX(dimensions.y, dimensions.z));
	PackedByteArray face_bytes;
	face_bytes.resize(side * side * NEIGHBOR_FACE_COUNT);
	face_bytes.fill(0);
	auto set_face_voxel = [&face_bytes, side](int p_face, int p_u, int p_v, int p_value) {
		face_bytes.set(p_u + p_v * side + p_face * side * side, p_value > 0 ? 255 : 0);
	};

	for (int face = 0; face < NEIGHBOR_FACE_COUNT; face++) {
		VoxelVolume3D *neighbor = p_neighbors[face];
		if (neighbor == nullptr || neighbor->get_voxel_data().is_null() || !neighbor->is_streaming_resident()) {
			continue;
		}
		neighbor_mask |= 1 << face;
		const Vector3i neighbor_dimensions = neighbor->get_voxel_data()->get_dimensions();
		if (face == NEIGHBOR_NEGATIVE_X || face == NEIGHBOR_POSITIVE_X) {
			const int neighbor_x = face == NEIGHBOR_NEGATIVE_X ? neighbor_dimensions.x - 1 : 0;
			for (int z = 0; z < dimensions.z; z++) {
				for (int y = 0; y < dimensions.y; y++) {
					set_face_voxel(face, y, z, neighbor->get_voxel(Vector3i(neighbor_x, y, z)));
				}
			}
		} else if (face == NEIGHBOR_NEGATIVE_Y || face == NEIGHBOR_POSITIVE_Y) {
			const int neighbor_y = face == NEIGHBOR_NEGATIVE_Y ? neighbor_dimensions.y - 1 : 0;
			for (int z = 0; z < dimensions.z; z++) {
				for (int x = 0; x < dimensions.x; x++) {
					set_face_voxel(face, x, z, neighbor->get_voxel(Vector3i(x, neighbor_y, z)));
				}
			}
		} else {
			const int neighbor_z = face == NEIGHBOR_NEGATIVE_Z ? neighbor_dimensions.z - 1 : 0;
			for (int y = 0; y < dimensions.y; y++) {
				for (int x = 0; x < dimensions.x; x++) {
					set_face_voxel(face, x, y, neighbor->get_voxel(Vector3i(x, y, neighbor_z)));
				}
			}
		}
	}
	if (neighbor_mask != 0) {
		neighbor_face_texture = _create_texture_3d(
				face_bytes, Vector3i(side, side, NEIGHBOR_FACE_COUNT), Image::FORMAT_L8, 1);
	}
	_update_material_bindings();
}

void VoxelVolume3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		if (VoxelVolumeStreamingManager::get_singleton() != nullptr) {
			VoxelVolumeStreamingManager::get_singleton()->register_volume(this);
		}
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		if (VoxelVolumeStreamingManager::get_singleton() != nullptr) {
			VoxelVolumeStreamingManager::get_singleton()->unregister_volume(this);
		}
	} else if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		if (VoxelVolumeStreamingManager::get_singleton() != nullptr) {
			VoxelVolumeStreamingManager::get_singleton()->mark_neighbors_dirty();
		}
	}
}

void VoxelVolume3D::set_voxel_material(const Ref<VoxelMaterial> &p_material) {
	if (voxel_material == p_material) {
		return;
	}
	if (voxel_material.is_valid()) {
		voxel_material->disconnect_changed(callable_mp(this, &VoxelVolume3D::_voxel_material_changed));
	}

	// Copy only authored configuration. Older scenes may contain a generated
	// Shader and ImageTexture3D uniforms inside p_material; retaining that
	// resource would keep hundreds of runtime texture slices serialized.
	voxel_material.unref();
	if (p_material.is_valid()) {
		voxel_material.instantiate();
		voxel_material->set_shading_mode(p_material->get_shading_mode());
		voxel_material->set_emission_energy(p_material->get_emission_energy());
		voxel_material->connect_changed(callable_mp(this, &VoxelVolume3D::_voxel_material_changed));
	}
	_rebuild_runtime_material();
	_rebuild_procedural_surface();
	_update_material_bindings();
}

Ref<VoxelMaterial> VoxelVolume3D::get_voxel_material() const { return voxel_material; }

AABB VoxelVolume3D::get_aabb() const { return local_aabb; }

VoxelVolume3D::VoxelVolume3D() {
	procedural_surface = RenderingServer::get_singleton()->mesh_create();
	set_base(procedural_surface);
	set_notify_transform(true);
	_rebuild_runtime_material();
}

VoxelVolume3D::~VoxelVolume3D() {
	if (voxel_data.is_valid()) {
		voxel_data->disconnect_changed(callable_mp(this, &VoxelVolume3D::_voxel_data_changed));
		voxel_data->disconnect(SNAME("voxels_changed"), callable_mp(this, &VoxelVolume3D::_voxel_data_voxels_changed));
	}
	if (voxel_material.is_valid()) {
		voxel_material->disconnect_changed(callable_mp(this, &VoxelVolume3D::_voxel_material_changed));
	}
	if (procedural_surface.is_valid() && RenderingServer::get_singleton() != nullptr) {
		RenderingServer::get_singleton()->free(procedural_surface);
	}
}
