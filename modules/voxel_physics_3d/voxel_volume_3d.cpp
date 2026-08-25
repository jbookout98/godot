#include "voxel_volume_3d.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/rendering_method.h"
#include "servers/rendering/renderer_rd/voxel_forward/voxel_forward_volume_storage.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_types.h"
#include "voxel_volume_streaming_manager.h"

namespace {

struct VoxelRuntimeMaterialCacheEntry {
	Ref<VoxelMaterial> material;
	uint32_t users = 0;
};

HashMap<String, VoxelRuntimeMaterialCacheEntry> voxel_runtime_material_cache;

uint64_t _texture_identity(const Ref<Texture2D> &p_texture) {
	return p_texture.is_valid() ? uint64_t(p_texture->get_instance_id()) : 0;
}

int64_t _quantized_absolute_rotation(real_t p_angle) {
	// 1e-5 radians is deterministic and far below a visible orientation change.
	return int64_t(Math::round(Math::abs(Math::wrapf(p_angle, real_t(-Math::PI), real_t(Math::PI))) * real_t(100000.0)));
}

} // namespace

static bool _uses_shadow_proxy() {
	return !RenderingMethod::is_current_voxel_forward_method() &&
			bool(GLOBAL_GET("rendering/voxel_volume/shadow_proxy/enabled"));
}

bool VoxelVolume3D::_uses_voxel_forward_shadow_mask() const {
	if (!RenderingMethod::is_current_voxel_forward_method() ||
			!bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/enabled")) ||
			!bool(GLOBAL_GET("rendering/driver/depth_prepass/enable"))) {
		return false;
	}
	const Basis basis = (is_inside_tree() ? get_global_transform() : get_transform()).basis;
	const Vector3 axis_x = basis.get_column(0);
	const Vector3 axis_y = basis.get_column(1);
	const Vector3 axis_z = basis.get_column(2);
	const float scale = axis_x.length();
	return scale > 0.0001f &&
			Math::is_equal_approx(axis_y.length(), scale) && Math::is_equal_approx(axis_z.length(), scale) &&
			axis_x.normalized().is_equal_approx(Vector3(1, 0, 0)) &&
			axis_y.normalized().is_equal_approx(Vector3(0, 1, 0)) &&
			axis_z.normalized().is_equal_approx(Vector3(0, 0, 1));
}

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
		std::memcpy(slice.ptrw(), p_bytes.ptr() + z * slice_size, slice_size);
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
		empty_faces.resize(NEIGHBOR_TEXTURE_LAYER_COUNT);
		empty_faces.fill(0);
		fallback_neighbor_face_texture = _create_texture_3d(
				empty_faces, Vector3i(1, 1, NEIGHBOR_TEXTURE_LAYER_COUNT), Image::FORMAT_L8, 1);
	}
}

void VoxelVolume3D::_rebuild_volume_textures() {
	const AABB previous_aabb = local_aabb;
	const bool resources_were_built = render_resources_built;
	local_aabb = voxel_data.is_valid() ?
			AABB(Vector3(), Vector3(voxel_data->get_dimensions()) * voxel_data->get_voxel_size()) : AABB();
	const bool surface_needs_rebuild = !resources_were_built || local_aabb != previous_aabb;
	if (!streaming_resident) {
		_sync_voxel_forward_volume(true);
		if (render_resources_built) {
			mixed_brick_atlas.unref();
			brick_directory_texture.unref();
			neighbor_face_texture.unref();
			RenderingServer::get_singleton()->mesh_clear(procedural_surface);
			render_resources_built = false;
		}
		_rebuild_shadow_proxy();
		return;
	}
	if (runtime_material.is_null()) {
		_rebuild_runtime_material();
	}
	mixed_brick_atlas.unref();
	brick_directory_texture.unref();
	if (voxel_data.is_null()) {
		_sync_voxel_forward_volume(true);
		rendered_revision = UINT64_MAX;
		render_resources_built = true;
		if (surface_needs_rebuild) {
			_rebuild_procedural_surface();
		}
		if (_uses_shadow_proxy() || shadow_proxy_has_surface) {
			_rebuild_shadow_proxy();
		}
		return;
	}

	const Vector3i dimensions = voxel_data->get_dimensions();
	const VoxelBrickStorage &storage = voxel_data->get_brick_storage();
	if (storage.get_occupied_voxel_count() == 0) {
		mixed_brick_atlas.unref();
		brick_directory_texture.unref();
		neighbor_face_texture.unref();
		neighbor_mask = 0;
		neighbor_diagonal_mask = 0;
		_sync_voxel_forward_volume(true);
		rendered_revision = voxel_data->get_revision();
		render_resources_built = true;
		RenderingServer::get_singleton()->mesh_clear(procedural_surface);
		if (_uses_shadow_proxy() || shadow_proxy_has_surface) {
			_rebuild_shadow_proxy();
		}
		return;
	}
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
	uint8_t *atlas_write = atlas_bytes.ptrw();
	uint8_t *directory_write = directory_bytes.ptrw();

	int mixed_slot = 0;
	for (int brick_index = 0; brick_index < storage.get_brick_count(); brick_index++) {
		const VoxelBrickStorage::Brick &brick = storage.get_brick(brick_index);
		uint32_t directory_code = 0;
		if (brick.type == VoxelBrickStorage::BRICK_UNIFORM) {
			directory_code = 1;
			directory_write[brick_index * 4 + 3] = brick.uniform_value;
		} else if (brick.type == VoxelBrickStorage::BRICK_MIXED) {
			directory_code = uint32_t(mixed_slot + 2);
			const uint8_t *mixed_values = storage.get_brick_mixed_values(brick_index);
			const Vector3i atlas_brick(
					mixed_slot % atlas_brick_dimensions.x,
					(mixed_slot / atlas_brick_dimensions.x) % atlas_brick_dimensions.y,
					mixed_slot / (atlas_brick_dimensions.x * atlas_brick_dimensions.y));
			const Vector3i atlas_origin = atlas_brick * VoxelBrickStorage::BRICK_SIZE;
			for (int z = 0; z < VoxelBrickStorage::BRICK_SIZE; z++) {
				for (int y = 0; y < VoxelBrickStorage::BRICK_SIZE; y++) {
					const int local_index = y * VoxelBrickStorage::BRICK_SIZE + z * VoxelBrickStorage::BRICK_SIZE * VoxelBrickStorage::BRICK_SIZE;
					const int atlas_index = atlas_origin.x +
							(atlas_origin.y + y) * atlas_dimensions.x +
							(atlas_origin.z + z) * atlas_dimensions.x * atlas_dimensions.y;
					std::memcpy(atlas_write + atlas_index, mixed_values + local_index, VoxelBrickStorage::BRICK_SIZE);
				}
			}
			mixed_slot++;
		}
		directory_write[brick_index * 4 + 0] = uint8_t(directory_code & 0xFF);
		directory_write[brick_index * 4 + 1] = uint8_t((directory_code >> 8) & 0xFF);
		directory_write[brick_index * 4 + 2] = uint8_t((directory_code >> 16) & 0xFF);
	}
	mixed_brick_atlas = _create_texture_3d(atlas_bytes, atlas_dimensions, Image::FORMAT_L8, 1);
	brick_directory_texture = _create_texture_3d(directory_bytes, brick_dimensions, Image::FORMAT_RGBA8, 4);
	rendered_revision = voxel_data->get_revision();
	render_resources_built = true;

	_update_material_bindings();
	if (surface_needs_rebuild) {
		_rebuild_procedural_surface();
		update_gizmos();
	}
	if (_uses_shadow_proxy() || shadow_proxy_has_surface) {
		_rebuild_shadow_proxy();
	}
}

