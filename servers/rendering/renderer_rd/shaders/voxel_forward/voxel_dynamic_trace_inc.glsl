// Shared exact traversal for voxel volumes which cannot be represented by the
// axis-aligned world occupancy hash. All distances remain in world units even
// while the ray is transformed into a volume's local voxel coordinates.

const float DYNAMIC_TRACE_EPSILON = 0.000001;
const float DYNAMIC_TRACE_HUGE = 1e30;
const uint DYNAMIC_NODE_LEAF = 1u;
const int DYNAMIC_BVH_STACK_SIZE = 32;

struct DynamicVoxelVolume {
	vec4 world_to_voxel[3];
	vec4 normal_to_world[3];
	vec4 bounds_min;
	vec4 bounds_max;
	uvec4 dimensions_directory_offset;
	uvec4 brick_dimensions_brick_offset;
	uvec4 storage;
};

struct DynamicVoxelBvhNode {
	vec4 bounds_min;
	vec4 bounds_max;
	uvec4 children;
};

layout(set = 0, binding = 22, std430) readonly buffer DynamicVoxelVolumes {
	uvec4 state;
	DynamicVoxelVolume values[];
}
dynamic_voxel_volumes;
layout(set = 0, binding = 23, std430) readonly buffer DynamicVoxelBvh {
	uvec4 state;
	DynamicVoxelBvhNode values[];
}
dynamic_voxel_bvh;
layout(set = 0, binding = 24, std430) readonly buffer DynamicVoxelDirectory {
	uint words[];
}
dynamic_voxel_directory;
layout(set = 0, binding = 25, std430) readonly buffer DynamicVoxelBricks {
	uint words[];
}
dynamic_voxel_bricks;

vec3 dynamic_transform_point(DynamicVoxelVolume volume, vec3 point) {
	return vec3(
			dot(volume.world_to_voxel[0].xyz, point) + volume.world_to_voxel[0].w,
			dot(volume.world_to_voxel[1].xyz, point) + volume.world_to_voxel[1].w,
			dot(volume.world_to_voxel[2].xyz, point) + volume.world_to_voxel[2].w);
}

vec3 dynamic_transform_direction(DynamicVoxelVolume volume, vec3 direction) {
	return vec3(
			dot(volume.world_to_voxel[0].xyz, direction),
			dot(volume.world_to_voxel[1].xyz, direction),
			dot(volume.world_to_voxel[2].xyz, direction));
}

vec3 dynamic_transform_normal(DynamicVoxelVolume volume, vec3 normal) {
	return normalize(vec3(
			dot(volume.normal_to_world[0].xyz, normal),
			dot(volume.normal_to_world[1].xyz, normal),
			dot(volume.normal_to_world[2].xyz, normal)));
}

vec3 dynamic_safe_inverse(vec3 direction) {
	return vec3(
			abs(direction.x) > DYNAMIC_TRACE_EPSILON ? 1.0 / direction.x : DYNAMIC_TRACE_HUGE,
			abs(direction.y) > DYNAMIC_TRACE_EPSILON ? 1.0 / direction.y : DYNAMIC_TRACE_HUGE,
			abs(direction.z) > DYNAMIC_TRACE_EPSILON ? 1.0 / direction.z : DYNAMIC_TRACE_HUGE);
}

bool dynamic_intersect_aabb(vec3 origin, vec3 direction, vec3 bounds_min, vec3 bounds_max, float maximum_distance, out float enter_t, out float exit_t) {
	vec3 inverse_direction = dynamic_safe_inverse(direction);
	vec3 t0 = (bounds_min - origin) * inverse_direction;
	vec3 t1 = (bounds_max - origin) * inverse_direction;
	vec3 near_t = min(t0, t1);
	vec3 far_t = max(t0, t1);
	enter_t = max(0.0, max(near_t.x, max(near_t.y, near_t.z)));
	exit_t = min(maximum_distance, min(far_t.x, min(far_t.y, far_t.z)));
	return exit_t >= enter_t;
}

