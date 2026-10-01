/**************************************************************************/
/*  voxel_forward_volume_storage.cpp                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "voxel_forward_volume_storage.h"

#include "core/config/project_settings.h"
#include "core/os/os.h"
#include "core/profiling/profiling.h"
#include "servers/rendering/rendering_device.h"

#include <cstdint>
#include <cfloat>
#include <cstring>

namespace RendererSceneRenderImplementation {

namespace {

constexpr int OCCUPANCY_BRICK_SIZE = 8;
constexpr int OCCUPANCY_BRICK_BYTES = 64;
constexpr int32_t WORLD_DIRECTORY_TOMBSTONE_COORDINATE = INT32_MIN;
constexpr uint32_t WORLD_DIRECTORY_TOMBSTONE_CODE = UINT32_MAX;

struct WorldBrickBits {
	uint64_t bits[8] = {};
};

struct WorldDirectoryEntry {
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
	uint32_t code = 0;
};

static_assert(sizeof(WorldDirectoryEntry) == 16);

struct GridAlignedVolumeTransform {
	Vector3i world_min_voxel;
	Vector3i world_dimensions;
	int world_axis_for_local[3] = { 0, 1, 2 };
	int sign_for_local[3] = { 1, 1, 1 };
	bool identity_orientation = true;
};

struct Vector3iLexicographicLess {
	_FORCE_INLINE_ bool operator()(const Vector3i &p_a, const Vector3i &p_b) const {
		if (p_a.x != p_b.x) return p_a.x < p_b.x;
		if (p_a.y != p_b.y) return p_a.y < p_b.y;
		return p_a.z < p_b.z;
	}
};

uint32_t _read_u32(const PackedByteArray &p_bytes, int p_offset) {
	return uint32_t(p_bytes[p_offset]) |
			(uint32_t(p_bytes[p_offset + 1]) << 8) |
			(uint32_t(p_bytes[p_offset + 2]) << 16) |
			(uint32_t(p_bytes[p_offset + 3]) << 24);
}

uint32_t _world_brick_hash(const Vector3i &p_position) {
	return uint32_t(p_position.x) * 73856093u ^ uint32_t(p_position.y) * 19349663u ^ uint32_t(p_position.z) * 83492791u;
}

bool _read_volume_brick_bits(const VoxelForwardVolumeStorage::Volume &p_volume, int p_brick_index, WorldBrickBits &r_bits) {
	r_bits = WorldBrickBits();
	const int brick_count = p_volume.brick_dimensions.x * p_volume.brick_dimensions.y * p_volume.brick_dimensions.z;
	if (p_brick_index < 0 || p_brick_index >= brick_count || p_volume.occupancy_directory_cpu.size() < (p_brick_index + 1) * 4) {
		return false;
	}
	const uint32_t code = _read_u32(p_volume.occupancy_directory_cpu, p_brick_index * 4);
	if (code == 0) {
		return true;
	}
	if (code == 1) {
		for (int word = 0; word < 8; word++) {
			r_bits.bits[word] = UINT64_MAX;
		}
	} else {
		const int source_offset = int(code - 2) * OCCUPANCY_BRICK_BYTES;
		if (source_offset < 0 || source_offset + OCCUPANCY_BRICK_BYTES > p_volume.occupancy_bricks_cpu.size()) {
			return false;
		}
		for (int word = 0; word < 8; word++) {
			uint64_t source_bits = 0;
			for (int byte = 0; byte < 8; byte++) {
				source_bits |= uint64_t(p_volume.occupancy_bricks_cpu[source_offset + word * 8 + byte]) << (byte * 8);
			}
			r_bits.bits[word] = source_bits;
		}
	}

	// Uniform edge bricks may extend past non-multiple-of-eight dimensions.
	const Vector3i local_brick(
			p_brick_index % p_volume.brick_dimensions.x,
			(p_brick_index / p_volume.brick_dimensions.x) % p_volume.brick_dimensions.y,
			p_brick_index / (p_volume.brick_dimensions.x * p_volume.brick_dimensions.y));
	const Vector3i local_start = local_brick * OCCUPANCY_BRICK_SIZE;
	if (local_start.x + OCCUPANCY_BRICK_SIZE <= p_volume.dimensions.x &&
			local_start.y + OCCUPANCY_BRICK_SIZE <= p_volume.dimensions.y &&
			local_start.z + OCCUPANCY_BRICK_SIZE <= p_volume.dimensions.z) {
		return true;
	}
	for (int local_index = 0; local_index < OCCUPANCY_BRICK_SIZE * OCCUPANCY_BRICK_SIZE * OCCUPANCY_BRICK_SIZE; local_index++) {
		const Vector3i local_voxel(local_index & 7, (local_index >> 3) & 7, local_index >> 6);
		const Vector3i volume_voxel = local_start + local_voxel;
		if (volume_voxel.x >= p_volume.dimensions.x || volume_voxel.y >= p_volume.dimensions.y || volume_voxel.z >= p_volume.dimensions.z) {
			r_bits.bits[local_index >> 6] &= ~(uint64_t(1) << (local_index & 63));
		}
	}
	return true;
}

bool _volume_voxel_occupied(const VoxelForwardVolumeStorage::Volume &p_volume, const Vector3i &p_voxel) {
	if (p_voxel.x < 0 || p_voxel.y < 0 || p_voxel.z < 0 || p_voxel.x >= p_volume.dimensions.x || p_voxel.y >= p_volume.dimensions.y || p_voxel.z >= p_volume.dimensions.z) {
		return false;
	}
	const Vector3i local_brick = p_voxel / OCCUPANCY_BRICK_SIZE;
	const int brick_index = local_brick.x + local_brick.y * p_volume.brick_dimensions.x + local_brick.z * p_volume.brick_dimensions.x * p_volume.brick_dimensions.y;
	if (p_volume.occupancy_directory_cpu.size() < (brick_index + 1) * 4) {
		return false;
	}
	const uint32_t code = _read_u32(p_volume.occupancy_directory_cpu, brick_index * 4);
	if (code == 0) {
		return false;
	}
	if (code == 1) {
		return true;
	}
	const int source_offset = int(code - 2) * OCCUPANCY_BRICK_BYTES;
	if (source_offset < 0 || source_offset + OCCUPANCY_BRICK_BYTES > p_volume.occupancy_bricks_cpu.size()) {
		return false;
	}
	const Vector3i local_voxel = p_voxel - local_brick * OCCUPANCY_BRICK_SIZE;
	const int local_index = local_voxel.x + local_voxel.y * OCCUPANCY_BRICK_SIZE + local_voxel.z * OCCUPANCY_BRICK_SIZE * OCCUPANCY_BRICK_SIZE;
	return (p_volume.occupancy_bricks_cpu[source_offset + (local_index >> 3)] & uint8_t(1u << (local_index & 7))) != 0;
}

bool _world_brick_bits_equal(const WorldBrickBits &p_a, const WorldBrickBits &p_b) {
	for (int word = 0; word < 8; word++) {
		if (p_a.bits[word] != p_b.bits[word]) {
			return false;
		}
	}
	return true;
}

bool _world_brick_bits_empty(const WorldBrickBits &p_bits) {
	for (int word = 0; word < 8; word++) {
		if (p_bits.bits[word] != 0) {
			return false;
		}
	}
	return true;
}

bool _world_brick_bits_uniform(const WorldBrickBits &p_bits) {
	for (int word = 0; word < 8; word++) {
		if (p_bits.bits[word] != UINT64_MAX) {
			return false;
		}
	}
	return true;
}

void _write_world_brick_bytes(PackedByteArray &r_bytes, uint32_t p_slot, const WorldBrickBits &p_bits) {
	const int required_size = int(p_slot + 1) * OCCUPANCY_BRICK_BYTES;
	if (r_bytes.size() < required_size) {
		r_bytes.resize(required_size);
	}
	uint8_t *write = r_bytes.ptrw() + int(p_slot) * OCCUPANCY_BRICK_BYTES;
	for (int word = 0; word < 8; word++) {
		for (int byte = 0; byte < 8; byte++) {
			write[word * 8 + byte] = uint8_t((p_bits.bits[word] >> (byte * 8)) & 0xFF);
		}
	}
}

bool _get_grid_orientation(const VoxelForwardVolumeStorage::Volume &p_volume, float &r_world_voxel_size, GridAlignedVolumeTransform &r_transform) {
	if (!p_volume.casts_shadow) {
		return false;
	}
	const float epsilon = 0.0001f;
	const float scale = p_volume.transform.basis.get_column(0).length();
	if (scale <= epsilon) {
		return false;
	}

	bool used_world_axis[3] = {};
	r_transform.identity_orientation = true;
	r_transform.world_dimensions = Vector3i();
	for (int local_axis = 0; local_axis < 3; local_axis++) {
		const Vector3 basis_axis = p_volume.transform.basis.get_column(local_axis);
		if (!Math::is_equal_approx(basis_axis.length(), scale)) {
			return false;
		}
		const Vector3 normalized_axis = basis_axis / scale;
		int mapped_world_axis = 0;
		float largest_component = Math::abs(normalized_axis[0]);
		for (int world_axis = 1; world_axis < 3; world_axis++) {
			const float component = Math::abs(normalized_axis[world_axis]);
			if (component > largest_component) {
				largest_component = component;
				mapped_world_axis = world_axis;
			}
		}
		const int sign = normalized_axis[mapped_world_axis] < 0.0f ? -1 : 1;
		Vector3 expected_axis;
		expected_axis[mapped_world_axis] = float(sign);
		if (!normalized_axis.is_equal_approx(expected_axis) || used_world_axis[mapped_world_axis]) {
			return false;
		}
		used_world_axis[mapped_world_axis] = true;
		r_transform.world_axis_for_local[local_axis] = mapped_world_axis;
		r_transform.sign_for_local[local_axis] = sign;
		r_transform.world_dimensions[mapped_world_axis] = p_volume.dimensions[local_axis];
		r_transform.identity_orientation = r_transform.identity_orientation && mapped_world_axis == local_axis && sign > 0;
	}

	r_world_voxel_size = p_volume.voxel_size * scale;
	return true;
}

bool _get_aligned_volume_transform(const VoxelForwardVolumeStorage::Volume &p_volume, float p_world_voxel_size, const Vector3 &p_world_origin, GridAlignedVolumeTransform &r_transform) {
	float volume_world_voxel_size = 0.0f;
	if (!_get_grid_orientation(p_volume, volume_world_voxel_size, r_transform) || !Math::is_equal_approx(volume_world_voxel_size, p_world_voxel_size)) {
		return false;
	}
	const Vector3 voxel_anchor = (p_volume.transform.origin - p_world_origin) / p_world_voxel_size;
	const Vector3i rounded_anchor(Math::round(voxel_anchor.x), Math::round(voxel_anchor.y), Math::round(voxel_anchor.z));
	if (!voxel_anchor.is_equal_approx(Vector3(rounded_anchor))) {
		return false;
	}
	r_transform.world_min_voxel = rounded_anchor;
	for (int local_axis = 0; local_axis < 3; local_axis++) {
		if (r_transform.sign_for_local[local_axis] < 0) {
			r_transform.world_min_voxel[r_transform.world_axis_for_local[local_axis]] -= p_volume.dimensions[local_axis];
		}
	}
	return true;
}

Vector3i _local_voxel_to_world(const VoxelForwardVolumeStorage::Volume &p_volume, const GridAlignedVolumeTransform &p_transform, const Vector3i &p_local_voxel) {
	Vector3i world_voxel = p_transform.world_min_voxel;
	for (int local_axis = 0; local_axis < 3; local_axis++) {
		const int world_axis = p_transform.world_axis_for_local[local_axis];
		world_voxel[world_axis] += p_transform.sign_for_local[local_axis] > 0 ? p_local_voxel[local_axis] : p_volume.dimensions[local_axis] - 1 - p_local_voxel[local_axis];
	}
	return world_voxel;
}

Vector3i _world_voxel_to_local(const VoxelForwardVolumeStorage::Volume &p_volume, const GridAlignedVolumeTransform &p_transform, const Vector3i &p_world_voxel) {
	const Vector3i world_offset = p_world_voxel - p_transform.world_min_voxel;
	Vector3i local_voxel;
	for (int local_axis = 0; local_axis < 3; local_axis++) {
		const int mapped_coordinate = world_offset[p_transform.world_axis_for_local[local_axis]];
		local_voxel[local_axis] = p_transform.sign_for_local[local_axis] > 0 ? mapped_coordinate : p_volume.dimensions[local_axis] - 1 - mapped_coordinate;
	}
	return local_voxel;
}

void _local_bounds_to_world(const VoxelForwardVolumeStorage::Volume &p_volume, const GridAlignedVolumeTransform &p_transform, const Vector3i &p_local_begin, const Vector3i &p_local_end, Vector3i &r_world_begin, Vector3i &r_world_end) {
	r_world_begin = p_transform.world_min_voxel;
	r_world_end = p_transform.world_min_voxel;
	for (int local_axis = 0; local_axis < 3; local_axis++) {
		const int world_axis = p_transform.world_axis_for_local[local_axis];
		if (p_transform.sign_for_local[local_axis] > 0) {
			r_world_begin[world_axis] += p_local_begin[local_axis];
			r_world_end[world_axis] += p_local_end[local_axis];
		} else {
			r_world_begin[world_axis] += p_volume.dimensions[local_axis] - p_local_end[local_axis];
			r_world_end[world_axis] += p_volume.dimensions[local_axis] - p_local_begin[local_axis];
		}
	}
}

int _floor_divide_by_brick_size(int p_value) {
	return p_value >= 0 ? p_value / OCCUPANCY_BRICK_SIZE : (p_value - (OCCUPANCY_BRICK_SIZE - 1)) / OCCUPANCY_BRICK_SIZE;
}

int _floor_divide(int p_value, int p_divisor) {
	return p_value >= 0 ? p_value / p_divisor : (p_value - (p_divisor - 1)) / p_divisor;
}

bool _ddgi_read_world_brick(const PackedByteArray &p_directory_bytes, const PackedByteArray &p_brick_bytes, uint32_t p_directory_mask, uint32_t p_max_probe_count, const Vector3i &p_position, WorldBrickBits &r_bits) {
	r_bits = WorldBrickBits();
	if (p_directory_bytes.size() < int(p_directory_mask + 1u) * int(sizeof(WorldDirectoryEntry))) {
		return false;
	}
	const WorldDirectoryEntry *directory = reinterpret_cast<const WorldDirectoryEntry *>(p_directory_bytes.ptr());
	uint32_t slot = _world_brick_hash(p_position) & p_directory_mask;
	for (uint32_t probe = 0; probe <= p_max_probe_count; probe++) {
		const WorldDirectoryEntry &entry = directory[slot];
		if (entry.code == 0) {
			return true;
		}
		if (entry.code != WORLD_DIRECTORY_TOMBSTONE_CODE && entry.x == p_position.x && entry.y == p_position.y && entry.z == p_position.z) {
			if (entry.code == 1) {
				for (int word = 0; word < 8; word++) {
					r_bits.bits[word] = UINT64_MAX;
				}
				return true;
			}
			const int source_offset = int(entry.code - 2u) * OCCUPANCY_BRICK_BYTES;
			if (source_offset < 0 || source_offset + OCCUPANCY_BRICK_BYTES > p_brick_bytes.size()) {
				return false;
			}
			for (int word = 0; word < 8; word++) {
				uint64_t value = 0;
				for (int byte = 0; byte < 8; byte++) {
					value |= uint64_t(p_brick_bytes[source_offset + word * 8 + byte]) << (byte * 8);
				}
				r_bits.bits[word] = value;
			}
			return true;
		}
		slot = (slot + 1u) & p_directory_mask;
	}
	return true;
}

bool _ddgi_world_voxel_occupied(const PackedByteArray &p_directory_bytes, const PackedByteArray &p_brick_bytes, uint32_t p_directory_mask, uint32_t p_max_probe_count, const Vector3i &p_voxel) {
	const Vector3i brick(
			_floor_divide_by_brick_size(p_voxel.x),
			_floor_divide_by_brick_size(p_voxel.y),
			_floor_divide_by_brick_size(p_voxel.z));
	WorldBrickBits bits;
	if (!_ddgi_read_world_brick(p_directory_bytes, p_brick_bytes, p_directory_mask, p_max_probe_count, brick, bits)) {
		return false;
	}
	const Vector3i local = p_voxel - brick * OCCUPANCY_BRICK_SIZE;
	const int index = local.x + local.y * OCCUPANCY_BRICK_SIZE + local.z * OCCUPANCY_BRICK_SIZE * OCCUPANCY_BRICK_SIZE;
	return (bits.bits[index >> 6] & (uint64_t(1) << (index & 63))) != 0;
}

struct DdgiRayTransition {
	bool hit = false;
	bool backface = false;
	float distance = 0.0f;
	Vector3 normal;
};

DdgiRayTransition _ddgi_trace_voxel_transition(const PackedByteArray &p_directory_bytes, const PackedByteArray &p_brick_bytes, uint32_t p_directory_mask, uint32_t p_max_probe_count, const Vector3 &p_origin, const Vector3 &p_direction, float p_max_distance, uint32_t p_max_steps) {
	DdgiRayTransition result;
	const Vector3 direction = p_direction.normalized();
	Vector3i voxel(Math::floor(p_origin.x), Math::floor(p_origin.y), Math::floor(p_origin.z));
	const bool started_inside = _ddgi_world_voxel_occupied(p_directory_bytes, p_brick_bytes, p_directory_mask, p_max_probe_count, voxel);
	Vector3i step(direction.x < 0.0f ? -1 : 1, direction.y < 0.0f ? -1 : 1, direction.z < 0.0f ? -1 : 1);
	Vector3 t_delta(
			direction.x == 0.0f ? FLT_MAX : Math::abs(1.0f / direction.x),
			direction.y == 0.0f ? FLT_MAX : Math::abs(1.0f / direction.y),
			direction.z == 0.0f ? FLT_MAX : Math::abs(1.0f / direction.z));
	Vector3 boundary(
			step.x > 0 ? float(voxel.x + 1) : float(voxel.x),
			step.y > 0 ? float(voxel.y + 1) : float(voxel.y),
			step.z > 0 ? float(voxel.z + 1) : float(voxel.z));
	Vector3 t_max(
			direction.x == 0.0f ? FLT_MAX : (boundary.x - p_origin.x) / direction.x,
			direction.y == 0.0f ? FLT_MAX : (boundary.y - p_origin.y) / direction.y,
			direction.z == 0.0f ? FLT_MAX : (boundary.z - p_origin.z) / direction.z);
	bool previous_occupied = started_inside;
	for (uint32_t iteration = 0; iteration < p_max_steps; iteration++) {
		int axis = 0;
		if (t_max.y < t_max.x) {
			axis = 1;
		}
		if (t_max.z < t_max[axis]) {
			axis = 2;
		}
		const float distance = t_max[axis];
		if (distance > p_max_distance) {
			break;
		}
		voxel[axis] += step[axis];
		t_max[axis] += t_delta[axis];
		const bool occupied = _ddgi_world_voxel_occupied(p_directory_bytes, p_brick_bytes, p_directory_mask, p_max_probe_count, voxel);
		if (occupied != previous_occupied) {
			result.hit = true;
			result.backface = previous_occupied && !occupied;
			result.distance = MAX(distance, 0.0f);
			result.normal = Vector3();
			result.normal[axis] = result.backface ? float(step[axis]) : -float(step[axis]);
			return result;
		}
		previous_occupied = occupied;
	}
	return result;
}

Vector3i _ddgi_cell_size_voxels(uint32_t p_spacing_voxels) {
	const int spacing = MAX(1, int(p_spacing_voxels));
	return Vector3i(spacing, spacing, spacing);
}

Vector3i _ddgi_page_coordinate(const Vector3i &p_cell) {
	const int size = VoxelForwardVolumeStorage::DDGI_PLACEMENT_PAGE_SIZE;
	return Vector3i(_floor_divide(p_cell.x, size), _floor_divide(p_cell.y, size), _floor_divide(p_cell.z, size));
}

uint32_t _ddgi_page_local_index(const Vector3i &p_cell, const Vector3i &p_page) {
	const Vector3i local = p_cell - p_page * VoxelForwardVolumeStorage::DDGI_PLACEMENT_PAGE_SIZE;
	return uint32_t(local.x + local.y * VoxelForwardVolumeStorage::DDGI_PLACEMENT_PAGE_SIZE + local.z * VoxelForwardVolumeStorage::DDGI_PLACEMENT_PAGE_SIZE * VoxelForwardVolumeStorage::DDGI_PLACEMENT_PAGE_SIZE);
}

uint32_t _ddgi_mask_bit_count(uint64_t p_mask) {
	uint32_t count = 0;
	while (p_mask != 0) {
		p_mask &= p_mask - 1;
		count++;
	}
	return count;
}

struct DdgiPagePriorityLess {
	_FORCE_INLINE_ bool operator()(const Vector3i &p_a, const Vector3i &p_b) const {
		// Descending distance means pop_back() consumes pages nearest the stable
		// world origin first. Ties remain deterministic and camera-independent.
		const int64_t distance_a = p_a.length_squared();
		const int64_t distance_b = p_b.length_squared();
		if (distance_a != distance_b) return distance_a > distance_b;
		if (p_a.x != p_b.x) return p_a.x > p_b.x;
		if (p_a.y != p_b.y) return p_a.y > p_b.y;
		return p_a.z > p_b.z;
	}
};

void _ddgi_mark_page_cell(HashMap<Vector3i, VoxelForwardVolumeStorage::DdgiPlacementPageSeed> &r_pages, const Vector3i &p_cell, bool p_occupied) {
	const Vector3i page_coordinate = _ddgi_page_coordinate(p_cell);
	VoxelForwardVolumeStorage::DdgiPlacementPageSeed *seed = r_pages.getptr(page_coordinate);
	if (seed == nullptr) {
		r_pages.insert(page_coordinate, VoxelForwardVolumeStorage::DdgiPlacementPageSeed());
		seed = r_pages.getptr(page_coordinate);
	}
	const uint64_t bit = uint64_t(1) << _ddgi_page_local_index(p_cell, page_coordinate);
	seed->relevant_mask |= bit;
	if (p_occupied) {
		seed->occupied_mask |= bit;
	}
}

void _ddgi_add_cells_for_brick(const Vector3i &p_brick, uint32_t p_spacing_voxels, HashSet<Vector3i> &r_cells) {
	const Vector3i cell_size = _ddgi_cell_size_voxels(p_spacing_voxels);
	const Vector3i voxel_begin = p_brick * OCCUPANCY_BRICK_SIZE;
	const Vector3i voxel_end = voxel_begin + Vector3i(OCCUPANCY_BRICK_SIZE - 1, OCCUPANCY_BRICK_SIZE - 1, OCCUPANCY_BRICK_SIZE - 1);
	const Vector3i first_cell(_floor_divide(voxel_begin.x, cell_size.x), _floor_divide(voxel_begin.y, cell_size.y), _floor_divide(voxel_begin.z, cell_size.z));
	const Vector3i last_cell(_floor_divide(voxel_end.x, cell_size.x), _floor_divide(voxel_end.y, cell_size.y), _floor_divide(voxel_end.z, cell_size.z));
	for (int z = first_cell.z; z <= last_cell.z; z++) {
		for (int y = first_cell.y; y <= last_cell.y; y++) {
			for (int x = first_cell.x; x <= last_cell.x; x++) {
				r_cells.insert(Vector3i(x, y, z));
			}
		}
	}
}

bool _ddgi_cell_has_occupied_brick(const HashSet<Vector3i> &p_occupied_bricks, uint32_t p_spacing_voxels, const Vector3i &p_cell) {
	const Vector3i cell_size = _ddgi_cell_size_voxels(p_spacing_voxels);
	const Vector3i voxel_begin = p_cell * cell_size;
	const Vector3i voxel_end = voxel_begin + cell_size - Vector3i(1, 1, 1);
	const Vector3i first_brick(_floor_divide(voxel_begin.x, OCCUPANCY_BRICK_SIZE), _floor_divide(voxel_begin.y, OCCUPANCY_BRICK_SIZE), _floor_divide(voxel_begin.z, OCCUPANCY_BRICK_SIZE));
	const Vector3i last_brick(_floor_divide(voxel_end.x, OCCUPANCY_BRICK_SIZE), _floor_divide(voxel_end.y, OCCUPANCY_BRICK_SIZE), _floor_divide(voxel_end.z, OCCUPANCY_BRICK_SIZE));
	for (int z = first_brick.z; z <= last_brick.z; z++) {
		for (int y = first_brick.y; y <= last_brick.y; y++) {
			for (int x = first_brick.x; x <= last_brick.x; x++) {
				if (p_occupied_bricks.has(Vector3i(x, y, z))) {
					return true;
				}
			}
		}
	}
	return false;
}

void _ddgi_add_brick_page_seeds(const Vector3i &p_brick, uint32_t p_spacing_voxels, HashMap<Vector3i, VoxelForwardVolumeStorage::DdgiPlacementPageSeed> &r_pages) {
	static const Vector3i axial[7] = {
		Vector3i(), Vector3i(1, 0, 0), Vector3i(-1, 0, 0), Vector3i(0, 1, 0),
		Vector3i(0, -1, 0), Vector3i(0, 0, 1), Vector3i(0, 0, -1)
	};
	HashSet<Vector3i> occupied_cells;
	_ddgi_add_cells_for_brick(p_brick, p_spacing_voxels, occupied_cells);
	for (const Vector3i &cell : occupied_cells) {
		for (uint32_t neighbor = 0; neighbor < 7; neighbor++) {
			_ddgi_mark_page_cell(r_pages, cell + axial[neighbor], neighbor == 0);
		}
	}
}

uint64_t _ddgi_stable_probe_id(uint32_t p_lod, const Vector3i &p_cell) {
	uint64_t value = uint64_t(_world_brick_hash(p_cell));
	value ^= uint64_t(_world_brick_hash(Vector3i(p_cell.z, p_cell.x, p_cell.y))) << 32;
	return value ^ (uint64_t(p_lod) << 60);
}

VoxelForwardVolumeStorage::DdgiProbePlacement _ddgi_relocate_probe_bounded(const PackedByteArray &p_directory_bytes, const PackedByteArray &p_brick_bytes, uint32_t p_directory_mask, uint32_t p_max_probe_count, const Vector3 &p_world_origin, float p_voxel_size, uint32_t p_lod, uint32_t p_spacing_voxels, const Vector3i &p_cell, bool p_cell_has_occupied_brick, uint64_t p_revision) {
	static constexpr uint32_t PLACEMENT_RAYS = 16;
	static constexpr uint32_t RELOCATION_ITERATIONS = 5;
	static constexpr uint32_t MAX_DDA_STEPS = 192;
	static constexpr float GOLDEN_ANGLE = 2.39996322972865332f;
	static constexpr float TAU = 6.28318530717958648f;
	VoxelForwardVolumeStorage::DdgiProbePlacement placement;
	placement.logical_cell = p_cell;
	placement.stable_id = _ddgi_stable_probe_id(p_lod, p_cell);
	placement.revision = p_revision;
	const Vector3i cell_size_i = _ddgi_cell_size_voxels(p_spacing_voxels);
	const Vector3 cell_size = Vector3(cell_size_i);
	const Vector3 logical_position = Vector3(p_cell * cell_size_i) + cell_size * 0.5f;
	Vector3 position = logical_position;
	placement.flags = p_cell_has_occupied_brick ? VoxelForwardVolumeStorage::DDGI_PROBE_CELL_OCCUPIED | VoxelForwardVolumeStorage::DDGI_PROBE_CELL_NEAR_SURFACE : 0u;
	if (!p_cell_has_occupied_brick) {
		placement.valid = true;
		placement.initial_state = VoxelForwardVolumeStorage::DDGI_PROBE_NEWLY_VIGILANT;
		placement.position = p_world_origin + position * p_voxel_size;
		return placement;
	}

	const Vector3 max_offset = cell_size * 0.45f;
	const float max_distance = MIN(float(MAX_DDA_STEPS), cell_size.length());
	for (uint32_t relocation = 0; relocation < RELOCATION_ITERATIONS; relocation++) {
		const bool inside = _ddgi_world_voxel_occupied(p_directory_bytes, p_brick_bytes, p_directory_mask, p_max_probe_count, Vector3i(Math::floor(position.x), Math::floor(position.y), Math::floor(position.z)));
		uint32_t backface_count = 0;
		float nearest_backface = FLT_MAX;
		Vector3 nearest_backface_direction;
		Vector3 repulsion;
		uint32_t close_frontfaces = 0;
		for (uint32_t ray = 0; ray < PLACEMENT_RAYS; ray++) {
			const float y = 1.0f - 2.0f * (float(ray) + 0.5f) / float(PLACEMENT_RAYS);
			const float radius = Math::sqrt(MAX(0.0f, 1.0f - y * y));
			const float angle = GOLDEN_ANGLE * float(ray) + float(placement.stable_id & 1023u) * (TAU / 1024.0f);
			const Vector3 direction(Math::cos(angle) * radius, y, Math::sin(angle) * radius);
			const DdgiRayTransition hit = _ddgi_trace_voxel_transition(p_directory_bytes, p_brick_bytes, p_directory_mask, p_max_probe_count, position, direction, max_distance, MAX_DDA_STEPS);
			if (!hit.hit) {
				continue;
			}
			if (hit.backface) {
				backface_count++;
				if (hit.distance < nearest_backface) {
					nearest_backface = hit.distance;
					nearest_backface_direction = direction;
				}
			} else if (hit.distance < MIN(cell_size.x, MIN(cell_size.y, cell_size.z)) * 0.35f) {
				repulsion += hit.normal * (1.0f - hit.distance / MAX(max_distance, 0.001f));
				close_frontfaces++;
			}
		}
		Vector3 candidate = position;
		if (inside && backface_count > PLACEMENT_RAYS / 4 && nearest_backface < FLT_MAX) {
			candidate += nearest_backface_direction * (nearest_backface + 0.55f);
		} else if (!inside && close_frontfaces > 0 && !repulsion.is_zero_approx()) {
			candidate += repulsion.normalized() * MIN(cell_size.x, MIN(cell_size.y, cell_size.z)) * 0.2f;
		} else {
			break;
		}
		Vector3 offset = candidate - logical_position;
		offset.x = CLAMP(offset.x, -max_offset.x, max_offset.x);
		offset.y = CLAMP(offset.y, -max_offset.y, max_offset.y);
		offset.z = CLAMP(offset.z, -max_offset.z, max_offset.z);
		candidate = logical_position + offset;
		if (_ddgi_world_voxel_occupied(p_directory_bytes, p_brick_bytes, p_directory_mask, p_max_probe_count, Vector3i(Math::floor(candidate.x), Math::floor(candidate.y), Math::floor(candidate.z))) && !inside) {
			break;
		}
		position = candidate;
	}
	const bool remains_inside = _ddgi_world_voxel_occupied(p_directory_bytes, p_brick_bytes, p_directory_mask, p_max_probe_count, Vector3i(Math::floor(position.x), Math::floor(position.y), Math::floor(position.z)));
	placement.valid = !remains_inside;
	placement.flags |= remains_inside ? VoxelForwardVolumeStorage::DDGI_PROBE_CELL_SOLID : 0u;
	placement.initial_state = remains_inside ? VoxelForwardVolumeStorage::DDGI_PROBE_OFF : VoxelForwardVolumeStorage::DDGI_PROBE_NEWLY_VIGILANT;
	placement.offset = (position - logical_position) * p_voxel_size;
	placement.position = p_world_origin + position * p_voxel_size;
	return placement;
}


Error _update_changed_buffer_units(RD *p_rd, RID p_buffer, const PackedByteArray &p_previous, const PackedByteArray &p_current, int p_unit_size) {
	ERR_FAIL_NULL_V(p_rd, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(!p_buffer.is_valid() || p_unit_size <= 0 || p_current.size() % p_unit_size != 0, ERR_INVALID_PARAMETER);
	Vector<Vector2i> changed_runs;
	int changed_bytes = 0;
	int run_start = -1;
	for (int offset = 0; offset < p_current.size(); offset += p_unit_size) {
		const bool changed = offset + p_unit_size > p_previous.size() ||
				std::memcmp(p_previous.ptr() + offset, p_current.ptr() + offset, p_unit_size) != 0;
		if (changed && run_start < 0) {
			run_start = offset;
		} else if (!changed && run_start >= 0) {
			changed_runs.push_back(Vector2i(run_start, offset - run_start));
			changed_bytes += offset - run_start;
			run_start = -1;
		}
	}
	if (run_start >= 0) {
		changed_runs.push_back(Vector2i(run_start, p_current.size() - run_start));
		changed_bytes += p_current.size() - run_start;
	}
	if (changed_runs.is_empty()) {
		return OK;
	}
	// A fragmented update is more expensive than one sequential staging copy.
	if (changed_runs.size() > 1024 || changed_bytes > p_current.size() / 4) {
		return p_rd->buffer_update(p_buffer, 0, p_current.size(), p_current.ptr());
	}
	for (const Vector2i &run : changed_runs) {
		const Error error = p_rd->buffer_update(p_buffer, run.x, run.y, p_current.ptr() + run.x);
		if (error != OK) {
			return error;
		}
	}
	return OK;
}

Error _upload_changed_buffer_slots(RD *p_rd, RID p_buffer, const PackedByteArray &p_source, Vector<uint32_t> &p_slots, int p_unit_size, uint32_t &r_uploaded_bytes, uint32_t &r_upload_calls) {
	ERR_FAIL_NULL_V(p_rd, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(!p_buffer.is_valid() || p_unit_size <= 0, ERR_INVALID_PARAMETER);
	if (p_slots.is_empty()) {
		return OK;
	}

	p_slots.sort();
	uint32_t run_begin = p_slots[0];
	uint32_t run_end = run_begin + 1;
	auto flush_run = [&]() -> Error {
		const uint32_t offset = run_begin * p_unit_size;
		const uint32_t byte_count = (run_end - run_begin) * p_unit_size;
		ERR_FAIL_COND_V(offset + byte_count > uint32_t(p_source.size()), ERR_INVALID_PARAMETER);
		const Error error = p_rd->buffer_update(p_buffer, offset, byte_count, p_source.ptr() + offset);
		if (error == OK) {
			r_uploaded_bytes += byte_count;
			r_upload_calls++;
		}
		return error;
	};
	for (int i = 1; i < p_slots.size(); i++) {
		const uint32_t slot = p_slots[i];
		if (slot < run_end) {
			continue;
		}
		if (slot == run_end) {
			run_end++;
			continue;
		}
		const Error error = flush_run();
		if (error != OK) {
			return error;
		}
		run_begin = slot;
		run_end = slot + 1;
	}
	return flush_run();
}

} // namespace

VoxelForwardVolumeStorage *VoxelForwardVolumeStorage::singleton = nullptr;

void VoxelForwardVolumeStorage::_clear_world_volume_spatial_index() {
	world_volume_spatial_cells.clear();
	world_volume_spatial_entries.clear();
	world_volume_unindexed.clear();
	world_volume_spatial_reference_count = 0;
	world_volume_spatial_index_valid = false;
	world_occupancy.spatial_index_payload_bytes = 0;
}

void VoxelForwardVolumeStorage::_remove_world_volume_spatial_entry(RID p_base) {
	WorldVolumeSpatialEntry *entry = world_volume_spatial_entries.getptr(p_base);
	if (entry == nullptr) {
		world_volume_unindexed.erase(p_base);
		return;
	}
	if (entry->indexed) {
		for (int z = entry->cell_begin.z; z <= entry->cell_end.z; z++) {
			for (int y = entry->cell_begin.y; y <= entry->cell_end.y; y++) {
				for (int x = entry->cell_begin.x; x <= entry->cell_end.x; x++) {
					const Vector3i cell_position(x, y, z);
					Vector<RID> *cell = world_volume_spatial_cells.getptr(cell_position);
					if (cell == nullptr) {
						continue;
					}
					const int index = cell->find(p_base);
					if (index >= 0) {
						cell->remove_at(index);
						world_volume_spatial_reference_count--;
					}
					if (cell->is_empty()) {
						world_volume_spatial_cells.erase(cell_position);
					}
				}
			}
		}
	} else {
		world_volume_unindexed.erase(p_base);
	}
	world_volume_spatial_entries.erase(p_base);
}

void VoxelForwardVolumeStorage::_insert_world_volume_spatial_entry(RID p_base, const Volume &p_volume) {
	_remove_world_volume_spatial_entry(p_base);
	if (!world_volume_spatial_index_valid) {
		return;
	}

	WorldVolumeSpatialEntry entry;
	GridAlignedVolumeTransform grid_transform;
	if (!_get_aligned_volume_transform(p_volume, world_occupancy.voxel_size, world_occupancy.origin, grid_transform) ||
			grid_transform.world_dimensions.x <= 0 || grid_transform.world_dimensions.y <= 0 || grid_transform.world_dimensions.z <= 0) {
		world_volume_unindexed.insert(p_base);
		world_volume_spatial_entries.insert(p_base, entry);
		return;
	}

	const Vector3i last_world_voxel = grid_transform.world_min_voxel + grid_transform.world_dimensions - Vector3i(1, 1, 1);
	const Vector3i first_world_brick(
			_floor_divide_by_brick_size(grid_transform.world_min_voxel.x),
			_floor_divide_by_brick_size(grid_transform.world_min_voxel.y),
			_floor_divide_by_brick_size(grid_transform.world_min_voxel.z));
	const Vector3i last_world_brick(
			_floor_divide_by_brick_size(last_world_voxel.x),
			_floor_divide_by_brick_size(last_world_voxel.y),
			_floor_divide_by_brick_size(last_world_voxel.z));
	entry.cell_begin = Vector3i(
			_floor_divide(first_world_brick.x, WORLD_VOLUME_SPATIAL_CELL_BRICKS),
			_floor_divide(first_world_brick.y, WORLD_VOLUME_SPATIAL_CELL_BRICKS),
			_floor_divide(first_world_brick.z, WORLD_VOLUME_SPATIAL_CELL_BRICKS));
	entry.cell_end = Vector3i(
			_floor_divide(last_world_brick.x, WORLD_VOLUME_SPATIAL_CELL_BRICKS),
			_floor_divide(last_world_brick.y, WORLD_VOLUME_SPATIAL_CELL_BRICKS),
			_floor_divide(last_world_brick.z, WORLD_VOLUME_SPATIAL_CELL_BRICKS));
	const uint64_t cell_count = uint64_t(entry.cell_end.x - entry.cell_begin.x + 1) *
			uint64_t(entry.cell_end.y - entry.cell_begin.y + 1) *
			uint64_t(entry.cell_end.z - entry.cell_begin.z + 1);
	if (cell_count > 4096) {
		world_volume_unindexed.insert(p_base);
		world_volume_spatial_entries.insert(p_base, entry);
		return;
	}

	entry.indexed = true;
	for (int z = entry.cell_begin.z; z <= entry.cell_end.z; z++) {
		for (int y = entry.cell_begin.y; y <= entry.cell_end.y; y++) {
			for (int x = entry.cell_begin.x; x <= entry.cell_end.x; x++) {
				world_volume_spatial_cells[Vector3i(x, y, z)].push_back(p_base);
				world_volume_spatial_reference_count++;
			}
		}
	}
	world_volume_spatial_entries.insert(p_base, entry);
}

void VoxelForwardVolumeStorage::_rebuild_world_volume_spatial_index() {
	_clear_world_volume_spatial_index();
	if (!world_occupancy.directory_buffer.is_valid() || !world_occupancy.brick_buffer.is_valid() || world_occupancy.voxel_size <= 0.0f) {
		return;
	}
	world_volume_spatial_index_valid = true;
	for (const KeyValue<RID, Volume> &entry : volumes) {
		_insert_world_volume_spatial_entry(entry.key, entry.value);
	}
	world_occupancy.spatial_index_payload_bytes = uint64_t(world_volume_spatial_reference_count) * sizeof(RID) +
			uint64_t(world_volume_spatial_cells.size()) * (sizeof(Vector3i) + sizeof(Vector<RID>)) +
			uint64_t(world_volume_spatial_entries.size()) * (sizeof(RID) + sizeof(WorldVolumeSpatialEntry)) +
			uint64_t(world_volume_unindexed.size()) * sizeof(RID);
	print_verbose(vformat("Voxel Forward: rebuilt world volume spatial index with %d cell(s), %d reference(s), %d fallback volume(s), and %.2f KiB payload.", world_volume_spatial_cells.size(), world_volume_spatial_reference_count, world_volume_unindexed.size(), double(world_occupancy.spatial_index_payload_bytes) / 1024.0));
}

void VoxelForwardVolumeStorage::_free_world_gpu_resources() {
	_clear_ddgi_placement_batch();
	_clear_world_volume_spatial_index();
	RD *rd = RD::get_singleton();
	if (rd != nullptr) {
		if (world_occupancy.directory_buffer.is_valid()) {
			rd->free_rid(world_occupancy.directory_buffer);
		}
		if (world_occupancy.brick_buffer.is_valid()) {
			rd->free_rid(world_occupancy.brick_buffer);
		}
	}
	occupancy_gpu_bytes = occupancy_gpu_bytes >= world_occupancy_gpu_bytes ? occupancy_gpu_bytes - world_occupancy_gpu_bytes : 0;
	world_occupancy_gpu_bytes = 0;
	world_occupancy_directory_capacity_bytes = 0;
	world_occupancy_brick_capacity_bytes = 0;
	world_occupancy_directory_cpu.clear();
	world_occupancy_bricks_cpu.clear();
	world_occupied_bricks_cpu.clear();
	world_occupancy.directory_buffer = RID();
	world_occupancy.brick_buffer = RID();
	world_occupancy.origin = Vector3();
	world_occupancy.voxel_size = 0.0f;
	world_occupancy.directory_mask = 0;
	world_occupancy.occupied_brick_count = 0;
	world_occupancy.mixed_brick_count = 0;
	world_occupancy.max_probe_count = 0;
	world_occupancy.tombstone_count = 0;
	world_occupancy.incompatible_volume_count = 0;
	world_occupancy.last_dirty_bricks.clear();
	world_occupancy.last_incremental_revision = 0;
	for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
		ddgi_placement_pages[lod].clear();
		ddgi_page_seeds[lod].clear();
		world_occupancy.ddgi_last_published_pages[lod].clear();
		world_occupancy.ddgi_last_published_invalidation_masks[lod].clear();
		pending_ddgi_page_set[lod].clear();
		pending_ddgi_pages[lod].clear();
		pending_ddgi_page_invalidation_masks[lod].clear();
		world_occupancy.ddgi_cached_probe_count[lod] = 0;
		world_occupancy.ddgi_cached_page_count[lod] = 0;
	}
	world_occupancy.ddgi_probe_cache_revision = 0;
	world_occupancy.ddgi_pending_page_count = 0;
	world_occupancy.ddgi_last_dirty_probe_cell_count = 0;
	pending_incremental_world_bricks.clear();
	world_occupancy_free_mixed_slots.clear();
}

bool VoxelForwardVolumeStorage::create_world_occupancy_snapshot(WorldOccupancy &r_snapshot) const {
	r_snapshot = WorldOccupancy();
	RD *rd = RD::get_singleton();
	if (rd == nullptr || !world_occupancy.directory_buffer.is_valid() || !world_occupancy.brick_buffer.is_valid() ||
			world_occupancy_directory_cpu.is_empty()) {
		return false;
	}

	// Copy the already-current GPU buffers instead of duplicating and uploading
	// the complete CPU mirrors for every small edit. RenderingDevice preserves
	// queue order, so these copies observe the incremental uploads submitted for
	// this revision while later edits continue to mutate only the live buffers.
	// This keeps the immutable staging contract without a frame-sized CPU upload.
	const uint32_t directory_bytes = uint32_t(world_occupancy_directory_cpu.size());
	const uint32_t brick_bytes = uint32_t(MAX(4, world_occupancy_bricks_cpu.size()));
	const RID snapshot_directory = rd->storage_buffer_create(directory_bytes);
	const RID snapshot_bricks = rd->storage_buffer_create(brick_bytes);
	if (!snapshot_directory.is_valid() || !snapshot_bricks.is_valid()) {
		if (snapshot_directory.is_valid()) {
			rd->free_rid(snapshot_directory);
		}
		if (snapshot_bricks.is_valid()) {
			rd->free_rid(snapshot_bricks);
		}
		return false;
	}
	const Error directory_copy_error = rd->buffer_copy(world_occupancy.directory_buffer, snapshot_directory, 0, 0, directory_bytes);
	const Error brick_copy_error = rd->buffer_copy(world_occupancy.brick_buffer, snapshot_bricks, 0, 0, brick_bytes);
	if (directory_copy_error != OK || brick_copy_error != OK) {
		rd->free_rid(snapshot_directory);
		rd->free_rid(snapshot_bricks);
		return false;
	}

	r_snapshot = world_occupancy;
	r_snapshot.directory_buffer = snapshot_directory;
	r_snapshot.brick_buffer = snapshot_bricks;
	rd->set_resource_name(snapshot_directory, vformat("Voxel World Occupancy Snapshot Directory r%d", world_occupancy.revision));
	rd->set_resource_name(snapshot_bricks, vformat("Voxel World Occupancy Snapshot Bricks r%d", world_occupancy.revision));
	return true;
}

void VoxelForwardVolumeStorage::_select_world_grid() {
	// Select the lattice supported by the most occupied physical geometry, not
	// registration order. Small actor parts may arrive before streamed map chunks.
	struct Candidate {
		const Volume *representative = nullptr;
		float voxel_size = 0.0f;
		uint64_t occupied_bricks = 0;
	};
	Vector<Candidate> candidates;
	for (const KeyValue<RID, Volume> &entry : volumes) {
		const Volume &volume = entry.value;
		GridAlignedVolumeTransform transform;
		float voxel_size = 0.0f;
		if (volume.occupied_brick_count == 0 || !_get_grid_orientation(volume, voxel_size, transform)) {
			continue;
		}
		int candidate_index = -1;
		for (int index = 0; index < candidates.size(); index++) {
			const Candidate &candidate = candidates[index];
			if (_get_aligned_volume_transform(volume, candidate.voxel_size, candidate.representative->transform.origin, transform)) {
				candidate_index = index;
				break;
			}
		}
		if (candidate_index == -1) {
			candidate_index = candidates.size();
			Candidate candidate;
			candidate.representative = &volume;
			candidate.voxel_size = voxel_size;
			candidates.push_back(candidate);
		}
		Candidate &candidate = candidates.write[candidate_index];
		candidate.occupied_bricks += volume.occupied_brick_count;
		if (volume.transform.origin < candidate.representative->transform.origin) {
			candidate.representative = &volume;
		}
	}
	const Candidate *best = nullptr;
	double best_weight = -1.0;
	for (const Candidate &candidate : candidates) {
		const double size = candidate.voxel_size;
		const double weight = double(candidate.occupied_bricks) * size * size * size;
		// Deterministic geometric tie-break, independent of HashMap/RID order.
		const bool wins_tie = best != nullptr && Math::is_equal_approx(weight, best_weight) &&
				(candidate.voxel_size < best->voxel_size || (candidate.voxel_size == best->voxel_size && candidate.representative->transform.origin < best->representative->transform.origin));
		if (best == nullptr || (weight > best_weight && !Math::is_equal_approx(weight, best_weight)) || wins_tie) {
			best = &candidate;
			best_weight = weight;
		}
	}
	world_grid_selection_dirty = false;
	if (best == nullptr) {
		return; // Keep the last lattice through temporary streaming gaps.
	}
	GridAlignedVolumeTransform transform;
	if (selected_world_voxel_size > 0.0f && _get_aligned_volume_transform(*best->representative, selected_world_voxel_size, selected_world_origin, transform)) {
		return; // Keep probe coordinates stable when more of the same map arrives.
	}
	selected_world_origin = best->representative->transform.origin;
	selected_world_voxel_size = best->voxel_size;
	_request_full_world_rebuild("dominant voxel world grid changed");
	print_verbose(vformat("Voxel Forward: selected world grid %.6f m at %s from %d compatible occupied bricks.", selected_world_voxel_size, selected_world_origin, best->occupied_bricks));
}

void VoxelForwardVolumeStorage::_request_full_world_rebuild(const char *p_reason) {
	if (!world_occupancy_dirty) {
		world_occupancy_wait_frames = 0;
		print_verbose(vformat("Voxel Forward: requested full world occupancy rebuild (%s).", p_reason));
	}
	world_occupancy.last_full_rebuild_reason = p_reason;
	pending_incremental_world_bricks.clear();
	world_occupancy_dirty = true;
	world_occupancy_quiet_frames = 0;
}

bool VoxelForwardVolumeStorage::_queue_incremental_volume_extent(const Volume &p_volume) {
	if (world_occupancy_dirty || world_occupancy_build_task != WorkerThreadPool::INVALID_TASK_ID ||
			!world_occupancy.directory_buffer.is_valid() || !world_occupancy.brick_buffer.is_valid()) {
		return false;
	}

	GridAlignedVolumeTransform grid_transform;
	if (!_get_aligned_volume_transform(p_volume, world_occupancy.voxel_size, world_occupancy.origin, grid_transform)) {
		return false;
	}

	for (int z = 0; z < p_volume.brick_dimensions.z; z++) {
		for (int y = 0; y < p_volume.brick_dimensions.y; y++) {
			for (int x = 0; x < p_volume.brick_dimensions.x; x++) {
				const Vector3i local_brick(x, y, z);
				const int brick_index = x + y * p_volume.brick_dimensions.x + z * p_volume.brick_dimensions.x * p_volume.brick_dimensions.y;
				WorldBrickBits bits;
				if (!_read_volume_brick_bits(p_volume, brick_index, bits)) {
					return false;
				}
				if (_world_brick_bits_empty(bits)) {
					continue;
				}

				const Vector3i changed_local_begin = local_brick * OCCUPANCY_BRICK_SIZE;
				const Vector3i changed_local_end(
						MIN((x + 1) * OCCUPANCY_BRICK_SIZE, p_volume.dimensions.x),
						MIN((y + 1) * OCCUPANCY_BRICK_SIZE, p_volume.dimensions.y),
						MIN((z + 1) * OCCUPANCY_BRICK_SIZE, p_volume.dimensions.z));
				Vector3i changed_world_begin;
				Vector3i changed_world_end;
				_local_bounds_to_world(p_volume, grid_transform, changed_local_begin, changed_local_end, changed_world_begin, changed_world_end);
				const Vector3i first_world_brick(
						_floor_divide_by_brick_size(changed_world_begin.x),
						_floor_divide_by_brick_size(changed_world_begin.y),
						_floor_divide_by_brick_size(changed_world_begin.z));
				const Vector3i last_world_voxel = changed_world_end - Vector3i(1, 1, 1);
				const Vector3i last_world_brick(
						_floor_divide_by_brick_size(last_world_voxel.x),
						_floor_divide_by_brick_size(last_world_voxel.y),
						_floor_divide_by_brick_size(last_world_voxel.z));
				for (int world_z = first_world_brick.z; world_z <= last_world_brick.z; world_z++) {
					for (int world_y = first_world_brick.y; world_y <= last_world_brick.y; world_y++) {
						for (int world_x = first_world_brick.x; world_x <= last_world_brick.x; world_x++) {
							pending_incremental_world_bricks.insert(Vector3i(world_x, world_y, world_z));
						}
					}
				}
			}
		}
	}
	return true;
}

bool VoxelForwardVolumeStorage::_queue_incremental_volume_change(const Volume &p_previous, const Volume &p_current, const Vector3i &p_dirty_position, const Vector3i &p_dirty_size) {
	if (world_occupancy_dirty || world_occupancy_build_task != WorkerThreadPool::INVALID_TASK_ID ||
			!world_occupancy.directory_buffer.is_valid() || !world_occupancy.brick_buffer.is_valid() ||
			p_previous.transform != p_current.transform || p_previous.dimensions != p_current.dimensions ||
			p_previous.brick_dimensions != p_current.brick_dimensions || !Math::is_equal_approx(p_previous.voxel_size, p_current.voxel_size) ||
			p_previous.occupancy_directory_cpu.size() != p_current.occupancy_directory_cpu.size()) {
		return false;
	}

	GridAlignedVolumeTransform grid_transform;
	if (!_get_aligned_volume_transform(p_current, world_occupancy.voxel_size, world_occupancy.origin, grid_transform)) {
		return false;
	}
	if (p_previous.revision == p_current.revision) return true;

	Vector3i first_local_brick;
	Vector3i last_local_brick = p_current.brick_dimensions - Vector3i(1, 1, 1);
	if (p_dirty_size.x > 0 && p_dirty_size.y > 0 && p_dirty_size.z > 0) {
		const Vector3i dirty_begin(
				CLAMP(p_dirty_position.x, 0, p_current.dimensions.x - 1),
				CLAMP(p_dirty_position.y, 0, p_current.dimensions.y - 1),
				CLAMP(p_dirty_position.z, 0, p_current.dimensions.z - 1));
		const Vector3i dirty_end(
				CLAMP(p_dirty_position.x + p_dirty_size.x, 1, p_current.dimensions.x),
				CLAMP(p_dirty_position.y + p_dirty_size.y, 1, p_current.dimensions.y),
				CLAMP(p_dirty_position.z + p_dirty_size.z, 1, p_current.dimensions.z));
		first_local_brick = dirty_begin / OCCUPANCY_BRICK_SIZE;
		last_local_brick = (dirty_end - Vector3i(1, 1, 1)) / OCCUPANCY_BRICK_SIZE;
	}
	for (int z = first_local_brick.z; z <= last_local_brick.z; z++) {
		for (int y = first_local_brick.y; y <= last_local_brick.y; y++) {
			for (int x = first_local_brick.x; x <= last_local_brick.x; x++) {
				const Vector3i local_brick(x, y, z);
				const int brick_index = x + y * p_current.brick_dimensions.x + z * p_current.brick_dimensions.x * p_current.brick_dimensions.y;
				WorldBrickBits previous_bits;
				WorldBrickBits current_bits;
				if (!_read_volume_brick_bits(p_previous, brick_index, previous_bits) || !_read_volume_brick_bits(p_current, brick_index, current_bits)) {
					return false;
				}
				if (_world_brick_bits_equal(previous_bits, current_bits)) {
					continue;
				}
				const Vector3i changed_local_begin = local_brick * OCCUPANCY_BRICK_SIZE;
				const Vector3i changed_local_end(
						MIN((local_brick.x + 1) * OCCUPANCY_BRICK_SIZE, p_current.dimensions.x),
						MIN((local_brick.y + 1) * OCCUPANCY_BRICK_SIZE, p_current.dimensions.y),
						MIN((local_brick.z + 1) * OCCUPANCY_BRICK_SIZE, p_current.dimensions.z));
				Vector3i changed_world_begin;
				Vector3i changed_world_end;
				_local_bounds_to_world(p_current, grid_transform, changed_local_begin, changed_local_end, changed_world_begin, changed_world_end);
				const Vector3i first_world_brick(
						_floor_divide_by_brick_size(changed_world_begin.x),
						_floor_divide_by_brick_size(changed_world_begin.y),
						_floor_divide_by_brick_size(changed_world_begin.z));
				const Vector3i last_world_voxel = changed_world_end - Vector3i(1, 1, 1);
				const Vector3i last_world_brick(
						_floor_divide_by_brick_size(last_world_voxel.x),
						_floor_divide_by_brick_size(last_world_voxel.y),
						_floor_divide_by_brick_size(last_world_voxel.z));
				for (int world_z = first_world_brick.z; world_z <= last_world_brick.z; world_z++) {
					for (int world_y = first_world_brick.y; world_y <= last_world_brick.y; world_y++) {
						for (int world_x = first_world_brick.x; world_x <= last_world_brick.x; world_x++) {
							pending_incremental_world_bricks.insert(Vector3i(world_x, world_y, world_z));
						}
					}
				}
			}
		}
	}
	return true;
}

bool VoxelForwardVolumeStorage::_apply_incremental_world_updates() {
	GodotProfileZone("Voxel Occupancy Incremental Update");
	if (pending_incremental_world_bricks.is_empty()) {
		return true;
	}
	const uint64_t update_started_usec = OS::get_singleton()->get_ticks_usec();
	RD *rd = RD::get_singleton();
	if (rd == nullptr || !world_occupancy.directory_buffer.is_valid() || !world_occupancy.brick_buffer.is_valid() ||
			world_occupancy_directory_cpu.size() != int(world_occupancy.directory_mask + 1) * int(sizeof(WorldDirectoryEntry))) {
		_request_full_world_rebuild("incremental buffers unavailable or incompatible");
		return false;
	}

	Vector<Vector3i> dirty_bricks;
	const int brick_budget = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/world_occupancy/incremental_bricks_per_frame")), 64, 8192);
	for (const Vector3i &position : pending_incremental_world_bricks) {
		dirty_bricks.push_back(position);
		if (dirty_bricks.size() >= brick_budget) {
			break;
		}
	}
	dirty_bricks.sort_custom<Vector3iLexicographicLess>();
	// Every processed brick is reconstructed from the latest volume data. Edits
	// arriving while a backlog is draining remain deduplicated in the set.
	for (const Vector3i &position : dirty_bricks) {
		pending_incremental_world_bricks.erase(position);
	}

	WorldDirectoryEntry *directory = reinterpret_cast<WorldDirectoryEntry *>(world_occupancy_directory_cpu.ptrw());
	const uint32_t directory_capacity = world_occupancy.directory_mask + 1;
	uint32_t uploaded_bytes = 0;
	uint32_t candidate_volume_checks = 0;
	uint32_t intersecting_volume_count = 0;
	uint32_t buffer_update_call_count = 0;
	uint32_t added_voxel_count = 0;
	uint32_t removed_voxel_count = 0;
	Vector<uint32_t> changed_directory_slots;
	Vector<uint32_t> changed_mixed_slots;

	for (const Vector3i &world_brick_position : dirty_bricks) {
		// Read the composed world brick before mutating its directory entry. Bit
		// transitions are the authoritative topology signal: adding occupancy
		// decreases face transmittance; removing it increases transmittance.
		WorldBrickBits previous_composed_bits;
		const bool previous_composed_valid = _ddgi_read_world_brick(world_occupancy_directory_cpu, world_occupancy_bricks_cpu,
				world_occupancy.directory_mask, world_occupancy.max_probe_count, world_brick_position, previous_composed_bits);
		WorldBrickBits composed_bits;
		const Vector3i world_voxel_begin = world_brick_position * OCCUPANCY_BRICK_SIZE;
		const Vector3i world_voxel_end = world_voxel_begin + Vector3i(OCCUPANCY_BRICK_SIZE, OCCUPANCY_BRICK_SIZE, OCCUPANCY_BRICK_SIZE);
		auto compose_volume = [&](const Volume &p_volume) {
			candidate_volume_checks++;
			GridAlignedVolumeTransform grid_transform;
			if (!_get_aligned_volume_transform(p_volume, world_occupancy.voxel_size, world_occupancy.origin, grid_transform)) {
				return;
			}
			const bool volume_brick_aligned = grid_transform.identity_orientation &&
					grid_transform.world_min_voxel.x % OCCUPANCY_BRICK_SIZE == 0 &&
					grid_transform.world_min_voxel.y % OCCUPANCY_BRICK_SIZE == 0 &&
					grid_transform.world_min_voxel.z % OCCUPANCY_BRICK_SIZE == 0;
			if (volume_brick_aligned) {
				const Vector3i local_brick = world_brick_position - grid_transform.world_min_voxel / OCCUPANCY_BRICK_SIZE;
				if (local_brick.x < 0 || local_brick.y < 0 || local_brick.z < 0 ||
						local_brick.x >= p_volume.brick_dimensions.x ||
						local_brick.y >= p_volume.brick_dimensions.y ||
						local_brick.z >= p_volume.brick_dimensions.z) {
					return;
				}
				intersecting_volume_count++;
				const int local_brick_index = local_brick.x +
						local_brick.y * p_volume.brick_dimensions.x +
						local_brick.z * p_volume.brick_dimensions.x * p_volume.brick_dimensions.y;
				WorldBrickBits source_bits;
				if (_read_volume_brick_bits(p_volume, local_brick_index, source_bits)) {
					for (int word = 0; word < 8; word++) {
						composed_bits.bits[word] |= source_bits.bits[word];
					}
				}
				return;
			}
			const Vector3i volume_voxel_end = grid_transform.world_min_voxel + grid_transform.world_dimensions;
			const Vector3i overlap_begin(
					MAX(world_voxel_begin.x, grid_transform.world_min_voxel.x),
					MAX(world_voxel_begin.y, grid_transform.world_min_voxel.y),
					MAX(world_voxel_begin.z, grid_transform.world_min_voxel.z));
			const Vector3i overlap_end(
					MIN(world_voxel_end.x, volume_voxel_end.x),
					MIN(world_voxel_end.y, volume_voxel_end.y),
					MIN(world_voxel_end.z, volume_voxel_end.z));
			if (overlap_begin.x >= overlap_end.x || overlap_begin.y >= overlap_end.y || overlap_begin.z >= overlap_end.z) {
				return;
			}
			intersecting_volume_count++;
			for (int z = overlap_begin.z; z < overlap_end.z; z++) {
				for (int y = overlap_begin.y; y < overlap_end.y; y++) {
					for (int x = overlap_begin.x; x < overlap_end.x; x++) {
						const Vector3i world_voxel(x, y, z);
						if (!_volume_voxel_occupied(p_volume, _world_voxel_to_local(p_volume, grid_transform, world_voxel))) {
							continue;
						}
						const Vector3i world_local = world_voxel - world_voxel_begin;
						const int world_local_index = world_local.x + world_local.y * OCCUPANCY_BRICK_SIZE + world_local.z * OCCUPANCY_BRICK_SIZE * OCCUPANCY_BRICK_SIZE;
						composed_bits.bits[world_local_index >> 6] |= uint64_t(1) << (world_local_index & 63);
					}
				}
			}
		};

		{
			GodotProfileZone("Voxel Occupancy Dirty-Brick Composition");
			if (world_volume_spatial_index_valid) {
				const Vector3i spatial_cell(
						_floor_divide(world_brick_position.x, WORLD_VOLUME_SPATIAL_CELL_BRICKS),
						_floor_divide(world_brick_position.y, WORLD_VOLUME_SPATIAL_CELL_BRICKS),
						_floor_divide(world_brick_position.z, WORLD_VOLUME_SPATIAL_CELL_BRICKS));
				const Vector<RID> *candidate_volumes = nullptr;
				{
					GodotProfileZone("Voxel Occupancy Candidate Lookup");
					candidate_volumes = world_volume_spatial_cells.getptr(spatial_cell);
				}
				if (candidate_volumes != nullptr) {
					for (const RID &candidate : *candidate_volumes) {
						const Volume *volume = volumes.getptr(candidate);
						if (volume != nullptr) {
							compose_volume(*volume);
						}
					}
				}
				for (const RID &candidate : world_volume_unindexed) {
					const Volume *volume = volumes.getptr(candidate);
					if (volume != nullptr) {
						compose_volume(*volume);
					}
				}
			} else {
				for (const KeyValue<RID, Volume> &entry : volumes) {
					compose_volume(entry.value);
				}
			}
		}
		if (previous_composed_valid) {
			for (int word = 0; word < 8; word++) {
				uint64_t added = composed_bits.bits[word] & ~previous_composed_bits.bits[word];
				uint64_t removed = previous_composed_bits.bits[word] & ~composed_bits.bits[word];
				while (added != 0) {
					added &= added - 1;
					added_voxel_count++;
				}
				while (removed != 0) {
					removed &= removed - 1;
					removed_voxel_count++;
				}
			}
		}

		GodotProfileZone("Voxel Occupancy Directory Update");
		uint32_t slot = _world_brick_hash(world_brick_position) & world_occupancy.directory_mask;
		uint32_t insertion_slot = UINT32_MAX;
		uint32_t probe_count = 1;
		bool found = false;
		for (; probe_count <= directory_capacity; probe_count++) {
			WorldDirectoryEntry &candidate = directory[slot];
			if (candidate.code == 0) {
				if (insertion_slot == UINT32_MAX) {
					insertion_slot = slot;
				}
				break;
			}
			if (candidate.code == WORLD_DIRECTORY_TOMBSTONE_CODE) {
				if (insertion_slot == UINT32_MAX) {
					insertion_slot = slot;
				}
			} else if (candidate.x == world_brick_position.x && candidate.y == world_brick_position.y && candidate.z == world_brick_position.z) {
				found = true;
				insertion_slot = slot;
				break;
			}
			slot = (slot + 1) & world_occupancy.directory_mask;
		}
		if (insertion_slot == UINT32_MAX) {
			_request_full_world_rebuild("directory insertion capacity exhausted");
			return false;
		}
		if (probe_count > 48) {
			_request_full_world_rebuild("directory probe threshold exceeded");
			return false;
		}
		slot = insertion_slot;
		WorldDirectoryEntry &directory_entry = directory[slot];
		const bool reused_tombstone = !found && directory_entry.code == WORLD_DIRECTORY_TOMBSTONE_CODE;
		const uint32_t previous_code = found ? directory_entry.code : 0;
		const bool empty = _world_brick_bits_empty(composed_bits);
		const bool uniform = !empty && _world_brick_bits_uniform(composed_bits);
		if (empty) {
			world_occupied_bricks_cpu.erase(world_brick_position);
		} else {
			world_occupied_bricks_cpu.insert(world_brick_position);
		}

		auto release_mixed_slot = [this](uint32_t p_code) {
			if (p_code >= 2 && p_code != WORLD_DIRECTORY_TOMBSTONE_CODE) {
				world_occupancy_free_mixed_slots.push_back(p_code - 2);
				world_occupancy.mixed_brick_count--;
			}
		};
		auto allocate_mixed_slot = [this]() -> uint32_t {
			uint32_t mixed_slot;
			if (!world_occupancy_free_mixed_slots.is_empty()) {
				mixed_slot = world_occupancy_free_mixed_slots[world_occupancy_free_mixed_slots.size() - 1];
				world_occupancy_free_mixed_slots.remove_at(world_occupancy_free_mixed_slots.size() - 1);
			} else {
				mixed_slot = world_occupancy_bricks_cpu.size() / OCCUPANCY_BRICK_BYTES;
			}
			return mixed_slot;
		};

		uint32_t new_code = previous_code;
		if (empty) {
			if (!found) {
				continue;
			}
			release_mixed_slot(previous_code);
			directory_entry.x = WORLD_DIRECTORY_TOMBSTONE_COORDINATE;
			directory_entry.y = WORLD_DIRECTORY_TOMBSTONE_COORDINATE;
			directory_entry.z = WORLD_DIRECTORY_TOMBSTONE_COORDINATE;
			new_code = WORLD_DIRECTORY_TOMBSTONE_CODE;
			world_occupancy.occupied_brick_count--;
			world_occupancy.tombstone_count++;
		} else if (uniform) {
			if (!found) {
				world_occupancy.occupied_brick_count++;
				if (reused_tombstone) {
					world_occupancy.tombstone_count--;
				}
			} else {
				release_mixed_slot(previous_code);
			}
			directory_entry.x = world_brick_position.x;
			directory_entry.y = world_brick_position.y;
			directory_entry.z = world_brick_position.z;
			new_code = 1;
		} else {
			uint32_t mixed_slot;
			if (previous_code >= 2 && previous_code != WORLD_DIRECTORY_TOMBSTONE_CODE) {
				mixed_slot = previous_code - 2;
			} else {
				mixed_slot = allocate_mixed_slot();
				if ((mixed_slot + 1) * OCCUPANCY_BRICK_BYTES > world_occupancy_brick_capacity_bytes) {
					_request_full_world_rebuild("mixed-brick capacity exhausted");
					return false;
				}
				world_occupancy.mixed_brick_count++;
			}
			{
				GodotProfileZone("Voxel Occupancy Mixed-Brick CPU Update");
				_write_world_brick_bytes(world_occupancy_bricks_cpu, mixed_slot, composed_bits);
			}
			changed_mixed_slots.push_back(mixed_slot);
			if (!found) {
				world_occupancy.occupied_brick_count++;
				if (reused_tombstone) {
					world_occupancy.tombstone_count--;
				}
			}
			directory_entry.x = world_brick_position.x;
			directory_entry.y = world_brick_position.y;
			directory_entry.z = world_brick_position.z;
			new_code = mixed_slot + 2;
		}

		if (new_code != previous_code || !found) {
			directory_entry.code = new_code;
			changed_directory_slots.push_back(slot);
		}

		world_occupancy.max_probe_count = MAX(world_occupancy.max_probe_count, probe_count);
	}
	{
		GodotProfileZone("Voxel Occupancy Incremental GPU Upload");
		const Error brick_error = _upload_changed_buffer_slots(rd, world_occupancy.brick_buffer, world_occupancy_bricks_cpu, changed_mixed_slots, OCCUPANCY_BRICK_BYTES, uploaded_bytes, buffer_update_call_count);
		const Error directory_error = brick_error == OK ? _upload_changed_buffer_slots(rd, world_occupancy.directory_buffer, world_occupancy_directory_cpu, changed_directory_slots, sizeof(WorldDirectoryEntry), uploaded_bytes, buffer_update_call_count) : brick_error;
		if (brick_error != OK || directory_error != OK) {
			_request_full_world_rebuild("incremental GPU upload failed");
			return false;
		}
	}

	const uint64_t next_revision = world_occupancy.revision + 1u;
	_queue_ddgi_pages_for_dirty_bricks(dirty_bricks);
	world_occupancy.last_dirty_brick_count = dirty_bricks.size();
	world_occupancy.last_uploaded_bytes = uploaded_bytes;
	world_occupancy.last_candidate_volume_checks = candidate_volume_checks;
	world_occupancy.last_intersecting_volume_count = intersecting_volume_count;
	world_occupancy.last_buffer_update_call_count = buffer_update_call_count;
	world_occupancy.last_added_voxel_count = added_voxel_count;
	world_occupancy.last_removed_voxel_count = removed_voxel_count;
	world_occupancy.last_face_transmittance_decreased = added_voxel_count > 0;
	world_occupancy.last_face_transmittance_increased = removed_voxel_count > 0;
	world_occupancy.last_incremental_update_usec = OS::get_singleton()->get_ticks_usec() - update_started_usec;
	world_occupancy.last_dirty_bricks = dirty_bricks;
	world_occupancy.incremental_update_count++;
	world_occupancy.revision = next_revision;
	world_occupancy.last_incremental_revision = world_occupancy.revision;
	print_verbose(vformat("Voxel Forward: incremental world occupancy revision %d updated %d dirty brick(s) in %.2f ms, %d candidate check(s), %d intersection(s), %d upload call(s), %d uploaded byte(s).", world_occupancy.revision, dirty_bricks.size(), double(world_occupancy.last_incremental_update_usec) / 1000.0, candidate_volume_checks, intersecting_volume_count, buffer_update_call_count, uploaded_bytes));
	if (world_occupancy.tombstone_count > directory_capacity / 8 ||
			world_occupancy.occupied_brick_count + world_occupancy.tombstone_count > directory_capacity * 3 / 4) {
		_request_full_world_rebuild(world_occupancy.tombstone_count > directory_capacity / 8 ? "directory tombstone threshold exceeded" : "directory load threshold exceeded");
	}
	return true;
}

void VoxelForwardVolumeStorage::_queue_ddgi_pages_for_dirty_bricks(const Vector<Vector3i> &p_dirty_bricks) {
	static const Vector3i axial[7] = {
		Vector3i(), Vector3i(1, 0, 0), Vector3i(-1, 0, 0), Vector3i(0, 1, 0),
		Vector3i(0, -1, 0), Vector3i(0, 0, 1), Vector3i(0, 0, -1)
	};
	for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
		HashMap<Vector3i, uint64_t> affected_invalidation_masks;
		for (const Vector3i &brick : p_dirty_bricks) {
			HashSet<Vector3i> edited_cells;
			_ddgi_add_cells_for_brick(brick, world_occupancy.ddgi_cell_spacing_voxels[lod], edited_cells);
			for (const Vector3i &cell : edited_cells) {
				for (uint32_t neighbor = 0; neighbor < 7; neighbor++) {
					const Vector3i affected_cell = cell + axial[neighbor];
					const Vector3i page_coordinate = _ddgi_page_coordinate(affected_cell);
					const uint64_t bit = uint64_t(1) << _ddgi_page_local_index(affected_cell, page_coordinate);
					uint64_t *mask = affected_invalidation_masks.getptr(page_coordinate);
					if (mask == nullptr) {
						affected_invalidation_masks.insert(page_coordinate, bit);
					} else {
						*mask |= bit;
					}
				}
			}
		}
		for (const KeyValue<Vector3i, uint64_t> &affected_page : affected_invalidation_masks) {
			const Vector3i &page_coordinate = affected_page.key;
			DdgiPlacementPageSeed replacement;
			const DdgiPlacementPageSeed *previous_seed = ddgi_page_seeds[lod].getptr(page_coordinate);
			if (previous_seed != nullptr) {
				replacement = *previous_seed;
			}
			for (uint32_t index = 0; index < DDGI_PLACEMENT_PAGE_PROBE_COUNT; index++) {
				const uint64_t bit = uint64_t(1) << index;
				if ((affected_page.value & bit) == 0) {
					continue;
				}
				const Vector3i local(index % DDGI_PLACEMENT_PAGE_SIZE, (index / DDGI_PLACEMENT_PAGE_SIZE) % DDGI_PLACEMENT_PAGE_SIZE, index / (DDGI_PLACEMENT_PAGE_SIZE * DDGI_PLACEMENT_PAGE_SIZE));
				const Vector3i logical_cell = page_coordinate * DDGI_PLACEMENT_PAGE_SIZE + local;
				const bool occupied = _ddgi_cell_has_occupied_brick(world_occupied_bricks_cpu, world_occupancy.ddgi_cell_spacing_voxels[lod], logical_cell);
				bool relevant = occupied;
				for (uint32_t neighbor = 1; neighbor < 7 && !relevant; neighbor++) {
					relevant = _ddgi_cell_has_occupied_brick(world_occupied_bricks_cpu, world_occupancy.ddgi_cell_spacing_voxels[lod], logical_cell + axial[neighbor]);
				}
				replacement.relevant_mask &= ~bit;
				replacement.occupied_mask &= ~bit;
				if (!relevant) {
					continue;
				}
				replacement.relevant_mask |= bit;
				if (occupied) {
					replacement.occupied_mask |= bit;
				}
			}
			if (replacement.relevant_mask == 0) {
				ddgi_page_seeds[lod].erase(page_coordinate);
			} else {
				ddgi_page_seeds[lod].insert(page_coordinate, replacement);
			}
			if (!pending_ddgi_page_set[lod].has(page_coordinate)) {
				pending_ddgi_page_set[lod].insert(page_coordinate);
				pending_ddgi_pages[lod].push_back(page_coordinate);
			}
			uint64_t *pending_mask = pending_ddgi_page_invalidation_masks[lod].getptr(page_coordinate);
			if (pending_mask == nullptr) {
				pending_ddgi_page_invalidation_masks[lod].insert(page_coordinate, affected_page.value);
			} else {
				*pending_mask |= affected_page.value;
			}
		}
	}
	uint32_t pending_count = 0;
	for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
		pending_count += pending_ddgi_pages[lod].size();
	}
	if (ddgi_placement_batch != nullptr) {
		pending_count += ddgi_placement_batch->results.size() - ddgi_placement_batch->publish_cursor;
	}
	world_occupancy.ddgi_pending_page_count = pending_count;
	world_occupancy.ddgi_incremental_probe_bake_count++;
}

void VoxelForwardVolumeStorage::_publish_ddgi_page_seeds(const HashMap<Vector3i, DdgiPlacementPageSeed> (&p_seeds)[DDGI_PLACEMENT_LOD_COUNT], bool p_reset_placement) {
	uint32_t pending_count = 0;
	for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
		if (p_reset_placement) {
			// Identical page coordinates refer to different world positions after
			// a lattice or LOD spacing change; none of that placement is reusable.
			ddgi_placement_pages[lod].clear();
			world_occupancy.ddgi_last_published_pages[lod].clear();
			world_occupancy.ddgi_last_published_invalidation_masks[lod].clear();
		}
		ddgi_page_seeds[lod] = p_seeds[lod];
		Vector<Vector3i> stale_pages;
		for (const KeyValue<Vector3i, DdgiPlacementPage> &page : ddgi_placement_pages[lod]) {
			if (!ddgi_page_seeds[lod].has(page.key)) stale_pages.push_back(page.key);
		}
		for (const Vector3i &page_coordinate : stale_pages) ddgi_placement_pages[lod].erase(page_coordinate);
		world_occupancy.ddgi_cached_probe_count[lod] = 0;
		for (const KeyValue<Vector3i, DdgiPlacementPage> &page : ddgi_placement_pages[lod]) {
			world_occupancy.ddgi_cached_probe_count[lod] += _ddgi_mask_bit_count(page.value.relevant_mask);
		}
		pending_ddgi_pages[lod].clear();
		pending_ddgi_page_set[lod].clear();
		pending_ddgi_page_invalidation_masks[lod].clear();
		for (const KeyValue<Vector3i, DdgiPlacementPageSeed> &entry : ddgi_page_seeds[lod]) {
			pending_ddgi_page_set[lod].insert(entry.key);
			pending_ddgi_pages[lod].push_back(entry.key);
			pending_ddgi_page_invalidation_masks[lod].insert(entry.key, UINT64_MAX);
		}
		pending_ddgi_pages[lod].sort_custom<DdgiPagePriorityLess>();
		pending_count += pending_ddgi_pages[lod].size();
		world_occupancy.ddgi_cached_page_count[lod] = ddgi_placement_pages[lod].size();
	}
	if (ddgi_load_priority_valid) {
		prioritize_queued_ddgi_pages_for_world_load(ddgi_load_priority_world_position, ddgi_load_priority_resolution);
	}
	world_occupancy.ddgi_pending_page_count = pending_count;
}

void VoxelForwardVolumeStorage::_build_ddgi_placement_page(void *p_userdata, uint32_t p_index) {
	DdgiPlacementBatch *batch = static_cast<DdgiPlacementBatch *>(p_userdata);
	ERR_FAIL_NULL(batch);
	ERR_FAIL_INDEX(p_index, uint32_t(batch->jobs.size()));
	const DdgiPlacementJob &job = batch->jobs[p_index];
	DdgiPlacementPage &replacement = batch->results.write[p_index];
	replacement.coordinate = job.page_coordinate;
	replacement.revision = batch->occupancy_revision;
	replacement.relevant_mask = job.seed.relevant_mask;
	for (uint32_t index = 0; index < DDGI_PLACEMENT_PAGE_PROBE_COUNT; index++) {
		if ((job.seed.relevant_mask & (uint64_t(1) << index)) == 0) {
			continue;
		}
		const Vector3i local(index % DDGI_PLACEMENT_PAGE_SIZE, (index / DDGI_PLACEMENT_PAGE_SIZE) % DDGI_PLACEMENT_PAGE_SIZE, index / (DDGI_PLACEMENT_PAGE_SIZE * DDGI_PLACEMENT_PAGE_SIZE));
		const Vector3i logical_cell = job.page_coordinate * DDGI_PLACEMENT_PAGE_SIZE + local;
		const bool occupied = (job.seed.occupied_mask & (uint64_t(1) << index)) != 0;
		replacement.records[index] = _ddgi_relocate_probe_bounded(batch->directory_bytes, batch->brick_bytes, batch->directory_mask, batch->max_probe_count, batch->world_origin, batch->voxel_size, job.lod, batch->ddgi_cell_spacing_voxels[job.lod], logical_cell, occupied, batch->occupancy_revision);
	}
}

void VoxelForwardVolumeStorage::_clear_ddgi_placement_batch() {
	if (ddgi_placement_group_task != -1) {
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(ddgi_placement_group_task);
		ddgi_placement_group_task = -1;
	}
	if (ddgi_placement_batch != nullptr) {
		memdelete(ddgi_placement_batch);
		ddgi_placement_batch = nullptr;
	}
}

void VoxelForwardVolumeStorage::_process_ddgi_placement_page_queue(uint32_t p_publish_budget) {
	if (world_occupancy_directory_cpu.is_empty() || world_occupancy.voxel_size <= 0.0f || p_publish_budget == 0) {
		return;
	}
	if (ddgi_placement_batch == nullptr) {
		uint32_t pending_count = 0;
		for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
			pending_count += pending_ddgi_pages[lod].size();
		}
		if (pending_count == 0) {
			return;
		}
		ddgi_placement_batch = memnew(DdgiPlacementBatch);
		DdgiPlacementBatch &batch = *ddgi_placement_batch;
		batch.directory_bytes = world_occupancy_directory_cpu;
		batch.brick_bytes = world_occupancy_bricks_cpu;
		batch.directory_mask = world_occupancy.directory_mask;
		batch.max_probe_count = world_occupancy.max_probe_count;
		batch.world_origin = world_occupancy.origin;
		batch.voxel_size = world_occupancy.voxel_size;
		batch.occupancy_revision = world_occupancy.revision;
		for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
			batch.ddgi_cell_spacing_voxels[lod] = world_occupancy.ddgi_cell_spacing_voxels[lod];
		}
		batch.started_usec = OS::get_singleton()->get_ticks_usec();
		static constexpr uint32_t MAX_PARALLEL_PAGE_BATCH = 256;
		batch.jobs.reserve(MIN(pending_count, MAX_PARALLEL_PAGE_BATCH));
		// Coarse pages enter the same parallel batch, but are listed first so a
		// completed batch publishes valid parent fallback before fine detail.
		static constexpr uint32_t PAGES_PER_LOD_PER_BATCH = MAX_PARALLEL_PAGE_BATCH / DDGI_PLACEMENT_LOD_COUNT;
		for (int lod = DDGI_PLACEMENT_LOD_COUNT - 1; lod >= 0 && uint32_t(batch.jobs.size()) < MAX_PARALLEL_PAGE_BATCH; lod--) {
			uint32_t lod_page_count = 0;
			while (!pending_ddgi_pages[lod].is_empty() && uint32_t(batch.jobs.size()) < MAX_PARALLEL_PAGE_BATCH && lod_page_count < PAGES_PER_LOD_PER_BATCH) {
				const Vector3i page_coordinate = pending_ddgi_pages[lod][pending_ddgi_pages[lod].size() - 1];
				pending_ddgi_pages[lod].resize(pending_ddgi_pages[lod].size() - 1);
				pending_ddgi_page_set[lod].erase(page_coordinate);
				DdgiPlacementJob job;
				job.lod = lod;
				job.page_coordinate = page_coordinate;
				const uint64_t *invalidation_mask = pending_ddgi_page_invalidation_masks[lod].getptr(page_coordinate);
				job.invalidation_mask = invalidation_mask != nullptr ? *invalidation_mask : UINT64_MAX;
				pending_ddgi_page_invalidation_masks[lod].erase(page_coordinate);
				const DdgiPlacementPageSeed *seed = ddgi_page_seeds[lod].getptr(page_coordinate);
				if (seed != nullptr) {
					job.seed = *seed;
				}
				batch.jobs.push_back(job);
				lod_page_count++;
			}
		}
		batch.results.resize(batch.jobs.size());
		world_occupancy.ddgi_pending_page_count = pending_count;
		// A bounded low-priority worker set keeps placement throughput high without
		// taking every core from rendering/streaming. A batch is also small enough that
		// orderly shutdown never waits on a world-sized relocation task.
		ddgi_placement_group_task = WorkerThreadPool::get_singleton()->add_native_group_task(_build_ddgi_placement_page, ddgi_placement_batch, batch.jobs.size(), 2, false, SNAME("VoxelDDGIPlacementPages"));
		return;
	}
	if (ddgi_placement_group_task != -1) {
		if (!WorkerThreadPool::get_singleton()->is_group_task_completed(ddgi_placement_group_task)) {
			return;
		}
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(ddgi_placement_group_task);
		ddgi_placement_group_task = -1;
		world_occupancy.ddgi_last_probe_bake_usec = OS::get_singleton()->get_ticks_usec() - ddgi_placement_batch->started_usec;
	}
	DdgiPlacementBatch &batch = *ddgi_placement_batch;
	const uint32_t publish_begin = batch.publish_cursor;
	const uint32_t publish_end = MIN(uint32_t(batch.results.size()), batch.publish_cursor + p_publish_budget);
	for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
		world_occupancy.ddgi_last_published_pages[lod].clear();
		world_occupancy.ddgi_last_published_invalidation_masks[lod].clear();
	}
	uint32_t probes_published = 0;
	for (; batch.publish_cursor < publish_end; batch.publish_cursor++) {
		const DdgiPlacementJob &job = batch.jobs[batch.publish_cursor];
		const DdgiPlacementPage &replacement = batch.results[batch.publish_cursor];
		world_occupancy.ddgi_last_published_pages[job.lod].push_back(job.page_coordinate);
		world_occupancy.ddgi_last_published_invalidation_masks[job.lod].push_back(job.invalidation_mask);
		const DdgiPlacementPage *previous_page = ddgi_placement_pages[job.lod].getptr(job.page_coordinate);
		if (previous_page != nullptr) {
			world_occupancy.ddgi_cached_probe_count[job.lod] -= MIN(world_occupancy.ddgi_cached_probe_count[job.lod], _ddgi_mask_bit_count(previous_page->relevant_mask));
		}
		if (replacement.relevant_mask == 0) {
			ddgi_placement_pages[job.lod].erase(job.page_coordinate);
		} else {
			ddgi_placement_pages[job.lod].insert(job.page_coordinate, replacement);
			world_occupancy.ddgi_cached_probe_count[job.lod] += _ddgi_mask_bit_count(replacement.relevant_mask);
			probes_published += _ddgi_mask_bit_count(job.invalidation_mask);
		}
		world_occupancy.ddgi_cached_page_count[job.lod] = ddgi_placement_pages[job.lod].size();
	}
	world_occupancy.ddgi_last_pages_built = publish_end - publish_begin;
	world_occupancy.ddgi_page_publication_serial++;
	world_occupancy.ddgi_last_dirty_probe_cell_count = probes_published;
	uint32_t pending_count = batch.results.size() - batch.publish_cursor;
	for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
		pending_count += pending_ddgi_pages[lod].size();
	}
	world_occupancy.ddgi_pending_page_count = pending_count;
	if (batch.publish_cursor >= uint32_t(batch.results.size())) {
		bool has_queued_pages = false;
		for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
			has_queued_pages = has_queued_pages || !pending_ddgi_pages[lod].is_empty();
		}
		world_occupancy.ddgi_probe_cache_revision = !has_queued_pages && batch.occupancy_revision == world_occupancy.revision ? world_occupancy.revision : 0;
		memdelete(ddgi_placement_batch);
		ddgi_placement_batch = nullptr;
	}
}

void VoxelForwardVolumeStorage::volume_set_on_render_thread(RID p_scenario, RID p_base, RID p_voxel_texture, RID p_brick_texture, RID p_neighbor_texture, RID p_palette_texture, RID p_material_texture, RID p_metallic_texture, RID p_specularity_texture, RID p_emission_texture, RID p_transparency_texture, bool p_has_metallic_texture, bool p_has_specularity_texture, bool p_has_emission_texture, bool p_has_transparency_texture, PackedByteArray p_occupancy_directory, PackedByteArray p_occupancy_bricks, Vector3i p_dimensions, Vector3i p_brick_dimensions, Vector3i p_atlas_brick_dimensions, Transform3D p_transform, float p_voxel_size, int p_occupied_brick_count, int p_neighbor_mask, int p_neighbor_diagonal_mask, bool p_outline_enabled, Color p_outline_color, float p_outline_width, Vector3i p_dirty_position, Vector3i p_dirty_size, int64_t p_revision, bool p_casts_shadow) {
	VoxelForwardVolumeStorage *storage = get_singleton();
	if (storage == nullptr || !p_base.is_valid()) {
		return;
	}
 singleton->base_scenarios.insert(p_base, p_scenario);
 storage = singleton->get_scenario_storage(p_scenario);

	if (p_occupancy_directory.is_empty()) {
		return;
	}

	storage->lighting_content_revision++;
	const Volume *previous_volume = storage->volumes.getptr(p_base);
	// A same-voxel-revision registration is the material/palette update path.
	// Count it even when an ImageTexture updates in place and retains its RID.
	const bool shading_content_changed = previous_volume == nullptr || previous_volume->revision == uint64_t(p_revision) ||
			previous_volume->palette_texture != p_palette_texture || previous_volume->material_texture != p_material_texture ||
			previous_volume->metallic_texture != p_metallic_texture || previous_volume->specularity_texture != p_specularity_texture ||
			previous_volume->emission_texture != p_emission_texture || previous_volume->transparency_texture != p_transparency_texture ||
			previous_volume->has_metallic_texture != p_has_metallic_texture || previous_volume->has_specularity_texture != p_has_specularity_texture ||
			previous_volume->has_emission_texture != p_has_emission_texture || previous_volume->has_transparency_texture != p_has_transparency_texture;
	// Occupancy-only users historically relied on world.revision for content
	// edits, but repainting an occupied voxel does not alter any occupancy bit.
	// Treat its voxel revision as radiance content so color/emission clipmaps and
	// the local DDGI dirty region advance even when the sparse mask is identical.
	const bool radiance_content_changed = shading_content_changed ||
			(previous_volume != nullptr && previous_volume->revision != uint64_t(p_revision));
	Volume volume;
	if (previous_volume != nullptr) {
		volume.batch_texture_index = previous_volume->batch_texture_index;
	} else if (!storage->free_batch_texture_indices.is_empty()) {
		volume.batch_texture_index = storage->free_batch_texture_indices[storage->free_batch_texture_indices.size() - 1];
		storage->free_batch_texture_indices.resize(storage->free_batch_texture_indices.size() - 1);
	} else {
		ERR_PRINT("Voxel Forward batch texture table is full; this volume cannot use the batched proxy shader.");
	}
	volume.base = p_base;
	volume.casts_shadow = p_casts_shadow;
	volume.voxel_texture = p_voxel_texture;
	volume.brick_texture = p_brick_texture;
	volume.neighbor_texture = p_neighbor_texture;
	volume.palette_texture = p_palette_texture;
	volume.material_texture = p_material_texture;
	volume.metallic_texture = p_metallic_texture;
	volume.specularity_texture = p_specularity_texture;
	volume.emission_texture = p_emission_texture;
	volume.transparency_texture = p_transparency_texture;
	volume.has_metallic_texture = p_has_metallic_texture;
	volume.has_specularity_texture = p_has_specularity_texture;
	volume.has_emission_texture = p_has_emission_texture;
	volume.has_transparency_texture = p_has_transparency_texture;
	volume.occupancy_directory_cpu = p_occupancy_directory;
	volume.occupancy_bricks_cpu = p_occupancy_bricks;
	volume.transform = p_transform;
	volume.dimensions = p_dimensions;
	volume.brick_dimensions = p_brick_dimensions;
	volume.atlas_brick_dimensions = p_atlas_brick_dimensions;
	volume.occupied_brick_count = uint32_t(MAX(0, p_occupied_brick_count));
	volume.neighbor_mask = uint32_t(MAX(0, p_neighbor_mask));
	volume.neighbor_diagonal_mask = uint32_t(MAX(0, p_neighbor_diagonal_mask));
	volume.voxel_size = p_voxel_size;
	volume.outline_width = p_outline_enabled ? CLAMP(p_outline_width, 0.25f, 4.0f) : 0.0f;
	volume.outline_color_rgba8 = p_outline_color.clamp().to_rgba32();
	volume.revision = uint64_t(p_revision);
	volume.lighting_revision = storage->lighting_content_revision;
	if (previous_volume != nullptr && radiance_content_changed) {
		AABB local_dirty;
		if (previous_volume->revision != volume.revision && p_dirty_size.x > 0 && p_dirty_size.y > 0 && p_dirty_size.z > 0) {
			const Vector3i begin = p_dirty_position.clamp(Vector3i(), volume.dimensions);
			const Vector3i end = (p_dirty_position + p_dirty_size).clamp(Vector3i(), volume.dimensions);
			local_dirty = AABB(Vector3(begin) * volume.voxel_size, Vector3(end - begin) * volume.voxel_size);
		} else {
			// A material texture can change in place while retaining its RID and the
			// voxel content revision. Conservatively dirty this volume, not the world.
			local_dirty = AABB(Vector3(), Vector3(volume.dimensions) * volume.voxel_size);
		}
		if (local_dirty.has_volume()) {
			storage->pending_lighting_dirty_bounds.push_back(volume.transform.xform(local_dirty));
		}
	}
	const bool previous_outline_enabled = previous_volume != nullptr && previous_volume->outline_width > 0.0f && (previous_volume->outline_color_rgba8 & 0xFFu) != 0u;
	const bool current_outline_enabled = volume.outline_width > 0.0f && (volume.outline_color_rgba8 & 0xFFu) != 0u;
	if (previous_outline_enabled != current_outline_enabled) {
		if (current_outline_enabled) {
			storage->enabled_outline_volume_count++;
		} else if (storage->enabled_outline_volume_count > 0) {
			storage->enabled_outline_volume_count--;
		}
	}
	// Membership/shape changes can introduce a better map lattice. Transform-only
	// animation must not continually reselect the static world's coordinate frame.
	if (previous_volume == nullptr || previous_volume->casts_shadow != volume.casts_shadow ||
			previous_volume->occupied_brick_count != volume.occupied_brick_count || !Math::is_equal_approx(previous_volume->voxel_size, volume.voxel_size)) {
		storage->world_grid_selection_dirty = true;
	}
	const bool dynamic_only_change = (!volume.casts_shadow && (previous_volume == nullptr || !previous_volume->casts_shadow)) ||
			(storage->world_occupancy.voxel_size > 0.0f && !storage->_is_volume_in_world_occupancy(volume) &&
					(previous_volume == nullptr || !storage->_is_volume_in_world_occupancy(*previous_volume)));
	const bool incremental_change = dynamic_only_change || (previous_volume != nullptr ?
			storage->_queue_incremental_volume_change(*previous_volume, volume, p_dirty_position, p_dirty_size) :
			storage->_queue_incremental_volume_extent(volume));
	const bool spatial_extent_changed = previous_volume == nullptr || previous_volume->casts_shadow != volume.casts_shadow || previous_volume->transform != volume.transform ||
			previous_volume->dimensions != volume.dimensions || !Math::is_equal_approx(previous_volume->voxel_size, volume.voxel_size);
	if (spatial_extent_changed && storage->world_volume_spatial_index_valid) {
		storage->_remove_world_volume_spatial_entry(p_base);
	}
	storage->volumes.insert(p_base, volume);
	if (radiance_content_changed) {
		storage->shading_texture_revision++;
	}
	if (spatial_extent_changed && storage->world_volume_spatial_index_valid) {
		storage->_insert_world_volume_spatial_entry(p_base, volume);
	}
	if (volume.batch_texture_index != INVALID_BATCH_TEXTURE_INDEX) {
		const uint32_t index = volume.batch_texture_index;
		const bool table_changed = storage->batch_voxel_textures[index] != p_voxel_texture ||
				storage->batch_brick_textures[index] != p_brick_texture ||
				storage->batch_neighbor_textures[index] != p_neighbor_texture;
		storage->batch_voxel_textures.write[index] = p_voxel_texture;
		storage->batch_brick_textures.write[index] = p_brick_texture;
		storage->batch_neighbor_textures.write[index] = p_neighbor_texture;
		storage->batch_texture_revision += table_changed ? 1 : 0;
	}
	if (!incremental_change) {
		storage->_request_full_world_rebuild("volume registration or content change requires rebuild");
	}
}

void VoxelForwardVolumeStorage::volume_neighbors_set_on_render_thread(RID p_base, RID p_neighbor_texture, int p_neighbor_mask, int p_neighbor_diagonal_mask) {
	VoxelForwardVolumeStorage *storage = get_singleton();
 if (!storage || !storage->base_scenarios.has(p_base)) return;
 RID scenario = storage->base_scenarios[p_base];
 storage = storage->get_scenario_storage(scenario);
	if (storage == nullptr) {
		return;
	}
	Volume *volume = storage->volumes.getptr(p_base);
	if (volume == nullptr) {
		return;
	}
	volume->neighbor_mask = uint32_t(MAX(0, p_neighbor_mask));
	volume->neighbor_diagonal_mask = uint32_t(MAX(0, p_neighbor_diagonal_mask));
	if (volume->neighbor_texture == p_neighbor_texture) {
		return;
	}
	volume->neighbor_texture = p_neighbor_texture;
	if (volume->batch_texture_index != INVALID_BATCH_TEXTURE_INDEX) {
		storage->batch_neighbor_textures.write[volume->batch_texture_index] = p_neighbor_texture;
		storage->batch_texture_revision++;
	}
}

void VoxelForwardVolumeStorage::volume_transform_set_on_render_thread(RID p_base, Transform3D p_transform) {
	VoxelForwardVolumeStorage *storage = get_singleton();
 if (!storage || !storage->base_scenarios.has(p_base)) return;
 RID scenario = storage->base_scenarios[p_base];
 storage = storage->get_scenario_storage(scenario);
	if (storage == nullptr) {
		return;
	}
	Volume *volume = storage->volumes.getptr(p_base);
	if (volume == nullptr || volume->transform == p_transform) {
		return;
	}

	// Recompose every occupied world brick touched by either transform. The
	// incremental updater reads the final volume table, so the old extent is
	// cleared and the new extent is filled deterministically in one revision.
	// Unsupported rotations/scales, an unavailable table, or an update already
	// being rebuilt retain the exact full-rebuild fallback.
	const Volume previous_volume = *volume;
	Volume transformed_volume = previous_volume;
	transformed_volume.transform = p_transform;
	const bool remains_dynamic = !previous_volume.casts_shadow || (storage->world_occupancy.voxel_size > 0.0f && !storage->_is_volume_in_world_occupancy(previous_volume) && !storage->_is_volume_in_world_occupancy(transformed_volume));
	const bool previous_extent_queued = remains_dynamic || storage->_queue_incremental_volume_extent(previous_volume);
	const bool transformed_extent_queued = remains_dynamic || (previous_extent_queued && storage->_queue_incremental_volume_extent(transformed_volume));
	if (storage->world_volume_spatial_index_valid) {
		storage->_remove_world_volume_spatial_entry(p_base);
	}
	storage->lighting_content_revision++;
	volume->transform = p_transform;
	volume->lighting_revision = storage->lighting_content_revision;
	if (storage->world_volume_spatial_index_valid) {
		storage->_insert_world_volume_spatial_entry(p_base, *volume);
	}
	if (!transformed_extent_queued) {
		storage->_request_full_world_rebuild("volume transform incompatible with incremental update");
	}
}

void VoxelForwardVolumeStorage::volume_remove_on_render_thread(RID p_base) {
	VoxelForwardVolumeStorage *storage = get_singleton();
 if (!storage || !storage->base_scenarios.has(p_base)) return;
 RID scenario = storage->base_scenarios[p_base];
 storage->base_scenarios.erase(p_base);
 storage = storage->get_scenario_storage(scenario);
	if (storage != nullptr) {
	const Volume *previous_volume = storage->volumes.getptr(p_base);
		if (previous_volume == nullptr) {
			return;
		}
		storage->lighting_content_revision++;
		if (previous_volume->outline_width > 0.0f && (previous_volume->outline_color_rgba8 & 0xFFu) != 0u && storage->enabled_outline_volume_count > 0) {
			storage->enabled_outline_volume_count--;
		}
		const bool dynamic_only_change = !previous_volume->casts_shadow || (storage->world_occupancy.voxel_size > 0.0f && !storage->_is_volume_in_world_occupancy(*previous_volume));
		const bool incremental_change = dynamic_only_change || storage->_queue_incremental_volume_extent(*previous_volume);
		if (storage->world_volume_spatial_index_valid) {
			storage->_remove_world_volume_spatial_entry(p_base);
		}
		if (previous_volume->batch_texture_index != INVALID_BATCH_TEXTURE_INDEX) {
			const uint32_t index = previous_volume->batch_texture_index;
			storage->batch_voxel_textures.write[index] = RID();
			storage->batch_brick_textures.write[index] = RID();
			storage->batch_neighbor_textures.write[index] = RID();
			storage->free_batch_texture_indices.push_back(index);
			storage->batch_texture_revision++;
		}
		storage->volumes.erase(p_base);
		storage->world_grid_selection_dirty = true;
		storage->shading_texture_revision++;
		if (!incremental_change) {
			storage->_request_full_world_rebuild("volume removal requires rebuild");
		}
	}
}

void VoxelForwardVolumeStorage::_build_world_occupancy(void *p_userdata) {
	WorldOccupancyBuild *build = static_cast<WorldOccupancyBuild *>(p_userdata);
	const uint64_t build_started_usec = OS::get_singleton()->get_ticks_usec();
	HashMap<Vector3i, WorldBrickBits> world_bricks;
	uint64_t source_brick_capacity = 0;
	for (const Volume &volume : build->volumes) {
		source_brick_capacity += volume.occupied_brick_count;
	}
	world_bricks.reserve(uint32_t(MIN(source_brick_capacity, uint64_t(UINT32_MAX))));
	const float world_voxel_size = build->voxel_size;
	const Vector3 world_origin = build->origin;
	uint32_t incompatible_volume_count = 0;
	auto get_world_brick = [&world_bricks](const Vector3i &p_position) -> WorldBrickBits * {
		return &world_bricks[p_position];
	};
	for (const Volume &volume : build->volumes) {
		GridAlignedVolumeTransform grid_transform;
		if (world_voxel_size == 0.0f || !_get_aligned_volume_transform(volume, world_voxel_size, world_origin, grid_transform)) {
			incompatible_volume_count++;
			continue;
		}

		const int expected_directory_bytes = volume.brick_dimensions.x * volume.brick_dimensions.y * volume.brick_dimensions.z * 4;
		if (volume.occupancy_directory_cpu.size() < expected_directory_bytes) {
			incompatible_volume_count++;
			continue;
		}
		const bool brick_aligned = grid_transform.identity_orientation &&
				grid_transform.world_min_voxel.x % OCCUPANCY_BRICK_SIZE == 0 &&
				grid_transform.world_min_voxel.y % OCCUPANCY_BRICK_SIZE == 0 &&
				grid_transform.world_min_voxel.z % OCCUPANCY_BRICK_SIZE == 0;
		const Vector3i world_brick_origin = brick_aligned ? grid_transform.world_min_voxel / OCCUPANCY_BRICK_SIZE : Vector3i();
		if (brick_aligned) {
			build->brick_aligned_volume_count++;
		}
		for (int brick_index = 0; brick_index < volume.brick_dimensions.x * volume.brick_dimensions.y * volume.brick_dimensions.z; brick_index++) {
			const uint32_t code = _read_u32(volume.occupancy_directory_cpu, brick_index * 4);
			if (code == 0) {
				continue;
			}
			WorldBrickBits source_occupancy;
			if (!_read_volume_brick_bits(volume, brick_index, source_occupancy)) {
				continue;
			}
			const Vector3i local_brick(
					brick_index % volume.brick_dimensions.x,
					(brick_index / volume.brick_dimensions.x) % volume.brick_dimensions.y,
					brick_index / (volume.brick_dimensions.x * volume.brick_dimensions.y));
			// Most streamed chunks are positioned on the same 8-voxel brick
			// grid. Merge their complete occupancy masks directly instead of
			// expanding every occupied brick into as many as 512 hash lookups.
			// This is bit-identical to the general path below and also handles
			// overlaps between volumes by OR-ing the eight 64-bit mask words.
			if (brick_aligned) {
				WorldBrickBits *world_brick = get_world_brick(world_brick_origin + local_brick);
				for (int word = 0; word < 8; word++) {
					world_brick->bits[word] |= source_occupancy.bits[word];
				}
				continue;
			}

			const Vector3i local_voxel_start = local_brick * OCCUPANCY_BRICK_SIZE;
			const Vector3i local_voxel_end(
					MIN(local_voxel_start.x + OCCUPANCY_BRICK_SIZE, volume.dimensions.x),
					MIN(local_voxel_start.y + OCCUPANCY_BRICK_SIZE, volume.dimensions.y),
					MIN(local_voxel_start.z + OCCUPANCY_BRICK_SIZE, volume.dimensions.z));
			Vector3i source_world_begin;
			Vector3i source_world_end;
			_local_bounds_to_world(volume, grid_transform, local_voxel_start, local_voxel_end, source_world_begin, source_world_end);
			if (source_world_begin.x >= source_world_end.x || source_world_begin.y >= source_world_end.y || source_world_begin.z >= source_world_end.z) {
				continue;
			}
			const Vector3i first_world_brick(
					_floor_divide_by_brick_size(source_world_begin.x),
					_floor_divide_by_brick_size(source_world_begin.y),
					_floor_divide_by_brick_size(source_world_begin.z));
			WorldBrickBits split_occupancy[8] = {};
			for (int local_index = 0; local_index < OCCUPANCY_BRICK_SIZE * OCCUPANCY_BRICK_SIZE * OCCUPANCY_BRICK_SIZE; local_index++) {
				if ((source_occupancy.bits[local_index >> 6] & (uint64_t(1) << (local_index & 63))) == 0) {
					continue;
				}
				const Vector3i local_voxel = local_voxel_start + Vector3i(local_index & 7, (local_index >> 3) & 7, local_index >> 6);
				if (local_voxel.x >= volume.dimensions.x || local_voxel.y >= volume.dimensions.y || local_voxel.z >= volume.dimensions.z) {
					continue;
				}
				const Vector3i world_voxel = _local_voxel_to_world(volume, grid_transform, local_voxel);
				const Vector3i destination_brick(
						_floor_divide_by_brick_size(world_voxel.x),
						_floor_divide_by_brick_size(world_voxel.y),
						_floor_divide_by_brick_size(world_voxel.z));
				const Vector3i destination_offset = destination_brick - first_world_brick;
				const int destination_index = destination_offset.x | (destination_offset.y << 1) | (destination_offset.z << 2);
				const Vector3i world_local = world_voxel - destination_brick * OCCUPANCY_BRICK_SIZE;
				const int world_local_index = world_local.x + world_local.y * OCCUPANCY_BRICK_SIZE + world_local.z * OCCUPANCY_BRICK_SIZE * OCCUPANCY_BRICK_SIZE;
				split_occupancy[destination_index].bits[world_local_index >> 6] |= uint64_t(1) << (world_local_index & 63);
			}
			for (int destination_index = 0; destination_index < 8; destination_index++) {
				if (_world_brick_bits_empty(split_occupancy[destination_index])) {
					continue;
				}
				const Vector3i destination_offset(destination_index & 1, (destination_index >> 1) & 1, destination_index >> 2);
				WorldBrickBits *world_brick = get_world_brick(first_world_brick + destination_offset);
				for (int word = 0; word < 8; word++) {
					world_brick->bits[word] |= split_occupancy[destination_index].bits[word];
				}
			}
		}
	}
	print_verbose(vformat("Voxel Forward: full occupancy merged %d source volumes into %d world bricks in %.2f ms.", build->volumes.size(), world_bricks.size(), double(OS::get_singleton()->get_ticks_usec() - build_started_usec) / 1000.0));

	uint32_t directory_capacity = MAX(1u, build->minimum_directory_capacity);
	while (directory_capacity < MAX(1u, uint32_t(world_bricks.size()) * 2u)) {
		directory_capacity <<= 1;
	}
	build->directory_bytes.resize(directory_capacity * sizeof(WorldDirectoryEntry));
	build->directory_bytes.fill(0);
	WorldDirectoryEntry *directory = reinterpret_cast<WorldDirectoryEntry *>(build->directory_bytes.ptrw());
	uint32_t mixed_brick_count = 0;
	uint32_t max_probe_count = 0;
	for (const KeyValue<Vector3i, WorldBrickBits> &entry : world_bricks) {
		build->occupied_bricks.insert(entry.key);
		for (uint32_t lod = 0; lod < VoxelForwardVolumeStorage::DDGI_PLACEMENT_LOD_COUNT; lod++) {
			_ddgi_add_brick_page_seeds(entry.key, build->ddgi_cell_spacing_voxels[lod], build->ddgi_page_seeds[lod]);
		}
		uint32_t slot = _world_brick_hash(entry.key) & (directory_capacity - 1);
		uint32_t probe_count = 1;
		while (directory[slot].code != 0) {
			slot = (slot + 1) & (directory_capacity - 1);
			probe_count++;
		}
		max_probe_count = MAX(max_probe_count, probe_count);
		directory[slot].x = entry.key.x;
		directory[slot].y = entry.key.y;
		directory[slot].z = entry.key.z;
		bool uniform = true;
		for (int word = 0; word < 8; word++) {
			uniform = uniform && entry.value.bits[word] == UINT64_MAX;
		}
		if (uniform) {
			directory[slot].code = 1;
		} else {
			directory[slot].code = mixed_brick_count + 2;
			for (int word = 0; word < 8; word++) {
				for (int byte = 0; byte < 8; byte++) {
					build->brick_bytes.push_back(uint8_t((entry.value.bits[word] >> (byte * 8)) & 0xFF));
				}
			}
			mixed_brick_count++;
		}
	}
	if (build->brick_bytes.is_empty()) {
		// Keep the CPU mirror aligned to the 64-byte unit used by incremental
		// comparisons and uploads. A four-byte placeholder made an otherwise
		// valid in-place rebuild fail _update_changed_buffer_units().
		build->brick_bytes.resize(OCCUPANCY_BRICK_BYTES);
		build->brick_bytes.fill(0);
	}
	build->voxel_size = world_voxel_size;
	build->origin = world_origin;
	build->directory_mask = directory_capacity - 1;
	build->occupied_brick_count = world_bricks.size();
	build->mixed_brick_count = mixed_brick_count;
	build->max_probe_count = max_probe_count;
	build->incompatible_volume_count = incompatible_volume_count;
	print_verbose(vformat("Voxel Forward: full occupancy packed %d world bricks (%d mixed) in %.2f ms.", world_bricks.size(), mixed_brick_count, double(OS::get_singleton()->get_ticks_usec() - build_started_usec) / 1000.0));
	build->build_usec = OS::get_singleton()->get_ticks_usec() - build_started_usec;
}

void VoxelForwardVolumeStorage::_finish_world_occupancy_build() {
	GodotProfileZone("Voxel Occupancy Full-Build Completion");
	if (world_occupancy_build_task == WorkerThreadPool::INVALID_TASK_ID ||
			!WorkerThreadPool::get_singleton()->is_task_completed(world_occupancy_build_task)) {
		return;
	}
	WorkerThreadPool::get_singleton()->wait_for_task_completion(world_occupancy_build_task);
	world_occupancy_build_task = WorkerThreadPool::INVALID_TASK_ID;
	WorldOccupancyBuild *build = world_occupancy_build;
	world_occupancy_build = nullptr;
	if (build == nullptr) {
		return;
	}
	// Publish each complete immutable snapshot even when newer edits are queued.
	// Discarding it on every animated transform starves the world indefinitely.
	// Keep world_occupancy_dirty set so the next snapshot catches up; incremental
	// updates remain disabled until that full rebuild has caught up as well.
	const uint64_t upload_started_usec = OS::get_singleton()->get_ticks_usec();
	GodotProfileZone("Voxel Occupancy Full-Build GPU Publication");
	RD *rd = RD::get_singleton();
	if (rd == nullptr) {
		world_occupancy_dirty = true;
		memdelete(build);
		return;
	}
	bool layout_changed = !Math::is_equal_approx(world_occupancy.voxel_size, build->voxel_size) || !world_occupancy.origin.is_equal_approx(build->origin);
	for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
		layout_changed = layout_changed || world_occupancy.ddgi_cell_spacing_voxels[lod] != build->ddgi_cell_spacing_voxels[lod];
	}
	const bool can_update_in_place = world_occupancy.directory_buffer.is_valid() && world_occupancy.brick_buffer.is_valid() &&
			world_occupancy_directory_capacity_bytes == uint32_t(build->directory_bytes.size()) &&
			world_occupancy_brick_capacity_bytes >= uint32_t(build->brick_bytes.size());
	if (can_update_in_place) {
		const Error directory_error = _update_changed_buffer_units(rd, world_occupancy.directory_buffer, world_occupancy_directory_cpu, build->directory_bytes, sizeof(WorldDirectoryEntry));
		const Error brick_error = _update_changed_buffer_units(rd, world_occupancy.brick_buffer, world_occupancy_bricks_cpu, build->brick_bytes, OCCUPANCY_BRICK_BYTES);
		if (directory_error == OK && brick_error == OK) {
			_clear_ddgi_placement_batch();
			world_occupancy_directory_cpu = build->directory_bytes;
			world_occupancy_bricks_cpu = build->brick_bytes;
			world_occupied_bricks_cpu = build->occupied_bricks;
			world_occupancy_free_mixed_slots.clear();
			world_occupancy.voxel_size = build->voxel_size;
			world_occupancy.origin = build->origin;
			world_occupancy.directory_mask = build->directory_mask;
			world_occupancy.occupied_brick_count = build->occupied_brick_count;
			world_occupancy.mixed_brick_count = build->mixed_brick_count;
			world_occupancy.max_probe_count = build->max_probe_count;
			for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
				world_occupancy.ddgi_cell_spacing_voxels[lod] = build->ddgi_cell_spacing_voxels[lod];
			}
			world_occupancy.tombstone_count = 0;
			world_occupancy.incompatible_volume_count = build->incompatible_volume_count;
			world_occupancy.last_dirty_brick_count = build->occupied_brick_count;
			world_occupancy.last_uploaded_bytes = build->directory_bytes.size() + build->brick_bytes.size();
			world_occupancy.last_full_upload_usec = OS::get_singleton()->get_ticks_usec() - upload_started_usec;
			world_occupancy.last_dirty_bricks.clear();
			world_occupancy.last_incremental_revision = 0;
			world_occupancy.full_rebuild_count++;
			world_occupancy.revision++;
			_publish_ddgi_page_seeds(build->ddgi_page_seeds, layout_changed);
			world_occupancy.ddgi_probe_cache_revision = 0;
			world_occupancy.ddgi_last_probe_bake_usec = 0;
			world_occupancy.ddgi_last_dirty_probe_cell_count = 0;
			world_occupancy.ddgi_full_probe_bake_count++;
			_rebuild_world_volume_spatial_index();
			print_verbose(vformat("Voxel Forward: world occupancy built in %.2f ms and updated on GPU in %.2f ms (%d bricks, %d/%d volumes brick-aligned; %d bounded DDGI page jobs queued).", double(build->build_usec) / 1000.0, double(OS::get_singleton()->get_ticks_usec() - upload_started_usec) / 1000.0, build->occupied_brick_count, build->brick_aligned_volume_count, build->volumes.size(), world_occupancy.ddgi_pending_page_count));
			memdelete(build);
			return;
		}
	}

	uint32_t brick_capacity = 4;
	const uint32_t requested_brick_capacity = MAX(4u, uint32_t(build->brick_bytes.size()) + uint32_t(build->brick_bytes.size()) / 2u);
	while (brick_capacity < requested_brick_capacity) {
		brick_capacity <<= 1;
	}
	const uint32_t directory_capacity_bytes = build->directory_bytes.size();
	const RID new_directory = rd->storage_buffer_create(directory_capacity_bytes);
	const RID new_bricks = rd->storage_buffer_create(brick_capacity);
	if (!new_directory.is_valid() || !new_bricks.is_valid()) {
		if (new_directory.is_valid()) {
			rd->free_rid(new_directory);
		}
		if (new_bricks.is_valid()) {
			rd->free_rid(new_bricks);
		}
		world_occupancy_dirty = true;
		memdelete(build);
		return;
	}
	if (rd->buffer_update(new_directory, 0, build->directory_bytes.size(), build->directory_bytes.ptr()) != OK ||
			rd->buffer_update(new_bricks, 0, build->brick_bytes.size(), build->brick_bytes.ptr()) != OK) {
		rd->free_rid(new_directory);
		rd->free_rid(new_bricks);
		world_occupancy_dirty = true;
		memdelete(build);
		return;
	}

	// Keep the previous table live while the CPU build runs. Swap only after
	// both replacement buffers exist so shadow rendering never observes a
	// half-built world.
	_free_world_gpu_resources();
	world_occupancy.directory_buffer = new_directory;
	world_occupancy.brick_buffer = new_bricks;
	world_occupancy_directory_capacity_bytes = directory_capacity_bytes;
	world_occupancy_brick_capacity_bytes = brick_capacity;
	world_occupancy_directory_cpu = build->directory_bytes;
	world_occupancy_bricks_cpu = build->brick_bytes;
	world_occupied_bricks_cpu = build->occupied_bricks;
	world_occupancy_gpu_bytes = uint64_t(directory_capacity_bytes) + uint64_t(brick_capacity);
	occupancy_gpu_bytes += world_occupancy_gpu_bytes;
	world_occupancy.voxel_size = build->voxel_size;
	world_occupancy.origin = build->origin;
	world_occupancy.directory_mask = build->directory_mask;
	world_occupancy.occupied_brick_count = build->occupied_brick_count;
	world_occupancy.mixed_brick_count = build->mixed_brick_count;
	world_occupancy.max_probe_count = build->max_probe_count;
	for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
		world_occupancy.ddgi_cell_spacing_voxels[lod] = build->ddgi_cell_spacing_voxels[lod];
	}
	world_occupancy.tombstone_count = 0;
	world_occupancy.incompatible_volume_count = build->incompatible_volume_count;
	world_occupancy.last_dirty_brick_count = build->occupied_brick_count;
	world_occupancy.last_uploaded_bytes = build->directory_bytes.size() + build->brick_bytes.size();
	world_occupancy.last_full_upload_usec = OS::get_singleton()->get_ticks_usec() - upload_started_usec;
	world_occupancy.last_dirty_bricks.clear();
	world_occupancy.last_incremental_revision = 0;
	world_occupancy.full_rebuild_count++;
	world_occupancy.revision++;
	_publish_ddgi_page_seeds(build->ddgi_page_seeds, layout_changed);
	world_occupancy.ddgi_probe_cache_revision = 0;
	world_occupancy.ddgi_last_probe_bake_usec = 0;
	world_occupancy.ddgi_last_dirty_probe_cell_count = 0;
	world_occupancy.ddgi_full_probe_bake_count++;
	_rebuild_world_volume_spatial_index();
	print_verbose(vformat("Voxel Forward: world occupancy built in %.2f ms and uploaded to GPU in %.2f ms (%d bricks, %d/%d volumes brick-aligned; %d bounded DDGI page jobs queued).", double(build->build_usec) / 1000.0, double(OS::get_singleton()->get_ticks_usec() - upload_started_usec) / 1000.0, build->occupied_brick_count, build->brick_aligned_volume_count, build->volumes.size(), world_occupancy.ddgi_pending_page_count));
	memdelete(build);
}

void VoxelForwardVolumeStorage::update_world_occupancy(bool p_enabled) {
	GodotProfileZone("Update Voxel World Occupancy");
	if (!p_enabled) {
		if (world_occupancy_build_task != WorkerThreadPool::INVALID_TASK_ID &&
				WorkerThreadPool::get_singleton()->is_task_completed(world_occupancy_build_task)) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(world_occupancy_build_task);
			world_occupancy_build_task = WorkerThreadPool::INVALID_TASK_ID;
			memdelete(world_occupancy_build);
			world_occupancy_build = nullptr;
		}
		if (world_occupancy.directory_buffer.is_valid() || world_occupancy.brick_buffer.is_valid() || world_occupancy.occupied_brick_count != 0) {
			_free_world_gpu_resources();
			world_occupancy.revision++;
		}
		world_occupancy_dirty = true;
		world_occupancy_quiet_frames = 0;
		world_occupancy_wait_frames = 0;
		return;
	}

	static const char *spacing_settings[DDGI_PLACEMENT_LOD_COUNT] = {
		"rendering/voxel_forward/indirect_light/ddgi/lod0_spacing_voxels",
		"rendering/voxel_forward/indirect_light/ddgi/lod1_spacing_voxels",
		"rendering/voxel_forward/indirect_light/ddgi/lod2_spacing_voxels",
		"rendering/voxel_forward/indirect_light/ddgi/lod3_spacing_voxels",
	};
	bool spacing_changed = false;
	uint32_t minimum_spacing = 4;
	for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
		// Cascades must remain nested. Clamp an invalid lower coarse spacing to
		// the preceding LOD instead of creating a coverage hole.
		const uint32_t requested_spacing = uint32_t(CLAMP(int(GLOBAL_GET(spacing_settings[lod])), int(minimum_spacing), 256));
		minimum_spacing = requested_spacing;
		if (ddgi_requested_cell_spacing_voxels[lod] != requested_spacing) {
			ddgi_requested_cell_spacing_voxels[lod] = requested_spacing;
			spacing_changed = true;
		}
	}
	if (spacing_changed) {
		_request_full_world_rebuild("DDGI cascade spacing changed");
	}

	if (world_grid_selection_dirty) {
		_select_world_grid();
	}
	_finish_world_occupancy_build();
	if (world_occupancy_build_task == WorkerThreadPool::INVALID_TASK_ID && !world_occupancy_dirty && !pending_incremental_world_bricks.is_empty()) {
		_apply_incremental_world_updates();
	}
	_process_ddgi_placement_page_queue();
	if (world_occupancy_build_task != WorkerThreadPool::INVALID_TASK_ID || !world_occupancy_dirty) {
		return;
	}
	// Streaming and destruction can enqueue several mutations close together.
	// Coalesce a quiet pair of frames, but never require actors or animations
	// to stop moving before the first occupancy table can be built. Snapshot
	// immutable inputs after at most four render frames; later edits remain queued.
	world_occupancy_wait_frames++;
	if (world_occupancy_quiet_frames < 2 && world_occupancy_wait_frames < 4) {
		world_occupancy_quiet_frames++;
		return;
	}
	world_occupancy_dirty = false;
	world_occupancy_quiet_frames = 0;
	world_occupancy_wait_frames = 0;
	world_occupancy_build = memnew(WorldOccupancyBuild);
	world_occupancy_build->origin = selected_world_origin;
	world_occupancy_build->voxel_size = selected_world_voxel_size;
	world_occupancy_build->minimum_directory_capacity = world_occupancy_directory_capacity_bytes / sizeof(WorldDirectoryEntry);
	world_occupancy_build->requested_revision = world_occupancy.revision + 1u;
	for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
		world_occupancy_build->ddgi_cell_spacing_voxels[lod] = ddgi_requested_cell_spacing_voxels[lod];
	}
	world_occupancy_build->volumes.resize(volumes.size());
	int volume_index = 0;
	for (const KeyValue<RID, Volume> &entry : volumes) {
		world_occupancy_build->volumes.write[volume_index++] = entry.value;
	}
	world_occupancy_build_task = WorkerThreadPool::get_singleton()->add_native_task(
			&_build_world_occupancy, world_occupancy_build, false, "VoxelWorldOccupancyBuild");
}

const VoxelForwardVolumeStorage::Volume *VoxelForwardVolumeStorage::get_volume(RID p_base) const {
	return volumes.getptr(p_base);
}

bool VoxelForwardVolumeStorage::_is_volume_in_world_occupancy(const Volume &p_volume) const {
	if (world_occupancy.voxel_size <= 0.0f) {
		return false;
	}
	GridAlignedVolumeTransform grid_transform;
	const int expected_directory_bytes = p_volume.brick_dimensions.x * p_volume.brick_dimensions.y * p_volume.brick_dimensions.z * int(sizeof(uint32_t));
	return p_volume.occupancy_directory_cpu.size() >= expected_directory_bytes &&
			_get_aligned_volume_transform(p_volume, world_occupancy.voxel_size, world_occupancy.origin, grid_transform);
}

void VoxelForwardVolumeStorage::get_non_world_occupancy_volumes(Vector<const Volume *> &r_volumes) const {
	r_volumes.clear();
	if (world_occupancy.voxel_size <= 0.0f) {
		return;
	}
	for (const KeyValue<RID, Volume> &entry : volumes) {
		const Volume &volume = entry.value;
		if (volume.casts_shadow && !_is_volume_in_world_occupancy(volume)) {
			r_volumes.push_back(&volume);
		}
	}
}

void VoxelForwardVolumeStorage::consume_lighting_dirty_bounds(Vector<AABB> &r_bounds) {
	r_bounds = pending_lighting_dirty_bounds;
	pending_lighting_dirty_bounds.clear();
}

bool VoxelForwardVolumeStorage::get_ddgi_probe_placement(uint32_t p_lod, const Vector3i &p_logical_cell, DdgiProbePlacement &r_placement) const {
	if (p_lod >= DDGI_PLACEMENT_LOD_COUNT) {
		return false;
	}
	const Vector3i page_coordinate = _ddgi_page_coordinate(p_logical_cell);
	const DdgiPlacementPage *page = ddgi_placement_pages[p_lod].getptr(page_coordinate);
	if (page == nullptr) {
		return false;
	}
	const uint32_t index = _ddgi_page_local_index(p_logical_cell, page_coordinate);
	if ((page->relevant_mask & (uint64_t(1) << index)) == 0) {
		return false;
	}
	r_placement = page->records[index];
	return true;
}

void VoxelForwardVolumeStorage::prioritize_queued_ddgi_pages_for_world_load(const Vector3 &p_world_position, uint32_t p_spatial_resolution) {
	ddgi_load_priority_world_position = p_world_position;
	ddgi_load_priority_resolution = p_spatial_resolution;
	ddgi_load_priority_valid = p_spatial_resolution > 0;
	if (world_occupancy.voxel_size <= 0.0f || p_spatial_resolution == 0 || ddgi_load_priority_applied_revision == world_occupancy.revision) {
		return;
	}

	// Placement remains a world-data operation. This only changes the order of
	// already queued world-load pages so the initial resident cascade windows
	// become usable before distant pages; camera movement never creates work.
	const Vector3 voxel_position = (p_world_position - world_occupancy.origin) / world_occupancy.voxel_size;
	for (uint32_t lod = 0; lod < DDGI_PLACEMENT_LOD_COUNT; lod++) {
		Vector<Vector3i> &pending = pending_ddgi_pages[lod];
		if (pending.is_empty()) {
			continue;
		}

		const Vector3i cell_size = _ddgi_cell_size_voxels(world_occupancy.ddgi_cell_spacing_voxels[lod]);
		const Vector3 cell_position(
				voxel_position.x / float(cell_size.x),
				voxel_position.y / float(cell_size.y),
				voxel_position.z / float(cell_size.z));
		const Vector3i center_cell(
				Math::floor(cell_position.x + 0.5f),
				Math::floor(cell_position.y + 0.5f),
				Math::floor(cell_position.z + 0.5f));
		const Vector3i first_cell = center_cell - Vector3i(int(p_spatial_resolution) / 2, int(p_spatial_resolution) / 2, int(p_spatial_resolution) / 2);
		const Vector3i last_cell = first_cell + Vector3i(int(p_spatial_resolution) - 1, int(p_spatial_resolution) - 1, int(p_spatial_resolution) - 1);
		const Vector3i first_page = _ddgi_page_coordinate(first_cell);
		const Vector3i last_page = _ddgi_page_coordinate(last_cell);

		Vector<Vector3i> ordinary_pages;
		Vector<Vector3i> resident_pages;
		ordinary_pages.reserve(pending.size());
		resident_pages.reserve(pending.size());
		for (const Vector3i &page : pending) {
			const bool resident = page.x >= first_page.x && page.x <= last_page.x &&
					page.y >= first_page.y && page.y <= last_page.y &&
					page.z >= first_page.z && page.z <= last_page.z;
			if (resident) {
				resident_pages.push_back(page);
			} else {
				ordinary_pages.push_back(page);
			}
		}
		ordinary_pages.append_array(resident_pages);
		pending = ordinary_pages;
	}
	ddgi_load_priority_applied_revision = world_occupancy.revision;
}

VoxelForwardVolumeStorage::VoxelForwardVolumeStorage(bool p_registry) {
 registry = p_registry;
	ERR_FAIL_COND(registry && singleton != nullptr);
	if (registry) singleton = this;
	batch_voxel_textures.resize(MAX_BATCH_TEXTURES);
	batch_brick_textures.resize(MAX_BATCH_TEXTURES);
	batch_neighbor_textures.resize(MAX_BATCH_TEXTURES);
	free_batch_texture_indices.resize(MAX_BATCH_TEXTURES);
	for (uint32_t i = 0; i < MAX_BATCH_TEXTURES; i++) {
		free_batch_texture_indices.write[i] = MAX_BATCH_TEXTURES - 1u - i;
	}
}

void VoxelForwardVolumeStorage::free_scenario_storage(RID p_scenario) {
 if (!scenario_stores.has(p_scenario)) return;
 Vector<RID> removed;
 for (const KeyValue<RID,RID> &entry:base_scenarios) if (entry.value==p_scenario) removed.push_back(entry.key);
 for (RID base:removed) base_scenarios.erase(base);
 memdelete(scenario_stores[p_scenario]);
 scenario_stores.erase(p_scenario);
}

VoxelForwardVolumeStorage *VoxelForwardVolumeStorage::get_scenario_storage(RID p_scenario) {
 if (!scenario_stores.has(p_scenario)) scenario_stores.insert(p_scenario, memnew(VoxelForwardVolumeStorage(false)));
 return scenario_stores[p_scenario];
}

VoxelForwardVolumeStorage::~VoxelForwardVolumeStorage() {
 for (const KeyValue<RID, VoxelForwardVolumeStorage *> &entry : scenario_stores) memdelete(entry.value);
 scenario_stores.clear();
	if (world_occupancy_build_task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(world_occupancy_build_task);
		world_occupancy_build_task = WorkerThreadPool::INVALID_TASK_ID;
	}
	if (world_occupancy_build != nullptr) {
		memdelete(world_occupancy_build);
		world_occupancy_build = nullptr;
	}
	_free_world_gpu_resources();
	volumes.clear();
	if (singleton == this) {
		singleton = nullptr;
	}
}

} // namespace RendererSceneRenderImplementation