void VoxelVolume3D::_rebuild_procedural_surface() {
	if (!streaming_resident) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	rendering_server->mesh_clear(procedural_surface);
	if (voxel_data.is_null() || voxel_data->get_brick_storage().get_occupied_voxel_count() == 0) {
		return;
	}

	RenderingServerTypes::SurfaceData surface;
	surface.format = RSE::ARRAY_FLAG_USES_EMPTY_VERTEX_ARRAY |
			RSE::ARRAY_FLAG_FORMAT_CURRENT_VERSION;
	surface.primitive = RSE::PRIMITIVE_TRIANGLES;
	surface.vertex_count = 36;
	surface.aabb = local_aabb;
	rendering_server->mesh_add_surface(procedural_surface, surface);
	if (runtime_material.is_valid()) {
		rendering_server->mesh_surface_set_material(
				procedural_surface, 0, runtime_material->get_rid());
	}
}

void VoxelVolume3D::_ensure_shadow_proxy() {
	if (!_uses_shadow_proxy() || shadow_proxy_instance.is_valid()) {
		return;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	shadow_proxy_mesh = rendering_server->mesh_create();
	shadow_proxy_instance = rendering_server->instance_create();
	rendering_server->instance_set_base(shadow_proxy_instance, shadow_proxy_mesh);
	rendering_server->instance_geometry_set_cast_shadows_setting(
			shadow_proxy_instance, RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY);
}

void VoxelVolume3D::_sync_shadow_proxy_instance() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	const bool enabled = _uses_shadow_proxy();
	if (enabled) {
		_ensure_shadow_proxy();
	}
	if (!shadow_proxy_instance.is_valid()) {
		return;
	}
	const bool active = enabled && streaming_resident && shadow_proxy_has_surface && is_inside_world();
	rendering_server->instance_set_scenario(
			shadow_proxy_instance,
			active ? get_world_3d()->get_scenario() : RID());
	if (is_inside_tree()) {
		rendering_server->instance_set_transform(shadow_proxy_instance, get_global_transform());
	}
	rendering_server->instance_set_visible(shadow_proxy_instance, active && is_visible_in_tree());
}

void VoxelVolume3D::_rebuild_shadow_proxy() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	if (_uses_shadow_proxy()) {
		_ensure_shadow_proxy();
	}
	if (!shadow_proxy_mesh.is_valid()) {
		shadow_proxy_has_surface = false;
		return;
	}
	rendering_server->mesh_clear(shadow_proxy_mesh);
	shadow_proxy_has_surface = false;
	if (!_uses_shadow_proxy() ||
			!streaming_resident || voxel_data.is_null()) {
		_sync_shadow_proxy_instance();
		return;
	}

	// Keep the shadow caster on the same voxel grid as the visible DDA surface.
	// Coarsening this geometry moves silhouettes and reintroduces contact gaps.
	static constexpr int PROXY_VOXEL_SCALE = 1;
	const Vector3i dimensions = voxel_data->get_dimensions();
	const Vector3i proxy_dimensions(
			(dimensions.x + PROXY_VOXEL_SCALE - 1) / PROXY_VOXEL_SCALE,
			(dimensions.y + PROXY_VOXEL_SCALE - 1) / PROXY_VOXEL_SCALE,
			(dimensions.z + PROXY_VOXEL_SCALE - 1) / PROXY_VOXEL_SCALE);
	const VoxelBrickStorage &storage = voxel_data->get_brick_storage();
	auto occupied = [&](const Vector3i &p_cell) {
		if (p_cell.x < 0 || p_cell.y < 0 || p_cell.z < 0 ||
				p_cell.x >= proxy_dimensions.x || p_cell.y >= proxy_dimensions.y || p_cell.z >= proxy_dimensions.z) {
			return false;
		}
		return storage.get_voxel(p_cell) != 0;
	};

	static const Vector3i face_directions[6] = {
		Vector3i(0, 0, -1), Vector3i(0, 0, 1),
		Vector3i(-1, 0, 0), Vector3i(1, 0, 0),
		Vector3i(0, -1, 0), Vector3i(0, 1, 0)
	};
	static const int cube_indices[36] = {
		0, 2, 1, 1, 2, 3,
		4, 5, 6, 5, 7, 6,
		0, 4, 2, 4, 6, 2,
		1, 3, 5, 3, 7, 5,
		0, 1, 4, 1, 5, 4,
		2, 6, 3, 3, 6, 7
	};
	PackedVector3Array vertices;
	const real_t voxel_size = voxel_data->get_voxel_size();
	for (int z = 0; z < proxy_dimensions.z; z++) {
		for (int y = 0; y < proxy_dimensions.y; y++) {
			for (int x = 0; x < proxy_dimensions.x; x++) {
				const Vector3i cell(x, y, z);
				if (!occupied(cell)) {
					continue;
				}
				const Vector3 minimum = Vector3(cell * PROXY_VOXEL_SCALE) * voxel_size;
				const Vector3 maximum = Vector3((cell + Vector3i(1, 1, 1)) * PROXY_VOXEL_SCALE).min(Vector3(dimensions)) * voxel_size;
				Vector3 corners[8];
				for (int corner = 0; corner < 8; corner++) {
					corners[corner] = Vector3(
							(corner & 1) != 0 ? maximum.x : minimum.x,
							(corner & 2) != 0 ? maximum.y : minimum.y,
							(corner & 4) != 0 ? maximum.z : minimum.z);
				}
				for (int face = 0; face < 6; face++) {
					if (occupied(cell + face_directions[face])) {
						continue;
					}
					for (int vertex = 0; vertex < 6; vertex++) {
						vertices.push_back(corners[cube_indices[face * 6 + vertex]]);
					}
				}
			}
		}
	}

	if (!vertices.is_empty()) {
		Array arrays;
		arrays.resize(RSE::ARRAY_MAX);
		arrays[RSE::ARRAY_VERTEX] = vertices;
		rendering_server->mesh_add_surface_from_arrays(shadow_proxy_mesh, RSE::PRIMITIVE_TRIANGLES, arrays);
		shadow_proxy_has_surface = true;
	}
	_sync_shadow_proxy_instance();
}

