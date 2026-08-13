#include "voxel_volume_streaming_manager.h"

#include "core/object/callable_mp.h"
#include "core/config/project_settings.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_method.h"
#include "servers/rendering/rendering_server.h"
#include "voxel_volume_3d.h"

#include <cstring>

VoxelVolumeStreamingManager *VoxelVolumeStreamingManager::singleton = nullptr;

static constexpr const char *OCCUPANCY_TEXTURE_NAME = "voxel_teardown_occupancy";
static constexpr const char *OCCUPANCY_COARSE_TEXTURE_NAME = "voxel_teardown_occupancy_coarse";
static constexpr const char *OCCUPANCY_ORIGIN_NAME = "voxel_teardown_origin";
static constexpr const char *OCCUPANCY_COARSE_ORIGIN_NAME = "voxel_teardown_coarse_origin";
static constexpr const char *OCCUPANCY_VOXEL_SIZE_NAME = "voxel_teardown_voxel_size";
static constexpr const char *OCCUPANCY_COARSE_VOXEL_SIZE_NAME = "voxel_teardown_coarse_voxel_size";
static constexpr const char *OCCUPANCY_FINE_DISTANCE_NAME = "voxel_teardown_fine_distance";
static constexpr const char *OCCUPANCY_RESOLUTION_NAME = "voxel_teardown_resolution";
static constexpr const char *OCCUPANCY_MAX_DISTANCE_NAME = "voxel_teardown_max_distance";
static constexpr const char *OCCUPANCY_SHADOW_BIAS_NAME = "voxel_teardown_shadow_bias";
static constexpr const char *OCCUPANCY_MAX_STEPS_NAME = "voxel_teardown_max_steps";
static constexpr const char *OCCUPANCY_ENABLED_NAME = "voxel_teardown_enabled";
static constexpr const char *VOXEL_FORWARD_SHADOW_MASK_NAME = "voxel_forward_shadow_mask";
static constexpr const char *VOXEL_FORWARD_SHADOW_LIGHT_DIRECTION_NAME = "voxel_forward_shadow_light_direction";
static constexpr const char *VOXEL_FORWARD_SHADOW_READY_NAME = "voxel_forward_shadow_ready";
static constexpr const char *VOXEL_FORWARD_INDIRECT_NEAR_NAME = "voxel_forward_indirect_near";
static constexpr const char *VOXEL_FORWARD_INDIRECT_FAR_NAME = "voxel_forward_indirect_far";
static constexpr const char *VOXEL_FORWARD_INDIRECT_DISTANT_NAME = "voxel_forward_indirect_distant";
static constexpr const char *VOXEL_FORWARD_INDIRECT_NEAR_ORIGIN_NAME = "voxel_forward_indirect_near_origin";
static constexpr const char *VOXEL_FORWARD_INDIRECT_FAR_ORIGIN_NAME = "voxel_forward_indirect_far_origin";
static constexpr const char *VOXEL_FORWARD_INDIRECT_DISTANT_ORIGIN_NAME = "voxel_forward_indirect_distant_origin";
static constexpr const char *VOXEL_FORWARD_INDIRECT_NEAR_CELL_SIZE_NAME = "voxel_forward_indirect_near_cell_size";
static constexpr const char *VOXEL_FORWARD_INDIRECT_FAR_CELL_SIZE_NAME = "voxel_forward_indirect_far_cell_size";
static constexpr const char *VOXEL_FORWARD_INDIRECT_DISTANT_CELL_SIZE_NAME = "voxel_forward_indirect_distant_cell_size";
static constexpr const char *VOXEL_FORWARD_INDIRECT_RESOLUTION_NAME = "voxel_forward_indirect_resolution";
static constexpr const char *VOXEL_FORWARD_INDIRECT_TRANSITION_CELLS_NAME = "voxel_forward_indirect_transition_cells";
static constexpr const char *VOXEL_FORWARD_INDIRECT_INTENSITY_NAME = "voxel_forward_indirect_intensity";
static constexpr const char *VOXEL_FORWARD_INDIRECT_READY_NAME = "voxel_forward_indirect_ready";

struct VoxelStreamingCandidate {
	VoxelVolume3D *volume = nullptr;
	real_t distance_squared = 0.0;
	bool operator<(const VoxelStreamingCandidate &p_other) const { return distance_squared < p_other.distance_squared; }
};

struct VoxelNeighborSet {
	VoxelVolume3D *volume = nullptr;
	VoxelVolume3D *faces[VoxelVolume3D::NEIGHBOR_FACE_COUNT] = {};
};

