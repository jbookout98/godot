#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0) uniform sampler3D source_grid;
layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image3D destination_grid;

layout(set = 0, binding = 2, std430) readonly buffer WorldDirectory {
	uvec4 entries[];
}
world_directory;

layout(set = 0, binding = 3, std430) readonly buffer MixedBricks {
	uint words[];
}
mixed_bricks;

layout(push_constant, std430) uniform Params {
	vec4 world_origin_voxel_size;
	vec4 grid_origin_cell_size;
	ivec4 grid_directory;
	vec4 propagation;
	ivec4 dispatch_origin;
}
params;

uint brick_hash(ivec3 position) {
	return uint(position.x) * 73856093u ^ uint(position.y) * 19349663u ^ uint(position.z) * 83492791u;
}

uint find_brick(ivec3 position) {
	uint slot = brick_hash(position) & uint(params.grid_directory.y);
	for (uint probe = 0u; probe < 64u; probe++) {
		uvec4 entry = world_directory.entries[slot];
		if (entry.w == 0u) {
			return 0u;
		}
		if (ivec3(entry.xyz) == position) {
			return entry.w;
		}
		slot = (slot + 1u) & uint(params.grid_directory.y);
	}
	return 0u;
}

bool world_occupied(vec3 world_position) {
	vec3 voxel_position_f = (world_position - params.world_origin_voxel_size.xyz) / params.world_origin_voxel_size.w;
	ivec3 voxel_position = ivec3(floor(voxel_position_f));
	ivec3 brick_position = ivec3(floor(vec3(voxel_position) / 8.0));
	uint code = find_brick(brick_position);
	if (code == 0u) {
		return false;
	}
	if (code == 1u) {
		return true;
	}
	ivec3 local_voxel = voxel_position - brick_position * 8;
	uint local_index = uint(local_voxel.x + local_voxel.y * 8 + local_voxel.z * 64);
	uint word = mixed_bricks.words[(code - 2u) * 16u + (local_index >> 5u)];
	return (word & (1u << (local_index & 31u))) != 0u;
}

bool connection_blocked(vec3 from, vec3 to) {
	ivec3 cached_brick_position = ivec3(0);
	uint cached_brick_code = 0u;
	bool brick_cached = false;
	for (int sample_index = 1; sample_index <= 3; sample_index++) {
		float weight = float(sample_index) * 0.25;
		vec3 world_position = mix(from, to, weight);
		ivec3 voxel_position = ivec3(floor((world_position - params.world_origin_voxel_size.xyz) / params.world_origin_voxel_size.w));
		ivec3 brick_position = ivec3(floor(vec3(voxel_position) / 8.0));
		if (!brick_cached || any(notEqual(brick_position, cached_brick_position))) {
			cached_brick_position = brick_position;
			cached_brick_code = find_brick(brick_position);
			brick_cached = true;
		}
		if (cached_brick_code == 1u) {
			return true;
		}
		if (cached_brick_code >= 2u) {
			ivec3 local_voxel = voxel_position - cached_brick_position * 8;
			uint local_index = uint(local_voxel.x + local_voxel.y * 8 + local_voxel.z * 64);
			uint word = mixed_bricks.words[(cached_brick_code - 2u) * 16u + (local_index >> 5u)];
			if ((word & (1u << (local_index & 31u))) != 0u) {
				return true;
			}
		}
	}
	return false;
}

void main() {
	ivec3 cell = params.dispatch_origin.xyz + ivec3(gl_GlobalInvocationID.xyz);
	int resolution = params.grid_directory.x;
	if (any(greaterThanEqual(cell, ivec3(resolution)))) {
		return;
	}
	vec3 cell_center = params.grid_origin_cell_size.xyz + (vec3(cell) + vec3(0.5)) * params.grid_origin_cell_size.w;
	if (world_occupied(cell_center)) {
		imageStore(destination_grid, cell, vec4(0.0));
		return;
	}

	vec3 current = texelFetch(source_grid, cell, 0).rgb;
	const ivec3 offsets[6] = ivec3[6](
		ivec3(1, 0, 0), ivec3(-1, 0, 0),
		ivec3(0, 1, 0), ivec3(0, -1, 0),
		ivec3(0, 0, 1), ivec3(0, 0, -1));
	vec3 neighbor_sum = vec3(0.0);
	float neighbor_count = 0.0;
	for (int index = 0; index < 6; index++) {
		ivec3 neighbor = cell + offsets[index];
		if (any(lessThan(neighbor, ivec3(0))) || any(greaterThanEqual(neighbor, ivec3(resolution)))) {
			continue;
		}
		vec3 neighbor_center = params.grid_origin_cell_size.xyz + (vec3(neighbor) + vec3(0.5)) * params.grid_origin_cell_size.w;
		if (connection_blocked(cell_center, neighbor_center)) {
			continue;
		}
		neighbor_sum += texelFetch(source_grid, neighbor, 0).rgb;
		neighbor_count += 1.0;
	}
	vec3 propagated = neighbor_count > 0.0 ? neighbor_sum / neighbor_count * params.propagation.x : vec3(0.0);
	imageStore(destination_grid, cell, vec4(max(current, propagated), 1.0));
}
