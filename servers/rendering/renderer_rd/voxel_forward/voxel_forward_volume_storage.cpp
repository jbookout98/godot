/**************************************************************************/
/*  voxel_forward_volume_storage.cpp                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/**************************************************************************/

#include "voxel_forward_volume_storage.h"

#include "core/os/os.h"
#include "servers/rendering/rendering_device.h"

#include <cstdint>
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

bool _get_axis_aligned_voxel_size(const VoxelForwardVolumeStorage::Volume &p_volume, float &r_world_voxel_size) {
	const Vector3 axis_x = p_volume.transform.basis.get_column(0);
	const Vector3 axis_y = p_volume.transform.basis.get_column(1);
	const Vector3 axis_z = p_volume.transform.basis.get_column(2);
	const float scale = axis_x.length();
	const float epsilon = 0.0001f;
	if (scale <= epsilon || !Math::is_equal_approx(axis_y.length(), scale) || !Math::is_equal_approx(axis_z.length(), scale) ||
			!axis_x.normalized().is_equal_approx(Vector3(1, 0, 0)) || !axis_y.normalized().is_equal_approx(Vector3(0, 1, 0)) || !axis_z.normalized().is_equal_approx(Vector3(0, 0, 1))) {
		return false;
	}
	r_world_voxel_size = p_volume.voxel_size * scale;
	return true;
}

bool _get_aligned_volume_origin(const VoxelForwardVolumeStorage::Volume &p_volume, float p_world_voxel_size, const Vector3 &p_world_origin, Vector3i &r_voxel_origin) {
	float volume_world_voxel_size = 0.0f;
	if (!_get_axis_aligned_voxel_size(p_volume, volume_world_voxel_size) || !Math::is_equal_approx(volume_world_voxel_size, p_world_voxel_size)) {
		return false;
	}
	const Vector3 voxel_origin = (p_volume.transform.origin - p_world_origin) / p_world_voxel_size;
	const Vector3i rounded_origin(Math::round(voxel_origin.x), Math::round(voxel_origin.y), Math::round(voxel_origin.z));
	if (!voxel_origin.is_equal_approx(Vector3(rounded_origin))) {
		return false;
	}
	r_voxel_origin = rounded_origin;
	return true;
}

int _floor_divide_by_brick_size(int p_value) {
	return p_value >= 0 ? p_value / OCCUPANCY_BRICK_SIZE : (p_value - (OCCUPANCY_BRICK_SIZE - 1)) / OCCUPANCY_BRICK_SIZE;
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

} // namespace

VoxelForwardVolumeStorage *VoxelForwardVolumeStorage::singleton = nullptr;

void VoxelForwardVolumeStorage::_free_world_gpu_resources() {
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

	// The CPU mirrors are updated before the live occupancy revision advances.
	// Uploading both mirrors into private buffers therefore captures one exact
	// revision without a GPU readback or a race with later incremental writes.
	PackedByteArray snapshot_brick_bytes = world_occupancy_bricks_cpu;
	if (snapshot_brick_bytes.is_empty()) {
		snapshot_brick_bytes.resize(4);
		snapshot_brick_bytes.fill(0);
	}
	const RID snapshot_directory = rd->storage_buffer_create(world_occupancy_directory_cpu.size(), world_occupancy_directory_cpu);
	const RID snapshot_bricks = rd->storage_buffer_create(snapshot_brick_bytes.size(), snapshot_brick_bytes);
	if (!snapshot_directory.is_valid() || !snapshot_bricks.is_valid()) {
		if (snapshot_directory.is_valid()) {
			rd->free_rid(snapshot_directory);
		}
		if (snapshot_bricks.is_valid()) {
			rd->free_rid(snapshot_bricks);
		}
		return false;
	}

	r_snapshot = world_occupancy;
	r_snapshot.directory_buffer = snapshot_directory;
	r_snapshot.brick_buffer = snapshot_bricks;
	rd->set_resource_name(snapshot_directory, vformat("Voxel World Occupancy Snapshot Directory r%d", world_occupancy.revision));
	rd->set_resource_name(snapshot_bricks, vformat("Voxel World Occupancy Snapshot Bricks r%d", world_occupancy.revision));
	return true;
}

void VoxelForwardVolumeStorage::_request_full_world_rebuild() {
	pending_incremental_world_bricks.clear();
	world_occupancy_dirty = true;
	world_occupancy_quiet_frames = 0;
}