static int _get_neighbor_face(const VoxelVolume3D *p_volume, const VoxelVolume3D *p_other) {
	const Ref<VoxelShapeData> data = p_volume->get_voxel_data();
	const Ref<VoxelShapeData> other_data = p_other->get_voxel_data();
	if (data.is_null() || other_data.is_null() || !p_volume->is_streaming_resident() || !p_other->is_streaming_resident()) {
		return -1;
	}
	if (!Math::is_equal_approx(data->get_voxel_size(), other_data->get_voxel_size())) {
		return -1;
	}
	const Transform3D relative = p_volume->get_global_transform().affine_inverse() * p_other->get_global_transform();
	if (!relative.basis.is_equal_approx(Basis())) {
		return -1;
	}
	const Vector3i dimensions = data->get_dimensions();
	const Vector3i other_dimensions = other_data->get_dimensions();
	const Vector3 size = Vector3(dimensions) * data->get_voxel_size();
	const Vector3 other_size = Vector3(other_dimensions) * other_data->get_voxel_size();
	const real_t tolerance = MAX(real_t(0.00001), data->get_voxel_size() * real_t(0.001));
	const Vector3 &origin = relative.origin;
	auto near_value = [tolerance](real_t p_a, real_t p_b) { return Math::abs(p_a - p_b) <= tolerance; };
	if (near_value(origin.x, -other_size.x) && near_value(origin.y, 0.0) && near_value(origin.z, 0.0) &&
			dimensions.y == other_dimensions.y && dimensions.z == other_dimensions.z) {
		return VoxelVolume3D::NEIGHBOR_NEGATIVE_X;
	}
	if (near_value(origin.x, size.x) && near_value(origin.y, 0.0) && near_value(origin.z, 0.0) &&
			dimensions.y == other_dimensions.y && dimensions.z == other_dimensions.z) {
		return VoxelVolume3D::NEIGHBOR_POSITIVE_X;
	}
	if (near_value(origin.x, 0.0) && near_value(origin.y, -other_size.y) && near_value(origin.z, 0.0) &&
			dimensions.x == other_dimensions.x && dimensions.z == other_dimensions.z) {
		return VoxelVolume3D::NEIGHBOR_NEGATIVE_Y;
	}
	if (near_value(origin.x, 0.0) && near_value(origin.y, size.y) && near_value(origin.z, 0.0) &&
			dimensions.x == other_dimensions.x && dimensions.z == other_dimensions.z) {
		return VoxelVolume3D::NEIGHBOR_POSITIVE_Y;
	}
	if (near_value(origin.x, 0.0) && near_value(origin.y, 0.0) && near_value(origin.z, -other_size.z) &&
			dimensions.x == other_dimensions.x && dimensions.y == other_dimensions.y) {
		return VoxelVolume3D::NEIGHBOR_NEGATIVE_Z;
	}
	if (near_value(origin.x, 0.0) && near_value(origin.y, 0.0) && near_value(origin.z, size.z) &&
			dimensions.x == other_dimensions.x && dimensions.y == other_dimensions.y) {
		return VoxelVolume3D::NEIGHBOR_POSITIVE_Z;
	}
	return -1;
}

VoxelVolumeStreamingManager::VoxelVolumeStreamingManager() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = this;
	_register_occupancy_globals();
}

VoxelVolumeStreamingManager::~VoxelVolumeStreamingManager() {
	// SceneTree removes Object callables during teardown. At this initialization
	// level its signals may already be gone, so querying them here is invalid.
	connected_tree = nullptr;
	_unregister_occupancy_globals();
	if (singleton == this) {
		singleton = nullptr;
	}
}

void VoxelVolumeStreamingManager::_connect_tree(SceneTree *p_tree) {
	if (connected_tree == p_tree) {
		return;
	}
	if (connected_tree != nullptr && connected_tree->is_connected(SNAME("process_frame"), callable_mp(this, &VoxelVolumeStreamingManager::_process_frame))) {
		connected_tree->disconnect(SNAME("process_frame"), callable_mp(this, &VoxelVolumeStreamingManager::_process_frame));
	}
	connected_tree = p_tree;
	if (connected_tree != nullptr) {
		connected_tree->connect(SNAME("process_frame"), callable_mp(this, &VoxelVolumeStreamingManager::_process_frame));
	}
}

void VoxelVolumeStreamingManager::register_volume(VoxelVolume3D *p_volume) {
	ERR_FAIL_NULL(p_volume);
	volumes.insert(p_volume->get_instance_id());
	neighbors_dirty = true;
	occupancy_dirty = true;
	_connect_tree(p_volume->get_tree());
}

void VoxelVolumeStreamingManager::unregister_volume(VoxelVolume3D *p_volume) {
	if (p_volume != nullptr) {
		volumes.erase(p_volume->get_instance_id());
		neighbors_dirty = true;
		occupancy_dirty = true;
	}
}

