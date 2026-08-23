#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(r32f, set = 0, binding = 0) uniform restrict writeonly image2D shadow_atlas;

layout(set = 0, binding = 1, std430) readonly buffer WorldDirectory {
	uvec4 entries[];
}
world_directory;

layout(set = 0, binding = 2, std430) readonly buffer MixedBricks {
	uint words[];
}
mixed_bricks;

layout(push_constant, std430) uniform Params {
	vec4 world_origin_voxel_size;
	vec4 atlas_center_depth;
	vec4 tangent_near_extent;
	vec4 bitangent_far_extent;
	ivec4 atlas_directory_steps;
}
params;

uint brick_hash(ivec3 position) {
	return uint(position.x) * 73856093u ^ uint(position.y) * 19349663u ^ uint(position.z) * 83492791u;
}

uint find_brick(ivec3 position) {
	uint slot = brick_hash(position) & uint(params.atlas_directory_steps.z);
	for (uint probe = 0u; probe < 64u; probe++) {
		uvec4 entry = world_directory.entries[slot];
		if (entry.w == 0u) {
			return 0u;
		}
		if (ivec3(entry.xyz) == position) {
			return entry.w;
		}
		slot = (slot + 1u) & uint(params.atlas_directory_steps.z);
	}
	return 0u;
}

bool mixed_brick_occupied(uint code, ivec3 local_voxel) {
	uint local_index = uint(local_voxel.x + local_voxel.y * 8 + local_voxel.z * 64);
	uint word = mixed_bricks.words[(code - 2u) * 16u + (local_index >> 5u)];
	return (word & (1u << (local_index & 31u))) != 0u;
}

float distance_to_cell_exit(vec3 position, vec3 direction, float cell_size) {
	vec3 cell = floor(position / cell_size);
	vec3 boundary = (cell + step(vec3(0.0), direction)) * cell_size;
	vec3 distance = vec3(1e30);
	if (abs(direction.x) > 1e-8) distance.x = (boundary.x - position.x) / direction.x;
	if (abs(direction.y) > 1e-8) distance.y = (boundary.y - position.y) / direction.y;
	if (abs(direction.z) > 1e-8) distance.z = (boundary.z - position.z) / direction.z;
	return max(min(distance.x, min(distance.y, distance.z)), 0.0001);
}

float trace_first_occupied(vec3 ray_position, vec3 ray_direction, float maximum_distance) {
	float traveled = 0.0;
	ivec3 cached_brick_position = ivec3(0);
	uint cached_brick_code = 0u;
	bool brick_cached = false;
	for (int step_index = 0; step_index < params.atlas_directory_steps.w && traveled < maximum_distance; step_index++) {
		ivec3 brick_position = ivec3(floor(ray_position / 8.0));
		if (!brick_cached || any(notEqual(brick_position, cached_brick_position))) {
			cached_brick_position = brick_position;
			cached_brick_code = find_brick(brick_position);
			brick_cached = true;
		}
		uint code = cached_brick_code;
		if (code == 1u) {
			return traveled;
		}
		if (code >= 2u) {
			ivec3 voxel_position = ivec3(floor(ray_position));
			ivec3 local_voxel = voxel_position - brick_position * 8;
			if (mixed_brick_occupied(code, local_voxel)) {
				return traveled;
			}
			float advance = distance_to_cell_exit(ray_position, ray_direction, 1.0) + 0.001;
			ray_position += ray_direction * advance;
			traveled += advance;
			continue;
		}
		float advance = distance_to_cell_exit(ray_position, ray_direction, 8.0) + 0.001;
		ray_position += ray_direction * advance;
		traveled += advance;
	}
	return maximum_distance + 1.0;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	int tile_resolution = params.atlas_directory_steps.x;
	int cascade_count = params.atlas_directory_steps.y;
	if (pixel.y >= tile_resolution || pixel.x >= tile_resolution * cascade_count) {
		return;
	}

	int cascade = pixel.x / tile_resolution;
	ivec2 tile_pixel = ivec2(pixel.x - cascade * tile_resolution, pixel.y);
	float extent = cascade == 0 ? params.tangent_near_extent.w : params.bitangent_far_extent.w;
	vec2 light_plane = ((vec2(tile_pixel) + vec2(0.5)) / float(tile_resolution) * 2.0 - 1.0) * extent;
	vec3 light_direction = normalize(cross(params.tangent_near_extent.xyz, params.bitangent_far_extent.xyz));
	float depth_extent = params.atlas_center_depth.w;
	vec3 ray_start_world = params.atlas_center_depth.xyz +
			params.tangent_near_extent.xyz * light_plane.x +
			params.bitangent_far_extent.xyz * light_plane.y +
			light_direction * depth_extent;
	float voxel_size = params.world_origin_voxel_size.w;
	vec3 ray_position = (ray_start_world - params.world_origin_voxel_size.xyz) / voxel_size;
	vec3 ray_direction = -light_direction;
	float maximum_distance_voxels = depth_extent * 2.0 / voxel_size;
	float hit_distance_voxels = trace_first_occupied(ray_position + ray_direction * 0.001, ray_direction, maximum_distance_voxels);
	imageStore(shadow_atlas, pixel, vec4(hit_distance_voxels * voxel_size));
}
