#include "voxel_brick_storage.h"

#include "core/error/error_macros.h"

#include <cstring>

namespace {

void _append_u32(PackedByteArray &r_data, uint32_t p_value) {
	for (int shift = 0; shift < 32; shift += 8) {
		r_data.push_back(uint8_t((p_value >> shift) & 0xFF));
	}
}

bool _read_u32(const PackedByteArray &p_data, int &r_cursor, uint32_t &r_value) {
	if (r_cursor + 4 > p_data.size()) {
		return false;
	}
	r_value = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		r_value |= uint32_t(p_data[r_cursor++]) << shift;
	}
	return true;
}

} // namespace

int VoxelBrickStorage::_get_brick_index(const Vector3i &p_brick) const {
	return p_brick.x + p_brick.y * brick_dimensions.x +
			p_brick.z * brick_dimensions.x * brick_dimensions.y;
}

int VoxelBrickStorage::_get_local_index(const Vector3i &p_position) const {
	return (p_position.x & (BRICK_SIZE - 1)) +
			(p_position.y & (BRICK_SIZE - 1)) * BRICK_SIZE +
			(p_position.z & (BRICK_SIZE - 1)) * BRICK_SIZE * BRICK_SIZE;
}

int VoxelBrickStorage::_get_valid_voxel_count(const Vector3i &p_brick) const {
	const Vector3i start = p_brick * BRICK_SIZE;
	const Vector3i size = (dimensions - start).clamp(Vector3i(), Vector3i(BRICK_SIZE, BRICK_SIZE, BRICK_SIZE));
	return size.x * size.y * size.z;
}

void VoxelBrickStorage::reset(const Vector3i &p_dimensions) {
	ERR_FAIL_COND(p_dimensions.x <= 0 || p_dimensions.y <= 0 || p_dimensions.z <= 0);
	dimensions = p_dimensions;
	brick_dimensions = Vector3i(
			(dimensions.x + BRICK_SIZE - 1) / BRICK_SIZE,
			(dimensions.y + BRICK_SIZE - 1) / BRICK_SIZE,
			(dimensions.z + BRICK_SIZE - 1) / BRICK_SIZE);
	bricks.clear();
	bricks.resize(brick_dimensions.x * brick_dimensions.y * brick_dimensions.z);
	revision++;
}

bool VoxelBrickStorage::is_inside(const Vector3i &p_position) const {
	return p_position.x >= 0 && p_position.y >= 0 && p_position.z >= 0 &&
			p_position.x < dimensions.x && p_position.y < dimensions.y && p_position.z < dimensions.z;
}

int VoxelBrickStorage::position_to_brick_index(const Vector3i &p_position) const {
	ERR_FAIL_COND_V(!is_inside(p_position), -1);
	return _get_brick_index(p_position / BRICK_SIZE);
}

Vector3i VoxelBrickStorage::brick_index_to_position(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, bricks.size(), Vector3i());
	const int plane = brick_dimensions.x * brick_dimensions.y;
	return Vector3i(
			p_index % brick_dimensions.x,
			(p_index / brick_dimensions.x) % brick_dimensions.y,
			p_index / plane);
}

uint8_t VoxelBrickStorage::get_voxel(const Vector3i &p_position) const {
	if (!is_inside(p_position)) {
		return 0;
	}
	const Brick &brick = bricks[position_to_brick_index(p_position)];
	if (brick.type == BRICK_EMPTY) {
		return 0;
	}
	if (brick.type == BRICK_UNIFORM) {
		return brick.uniform_value;
	}
	return brick.mixed_values[_get_local_index(p_position)];
}

void VoxelBrickStorage::_normalize_brick(int p_brick_index) {
	Brick &brick = bricks.write[p_brick_index];
	if (brick.type != BRICK_MIXED) {
		return;
	}
	if (brick.occupied_count == 0) {
		brick.type = BRICK_EMPTY;
		brick.uniform_value = 0;
		brick.mixed_values.clear();
		brick.coarse_occupancy_4 = 0;
		return;
	}

	const Vector3i brick_position = brick_index_to_position(p_brick_index);
	const int valid_count = _get_valid_voxel_count(brick_position);
	if (brick.occupied_count != valid_count) {
		return;
	}
	const Vector3i start = brick_position * BRICK_SIZE;
	const Vector3i valid_size = (dimensions - start).clamp(Vector3i(), Vector3i(BRICK_SIZE, BRICK_SIZE, BRICK_SIZE));
	uint8_t uniform_value = 0;
	bool first = true;
	for (int z = 0; z < valid_size.z; z++) {
		for (int y = 0; y < valid_size.y; y++) {
			for (int x = 0; x < valid_size.x; x++) {
				const int local_index = x + y * BRICK_SIZE + z * BRICK_SIZE * BRICK_SIZE;
				const uint8_t value = brick.mixed_values[local_index];
				if (first) {
					uniform_value = value;
					first = false;
				} else if (value != uniform_value) {
					return;
				}
			}
		}
	}
	brick.type = BRICK_UNIFORM;
	brick.uniform_value = uniform_value;
	brick.mixed_values.clear();
	brick.coarse_occupancy_4 = 0xFF;
}