void VoxelVolume3D::_update_material_bindings(bool p_sync_volume) {
	if (!streaming_resident || voxel_data.is_null() || runtime_material.is_null()) {
		return;
	}
	_ensure_fallback_textures();
	const Ref<Texture2D> palette = voxel_material.is_valid() && voxel_material->get_palette_texture().is_valid() ? voxel_material->get_palette_texture() : voxel_data->get_palette_texture();
	const Ref<Texture2D> material = voxel_material.is_valid() && voxel_material->get_material_texture().is_valid() ? voxel_material->get_material_texture() : voxel_data->get_material_texture();
	const Ref<Texture2D> metallic = voxel_material.is_valid() && voxel_material->get_metallic_texture().is_valid() ? voxel_material->get_metallic_texture() : voxel_data->get_metallic_texture();
	const Ref<Texture2D> transparency_texture = voxel_material.is_valid() && voxel_material->get_transparency_texture().is_valid() ? voxel_material->get_transparency_texture() : voxel_data->get_transparency_texture();
	const Ref<Texture2D> specularity = voxel_material.is_valid() && voxel_material->get_specularity_texture().is_valid() ? voxel_material->get_specularity_texture() : voxel_data->get_specularity_texture();
	const Ref<Texture2D> emission = voxel_material.is_valid() && voxel_material->get_emission_texture().is_valid() ? voxel_material->get_emission_texture() : voxel_data->get_emission_texture();
	const bool has_metallic = metallic.is_valid();
	const bool has_transparency = transparency_texture.is_valid();
	const bool has_specularity = specularity.is_valid();
	const bool has_emission = emission.is_valid();
	runtime_material->set_transparency_enabled(has_transparency);
	runtime_material->set_texture_features(has_metallic, has_specularity, has_emission);
	runtime_material->ensure_shader();
	const Vector3i dimensions = voxel_data->get_dimensions();
	const Vector3i brick_dimensions(
			(dimensions.x + 7) / 8,
			(dimensions.y + 7) / 8,
			(dimensions.z + 7) / 8);
	if (!runtime_material->is_batched_resources_enabled()) {
		runtime_material->set_shader_parameter("u_voxels", mixed_brick_atlas);
		runtime_material->set_shader_parameter("u_bricks", brick_directory_texture);
		runtime_material->set_shader_parameter(
				"u_neighbor_faces",
				neighbor_face_texture.is_valid() ? neighbor_face_texture : fallback_neighbor_face_texture);
		runtime_material->set_shader_parameter("u_neighbor_mask", neighbor_mask);
		runtime_material->set_shader_parameter("u_neighbor_diagonal_mask", neighbor_diagonal_mask);
	}
	runtime_material->set_shader_parameter(
			"u_palette",
			palette.is_valid() ? palette : fallback_palette);
	runtime_material->set_shader_parameter(
			"u_material",
			material.is_valid() ? material : fallback_material);
	if (has_metallic) runtime_material->set_shader_parameter("u_metallic", metallic);
	if (has_specularity) runtime_material->set_shader_parameter("u_specularity", specularity);
	if (has_emission) runtime_material->set_shader_parameter("u_emission", emission);
	if (has_transparency) runtime_material->set_shader_parameter("u_transparency", transparency_texture);
	if (!runtime_material->is_batched_resources_enabled()) {
		runtime_material->set_shader_parameter("u_volume_dims", dimensions);
		runtime_material->set_shader_parameter("u_brick_dims", brick_dimensions);
		runtime_material->set_shader_parameter("u_atlas_brick_dims", atlas_brick_dimensions);
		runtime_material->set_shader_parameter("u_voxel_size", voxel_data->get_voxel_size());
	}
	if (p_sync_volume) {
		_sync_voxel_forward_volume();
	}
}