void VoxelVolumeStreamingManager::_process_frame() {
	// Residency does not need per-frame precision. This amortizes ranking costs
	// even with hundreds of 100^3 volumes.
	if (streaming_pending || ++frame_counter >= 12) {
		frame_counter = 0;
		_update_streaming();
	}
	// Loading a large resident set is deliberately spread over several frames.
	// Rebuilding all adjacency and occupancy data after every small batch would
	// turn that amortization back into a startup stall.
	if (!streaming_pending && neighbors_dirty) {
		_update_neighbors();
	} else if (!streaming_pending && !neighbor_refresh_volumes.is_empty()) {
		_update_dirty_neighbors();
	}
	const int occupancy_update_frames = CLAMP(int(GLOBAL_GET("rendering/voxel_volume/occupancy_shadows/update_frames")), 1, 120);
	if (!streaming_pending && ++occupancy_frame_counter >= occupancy_update_frames) {
		occupancy_frame_counter = 0;
		_update_occupancy();
	}
}

void VoxelVolumeStreamingManager::_upload_occupancy(const PackedByteArray &p_bytes, int p_resolution, bool p_coarse) {
	Vector<Ref<Image>> slices;
	slices.resize(p_resolution);
	const int slice_size = p_resolution * p_resolution;
	for (int z = 0; z < p_resolution; z++) {
		PackedByteArray slice;
		slice.resize(slice_size);
		std::memcpy(slice.ptrw(), p_bytes.ptr() + z * slice_size, slice_size);
		slices.write[z] = Image::create_from_data(p_resolution, p_resolution, false, Image::FORMAT_L8, slice);
	}
	Ref<ImageTexture3D> &texture = p_coarse ? occupancy_coarse_texture : occupancy_texture;
	bool &texture_created = p_coarse ? occupancy_coarse_texture_created : occupancy_texture_created;
	if (texture.is_null()) {
		texture.instantiate();
	}
	if (!texture_created || texture->get_width() != p_resolution || texture->get_height() != p_resolution || texture->get_depth() != p_resolution) {
		ERR_FAIL_COND(texture->create(Image::FORMAT_L8, p_resolution, p_resolution, p_resolution, false, slices) != OK);
		texture_created = true;
	} else {
		texture->update(slices);
	}
}

void VoxelVolumeStreamingManager::_register_occupancy_globals() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	PackedByteArray empty;
	empty.resize(1);
	empty.set(0, 0);
	_upload_occupancy(empty, 1, false);
	_upload_occupancy(empty, 1, true);

	rendering_server->global_shader_parameter_add(OCCUPANCY_TEXTURE_NAME, RSE::GLOBAL_VAR_TYPE_SAMPLER3D, occupancy_texture->get_rid());
	rendering_server->global_shader_parameter_add(OCCUPANCY_COARSE_TEXTURE_NAME, RSE::GLOBAL_VAR_TYPE_SAMPLER3D, occupancy_coarse_texture->get_rid());
	rendering_server->global_shader_parameter_add(OCCUPANCY_ORIGIN_NAME, RSE::GLOBAL_VAR_TYPE_VEC3, Vector3());
	rendering_server->global_shader_parameter_add(OCCUPANCY_COARSE_ORIGIN_NAME, RSE::GLOBAL_VAR_TYPE_VEC3, Vector3());
	rendering_server->global_shader_parameter_add(OCCUPANCY_VOXEL_SIZE_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.1);
	rendering_server->global_shader_parameter_add(OCCUPANCY_COARSE_VOXEL_SIZE_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.4);
	rendering_server->global_shader_parameter_add(OCCUPANCY_FINE_DISTANCE_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 2.0);
	rendering_server->global_shader_parameter_add(OCCUPANCY_RESOLUTION_NAME, RSE::GLOBAL_VAR_TYPE_INT, 1);
	rendering_server->global_shader_parameter_add(OCCUPANCY_MAX_DISTANCE_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.0);
	rendering_server->global_shader_parameter_add(OCCUPANCY_SHADOW_BIAS_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.003);
	rendering_server->global_shader_parameter_add(OCCUPANCY_MAX_STEPS_NAME, RSE::GLOBAL_VAR_TYPE_INT, 1);
	rendering_server->global_shader_parameter_add(OCCUPANCY_ENABLED_NAME, RSE::GLOBAL_VAR_TYPE_BOOL, false);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_SHADOW_MASK_NAME, RSE::GLOBAL_VAR_TYPE_SAMPLER2D, RID());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_SHADOW_LIGHT_DIRECTION_NAME, RSE::GLOBAL_VAR_TYPE_VEC3, Vector3(0, 1, 0));
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_SHADOW_READY_NAME, RSE::GLOBAL_VAR_TYPE_BOOL, false);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_NEAR_NAME, RSE::GLOBAL_VAR_TYPE_SAMPLER3D, occupancy_texture->get_rid());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_FAR_NAME, RSE::GLOBAL_VAR_TYPE_SAMPLER3D, occupancy_texture->get_rid());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_DISTANT_NAME, RSE::GLOBAL_VAR_TYPE_SAMPLER3D, occupancy_texture->get_rid());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_NEAR_ORIGIN_NAME, RSE::GLOBAL_VAR_TYPE_VEC3, Vector3());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_FAR_ORIGIN_NAME, RSE::GLOBAL_VAR_TYPE_VEC3, Vector3());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_DISTANT_ORIGIN_NAME, RSE::GLOBAL_VAR_TYPE_VEC3, Vector3());
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_NEAR_CELL_SIZE_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_FAR_CELL_SIZE_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_DISTANT_CELL_SIZE_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_RESOLUTION_NAME, RSE::GLOBAL_VAR_TYPE_INT, 1);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_TRANSITION_CELLS_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 1.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_INTENSITY_NAME, RSE::GLOBAL_VAR_TYPE_FLOAT, 0.0);
	rendering_server->global_shader_parameter_add(VOXEL_FORWARD_INDIRECT_READY_NAME, RSE::GLOBAL_VAR_TYPE_BOOL, false);
}