void VoxelBrickStorage::_rebuild_coarse_occupancy_4(int p_brick_index) {
	Brick &brick = bricks.write[p_brick_index];
	if (brick.type == BRICK_EMPTY) {
		brick.coarse_occupancy_4 = 0;
		return;
	}
	if (brick.type == BRICK_UNIFORM) {
		brick.coarse_occupancy_4 = brick.uniform_value != 0 ? 0xFF : 0;
		return;
	}
	uint8_t mask = 0;
	for (int local = 0; local < BRICK_VOXEL_COUNT; local++) {
		if (brick.mixed_values[local] == 0) {
			continue;
		}
		const int x = local % BRICK_SIZE;
		const int y = (local / BRICK_SIZE) % BRICK_SIZE;
		const int z = local / (BRICK_SIZE * BRICK_SIZE);
		mask |= uint8_t(1 << ((x >> 2) | ((y >> 2) << 1) | ((z >> 2) << 2)));
	}
	brick.coarse_occupancy_4 = mask;
}

bool VoxelBrickStorage::set_voxel(const Vector3i &p_position, uint8_t p_value) {
	ERR_FAIL_COND_V(!is_inside(p_position), false);
	const int brick_index = position_to_brick_index(p_position);
	Brick &brick = bricks.write[brick_index];
	const uint8_t old_value = get_voxel(p_position);
	if (old_value == p_value) {
		return false;
	}

	if (brick.type != BRICK_MIXED) {
		const uint8_t fill_value = brick.type == BRICK_UNIFORM ? brick.uniform_value : 0;
		brick.mixed_values.resize(BRICK_VOXEL_COUNT);
		brick.mixed_values.fill(fill_value);
		brick.type = BRICK_MIXED;
	}
	brick.mixed_values.set(_get_local_index(p_position), p_value);
	if (old_value == 0 && p_value != 0) {
		brick.occupied_count++;
	} else if (old_value != 0 && p_value == 0) {
		brick.occupied_count--;
	}
	brick.dirty_flags |= DIRTY_ALL;
	brick.revision = ++revision;
	_normalize_brick(brick_index);
	_rebuild_coarse_occupancy_4(brick_index);
	return true;
}