void VoxelVolume3D::_sync_voxel_forward_volume(bool p_remove) {
	using RendererSceneRenderImplementation::VoxelForwardVolumeStorage;
	if (VoxelForwardVolumeStorage::get_singleton() == nullptr || !procedural_surface.is_valid()) {
		return;
	}

	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	if (p_remove || !streaming_resident || voxel_data.is_null() || voxel_data->get_brick_storage().get_occupied_voxel_count() == 0 || mixed_brick_atlas.is_null() || brick_directory_texture.is_null()) {
		rendering_server->call_on_render_thread(callable_mp_static(&VoxelForwardVolumeStorage::volume_remove_on_render_thread).bind(procedural_surface));
		voxel_forward_dirty_valid = false;
		return;
	}

	_ensure_fallback_textures();
	const Vector3i dimensions = voxel_data->get_dimensions();
	const Vector3i brick_dimensions(
			(dimensions.x + VoxelBrickStorage::BRICK_SIZE - 1) / VoxelBrickStorage::BRICK_SIZE,
			(dimensions.y + VoxelBrickStorage::BRICK_SIZE - 1) / VoxelBrickStorage::BRICK_SIZE,
			(dimensions.z + VoxelBrickStorage::BRICK_SIZE - 1) / VoxelBrickStorage::BRICK_SIZE);
	const Ref<Texture2D> palette_texture = voxel_material.is_valid() && voxel_material->get_palette_texture().is_valid() ? voxel_material->get_palette_texture() : voxel_data->get_palette_texture();
	const Ref<Texture2D> material_texture = voxel_material.is_valid() && voxel_material->get_material_texture().is_valid() ? voxel_material->get_material_texture() : voxel_data->get_material_texture();
	const RID palette = palette_texture.is_valid() ? palette_texture->get_rid() : fallback_palette->get_rid();
	const RID material = material_texture.is_valid() ? material_texture->get_rid() : fallback_material->get_rid();
	const RID neighbor = neighbor_face_texture.is_valid() ? neighbor_face_texture->get_rid() : fallback_neighbor_face_texture->get_rid();
	const VoxelBrickStorage &storage = voxel_data->get_brick_storage();
	PackedByteArray occupancy_directory;
	occupancy_directory.resize(storage.get_brick_count() * sizeof(uint32_t));
	occupancy_directory.fill(0);
	int mixed_brick_count = 0;
	for (int brick_index = 0; brick_index < storage.get_brick_count(); brick_index++) {
		if (storage.get_brick(brick_index).type == VoxelBrickStorage::BRICK_MIXED) {
			mixed_brick_count++;
		}
	}
	PackedByteArray occupancy_bricks;
	occupancy_bricks.resize(mixed_brick_count * (VoxelBrickStorage::BRICK_VOXEL_COUNT / 8));
	occupancy_bricks.fill(0);
	uint8_t *directory_write = occupancy_directory.ptrw();
	uint8_t *brick_write = occupancy_bricks.ptrw();
	int mixed_slot = 0;
	int occupied_brick_count = 0;
	for (int brick_index = 0; brick_index < storage.get_brick_count(); brick_index++) {
		const VoxelBrickStorage::Brick &brick = storage.get_brick(brick_index);
		uint32_t directory_code = 0;
		if (brick.type == VoxelBrickStorage::BRICK_UNIFORM && brick.uniform_value != 0) {
			directory_code = 1;
		} else if (brick.type == VoxelBrickStorage::BRICK_MIXED) {
			directory_code = uint32_t(mixed_slot + 2);
			const uint8_t *mixed_values = storage.get_brick_mixed_values(brick_index);
			uint8_t *mixed_write = brick_write + mixed_slot * (VoxelBrickStorage::BRICK_VOXEL_COUNT / 8);
			for (int local_index = 0; local_index < VoxelBrickStorage::BRICK_VOXEL_COUNT; local_index++) {
				if (mixed_values[local_index] != 0) {
					mixed_write[local_index >> 3] |= uint8_t(1u << (local_index & 7));
				}
			}
			mixed_slot++;
		}
		occupied_brick_count += directory_code != 0 ? 1 : 0;
		directory_write[brick_index * 4 + 0] = uint8_t(directory_code & 0xFF);
		directory_write[brick_index * 4 + 1] = uint8_t((directory_code >> 8) & 0xFF);
		directory_write[brick_index * 4 + 2] = uint8_t((directory_code >> 16) & 0xFF);
		directory_write[brick_index * 4 + 3] = uint8_t((directory_code >> 24) & 0xFF);
	}
	rendering_server->call_on_render_thread(callable_mp_static(&VoxelForwardVolumeStorage::volume_set_on_render_thread).bind(
			procedural_surface,
			mixed_brick_atlas->get_rid(),
			brick_directory_texture->get_rid(),
			neighbor,
			palette,
			material,
			occupancy_directory,
			occupancy_bricks,
			dimensions,
			brick_dimensions,
			atlas_brick_dimensions,
			is_inside_tree() ? get_global_transform() : get_transform(),
			float(voxel_data->get_voxel_size()),
			occupied_brick_count,
			neighbor_mask,
			neighbor_diagonal_mask,
			voxel_forward_dirty_valid ? voxel_forward_dirty_position : Vector3i(),
			voxel_forward_dirty_valid ? voxel_forward_dirty_size : Vector3i(),
			int64_t(voxel_data->get_revision())));
	voxel_forward_dirty_valid = false;
}

String VoxelVolume3D::_make_runtime_material_batch_key() const {
	if (!RenderingMethod::is_current_voxel_forward_method() || voxel_data.is_null()) {
		return String();
	}
	const Ref<Texture2D> palette = voxel_material.is_valid() && voxel_material->get_palette_texture().is_valid() ? voxel_material->get_palette_texture() : voxel_data->get_palette_texture();
	const Ref<Texture2D> material = voxel_material.is_valid() && voxel_material->get_material_texture().is_valid() ? voxel_material->get_material_texture() : voxel_data->get_material_texture();
	const Ref<Texture2D> metallic = voxel_material.is_valid() && voxel_material->get_metallic_texture().is_valid() ? voxel_material->get_metallic_texture() : voxel_data->get_metallic_texture();
	const Ref<Texture2D> transparency_texture = voxel_material.is_valid() && voxel_material->get_transparency_texture().is_valid() ? voxel_material->get_transparency_texture() : voxel_data->get_transparency_texture();
	const Ref<Texture2D> specularity = voxel_material.is_valid() && voxel_material->get_specularity_texture().is_valid() ? voxel_material->get_specularity_texture() : voxel_data->get_specularity_texture();
	const Ref<Texture2D> emission = voxel_material.is_valid() && voxel_material->get_emission_texture().is_valid() ? voxel_material->get_emission_texture() : voxel_data->get_emission_texture();
	const Transform3D transform = is_inside_tree() ? get_global_transform() : get_transform();
	const Vector3 rotation = transform.basis.orthonormalized().get_euler();
	return vformat("%d:%d:%d:%d:%d:%d:%d:%d:%d:%d",
			voxel_material.is_valid() ? uint64_t(voxel_material->get_instance_id()) : 0,
			_texture_identity(palette), _texture_identity(material), _texture_identity(metallic),
			_texture_identity(transparency_texture), _texture_identity(specularity), _texture_identity(emission),
			_quantized_absolute_rotation(rotation.x), _quantized_absolute_rotation(rotation.y), _quantized_absolute_rotation(rotation.z));
}