void VoxelVolumeStreamingManager::_unregister_occupancy_globals() {
	// RenderingServer owns the global table and clears it after scene-level
	// resources are released. Removing entries here invalidates materials that
	// may still be queued for destruction during shutdown.
}

void VoxelVolumeStreamingManager::_update_occupancy() {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rendering_server);
	const bool enabled = RenderingMethod::is_current_voxel_forward_method() &&
			bool(GLOBAL_GET("rendering/voxel_volume/occupancy_shadows/enabled"));
	if (!enabled) {
		rendering_server->global_shader_parameter_set(OCCUPANCY_ENABLED_NAME, false);
		return;
	}

	Camera3D *camera = nullptr;
	for (const ObjectID &id : volumes) {
		VoxelVolume3D *volume = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(id));
		if (volume != nullptr && volume->get_viewport() != nullptr) {
			camera = volume->get_viewport()->get_camera_3d();
			if (camera != nullptr) {
				break;
			}
		}
	}
	if (camera == nullptr) {
		rendering_server->global_shader_parameter_set(OCCUPANCY_ENABLED_NAME, false);
		return;
	}

	const int resolution = CLAMP(int(GLOBAL_GET("rendering/voxel_volume/occupancy_shadows/resolution")), 32, 256);
	const real_t voxel_size = CLAMP(real_t(GLOBAL_GET("rendering/voxel_volume/occupancy_shadows/voxel_size")), real_t(0.01), real_t(10.0));
	const int coarse_scale = CLAMP(int(GLOBAL_GET("rendering/voxel_volume/occupancy_shadows/coarse_scale")), 2, 16);
	const real_t coarse_voxel_size = voxel_size * real_t(coarse_scale);
	const Vector3 camera_position = camera->get_global_position();
	const real_t recenter_distance = real_t(resolution) * voxel_size * real_t(0.2);
	const bool camera_left_center = !occupancy_initialized ||
			Math::abs(camera_position.x - occupancy_center.x) > recenter_distance ||
			Math::abs(camera_position.y - occupancy_center.y) > recenter_distance ||
			Math::abs(camera_position.z - occupancy_center.z) > recenter_distance;
	if (!occupancy_dirty && !camera_left_center && resolution == occupancy_resolution && Math::is_equal_approx(voxel_size, occupancy_voxel_size)) {
		return;
	}

	occupancy_resolution = resolution;
	occupancy_voxel_size = voxel_size;
	occupancy_center = camera_position;
	const real_t half_extent = real_t(resolution) * voxel_size * real_t(0.5);
	const real_t coarse_half_extent = real_t(resolution) * coarse_voxel_size * real_t(0.5);
	occupancy_origin = Vector3(
			Math::floor((camera_position.x - half_extent) / voxel_size) * voxel_size,
			Math::floor((camera_position.y - half_extent) / voxel_size) * voxel_size,
			Math::floor((camera_position.z - half_extent) / voxel_size) * voxel_size);
	occupancy_coarse_origin = Vector3(
			Math::floor((camera_position.x - coarse_half_extent) / coarse_voxel_size) * coarse_voxel_size,
			Math::floor((camera_position.y - coarse_half_extent) / coarse_voxel_size) * coarse_voxel_size,
			Math::floor((camera_position.z - coarse_half_extent) / coarse_voxel_size) * coarse_voxel_size);
	const AABB occupancy_bounds(occupancy_coarse_origin, Vector3(real_t(resolution) * coarse_voxel_size, real_t(resolution) * coarse_voxel_size, real_t(resolution) * coarse_voxel_size));
	const AABB fine_occupancy_bounds(occupancy_origin, Vector3(real_t(resolution) * voxel_size, real_t(resolution) * voxel_size, real_t(resolution) * voxel_size));
	const int mask_resolution = (resolution + 1) / 2;
	const int mask_plane = mask_resolution * mask_resolution;
	PackedByteArray occupancy;
	occupancy.resize(mask_resolution * mask_resolution * mask_resolution);
	occupancy.fill(0);
	PackedByteArray coarse_occupancy;
	coarse_occupancy.resize(mask_resolution * mask_resolution * mask_resolution);
	coarse_occupancy.fill(0);
	uint8_t *occupancy_write = occupancy.ptrw();
	uint8_t *coarse_occupancy_write = coarse_occupancy.ptrw();

	auto set_cell = [&](uint8_t *p_occupancy, const Vector3i &p_cell) {
		if (p_cell.x < 0 || p_cell.y < 0 || p_cell.z < 0 ||
				p_cell.x >= resolution || p_cell.y >= resolution || p_cell.z >= resolution) {
			return;
		}
		const int texel_index = (p_cell.x >> 1) + (p_cell.y >> 1) * mask_resolution + (p_cell.z >> 1) * mask_plane;
		const int bit_index = (p_cell.x & 1) | ((p_cell.y & 1) << 1) | ((p_cell.z & 1) << 2);
		p_occupancy[texel_index] |= uint8_t(1 << bit_index);
	};

	auto fill_world_box = [&](uint8_t *p_occupancy, const Vector3 &p_origin, real_t p_voxel_size, const Vector3 &p_world_minimum, const Vector3 &p_world_maximum) {
		// Pull exact cell boundaries inward before floor/ceil. Decimal voxel
		// sizes such as 0.1 are not exact binary floats; without this tolerance,
		// some boxes grow an extra occupancy cell and create repeating bands.
		const Vector3 cell_epsilon(0.0001, 0.0001, 0.0001);
		const Vector3 relative_minimum = (p_world_minimum - p_origin) / p_voxel_size + cell_epsilon;
		const Vector3 relative_maximum = (p_world_maximum - p_origin) / p_voxel_size - cell_epsilon;
		Vector3i from(
				Math::floor(relative_minimum.x),
				Math::floor(relative_minimum.y),
				Math::floor(relative_minimum.z));
		Vector3i to(
				Math::ceil(relative_maximum.x),
				Math::ceil(relative_maximum.y),
				Math::ceil(relative_maximum.z));
		from = from.clamp(Vector3i(), Vector3i(resolution, resolution, resolution));
		to = to.clamp(Vector3i(), Vector3i(resolution, resolution, resolution));
		for (int z = from.z; z < to.z; z++) {
			for (int y = from.y; y < to.y; y++) {
				for (int x = from.x; x < to.x; x++) {
					set_cell(p_occupancy, Vector3i(x, y, z));
				}
			}
		}
	};

	for (const ObjectID &id : volumes) {
		VoxelVolume3D *volume = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(id));
		if (volume == nullptr || !volume->is_streaming_resident() || volume->get_voxel_data().is_null()) {
			continue;
		}
		const Transform3D transform = volume->get_global_transform();
		if (!transform.basis.is_equal_approx(Basis())) {
			continue;
		}
		const AABB world_bounds = transform.xform(volume->get_aabb());
		if (!world_bounds.intersects(occupancy_bounds)) {
			continue;
		}
		const Ref<VoxelShapeData> data = volume->get_voxel_data();
		const VoxelBrickStorage &storage = data->get_brick_storage();
		const Vector3i dimensions = data->get_dimensions();
		const real_t source_voxel_size = data->get_voxel_size();
		const bool direct_fine_splat = Math::is_equal_approx(source_voxel_size, voxel_size);
		const Vector3 fine_cell_offset_f = (transform.origin - occupancy_origin) / voxel_size;
		const Vector3i fine_cell_offset(
				Math::round(fine_cell_offset_f.x),
				Math::round(fine_cell_offset_f.y),
				Math::round(fine_cell_offset_f.z));
		const bool fine_grid_aligned = direct_fine_splat && Vector3(fine_cell_offset).is_equal_approx(fine_cell_offset_f);
		const Vector3 coarse_cell_offset = (transform.origin - occupancy_coarse_origin) / coarse_voxel_size;
		const Vector3 coarse_fine_offset_f = (transform.origin - occupancy_coarse_origin) / source_voxel_size;
		const Vector3i coarse_fine_offset(
				Math::round(coarse_fine_offset_f.x),
				Math::round(coarse_fine_offset_f.y),
				Math::round(coarse_fine_offset_f.z));
		const bool coarse_source_aligned = direct_fine_splat && Vector3(coarse_fine_offset).is_equal_approx(coarse_fine_offset_f);
		for (int brick_index = 0; brick_index < storage.get_brick_count(); brick_index++) {
			const VoxelBrickStorage::Brick &brick = storage.get_brick(brick_index);
			if (brick.type == VoxelBrickStorage::BRICK_EMPTY ||
					(brick.type == VoxelBrickStorage::BRICK_UNIFORM && brick.uniform_value == 0)) {
				continue;
			}
			const Vector3i brick_origin = storage.brick_index_to_position(brick_index) * VoxelBrickStorage::BRICK_SIZE;
			const Vector3i valid_size = (dimensions - brick_origin).clamp(
					Vector3i(), Vector3i(VoxelBrickStorage::BRICK_SIZE, VoxelBrickStorage::BRICK_SIZE, VoxelBrickStorage::BRICK_SIZE));
			const Vector3 world_minimum = transform.origin + Vector3(brick_origin) * source_voxel_size;
			const Vector3 world_maximum = world_minimum + Vector3(valid_size) * source_voxel_size;
			if (brick.type == VoxelBrickStorage::BRICK_UNIFORM) {
				fill_world_box(occupancy_write, occupancy_origin, voxel_size, world_minimum, world_maximum);
				fill_world_box(coarse_occupancy_write, occupancy_coarse_origin, coarse_voxel_size, world_minimum, world_maximum);
				continue;
			}
			const bool brick_intersects_fine = AABB(world_minimum, world_maximum - world_minimum).intersects(fine_occupancy_bounds);
			const Vector3i coarse_brick_fine = coarse_fine_offset + brick_origin;
			const bool direct_coarse_brick = coarse_source_aligned && coarse_scale <= VoxelBrickStorage::BRICK_SIZE &&
					VoxelBrickStorage::BRICK_SIZE % coarse_scale == 0 &&
					coarse_brick_fine.x % coarse_scale == 0 && coarse_brick_fine.y % coarse_scale == 0 && coarse_brick_fine.z % coarse_scale == 0;
			const int coarse_cells_per_brick = direct_coarse_brick ? VoxelBrickStorage::BRICK_SIZE / coarse_scale : 0;
			uint64_t coarse_subcells = direct_coarse_brick && coarse_scale == 4 ? brick.coarse_occupancy_4 : 0;
			const bool coarse_mask_cached = direct_coarse_brick && coarse_scale == 4;
			if (coarse_mask_cached && coarse_subcells != 0) {
				const Vector3i coarse_cell_base = coarse_brick_fine / coarse_scale;
				for (int subcell = 0; subcell < 8; subcell++) {
					if ((coarse_subcells & (UINT64_C(1) << subcell)) != 0) {
						set_cell(coarse_occupancy_write, coarse_cell_base + Vector3i(
								subcell & 1, (subcell >> 1) & 1, (subcell >> 2) & 1));
					}
				}
				if (!brick_intersects_fine) {
					continue;
				}
			}
			for (int z = 0; z < valid_size.z; z++) {
				for (int y = 0; y < valid_size.y; y++) {
					for (int x = 0; x < valid_size.x; x++) {
						const int local_index = x + y * VoxelBrickStorage::BRICK_SIZE + z * VoxelBrickStorage::BRICK_SIZE * VoxelBrickStorage::BRICK_SIZE;
						if (brick.mixed_values[local_index] == 0) {
							continue;
						}
						const Vector3i source_cell = brick_origin + Vector3i(x, y, z);
						if (fine_grid_aligned) {
							if (brick_intersects_fine) {
								set_cell(occupancy_write, fine_cell_offset + source_cell);
							}
							if (direct_coarse_brick && !coarse_mask_cached) {
								const int subcell = (x / coarse_scale) + (y / coarse_scale) * coarse_cells_per_brick +
										(z / coarse_scale) * coarse_cells_per_brick * coarse_cells_per_brick;
								coarse_subcells |= UINT64_C(1) << subcell;
							} else {
								const Vector3 coarse_cell_f = coarse_cell_offset +
										(Vector3(source_cell) + Vector3(0.5, 0.5, 0.5)) * (source_voxel_size / coarse_voxel_size);
								set_cell(coarse_occupancy_write, Vector3i(
										Math::floor(coarse_cell_f.x),
										Math::floor(coarse_cell_f.y),
										Math::floor(coarse_cell_f.z)));
							}
							continue;
						}
						const Vector3 world_minimum = transform.origin + Vector3(source_cell) * source_voxel_size;
						const Vector3 world_maximum = world_minimum + Vector3(source_voxel_size, source_voxel_size, source_voxel_size);
						fill_world_box(occupancy_write, occupancy_origin, voxel_size, world_minimum, world_maximum);
						fill_world_box(coarse_occupancy_write, occupancy_coarse_origin, coarse_voxel_size, world_minimum, world_maximum);
					}
				}
			}
			if (direct_coarse_brick && !coarse_mask_cached && coarse_subcells != 0) {
				const Vector3i coarse_cell_base = coarse_brick_fine / coarse_scale;
				for (int subcell = 0; subcell < coarse_cells_per_brick * coarse_cells_per_brick * coarse_cells_per_brick; subcell++) {
					if ((coarse_subcells & (UINT64_C(1) << subcell)) == 0) {
						continue;
					}
					set_cell(coarse_occupancy_write, coarse_cell_base + Vector3i(
							subcell % coarse_cells_per_brick,
							(subcell / coarse_cells_per_brick) % coarse_cells_per_brick,
							subcell / (coarse_cells_per_brick * coarse_cells_per_brick)));
				}
			}
		}
	}

	_upload_occupancy(occupancy, mask_resolution, false);
	_upload_occupancy(coarse_occupancy, mask_resolution, true);
	rendering_server->global_shader_parameter_set(OCCUPANCY_TEXTURE_NAME, occupancy_texture->get_rid());
	rendering_server->global_shader_parameter_set(OCCUPANCY_COARSE_TEXTURE_NAME, occupancy_coarse_texture->get_rid());
	rendering_server->global_shader_parameter_set(OCCUPANCY_ORIGIN_NAME, occupancy_origin);
	rendering_server->global_shader_parameter_set(OCCUPANCY_COARSE_ORIGIN_NAME, occupancy_coarse_origin);
	rendering_server->global_shader_parameter_set(OCCUPANCY_VOXEL_SIZE_NAME, voxel_size);
	rendering_server->global_shader_parameter_set(OCCUPANCY_COARSE_VOXEL_SIZE_NAME, coarse_voxel_size);
	rendering_server->global_shader_parameter_set(OCCUPANCY_FINE_DISTANCE_NAME, MAX(real_t(0.0), real_t(GLOBAL_GET("rendering/voxel_volume/occupancy_shadows/fine_distance"))));
	rendering_server->global_shader_parameter_set(OCCUPANCY_RESOLUTION_NAME, resolution);
	rendering_server->global_shader_parameter_set(OCCUPANCY_MAX_DISTANCE_NAME, MIN(real_t(GLOBAL_GET("rendering/voxel_volume/occupancy_shadows/max_distance")), coarse_half_extent * real_t(0.95)));
	rendering_server->global_shader_parameter_set(OCCUPANCY_SHADOW_BIAS_NAME, MAX(real_t(0.0001), real_t(GLOBAL_GET("rendering/voxel_volume/occupancy_shadows/bias"))));
	rendering_server->global_shader_parameter_set(OCCUPANCY_MAX_STEPS_NAME, CLAMP(int(GLOBAL_GET("rendering/voxel_volume/occupancy_shadows/max_steps")), 1, 256));
	rendering_server->global_shader_parameter_set(OCCUPANCY_ENABLED_NAME, true);
	occupancy_dirty = false;
	occupancy_initialized = true;
}