int VoxelBrickStorage::fill_region(const Vector3i &p_position, const Vector3i &p_size, uint8_t p_value) {
	const Vector3i from(MAX(0, p_position.x), MAX(0, p_position.y), MAX(0, p_position.z));
	const Vector3i requested_end = p_position + p_size;
	const Vector3i to(MIN(dimensions.x, requested_end.x), MIN(dimensions.y, requested_end.y), MIN(dimensions.z, requested_end.z));
	if (from.x >= to.x || from.y >= to.y || from.z >= to.z) {
		return 0;
	}
	const Vector3i brick_from = from / BRICK_SIZE;
	const Vector3i brick_to = (to - Vector3i(1, 1, 1)) / BRICK_SIZE;
	int changed = 0;
	for (int bz = brick_from.z; bz <= brick_to.z; bz++) {
		for (int by = brick_from.y; by <= brick_to.y; by++) {
			for (int bx = brick_from.x; bx <= brick_to.x; bx++) {
				const Vector3i brick_position(bx, by, bz);
				const int brick_index = _get_brick_index(brick_position);
				const Vector3i start = brick_position * BRICK_SIZE;
				const Vector3i valid_size = (dimensions - start).clamp(Vector3i(), Vector3i(BRICK_SIZE, BRICK_SIZE, BRICK_SIZE));
				const Vector3i valid_end = start + valid_size;
				const bool complete = from.x <= start.x && from.y <= start.y && from.z <= start.z &&
						to.x >= valid_end.x && to.y >= valid_end.y && to.z >= valid_end.z;
				if (complete) {
					Brick &brick = bricks.write[brick_index];
					const int valid_count = valid_size.x * valid_size.y * valid_size.z;
					if (brick.type == BRICK_EMPTY) {
						changed += p_value == 0 ? 0 : valid_count;
					} else if (brick.type == BRICK_UNIFORM) {
						changed += brick.uniform_value == p_value ? 0 : valid_count;
					} else {
						for (int z = 0; z < valid_size.z; z++) {
							for (int y = 0; y < valid_size.y; y++) {
								for (int x = 0; x < valid_size.x; x++) {
									changed += brick.mixed_values[x + y * BRICK_SIZE + z * BRICK_SIZE * BRICK_SIZE] != p_value ? 1 : 0;
								}
							}
						}
					}
					if ((brick.type == BRICK_EMPTY && p_value == 0) ||
							(brick.type == BRICK_UNIFORM && brick.uniform_value == p_value)) {
						continue;
					}
					brick = Brick();
					if (p_value != 0) {
						brick.type = BRICK_UNIFORM;
						brick.uniform_value = p_value;
						brick.occupied_count = valid_count;
						brick.coarse_occupancy_4 = 0xFF;
					}
					brick.dirty_flags = DIRTY_ALL;
					brick.revision = ++revision;
					continue;
				}

				const Vector3i partial_from(MAX(from.x, start.x), MAX(from.y, start.y), MAX(from.z, start.z));
				const Vector3i partial_to(MIN(to.x, valid_end.x), MIN(to.y, valid_end.y), MIN(to.z, valid_end.z));
				for (int z = partial_from.z; z < partial_to.z; z++) {
					for (int y = partial_from.y; y < partial_to.y; y++) {
						for (int x = partial_from.x; x < partial_to.x; x++) {
							changed += set_voxel(Vector3i(x, y, z), p_value) ? 1 : 0;
						}
					}
				}
			}
		}
	}
	return changed;
}

void VoxelBrickStorage::import_dense(const PackedByteArray &p_voxels, const Vector3i &p_dimensions) {
	ERR_FAIL_COND(p_voxels.size() != p_dimensions.x * p_dimensions.y * p_dimensions.z);
	reset(p_dimensions);
	for (int brick_index = 0; brick_index < bricks.size(); brick_index++) {
		const Vector3i brick_position = brick_index_to_position(brick_index);
		const Vector3i start = brick_position * BRICK_SIZE;
		const Vector3i valid_size = (dimensions - start).clamp(Vector3i(), Vector3i(BRICK_SIZE, BRICK_SIZE, BRICK_SIZE));
		Brick &brick = bricks.write[brick_index];
		brick.type = BRICK_MIXED;
		brick.mixed_values.resize(BRICK_VOXEL_COUNT);
		brick.mixed_values.fill(0);
		for (int z = 0; z < valid_size.z; z++) {
			for (int y = 0; y < valid_size.y; y++) {
				for (int x = 0; x < valid_size.x; x++) {
					const Vector3i position = start + Vector3i(x, y, z);
					const int dense_index = position.x + position.y * dimensions.x +
							position.z * dimensions.x * dimensions.y;
					const uint8_t value = p_voxels[dense_index];
					brick.mixed_values.set(x + y * BRICK_SIZE + z * BRICK_SIZE * BRICK_SIZE, value);
					brick.occupied_count += value != 0 ? 1 : 0;
				}
			}
		}
		brick.dirty_flags = DIRTY_ALL;
		brick.revision = revision;
		_normalize_brick(brick_index);
		_rebuild_coarse_occupancy_4(brick_index);
	}
	revision++;
}

PackedByteArray VoxelBrickStorage::export_dense() const {
	PackedByteArray dense;
	dense.resize(dimensions.x * dimensions.y * dimensions.z);
	dense.fill(0);
	uint8_t *dense_write = dense.ptrw();
	for (int brick_index = 0; brick_index < bricks.size(); brick_index++) {
		const Brick &brick = bricks[brick_index];
		if (brick.type == BRICK_EMPTY) {
			continue;
		}
		const Vector3i start = brick_index_to_position(brick_index) * BRICK_SIZE;
		const Vector3i valid_size = (dimensions - start).clamp(Vector3i(), Vector3i(BRICK_SIZE, BRICK_SIZE, BRICK_SIZE));
		const uint8_t *mixed_read = brick.type == BRICK_MIXED ? brick.mixed_values.ptr() : nullptr;
		for (int z = 0; z < valid_size.z; z++) {
			for (int y = 0; y < valid_size.y; y++) {
				for (int x = 0; x < valid_size.x; x++) {
					const int dense_index = (start.x + x) + (start.y + y) * dimensions.x + (start.z + z) * dimensions.x * dimensions.y;
					const int local_index = x + y * BRICK_SIZE + z * BRICK_SIZE * BRICK_SIZE;
					dense_write[dense_index] = mixed_read != nullptr ? mixed_read[local_index] : brick.uniform_value;
				}
			}
		}
	}
	return dense;
}