void VoxelVolume3D::_release_runtime_material() {
	if (runtime_material_cached) {
		VoxelRuntimeMaterialCacheEntry *entry = voxel_runtime_material_cache.getptr(runtime_material_batch_key);
		if (entry != nullptr) {
			entry->users--;
			if (entry->users == 0) {
				voxel_runtime_material_cache.erase(runtime_material_batch_key);
			}
		}
	}
	runtime_material_cached = false;
	runtime_material_batch_key = String();
	runtime_material.unref();
}

void VoxelVolume3D::_rebuild_runtime_material() {
	_ensure_fallback_textures();
	const String requested_batch_key = _make_runtime_material_batch_key();
	if (runtime_material.is_null() || requested_batch_key != runtime_material_batch_key) {
		_release_runtime_material();
		if (!requested_batch_key.is_empty()) {
			VoxelRuntimeMaterialCacheEntry *entry = voxel_runtime_material_cache.getptr(requested_batch_key);
			if (entry == nullptr) {
				VoxelRuntimeMaterialCacheEntry new_entry;
				new_entry.material.instantiate();
				new_entry.users = 1;
				voxel_runtime_material_cache.insert(requested_batch_key, new_entry);
				runtime_material = new_entry.material;
			} else {
				entry->users++;
				runtime_material = entry->material;
			}
			runtime_material_batch_key = requested_batch_key;
			runtime_material_cached = true;
		} else {
			runtime_material.instantiate();
		}
	}
	runtime_material->set_batched_resources_enabled(!requested_batch_key.is_empty());
	if (voxel_material.is_valid()) {
		runtime_material->set_shading_mode(voxel_material->get_shading_mode());
		runtime_material->set_lighting_position_mode(voxel_material->get_lighting_position_mode());
		runtime_material->set_albedo_modulate(voxel_material->get_albedo_modulate());
		runtime_material->set_roughness_multiplier(voxel_material->get_roughness_multiplier());
		runtime_material->set_metallic_multiplier(voxel_material->get_metallic_multiplier());
		runtime_material->set_specularity_multiplier(voxel_material->get_specularity_multiplier());
		runtime_material->set_emission_energy(voxel_material->get_emission_energy());
		runtime_material->set_ambient_occlusion_enabled(voxel_material->is_ambient_occlusion_enabled());
		runtime_material->set_ambient_occlusion_color(voxel_material->get_ambient_occlusion_color());
		runtime_material->set_ambient_occlusion_strength(voxel_material->get_ambient_occlusion_strength());
		runtime_material->set_ambient_occlusion_hardness(voxel_material->get_ambient_occlusion_hardness());
		runtime_material->set_ambient_occlusion_mode(voxel_material->get_ambient_occlusion_mode());
		runtime_material->set_ambient_occlusion_face_mode(voxel_material->get_ambient_occlusion_face_mode());
		runtime_material->set_outline_enabled(voxel_material->is_outline_enabled());
		runtime_material->set_outline_color(voxel_material->get_outline_color());
		runtime_material->set_outline_width(voxel_material->get_outline_width());
	} else {
		runtime_material->set_shading_mode(VoxelMaterial::SHADING_MODE_PBR);
		runtime_material->set_lighting_position_mode(VoxelMaterial::LIGHTING_POSITION_VOXEL_FACE_CENTER);
		runtime_material->set_albedo_modulate(Color(1.0, 1.0, 1.0, 1.0));
		runtime_material->set_roughness_multiplier(1.0);
		runtime_material->set_metallic_multiplier(1.0);
		runtime_material->set_specularity_multiplier(1.0);
		runtime_material->set_emission_energy(1.0);
		runtime_material->set_ambient_occlusion_enabled(true);
		runtime_material->set_ambient_occlusion_color(Color(0.0, 0.0, 0.0, 1.0));
		runtime_material->set_ambient_occlusion_strength(1.0);
		runtime_material->set_ambient_occlusion_hardness(0.5);
		runtime_material->set_ambient_occlusion_mode(VoxelMaterial::AMBIENT_OCCLUSION_MODE_VOXELIZED);
		runtime_material->set_ambient_occlusion_face_mode(VoxelMaterial::AMBIENT_OCCLUSION_FACE_MODE_ALL);
		runtime_material->set_outline_enabled(false);
		runtime_material->set_outline_color(Color(0.0, 0.0, 0.0, 1.0));
		runtime_material->set_outline_width(1.0);
	}
}

void VoxelVolume3D::_voxel_data_changed() {
	if (voxel_change_notification_pending) {
		voxel_change_notification_pending = false;
		return;
	}
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
	if (!streaming_resident) {
		update_gizmos();
		return;
	}
	_rebuild_procedural_surface();
	_rebuild_shadow_proxy();
	_update_material_bindings();
	update_gizmos();
}