bool VoxelForwardVolumeStorage::_queue_incremental_volume_extent(const Volume &p_volume) {
	if (world_occupancy_dirty || world_occupancy_build_task != WorkerThreadPool::INVALID_TASK_ID ||
			!world_occupancy.directory_buffer.is_valid() || !world_occupancy.brick_buffer.is_valid()) {
		return false;
	}

	Vector3i volume_voxel_origin;
	if (!_get_aligned_volume_origin(p_volume, world_occupancy.voxel_size, world_occupancy.origin, volume_voxel_origin)) {
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

				const Vector3i changed_world_begin = volume_voxel_origin + local_brick * OCCUPANCY_BRICK_SIZE;
				const Vector3i changed_world_end = volume_voxel_origin + Vector3i(
						MIN((x + 1) * OCCUPANCY_BRICK_SIZE, p_volume.dimensions.x),
						MIN((y + 1) * OCCUPANCY_BRICK_SIZE, p_volume.dimensions.y),
						MIN((z + 1) * OCCUPANCY_BRICK_SIZE, p_volume.dimensions.z));
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

	Vector3i volume_voxel_origin;
	if (!_get_aligned_volume_origin(p_current, world_occupancy.voxel_size, world_occupancy.origin, volume_voxel_origin)) {
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
				const Vector3i changed_world_begin = volume_voxel_origin + local_brick * OCCUPANCY_BRICK_SIZE;
				const Vector3i changed_world_end = volume_voxel_origin + Vector3i(
						MIN((local_brick.x + 1) * OCCUPANCY_BRICK_SIZE, p_current.dimensions.x),
						MIN((local_brick.y + 1) * OCCUPANCY_BRICK_SIZE, p_current.dimensions.y),
						MIN((local_brick.z + 1) * OCCUPANCY_BRICK_SIZE, p_current.dimensions.z));
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
	if (pending_incremental_world_bricks.is_empty()) {
		return true;
	}
	RD *rd = RD::get_singleton();
	if (rd == nullptr || !world_occupancy.directory_buffer.is_valid() || !world_occupancy.brick_buffer.is_valid() ||
			world_occupancy_directory_cpu.size() != int(world_occupancy.directory_mask + 1) * int(sizeof(WorldDirectoryEntry))) {
		_request_full_world_rebuild();
		return false;
	}

	Vector<Vector3i> dirty_bricks;
	for (const Vector3i &position : pending_incremental_world_bricks) {
		dirty_bricks.push_back(position);
	}
	pending_incremental_world_bricks.clear();

	WorldDirectoryEntry *directory = reinterpret_cast<WorldDirectoryEntry *>(world_occupancy_directory_cpu.ptrw());
	const uint32_t directory_capacity = world_occupancy.directory_mask + 1;
	uint32_t uploaded_bytes = 0;

	for (const Vector3i &world_brick_position : dirty_bricks) {
		WorldBrickBits composed_bits;
		const Vector3i world_voxel_begin = world_brick_position * OCCUPANCY_BRICK_SIZE;
		const Vector3i world_voxel_end = world_voxel_begin + Vector3i(OCCUPANCY_BRICK_SIZE, OCCUPANCY_BRICK_SIZE, OCCUPANCY_BRICK_SIZE);
		for (const KeyValue<RID, Volume> &entry : volumes) {
			Vector3i volume_voxel_origin;
			if (!_get_aligned_volume_origin(entry.value, world_occupancy.voxel_size, world_occupancy.origin, volume_voxel_origin)) {
				continue;
			}
			const bool volume_brick_aligned =
					volume_voxel_origin.x % OCCUPANCY_BRICK_SIZE == 0 &&
					volume_voxel_origin.y % OCCUPANCY_BRICK_SIZE == 0 &&
					volume_voxel_origin.z % OCCUPANCY_BRICK_SIZE == 0;
			if (volume_brick_aligned) {
				const Vector3i local_brick = world_brick_position - volume_voxel_origin / OCCUPANCY_BRICK_SIZE;
				if (local_brick.x < 0 || local_brick.y < 0 || local_brick.z < 0 ||
						local_brick.x >= entry.value.brick_dimensions.x ||
						local_brick.y >= entry.value.brick_dimensions.y ||
						local_brick.z >= entry.value.brick_dimensions.z) {
					continue;
				}
				const int local_brick_index = local_brick.x +
						local_brick.y * entry.value.brick_dimensions.x +
						local_brick.z * entry.value.brick_dimensions.x * entry.value.brick_dimensions.y;
				WorldBrickBits source_bits;
				if (_read_volume_brick_bits(entry.value, local_brick_index, source_bits)) {
					for (int word = 0; word < 8; word++) {
						composed_bits.bits[word] |= source_bits.bits[word];
					}
				}
				continue;
			}
			const Vector3i volume_voxel_end = volume_voxel_origin + entry.value.dimensions;
			const Vector3i overlap_begin(
					MAX(world_voxel_begin.x, volume_voxel_origin.x),
					MAX(world_voxel_begin.y, volume_voxel_origin.y),
					MAX(world_voxel_begin.z, volume_voxel_origin.z));
			const Vector3i overlap_end(
					MIN(world_voxel_end.x, volume_voxel_end.x),
					MIN(world_voxel_end.y, volume_voxel_end.y),
					MIN(world_voxel_end.z, volume_voxel_end.z));
			if (overlap_begin.x >= overlap_end.x || overlap_begin.y >= overlap_end.y || overlap_begin.z >= overlap_end.z) {
				continue;
			}
			for (int z = overlap_begin.z; z < overlap_end.z; z++) {
				for (int y = overlap_begin.y; y < overlap_end.y; y++) {
					for (int x = overlap_begin.x; x < overlap_end.x; x++) {
						const Vector3i world_voxel(x, y, z);
						if (!_volume_voxel_occupied(entry.value, world_voxel - volume_voxel_origin)) {
							continue;
						}
						const Vector3i world_local = world_voxel - world_voxel_begin;
						const int world_local_index = world_local.x + world_local.y * OCCUPANCY_BRICK_SIZE + world_local.z * OCCUPANCY_BRICK_SIZE * OCCUPANCY_BRICK_SIZE;
						composed_bits.bits[world_local_index >> 6] |= uint64_t(1) << (world_local_index & 63);
					}
				}
			}
		}

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
			_request_full_world_rebuild();
			return false;
		}
		if (probe_count > 48) {
			_request_full_world_rebuild();
			return false;
		}
		slot = insertion_slot;
		WorldDirectoryEntry &directory_entry = directory[slot];
		const bool reused_tombstone = !found && directory_entry.code == WORLD_DIRECTORY_TOMBSTONE_CODE;
		const uint32_t previous_code = found ? directory_entry.code : 0;
		const bool empty = _world_brick_bits_empty(composed_bits);
		const bool uniform = !empty && _world_brick_bits_uniform(composed_bits);

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
					_request_full_world_rebuild();
					return false;
				}
				world_occupancy.mixed_brick_count++;
			}
			_write_world_brick_bytes(world_occupancy_bricks_cpu, mixed_slot, composed_bits);
			if (rd->buffer_update(world_occupancy.brick_buffer, mixed_slot * OCCUPANCY_BRICK_BYTES, OCCUPANCY_BRICK_BYTES,
						world_occupancy_bricks_cpu.ptr() + mixed_slot * OCCUPANCY_BRICK_BYTES) != OK) {
				_request_full_world_rebuild();
				return false;
			}
			uploaded_bytes += OCCUPANCY_BRICK_BYTES;
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
			if (rd->buffer_update(world_occupancy.directory_buffer, slot * sizeof(WorldDirectoryEntry), sizeof(WorldDirectoryEntry), &directory_entry) != OK) {
				_request_full_world_rebuild();
				return false;
			}
			uploaded_bytes += sizeof(WorldDirectoryEntry);
		}

		world_occupancy.max_probe_count = MAX(world_occupancy.max_probe_count, probe_count);
	}

	world_occupancy.last_dirty_brick_count = dirty_bricks.size();
	world_occupancy.last_uploaded_bytes = uploaded_bytes;
	world_occupancy.last_dirty_bricks = dirty_bricks;
	world_occupancy.incremental_update_count++;
	world_occupancy.revision++;
	world_occupancy.last_incremental_revision = world_occupancy.revision;
	if (world_occupancy.tombstone_count > directory_capacity / 8 ||
			world_occupancy.occupied_brick_count + world_occupancy.tombstone_count > directory_capacity * 3 / 4) {
		_request_full_world_rebuild();
	}
	return true;
}