bool dynamic_volume_occupied(DynamicVoxelVolume volume, ivec3 cell) {
	if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(volume.dimensions_directory_offset.xyz)))) {
		return false;
	}
	ivec3 brick = cell / 8;
	uint directory_index = uint(brick.x) + uint(brick.y) * volume.brick_dimensions_brick_offset.x + uint(brick.z) * volume.brick_dimensions_brick_offset.x * volume.brick_dimensions_brick_offset.y;
	if (directory_index >= volume.storage.x) {
		return false;
	}
	uint code = dynamic_voxel_directory.words[volume.dimensions_directory_offset.w + directory_index];
	if (code == 0u) {
		return false;
	}
	if (code == 1u) {
		return true;
	}
	ivec3 local_voxel = cell - brick * 8;
	uint local_index = uint(local_voxel.x + local_voxel.y * 8 + local_voxel.z * 64);
	uint word_index = (code - 2u) * 16u + (local_index >> 5u);
	if (word_index >= volume.storage.y) {
		return false;
	}
	uint word = dynamic_voxel_bricks.words[volume.brick_dimensions_brick_offset.w + word_index];
	return (word & (1u << (local_index & 31u))) != 0u;
}

int dynamic_minimum_axis(vec3 value) {
	if (value.x <= value.y && value.x <= value.z) {
		return 0;
	}
	return value.y <= value.z ? 1 : 2;
}

bool dynamic_trace_volume(uint volume_index, vec3 start_world, vec3 direction_world, float maximum_distance,
		out vec3 hit_position, out vec3 hit_normal, out float hit_distance, out bool hit_backface) {
	DynamicVoxelVolume volume = dynamic_voxel_volumes.values[volume_index];
	float world_enter;
	float world_exit;
	if (!dynamic_intersect_aabb(start_world, direction_world, volume.bounds_min.xyz, volume.bounds_max.xyz, maximum_distance, world_enter, world_exit)) {
		return false;
	}

	vec3 local_origin = dynamic_transform_point(volume, start_world);
	vec3 local_direction = dynamic_transform_direction(volume, direction_world);
	vec3 local_bounds_max = vec3(volume.dimensions_directory_offset.xyz);
	float enter_t;
	float exit_t;
	if (!dynamic_intersect_aabb(local_origin, local_direction, vec3(0.0), local_bounds_max, maximum_distance, enter_t, exit_t)) {
		return false;
	}

	float epsilon_t = max(DYNAMIC_TRACE_EPSILON, 0.0001 / max(length(local_direction), DYNAMIC_TRACE_EPSILON));
	vec3 entry_position = local_origin + local_direction * min(enter_t + epsilon_t, exit_t);
	ivec3 cell = ivec3(clamp(floor(entry_position), vec3(0.0), local_bounds_max - vec3(1.0)));
	bool origin_inside = all(greaterThanEqual(local_origin, vec3(0.0))) && all(lessThan(local_origin, local_bounds_max));
	if (dynamic_volume_occupied(volume, cell)) {
		vec3 local_normal = vec3(0.0);
		if (origin_inside && enter_t <= epsilon_t) {
			hit_backface = true;
			int axis = dynamic_minimum_axis(abs(local_direction));
			local_normal[axis] = local_direction[axis] >= 0.0 ? 1.0 : -1.0;
		} else {
			hit_backface = false;
			vec3 entry = local_origin + local_direction * enter_t;
			vec3 boundary_distance = min(abs(entry), abs(local_bounds_max - entry));
			int axis = dynamic_minimum_axis(boundary_distance);
			local_normal[axis] = local_direction[axis] >= 0.0 ? -1.0 : 1.0;
		}
		hit_distance = enter_t;
		hit_position = start_world + direction_world * hit_distance;
		hit_normal = dynamic_transform_normal(volume, local_normal);
		return true;
	}

	ivec3 step_direction = ivec3(
			abs(local_direction.x) > DYNAMIC_TRACE_EPSILON ? (local_direction.x > 0.0 ? 1 : -1) : 0,
			abs(local_direction.y) > DYNAMIC_TRACE_EPSILON ? (local_direction.y > 0.0 ? 1 : -1) : 0,
			abs(local_direction.z) > DYNAMIC_TRACE_EPSILON ? (local_direction.z > 0.0 ? 1 : -1) : 0);
	vec3 inverse_direction = dynamic_safe_inverse(local_direction);
	vec3 next_boundary = vec3(cell + max(step_direction, ivec3(0)));
	vec3 next_t = (next_boundary - local_origin) * inverse_direction;
	if (step_direction.x == 0) {
		next_t.x = DYNAMIC_TRACE_HUGE;
	}
	if (step_direction.y == 0) {
		next_t.y = DYNAMIC_TRACE_HUGE;
	}
	if (step_direction.z == 0) {
		next_t.z = DYNAMIC_TRACE_HUGE;
	}
	vec3 delta_t = abs(inverse_direction);
	uint maximum_steps = max(dynamic_voxel_volumes.state.w, 1u);
	for (uint step = 0u; step < 4096u && step < maximum_steps; step++) {
		int axis = dynamic_minimum_axis(next_t);
		float travel_t = next_t[axis];
		if (travel_t > exit_t || travel_t > maximum_distance) {
			break;
		}
		cell[axis] += step_direction[axis];
		if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(volume.dimensions_directory_offset.xyz)))) {
			break;
		}
		next_t[axis] += delta_t[axis];
		if (dynamic_volume_occupied(volume, cell)) {
			vec3 local_normal = vec3(0.0);
			local_normal[axis] = -float(step_direction[axis]);
			hit_distance = travel_t;
			hit_position = start_world + direction_world * hit_distance;
			hit_normal = dynamic_transform_normal(volume, local_normal);
			hit_backface = false;
			return true;
		}
	}
	return false;
}