void VoxelVolume3D::_voxel_data_voxels_changed(const Vector3i &p_position, const Vector3i &p_size, int64_t p_revision) {
	voxel_change_notification_pending = true;
	if (!voxel_forward_dirty_valid) {
		voxel_forward_dirty_position = p_position;
		voxel_forward_dirty_size = p_size;
		voxel_forward_dirty_valid = true;
	} else {
		const Vector3i previous_end = voxel_forward_dirty_position + voxel_forward_dirty_size;
		const Vector3i changed_end = p_position + p_size;
		const Vector3i merged_begin(
				MIN(voxel_forward_dirty_position.x, p_position.x),
				MIN(voxel_forward_dirty_position.y, p_position.y),
				MIN(voxel_forward_dirty_position.z, p_position.z));
		const Vector3i merged_end(
				MAX(previous_end.x, changed_end.x),
				MAX(previous_end.y, changed_end.y),
				MAX(previous_end.z, changed_end.z));
		voxel_forward_dirty_position = merged_begin;
		voxel_forward_dirty_size = merged_end - merged_begin;
	}
	if (streaming_resident) {
		_rebuild_volume_textures();
	}
	if (VoxelVolumeStreamingManager::get_singleton() != nullptr) {
		// Neighbor face textures only depend on voxels at this volume's six
		// boundaries. Interior destruction must not trigger an O(volume^2)
		// adjacency scan across the entire streamed world.
		const Vector3i dimensions = voxel_data.is_valid() ? voxel_data->get_dimensions() : Vector3i();
		const Vector3i changed_end = p_position + p_size;
		if (p_position.x <= 0 || p_position.y <= 0 || p_position.z <= 0 ||
				changed_end.x >= dimensions.x || changed_end.y >= dimensions.y || changed_end.z >= dimensions.z) {
			VoxelVolumeStreamingManager::get_singleton()->mark_volume_neighbors_dirty(this);
		}
	}
}

void VoxelVolume3D::_voxel_material_changed() {
	if (!streaming_resident) {
		_release_runtime_material();
		return;
	}
	if (voxel_material.is_valid() && runtime_material.is_valid() && voxel_material->is_ambient_occlusion_change_in_progress()) {
		// AO edits only change shared shader uniforms (or select the cached enabled
		// variant). Avoid rebinding every per-volume atlas and neighbor texture when
		// a shared material slider is dragged in the Inspector.
		runtime_material->set_ambient_occlusion_enabled(voxel_material->is_ambient_occlusion_enabled());
		runtime_material->set_ambient_occlusion_color(voxel_material->get_ambient_occlusion_color());
		runtime_material->set_ambient_occlusion_strength(voxel_material->get_ambient_occlusion_strength());
		runtime_material->set_ambient_occlusion_hardness(voxel_material->get_ambient_occlusion_hardness());
		runtime_material->set_ambient_occlusion_mode(voxel_material->get_ambient_occlusion_mode());
		runtime_material->set_ambient_occlusion_face_mode(voxel_material->get_ambient_occlusion_face_mode());
		return;
	}
	_rebuild_runtime_material();
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
	notify_property_list_changed();
	if (streaming_mode == STREAMING_ALWAYS_RESIDENT) {
		set_streaming_resident(true);
	}
}

void VoxelVolume3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "streaming_resident" && streaming_mode != STREAMING_MANUAL) {
		// Automatic residency is derived from the camera and budget. Persisting it
		// makes scene files depend on the last editor view and restores stale state.
		p_property.usage = PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY;
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
	// Older scenes serialized the runtime residency flag as true on every
	// automatic volume. Do not eagerly build all GPU textures while those nodes
	// are still being deserialized; the first manager frame ranks them by camera
	// distance and applies the actual resident budget.
	if (!is_inside_tree() && streaming_mode == STREAMING_AUTOMATIC && p_resident) {
		return;
	}
	if (streaming_resident == p_resident) {
		return;
	}
	streaming_resident = p_resident;
	_rebuild_volume_textures();
	if (VoxelVolumeStreamingManager::get_singleton() != nullptr) {
		if (p_resident) {
			VoxelVolumeStreamingManager::get_singleton()->mark_volume_neighbors_dirty(this);
		} else {
			// Unloading also invalidates the previously adjacent resident volume.
			// A full resident-only pass is rare and guarantees those stale slices
			// are removed even though this volume is no longer in the face index.
			VoxelVolumeStreamingManager::get_singleton()->mark_neighbors_dirty();
		}
	}
}

bool VoxelVolume3D::is_streaming_resident() const { return streaming_resident; }

int VoxelVolume3D::get_neighbor_diagonal_index(const Vector3i &p_offset) {
	const int nonzero_count = int(p_offset.x != 0) + int(p_offset.y != 0) + int(p_offset.z != 0);
	if (nonzero_count == 2) {
		if (p_offset.z == 0) return (p_offset.x > 0 ? 2 : 0) + (p_offset.y > 0 ? 1 : 0);
		if (p_offset.y == 0) return 4 + (p_offset.x > 0 ? 2 : 0) + (p_offset.z > 0 ? 1 : 0);
		return 8 + (p_offset.y > 0 ? 2 : 0) + (p_offset.z > 0 ? 1 : 0);
	}
	if (nonzero_count == 3) {
		return 12 + (p_offset.x > 0 ? 4 : 0) + (p_offset.y > 0 ? 2 : 0) + (p_offset.z > 0 ? 1 : 0);
	}
	return -1;
}

Vector3i VoxelVolume3D::get_neighbor_diagonal_offset(int p_index) {
	ERR_FAIL_INDEX_V(p_index, NEIGHBOR_DIAGONAL_COUNT, Vector3i());
	if (p_index < 4) {
		return Vector3i((p_index & 2) != 0 ? 1 : -1, (p_index & 1) != 0 ? 1 : -1, 0);
	}
	if (p_index < 8) {
		const int index = p_index - 4;
		return Vector3i((index & 2) != 0 ? 1 : -1, 0, (index & 1) != 0 ? 1 : -1);
	}
	if (p_index < 12) {
		const int index = p_index - 8;
		return Vector3i(0, (index & 2) != 0 ? 1 : -1, (index & 1) != 0 ? 1 : -1);
	}
	const int index = p_index - 12;
	return Vector3i((index & 4) != 0 ? 1 : -1, (index & 2) != 0 ? 1 : -1, (index & 1) != 0 ? 1 : -1);
}