void VoxelForwardVolumeStorage::volume_set_on_render_thread(RID p_base, RID p_voxel_texture, RID p_brick_texture, RID p_neighbor_texture, RID p_palette_texture, RID p_material_texture, PackedByteArray p_occupancy_directory, PackedByteArray p_occupancy_bricks, Vector3i p_dimensions, Vector3i p_brick_dimensions, Vector3i p_atlas_brick_dimensions, Transform3D p_transform, float p_voxel_size, int p_occupied_brick_count, int p_neighbor_mask, int p_neighbor_diagonal_mask, Vector3i p_dirty_position, Vector3i p_dirty_size, int64_t p_revision) {
	VoxelForwardVolumeStorage *storage = get_singleton();
	if (storage == nullptr || !p_base.is_valid()) {
		return;
	}
	if (p_occupancy_directory.is_empty()) {
		return;
	}

	const Volume *previous_volume = storage->volumes.getptr(p_base);
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
	volume.voxel_texture = p_voxel_texture;
	volume.brick_texture = p_brick_texture;
	volume.neighbor_texture = p_neighbor_texture;
	volume.palette_texture = p_palette_texture;
	volume.material_texture = p_material_texture;
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
	volume.revision = uint64_t(p_revision);
	const bool incremental_change = previous_volume != nullptr ?
			storage->_queue_incremental_volume_change(*previous_volume, volume, p_dirty_position, p_dirty_size) :
			storage->_queue_incremental_volume_extent(volume);
	storage->volumes.insert(p_base, volume);
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
		storage->_request_full_world_rebuild();
	}
}