bool dynamic_trace_voxels(vec3 start_world, vec3 direction_world, float maximum_distance,
		out vec3 hit_position, out vec3 hit_normal, out float hit_distance, out bool hit_backface) {
	uint volume_count = dynamic_voxel_volumes.state.x;
	uint node_count = dynamic_voxel_volumes.state.y;
	if (volume_count == 0u || node_count == 0u) {
		return false;
	}
	uint stack[DYNAMIC_BVH_STACK_SIZE];
	int stack_size = 1;
	stack[0] = dynamic_voxel_bvh.state.x;
	bool found = false;
	float closest = maximum_distance;
	uint visited = 0u;
	while (stack_size > 0 && visited++ < node_count) {
		uint node_index = stack[--stack_size];
		if (node_index >= node_count) {
			continue;
		}
		DynamicVoxelBvhNode node = dynamic_voxel_bvh.values[node_index];
		float node_enter;
		float node_exit;
		if (!dynamic_intersect_aabb(start_world, direction_world, node.bounds_min.xyz, node.bounds_max.xyz, closest, node_enter, node_exit)) {
			continue;
		}
		if (node.children.z == DYNAMIC_NODE_LEAF) {
			vec3 candidate_position;
			vec3 candidate_normal;
			float candidate_distance;
			bool candidate_backface;
			if (node.children.x < volume_count && dynamic_trace_volume(node.children.x, start_world, direction_world, closest, candidate_position, candidate_normal, candidate_distance, candidate_backface)) {
				found = true;
				closest = candidate_distance;
				hit_position = candidate_position;
				hit_normal = candidate_normal;
				hit_distance = candidate_distance;
				hit_backface = candidate_backface;
			}
		} else {
			// A balanced binary tree addressed by uint cannot exceed this stack
			// depth. Candidate overflow therefore cannot silently drop a blocker.
			if (stack_size + 2 <= DYNAMIC_BVH_STACK_SIZE) {
				stack[stack_size++] = node.children.x;
				stack[stack_size++] = node.children.y;
			}
		}
	}
	return found;
}