PackedByteArray VoxelBrickStorage::serialize_sparse() const {
	PackedByteArray result;
	result.push_back('V');
	result.push_back('B');
	result.push_back('S');
	result.push_back('1');
	uint32_t record_count = 0;
	for (const Brick &brick : bricks) {
		record_count += brick.type != BRICK_EMPTY ? 1 : 0;
	}
	_append_u32(result, record_count);
	for (int index = 0; index < bricks.size(); index++) {
		const Brick &brick = bricks[index];
		if (brick.type == BRICK_EMPTY) {
			continue;
		}
		_append_u32(result, index);
		result.push_back(uint8_t(brick.type));
		if (brick.type == BRICK_UNIFORM) {
			result.push_back(brick.uniform_value);
		} else {
			result.append_array(brick.mixed_values);
		}
	}
	return result;
}

bool VoxelBrickStorage::deserialize_sparse(const PackedByteArray &p_data, const Vector3i &p_dimensions) {
	if (p_data.size() < 8 || p_data[0] != 'V' || p_data[1] != 'B' || p_data[2] != 'S' || p_data[3] != '1') {
		return false;
	}
	reset(p_dimensions);
	int cursor = 4;
	uint32_t record_count = 0;
	if (!_read_u32(p_data, cursor, record_count)) {
		return false;
	}
	PackedByteArray seen;
	seen.resize(bricks.size());
	seen.fill(0);
	for (uint32_t record = 0; record < record_count; record++) {
		uint32_t brick_index = 0;
		if (!_read_u32(p_data, cursor, brick_index) || brick_index >= uint32_t(bricks.size()) || cursor >= p_data.size() || seen[brick_index] != 0) {
			return false;
		}
		seen.set(brick_index, 1);
		Brick &brick = bricks.write[brick_index];
		brick = Brick();
		brick.type = BrickType(p_data[cursor++]);
		if (brick.type == BRICK_UNIFORM) {
			if (cursor >= p_data.size() || p_data[cursor] == 0) {
				return false;
			}
			brick.uniform_value = p_data[cursor++];
			brick.occupied_count = _get_valid_voxel_count(brick_index_to_position(brick_index));
			brick.coarse_occupancy_4 = 0xFF;
		} else if (brick.type == BRICK_MIXED) {
			if (cursor + BRICK_VOXEL_COUNT > p_data.size()) {
				return false;
			}
			brick.mixed_values.resize(BRICK_VOXEL_COUNT);
			uint8_t *mixed_write = brick.mixed_values.ptrw();
			const Vector3i brick_position = brick_index_to_position(brick_index);
			const Vector3i start = brick_position * BRICK_SIZE;
			const Vector3i valid_size = (dimensions - start).clamp(Vector3i(), Vector3i(BRICK_SIZE, BRICK_SIZE, BRICK_SIZE));
			for (int i = 0; i < BRICK_VOXEL_COUNT; i++) {
				uint8_t value = p_data[cursor++];
				const int x = i % BRICK_SIZE;
				const int y = (i / BRICK_SIZE) % BRICK_SIZE;
				const int z = i / (BRICK_SIZE * BRICK_SIZE);
				if (x >= valid_size.x || y >= valid_size.y || z >= valid_size.z) {
					value = 0;
				}
				mixed_write[i] = value;
				brick.occupied_count += value != 0 ? 1 : 0;
			}
			_rebuild_coarse_occupancy_4(brick_index);
		} else {
			return false;
		}
		brick.dirty_flags = DIRTY_ALL;
		brick.revision = revision;
	}
	revision++;
	return true;
}

void VoxelBrickStorage::get_non_empty_brick_indices(Vector<int> &r_indices) const {
	r_indices.clear();
	for (int index = 0; index < bricks.size(); index++) {
		if (bricks[index].type != BRICK_EMPTY) {
			r_indices.push_back(index);
		}
	}
}

void VoxelBrickStorage::clear_dirty_flags(uint8_t p_flags) {
	for (Brick &brick : bricks) {
		brick.dirty_flags &= ~p_flags;
	}
}