void VoxelForwardVolumeStorage::volume_neighbors_set_on_render_thread(RID p_base, RID p_neighbor_texture, int p_neighbor_mask, int p_neighbor_diagonal_mask) {
	VoxelForwardVolumeStorage *storage = get_singleton();
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
	const bool previous_extent_queued = storage->_queue_incremental_volume_extent(previous_volume);
	const bool transformed_extent_queued = previous_extent_queued && storage->_queue_incremental_volume_extent(transformed_volume);
	volume->transform = p_transform;
	if (!transformed_extent_queued) {
		storage->_request_full_world_rebuild();
	}
}

void VoxelForwardVolumeStorage::volume_remove_on_render_thread(RID p_base) {
	VoxelForwardVolumeStorage *storage = get_singleton();
	if (storage != nullptr) {
		const Volume *previous_volume = storage->volumes.getptr(p_base);
		if (previous_volume == nullptr) {
			return;
		}
		const bool incremental_change = storage->_queue_incremental_volume_extent(*previous_volume);
		if (previous_volume->batch_texture_index != INVALID_BATCH_TEXTURE_INDEX) {
			const uint32_t index = previous_volume->batch_texture_index;
			storage->batch_voxel_textures.write[index] = RID();
			storage->batch_brick_textures.write[index] = RID();
			storage->batch_neighbor_textures.write[index] = RID();
			storage->free_batch_texture_indices.push_back(index);
			storage->batch_texture_revision++;
		}
		storage->volumes.erase(p_base);
		if (!incremental_change) {
			storage->_request_full_world_rebuild();
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
	float world_voxel_size = 0.0f;
	Vector3 world_origin;
	uint32_t incompatible_volume_count = 0;
	for (const Volume &volume : build->volumes) {
		if (_get_axis_aligned_voxel_size(volume, world_voxel_size)) {
			world_origin = volume.transform.origin;
			break;
		}
	}
	auto get_world_brick = [&world_bricks](const Vector3i &p_position) -> WorldBrickBits * {
		return &world_bricks[p_position];
	};
	for (const Volume &volume : build->volumes) {
		Vector3i volume_voxel_origin;
		if (world_voxel_size == 0.0f || !_get_aligned_volume_origin(volume, world_voxel_size, world_origin, volume_voxel_origin)) {
			incompatible_volume_count++;
			continue;
		}

		const int expected_directory_bytes = volume.brick_dimensions.x * volume.brick_dimensions.y * volume.brick_dimensions.z * 4;
		if (volume.occupancy_directory_cpu.size() < expected_directory_bytes) {
			incompatible_volume_count++;
			continue;
		}
		const bool brick_aligned =
				volume_voxel_origin.x % OCCUPANCY_BRICK_SIZE == 0 &&
				volume_voxel_origin.y % OCCUPANCY_BRICK_SIZE == 0 &&
				volume_voxel_origin.z % OCCUPANCY_BRICK_SIZE == 0;
		const Vector3i world_brick_origin = brick_aligned ? volume_voxel_origin / OCCUPANCY_BRICK_SIZE : Vector3i();
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

			// An integer-shifted 8x8x8 source brick overlaps at most two bricks
			// per axis. Accumulate those eight possible masks locally so sparse
			// world-map hashing happens per destination brick, not per voxel.
			const Vector3i local_voxel_start = local_brick * OCCUPANCY_BRICK_SIZE;
			const Vector3i source_world_start = volume_voxel_origin + local_voxel_start;
			const Vector3i first_world_brick(
					_floor_divide_by_brick_size(source_world_start.x),
					_floor_divide_by_brick_size(source_world_start.y),
					_floor_divide_by_brick_size(source_world_start.z));
			const Vector3i first_world_local = source_world_start - first_world_brick * OCCUPANCY_BRICK_SIZE;
			WorldBrickBits split_occupancy[8] = {};
			for (int local_index = 0; local_index < OCCUPANCY_BRICK_SIZE * OCCUPANCY_BRICK_SIZE * OCCUPANCY_BRICK_SIZE; local_index++) {
				if ((source_occupancy.bits[local_index >> 6] & (uint64_t(1) << (local_index & 63))) == 0) {
					continue;
				}
				const Vector3i local_voxel(local_index & 7, (local_index >> 3) & 7, local_index >> 6);
				const Vector3i unwrapped_local = first_world_local + local_voxel;
				const Vector3i destination_offset(unwrapped_local.x >> 3, unwrapped_local.y >> 3, unwrapped_local.z >> 3);
				const int destination_index = destination_offset.x | (destination_offset.y << 1) | (destination_offset.z << 2);
				const Vector3i world_local(unwrapped_local.x & 7, unwrapped_local.y & 7, unwrapped_local.z & 7);
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
	build->build_usec = OS::get_singleton()->get_ticks_usec() - build_started_usec;
}

void VoxelForwardVolumeStorage::_finish_world_occupancy_build() {
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
	// A transform, stream event, or voxel edit may arrive while the immutable
	// worker snapshot is being built. Never publish that stale snapshot for a
	// frame; discard it and let update_world_occupancy schedule the newer state.
	if (world_occupancy_dirty) {
		print_verbose(vformat("Voxel Forward: discarded stale world occupancy build after %.2f ms (%d/%d volumes brick-aligned).", double(build->build_usec) / 1000.0, build->brick_aligned_volume_count, build->volumes.size()));
		memdelete(build);
		return;
	}
	const uint64_t upload_started_usec = OS::get_singleton()->get_ticks_usec();
	RD *rd = RD::get_singleton();
	if (rd == nullptr) {
		world_occupancy_dirty = true;
		memdelete(build);
		return;
	}
	const bool can_update_in_place = world_occupancy.directory_buffer.is_valid() && world_occupancy.brick_buffer.is_valid() &&
			world_occupancy_directory_capacity_bytes == uint32_t(build->directory_bytes.size()) &&
			world_occupancy_brick_capacity_bytes >= uint32_t(build->brick_bytes.size());
	if (can_update_in_place) {
		const Error directory_error = _update_changed_buffer_units(rd, world_occupancy.directory_buffer, world_occupancy_directory_cpu, build->directory_bytes, sizeof(WorldDirectoryEntry));
		const Error brick_error = _update_changed_buffer_units(rd, world_occupancy.brick_buffer, world_occupancy_bricks_cpu, build->brick_bytes, OCCUPANCY_BRICK_BYTES);
		if (directory_error == OK && brick_error == OK) {
			world_occupancy_directory_cpu = build->directory_bytes;
			world_occupancy_bricks_cpu = build->brick_bytes;
			world_occupancy_free_mixed_slots.clear();
			world_occupancy.voxel_size = build->voxel_size;
			world_occupancy.origin = build->origin;
			world_occupancy.directory_mask = build->directory_mask;
			world_occupancy.occupied_brick_count = build->occupied_brick_count;
			world_occupancy.mixed_brick_count = build->mixed_brick_count;
			world_occupancy.max_probe_count = build->max_probe_count;
			world_occupancy.tombstone_count = 0;
			world_occupancy.incompatible_volume_count = build->incompatible_volume_count;
			world_occupancy.last_dirty_brick_count = build->occupied_brick_count;
			world_occupancy.last_uploaded_bytes = build->directory_bytes.size() + build->brick_bytes.size();
			world_occupancy.last_dirty_bricks.clear();
			world_occupancy.last_incremental_revision = 0;
			world_occupancy.full_rebuild_count++;
			world_occupancy.revision++;
			print_verbose(vformat("Voxel Forward: world occupancy built in %.2f ms and updated on GPU in %.2f ms (%d bricks, %d/%d volumes brick-aligned).", double(build->build_usec) / 1000.0, double(OS::get_singleton()->get_ticks_usec() - upload_started_usec) / 1000.0, build->occupied_brick_count, build->brick_aligned_volume_count, build->volumes.size()));
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
	world_occupancy_gpu_bytes = uint64_t(directory_capacity_bytes) + uint64_t(brick_capacity);
	occupancy_gpu_bytes += world_occupancy_gpu_bytes;
	world_occupancy.voxel_size = build->voxel_size;
	world_occupancy.origin = build->origin;
	world_occupancy.directory_mask = build->directory_mask;
	world_occupancy.occupied_brick_count = build->occupied_brick_count;
	world_occupancy.mixed_brick_count = build->mixed_brick_count;
	world_occupancy.max_probe_count = build->max_probe_count;
	world_occupancy.tombstone_count = 0;
	world_occupancy.incompatible_volume_count = build->incompatible_volume_count;
	world_occupancy.last_dirty_brick_count = build->occupied_brick_count;
	world_occupancy.last_uploaded_bytes = build->directory_bytes.size() + build->brick_bytes.size();
	world_occupancy.last_dirty_bricks.clear();
	world_occupancy.last_incremental_revision = 0;
	world_occupancy.full_rebuild_count++;
	world_occupancy.revision++;
	print_verbose(vformat("Voxel Forward: world occupancy built in %.2f ms and uploaded to GPU in %.2f ms (%d bricks, %d/%d volumes brick-aligned).", double(build->build_usec) / 1000.0, double(OS::get_singleton()->get_ticks_usec() - upload_started_usec) / 1000.0, build->occupied_brick_count, build->brick_aligned_volume_count, build->volumes.size()));
	memdelete(build);
}

void VoxelForwardVolumeStorage::update_world_occupancy(bool p_enabled) {
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
		return;
	}

	_finish_world_occupancy_build();
	if (world_occupancy_build_task == WorkerThreadPool::INVALID_TASK_ID && !world_occupancy_dirty && !pending_incremental_world_bricks.is_empty()) {
		_apply_incremental_world_updates();
		return;
	}
	if (world_occupancy_build_task != WorkerThreadPool::INVALID_TASK_ID || !world_occupancy_dirty) {
		return;
	}
	// Streaming and destruction can enqueue several mutations close together.
	// Wait for a quiet pair of frames, then snapshot immutable packed arrays and
	// build the large sparse table on a low-priority worker instead of blocking
	// the render thread for hundreds of milliseconds.
	if (world_occupancy_quiet_frames < 2) {
		world_occupancy_quiet_frames++;
		return;
	}
	world_occupancy_dirty = false;
	world_occupancy_quiet_frames = 0;
	world_occupancy_build = memnew(WorldOccupancyBuild);
	world_occupancy_build->minimum_directory_capacity = world_occupancy_directory_capacity_bytes / sizeof(WorldDirectoryEntry);
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

VoxelForwardVolumeStorage::VoxelForwardVolumeStorage() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = this;
	batch_voxel_textures.resize(MAX_BATCH_TEXTURES);
	batch_brick_textures.resize(MAX_BATCH_TEXTURES);
	batch_neighbor_textures.resize(MAX_BATCH_TEXTURES);
	free_batch_texture_indices.resize(MAX_BATCH_TEXTURES);
	for (uint32_t i = 0; i < MAX_BATCH_TEXTURES; i++) {
		free_batch_texture_indices.write[i] = MAX_BATCH_TEXTURES - 1u - i;
	}
}

VoxelForwardVolumeStorage::~VoxelForwardVolumeStorage() {
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