void VoxelVolume3D::update_neighbor_faces(VoxelVolume3D *const p_neighbors[NEIGHBOR_FACE_COUNT], VoxelVolume3D *const p_diagonal_neighbors[NEIGHBOR_DIAGONAL_COUNT]) {
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
	ObjectID new_diagonal_ids[NEIGHBOR_DIAGONAL_COUNT];
	uint64_t new_diagonal_revisions[NEIGHBOR_DIAGONAL_COUNT];
	for (int diagonal = 0; diagonal < NEIGHBOR_DIAGONAL_COUNT; diagonal++) {
		VoxelVolume3D *neighbor = p_diagonal_neighbors[diagonal];
		new_diagonal_ids[diagonal] = neighbor != nullptr ? neighbor->get_instance_id() : ObjectID();
		new_diagonal_revisions[diagonal] = neighbor != nullptr && neighbor->get_voxel_data().is_valid() ?
				neighbor->get_voxel_data()->get_revision() : UINT64_MAX;
		unchanged = unchanged && neighbor_diagonal_ids[diagonal] == new_diagonal_ids[diagonal] && neighbor_diagonal_revisions[diagonal] == new_diagonal_revisions[diagonal];
	}
	if (unchanged) {
		return;
	}

	neighbor_self_revision = self_revision;
	neighbor_self_dimensions = self_dimensions;
	neighbor_mask = 0;
	neighbor_diagonal_mask = 0;
	const RID previous_neighbor_texture = neighbor_face_texture.is_valid() ? neighbor_face_texture->get_rid() : RID();
	for (int face = 0; face < NEIGHBOR_FACE_COUNT; face++) {
		neighbor_ids[face] = new_ids[face];
		neighbor_revisions[face] = new_revisions[face];
	}
	for (int diagonal = 0; diagonal < NEIGHBOR_DIAGONAL_COUNT; diagonal++) {
		neighbor_diagonal_ids[diagonal] = new_diagonal_ids[diagonal];
		neighbor_diagonal_revisions[diagonal] = new_diagonal_revisions[diagonal];
	}
	if (voxel_data.is_null() || !streaming_resident) {
		return;
	}

	const Vector3i dimensions = voxel_data->get_dimensions();
	const int side = MAX(dimensions.x, MAX(dimensions.y, dimensions.z));
	const int packed_width = (side + 7) / 8;
	PackedByteArray face_bytes;
	face_bytes.resize(packed_width * side * NEIGHBOR_TEXTURE_LAYER_COUNT);
	face_bytes.fill(0);
	auto set_face_voxel = [&face_bytes, packed_width, side](int p_face, int p_u, int p_v, int p_value) {
		if (p_value <= 0) {
			return;
		}
		const int byte_index = (p_u >> 3) + p_v * packed_width + p_face * packed_width * side;
		face_bytes.set(byte_index, face_bytes[byte_index] | (1 << (p_u & 7)));
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
	for (int diagonal = 0; diagonal < NEIGHBOR_DIAGONAL_COUNT; diagonal++) {
		VoxelVolume3D *neighbor = p_diagonal_neighbors[diagonal];
		if (neighbor == nullptr || neighbor->get_voxel_data().is_null() || !neighbor->is_streaming_resident()) {
			continue;
		}
		neighbor_diagonal_mask |= 1 << diagonal;
		const Vector3i offset = get_neighbor_diagonal_offset(diagonal);
		const Vector3i neighbor_dimensions = neighbor->get_voxel_data()->get_dimensions();
		Vector3i neighbor_voxel(
				offset.x < 0 ? neighbor_dimensions.x - 1 : 0,
				offset.y < 0 ? neighbor_dimensions.y - 1 : 0,
				offset.z < 0 ? neighbor_dimensions.z - 1 : 0);
		const int layer = NEIGHBOR_FACE_COUNT + diagonal;
		const int nonzero_count = int(offset.x != 0) + int(offset.y != 0) + int(offset.z != 0);
		if (nonzero_count == 3) {
			set_face_voxel(layer, 0, 0, neighbor->get_voxel(neighbor_voxel));
		} else {
			const int varying_axis = offset.x == 0 ? 0 : (offset.y == 0 ? 1 : 2);
			const int length = dimensions[varying_axis];
			for (int coordinate = 0; coordinate < length; coordinate++) {
				neighbor_voxel[varying_axis] = coordinate;
				set_face_voxel(layer, coordinate, 0, neighbor->get_voxel(neighbor_voxel));
			}
		}
	}
	if (neighbor_mask != 0 || neighbor_diagonal_mask != 0) {
		if (neighbor_face_texture.is_valid() && neighbor_face_texture->get_width() == packed_width && neighbor_face_texture->get_height() == side &&
				neighbor_face_texture->get_depth() == NEIGHBOR_TEXTURE_LAYER_COUNT) {
			Vector<Ref<Image>> slices;
			slices.resize(NEIGHBOR_TEXTURE_LAYER_COUNT);
			const int slice_size = packed_width * side;
			for (int face = 0; face < NEIGHBOR_TEXTURE_LAYER_COUNT; face++) {
				PackedByteArray slice;
				slice.resize(slice_size);
				std::memcpy(slice.ptrw(), face_bytes.ptr() + face * slice_size, slice_size);
				slices.write[face] = Image::create_from_data(packed_width, side, false, Image::FORMAT_L8, slice);
			}
			neighbor_face_texture->update(slices);
		} else {
			neighbor_face_texture = _create_texture_3d(
					face_bytes, Vector3i(packed_width, side, NEIGHBOR_TEXTURE_LAYER_COUNT), Image::FORMAT_L8, 1);
		}
	} else {
		neighbor_face_texture.unref();
	}
	// Only the neighbor uniforms changed. A full material rebind would also repack
	// and resend the much larger world-occupancy payload even though neither the
	// volume voxels nor its transform changed.
	if (runtime_material.is_valid() && !runtime_material->is_batched_resources_enabled()) {
		_ensure_fallback_textures();
		const RID current_neighbor_texture = neighbor_face_texture.is_valid() ? neighbor_face_texture->get_rid() : RID();
		if (current_neighbor_texture != previous_neighbor_texture) {
			runtime_material->set_shader_parameter(
					"u_neighbor_faces",
					neighbor_face_texture.is_valid() ? neighbor_face_texture : fallback_neighbor_face_texture);
		}
		runtime_material->set_shader_parameter("u_neighbor_mask", neighbor_mask);
		runtime_material->set_shader_parameter("u_neighbor_diagonal_mask", neighbor_diagonal_mask);
	}
	if (procedural_surface.is_valid() && RendererSceneRenderImplementation::VoxelForwardVolumeStorage::get_singleton() != nullptr) {
		_ensure_fallback_textures();
		const RID neighbor = neighbor_face_texture.is_valid() ? neighbor_face_texture->get_rid() : fallback_neighbor_face_texture->get_rid();
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&RendererSceneRenderImplementation::VoxelForwardVolumeStorage::volume_neighbors_set_on_render_thread).bind(procedural_surface, neighbor, neighbor_mask, neighbor_diagonal_mask));
	}
}