void VoxelVolumeStreamingManager::_update_streaming() {
	const int max_resident = MAX(1, int(GLOBAL_GET("rendering/voxel_volume/max_resident_volumes")));
	const int configured_loads = CLAMP(int(GLOBAL_GET("rendering/voxel_volume/max_loads_per_frame")), 1, 64);
	const real_t global_distance = MAX(real_t(0.0), real_t(GLOBAL_GET("rendering/voxel_volume/streaming_distance")));
	streaming_pending = false;
	Vector<VoxelStreamingCandidate> candidates;
	Vector<ObjectID> stale;
	for (const ObjectID &id : volumes) {
		VoxelVolume3D *volume = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(id));
		if (volume == nullptr) {
			stale.push_back(id);
			continue;
		}
		if (volume->get_streaming_mode() == VoxelVolume3D::STREAMING_ALWAYS_RESIDENT) {
			volume->set_streaming_resident(true);
			continue;
		}
		if (volume->get_streaming_mode() == VoxelVolume3D::STREAMING_MANUAL) {
			continue;
		}
		Viewport *viewport = volume->get_viewport();
		Camera3D *camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
		if (camera == nullptr) {
			// A scene can enter the tree before its camera becomes current. Waiting
			// one frame prevents every automatic volume from uploading at once.
			streaming_pending = true;
			continue;
		}
		const real_t distance_limit = volume->get_streaming_distance() > 0.0 ? volume->get_streaming_distance() : global_distance;
		const Vector3 center = volume->get_global_transform().xform(volume->get_aabb().get_center());
		const real_t distance_squared = center.distance_squared_to(camera->get_global_position());
		if (distance_limit <= 0.0 || distance_squared <= distance_limit * distance_limit) {
			VoxelStreamingCandidate candidate;
			candidate.volume = volume;
			candidate.distance_squared = distance_squared;
			candidates.push_back(candidate);
		} else {
			volume->set_streaming_resident(false);
		}
	}
	for (const ObjectID &id : stale) {
		volumes.erase(id);
	}
	candidates.sort();
	int remaining_loads = configured_loads;
	if (initial_residency_fill) {
		int initial_pending_count = 0;
		const int initial_candidate_count = MIN(candidates.size(), max_resident);
		for (int i = 0; i < initial_candidate_count; i++) {
			initial_pending_count += candidates[i].volume->is_streaming_resident() ? 0 : 1;
		}
		// A fixed low streaming budget makes shadows appear many frames after
		// a large scene has loaded because the world table cannot be finalized
		// until residency stops changing. Complete only the initial fill in
		// roughly eight batches, while retaining the configured low budget for
		// later camera-driven streaming during gameplay.
		if (initial_load_batch_size == 0 && initial_pending_count > 0) {
			initial_load_batch_size = CLAMP((initial_pending_count + 7) / 8, 1, 64);
		}
		remaining_loads = MAX(configured_loads, initial_load_batch_size);
	}
	for (int i = 0; i < candidates.size(); i++) {
		VoxelVolume3D *volume = candidates[i].volume;
		if (i >= max_resident) {
			volume->set_streaming_resident(false);
			continue;
		}
		if (volume->is_streaming_resident()) {
			continue;
		}
		if (remaining_loads > 0) {
			volume->set_streaming_resident(true);
			remaining_loads--;
		} else {
			streaming_pending = true;
		}
	}
	if (!streaming_pending) {
		initial_residency_fill = false;
	}
}

void VoxelVolumeStreamingManager::_update_neighbors() {
	neighbors_dirty = false;
	neighbor_refresh_volumes.clear();
	Vector<VoxelNeighborSet> sets;
	Vector<ObjectID> stale;
	for (const ObjectID &id : volumes) {
		VoxelVolume3D *volume = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(id));
		if (volume == nullptr) {
			stale.push_back(id);
			continue;
		}
		VoxelNeighborSet set;
		set.volume = volume;
		sets.push_back(set);
	}
	for (const ObjectID &id : stale) {
		volumes.erase(id);
	}

	for (int i = 0; i < sets.size(); i++) {
		for (int j = 0; j < sets.size(); j++) {
			if (i == j) {
				continue;
			}
			const int face = _get_neighbor_face(sets[i].volume, sets[j].volume);
			if (face >= 0 && sets[i].faces[face] == nullptr) {
				sets.write[i].faces[face] = sets[j].volume;
			}
		}
	}
	for (VoxelNeighborSet &set : sets) {
		set.volume->update_neighbor_faces(set.faces);
	}
}

void VoxelVolumeStreamingManager::mark_volume_neighbors_dirty(VoxelVolume3D *p_volume) {
	if (p_volume != nullptr) {
		neighbor_refresh_volumes.insert(p_volume->get_instance_id());
	}
}