void VoxelVolume3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		// Serialized scenes may restore the main instance's shadow flag after the
		// constructor. The proxy must be the only caster or the DDA shader will
		// still run in every cascade and erase the performance gain.
		if (_uses_voxel_forward_shadow_mask() || _uses_shadow_proxy()) {
			set_cast_shadows_setting(SHADOW_CASTING_SETTING_OFF);
		} else {
			set_cast_shadows_setting(SHADOW_CASTING_SETTING_ON);
		}
		if (VoxelVolumeStreamingManager::get_singleton() != nullptr) {
			VoxelVolumeStreamingManager::get_singleton()->register_volume(this);
		}
		// Runtime resources can be prepared before entering the tree, when only
		// the local transform is available. Register again here so nested volumes
		// use their final global transform, and so a volume removed and re-added to
		// the tree is restored to the render-thread occupancy world.
		if (streaming_resident) {
			_sync_voxel_forward_volume();
		}
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		// A detached node must stop contributing immediately. Waiting until its
		// destructor leaves stale occupancy behind and produces ghost shadows.
		_sync_voxel_forward_volume(true);
		if (VoxelVolumeStreamingManager::get_singleton() != nullptr) {
			VoxelVolumeStreamingManager::get_singleton()->unregister_volume(this);
		}
	} else if (p_what == NOTIFICATION_ENTER_WORLD) {
		_sync_shadow_proxy_instance();
	} else if (p_what == NOTIFICATION_EXIT_WORLD) {
		if (shadow_proxy_instance.is_valid()) {
			RenderingServer::get_singleton()->instance_set_scenario(shadow_proxy_instance, RID());
		}
	} else if (p_what == NOTIFICATION_VISIBILITY_CHANGED) {
		_sync_shadow_proxy_instance();
	} else if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		if (is_inside_tree()) {
			set_cast_shadows_setting((_uses_voxel_forward_shadow_mask() || _uses_shadow_proxy()) ? SHADOW_CASTING_SETTING_OFF : SHADOW_CASTING_SETTING_ON);
		}
		_sync_shadow_proxy_instance();
		const String requested_batch_key = _make_runtime_material_batch_key();
		if (streaming_resident && requested_batch_key != runtime_material_batch_key) {
			_rebuild_runtime_material();
			_update_material_bindings(false);
			_rebuild_procedural_surface();
		}
		if (is_inside_tree() && streaming_resident && procedural_surface.is_valid() && RendererSceneRenderImplementation::VoxelForwardVolumeStorage::get_singleton() != nullptr) {
			RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&RendererSceneRenderImplementation::VoxelForwardVolumeStorage::volume_transform_set_on_render_thread).bind(procedural_surface, get_global_transform()));
		}
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

	// Retain the authored resource itself so one VoxelMaterial can be assigned
	// to any number of volumes and edited once. Generated shaders and per-volume
	// texture bindings live exclusively in runtime_material and are never added
	// to this shared resource.
	voxel_material = p_material;
	if (p_material.is_valid()) {
		voxel_material->connect_changed(callable_mp(this, &VoxelVolume3D::_voxel_material_changed));
	}
	if (streaming_resident) {
		_rebuild_runtime_material();
		_update_material_bindings();
		_rebuild_procedural_surface();
	} else {
		_release_runtime_material();
	}
}

Ref<VoxelMaterial> VoxelVolume3D::get_voxel_material() const { return voxel_material; }

AABB VoxelVolume3D::get_aabb() const { return local_aabb; }

VoxelVolume3D::VoxelVolume3D() {
	for (uint64_t &revision : neighbor_diagonal_revisions) {
		revision = UINT64_MAX;
	}
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	procedural_surface = rendering_server->mesh_create();
	set_base(procedural_surface);
	// Scene deserialization can apply transforms before the node enters the
	// tree. Shadow-path selection needs the final global transform, so defer it
	// to NOTIFICATION_ENTER_TREE.
	if (_uses_shadow_proxy()) {
		set_cast_shadows_setting(SHADOW_CASTING_SETTING_OFF);
	}
	set_notify_transform(true);
}

VoxelVolume3D::~VoxelVolume3D() {
	_sync_voxel_forward_volume(true);
	_release_runtime_material();
	if (voxel_data.is_valid()) {
		voxel_data->disconnect_changed(callable_mp(this, &VoxelVolume3D::_voxel_data_changed));
		voxel_data->disconnect(SNAME("voxels_changed"), callable_mp(this, &VoxelVolume3D::_voxel_data_voxels_changed));
	}
	if (voxel_material.is_valid()) {
		voxel_material->disconnect_changed(callable_mp(this, &VoxelVolume3D::_voxel_material_changed));
	}
	if (procedural_surface.is_valid() && RenderingServer::get_singleton() != nullptr) {
		RenderingServer::get_singleton()->free_rid(procedural_surface);
	}
	if (shadow_proxy_instance.is_valid() && RenderingServer::get_singleton() != nullptr) {
		RenderingServer::get_singleton()->free_rid(shadow_proxy_instance);
	}
	if (shadow_proxy_mesh.is_valid() && RenderingServer::get_singleton() != nullptr) {
		RenderingServer::get_singleton()->free_rid(shadow_proxy_mesh);
	}
}