void VoxelVolumeStreamingManager::_update_dirty_neighbors() {
	HashSet<ObjectID> affected(neighbor_refresh_volumes);
	neighbor_refresh_volumes.clear();
	Vector<VoxelVolume3D *> live_volumes;
	Vector<ObjectID> stale;
	for (const ObjectID &id : volumes) {
		VoxelVolume3D *volume = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(id));
		if (volume == nullptr) {
			stale.push_back(id);
		} else {
			live_volumes.push_back(volume);
		}
	}
	for (const ObjectID &id : stale) {
		volumes.erase(id);
	}

	// A boundary edit changes the edited face and the reciprocal face on the
	// adjacent volume. Expand only to those immediate neighbors.
	Vector<ObjectID> requested;
	for (const ObjectID &id : affected) {
		requested.push_back(id);
	}
	for (const ObjectID &id : requested) {
		VoxelVolume3D *edited = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(id));
		if (edited == nullptr) {
			continue;
		}
		for (VoxelVolume3D *candidate : live_volumes) {
			if (candidate != edited && (_get_neighbor_face(edited, candidate) >= 0 || _get_neighbor_face(candidate, edited) >= 0)) {
				affected.insert(candidate->get_instance_id());
			}
		}
	}

	for (const ObjectID &id : affected) {
		VoxelVolume3D *target = Object::cast_to<VoxelVolume3D>(ObjectDB::get_instance(id));
		if (target == nullptr) {
			continue;
		}
		VoxelVolume3D *faces[VoxelVolume3D::NEIGHBOR_FACE_COUNT] = {};
		for (VoxelVolume3D *candidate : live_volumes) {
			if (candidate == target) {
				continue;
			}
			const int face = _get_neighbor_face(target, candidate);
			if (face >= 0 && faces[face] == nullptr) {
				faces[face] = candidate;
			}
		}
		target->update_neighbor_faces(faces);
	}
}
