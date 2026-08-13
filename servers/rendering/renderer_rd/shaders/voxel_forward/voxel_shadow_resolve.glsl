#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
layout(set = 0, binding = 1) uniform sampler2D shadow_atlas;
layout(r8, set = 0, binding = 2) uniform restrict writeonly image2D shadow_mask;

layout(set = 0, binding = 3, std140) uniform OccupancyData {
	vec4 world_origin_voxel_size;
	ivec4 directory_steps;
	vec4 limits;
}
occupancy;

layout(set = 0, binding = 4, std430) readonly buffer WorldDirectory {
	uvec4 entries[];
}
world_directory;

layout(set = 0, binding = 5, std430) readonly buffer MixedBricks {
	uint words[];
}
mixed_bricks;

layout(push_constant, std430) uniform Params {
	mat4 inv_view_projection;
	vec4 atlas_center_voxel_size;
	vec4 tangent_near_extent;
	vec4 bitangent_far_extent;
	ivec4 screen_atlas_filter;
}
params;

vec2 disk_sample(int index) {
	if (index == 0) return vec2(-0.625, -0.250);
	if (index == 1) return vec2(0.250, -0.625);
	if (index == 2) return vec2(0.625, 0.250);
	if (index == 3) return vec2(-0.250, 0.625);
	if (index == 4) return vec2(-0.300, -0.100);
	if (index == 5) return vec2(0.100, -0.300);
	if (index == 6) return vec2(0.300, 0.100);
	return vec2(-0.100, 0.300);
}

uint brick_hash(ivec3 position) {
	return uint(position.x) * 73856093u ^ uint(position.y) * 19349663u ^ uint(position.z) * 83492791u;
}

uint find_brick(ivec3 position) {
	uint slot = brick_hash(position) & uint(occupancy.directory_steps.x);
	for (uint probe = 0u; probe < 64u; probe++) {
		uvec4 entry = world_directory.entries[slot];
		if (entry.w == 0u) {
			return 0u;
		}
		if (ivec3(entry.xyz) == position) {
			return entry.w;
		}
		slot = (slot + 1u) & uint(occupancy.directory_steps.x);
	}
	return 0u;
}

bool voxel_occupied(ivec3 voxel_position) {
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

bool exposed_plane_normal(vec3 voxel_position, int axis, out float normal_sign) {
	vec3 axis_vector = vec3(0.0);
	axis_vector[axis] = 1.0;
	bool negative_occupied = voxel_occupied(ivec3(floor(voxel_position - axis_vector * 0.01)));
	bool positive_occupied = voxel_occupied(ivec3(floor(voxel_position + axis_vector * 0.01)));
	normal_sign = negative_occupied ? 1.0 : -1.0;
	return negative_occupied != positive_occupied;
}

bool receiver_face_data(vec3 world_position, vec3 view_ray_direction, vec3 tangent, vec3 bitangent, vec3 light_direction, out vec3 face_center_world, out vec2 depth_slope) {
	vec3 voxel_position = (world_position - occupancy.world_origin_voxel_size.xyz) / occupancy.world_origin_voxel_size.w;
	vec3 plane_distance = abs(voxel_position - round(voxel_position));
	int axis = -1;
	float normal_sign = 1.0;
	float best_facing = -1.0;
	for (int candidate_axis = 0; candidate_axis < 3; candidate_axis++) {
		float candidate_normal_sign;
		if (plane_distance[candidate_axis] <= 0.02 && exposed_plane_normal(voxel_position, candidate_axis, candidate_normal_sign)) {
			float facing = abs(view_ray_direction[candidate_axis]);
			if (facing > best_facing) {
				axis = candidate_axis;
				normal_sign = candidate_normal_sign;
				best_facing = facing;
			}
		}
	}
	// Depth precision can put a distant hit slightly outside the close-plane
	// window. In that case choose the nearest genuinely exposed voxel plane.
	if (axis < 0) {
		float best_distance = 1e30;
		for (int candidate_axis = 0; candidate_axis < 3; candidate_axis++) {
			float candidate_normal_sign;
			if (plane_distance[candidate_axis] < best_distance && exposed_plane_normal(voxel_position, candidate_axis, candidate_normal_sign)) {
				axis = candidate_axis;
				normal_sign = candidate_normal_sign;
				best_distance = plane_distance[candidate_axis];
			}
		}
	}
	if (axis < 0) {
		face_center_world = world_position;
		depth_slope = vec2(0.0);
		return false;
	}
	vec3 receiver_normal = vec3(0.0);
	receiver_normal[axis] = normal_sign;
	float light_denominator = dot(receiver_normal, light_direction);
	if (abs(light_denominator) <= 0.0001) {
		depth_slope = vec2(0.0);
	} else {
		depth_slope = vec2(dot(receiver_normal, tangent), dot(receiver_normal, bitangent)) / light_denominator;
	}

	vec3 axis_vector = vec3(0.0);
	axis_vector[axis] = 1.0;
	ivec3 occupied_voxel = normal_sign > 0.0 ? ivec3(floor(voxel_position - axis_vector * 0.01)) : ivec3(floor(voxel_position + axis_vector * 0.01));
	vec3 face_center_voxel = vec3(occupied_voxel) + vec3(0.5);
	face_center_voxel[axis] = float(occupied_voxel[axis]) + (normal_sign > 0.0 ? 1.0 : 0.0);
	face_center_world = occupancy.world_origin_voxel_size.xyz + face_center_voxel * occupancy.world_origin_voxel_size.w;
	return true;
}

float nearest_shadow_compare(vec2 atlas_position, vec2 receiver_depth_slope, float atlas_texel_world_size, int cascade, int tile_resolution, float receiver_depth, float bias_world) {
	ivec2 tile_texel = clamp(ivec2(round(atlas_position)), ivec2(0), ivec2(tile_resolution - 1));
	vec2 receiver_plane_offset = (vec2(tile_texel) - atlas_position) * atlas_texel_world_size;
	float sample_receiver_depth = receiver_depth + dot(receiver_depth_slope, receiver_plane_offset);
	ivec2 atlas_texel = tile_texel;
	atlas_texel.x += cascade * tile_resolution;
	float occluder_depth = texelFetch(shadow_atlas, atlas_texel, 0).r;
	return sample_receiver_depth <= occluder_depth + bias_world ? 1.0 : 0.0;
}

float bilinear_shadow_compare(vec2 filtered_atlas_position, vec2 receiver_atlas_position, vec2 receiver_depth_slope, float atlas_texel_world_size, int cascade, int tile_resolution, float receiver_depth, float bias_world) {
	vec2 base_float = floor(filtered_atlas_position);
	vec2 blend = fract(filtered_atlas_position);
	ivec2 base = ivec2(base_float);
	float comparisons[4];
	for (int corner = 0; corner < 4; corner++) {
		ivec2 offset = ivec2(corner & 1, corner >> 1);
		ivec2 texel = clamp(base + offset, ivec2(0), ivec2(tile_resolution - 1));
		vec2 receiver_plane_offset = (vec2(texel) - receiver_atlas_position) * atlas_texel_world_size;
		float sample_receiver_depth = receiver_depth + dot(receiver_depth_slope, receiver_plane_offset);
		texel.x += cascade * tile_resolution;
		float occluder_depth = texelFetch(shadow_atlas, texel, 0).r;
		comparisons[corner] = sample_receiver_depth <= occluder_depth + bias_world ? 1.0 : 0.0;
	}
	float lower = mix(comparisons[0], comparisons[1], blend.x);
	float upper = mix(comparisons[2], comparisons[3], blend.x);
	return mix(lower, upper, blend.y);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 output_size = params.screen_atlas_filter.xy;
	if (any(greaterThanEqual(pixel, output_size))) {
		return;
	}

	ivec2 depth_size = textureSize(depth_buffer, 0);
	ivec2 depth_pixel = clamp(ivec2((vec2(pixel) + vec2(0.5)) * vec2(depth_size) / vec2(output_size)), ivec2(0), depth_size - 1);
	float depth = texelFetch(depth_buffer, depth_pixel, 0).r;
	if (depth <= 0.000001) {
		imageStore(shadow_mask, pixel, vec4(1.0));
		return;
	}

	vec2 ndc_xy = (vec2(depth_pixel) + vec2(0.5)) / vec2(depth_size) * 2.0 - 1.0;
	vec4 world_h = params.inv_view_projection * vec4(ndc_xy, depth, 1.0);
	vec3 world_position = world_h.xyz / world_h.w;
	vec4 near_h = params.inv_view_projection * vec4(ndc_xy, 1.0, 1.0);
	vec3 near_position = near_h.xyz / near_h.w;
	vec3 view_ray_direction = normalize(world_position - near_position);
	uint packed_filter = uint(params.screen_atlas_filter.w);
	bool soft_shadow = (packed_filter & 0x80u) != 0u;
	int sample_count = int(packed_filter & 0x7fu);
	vec3 receiver_position = world_position;
	vec2 receiver_depth_slope;
	vec3 face_center_world;
	bool voxel_receiver = receiver_face_data(world_position, view_ray_direction, params.tangent_near_extent.xyz, params.bitangent_far_extent.xyz, normalize(cross(params.tangent_near_extent.xyz, params.bitangent_far_extent.xyz)), face_center_world, receiver_depth_slope);
	// Both hard and soft modes are voxelized: filtering changes the value for a
	// face, never the value from pixel to pixel across that face.
	if (voxel_receiver) {
		receiver_position = face_center_world;
	}
	vec3 relative = receiver_position - params.atlas_center_voxel_size.xyz;
	float light_x = dot(relative, params.tangent_near_extent.xyz);
	float light_y = dot(relative, params.bitangent_far_extent.xyz);
	float edge_distance = max(abs(light_x), abs(light_y));
	float near_extent = params.tangent_near_extent.w;
	float far_extent = params.bitangent_far_extent.w;
	int cascade = edge_distance <= near_extent * 0.95 ? 0 : 1;
	float extent = cascade == 0 ? near_extent : far_extent;
	if (edge_distance >= extent) {
		imageStore(shadow_mask, pixel, vec4(1.0));
		return;
	}

	int tile_resolution = params.screen_atlas_filter.z;
	vec2 atlas_position = (vec2(light_x, light_y) / (2.0 * extent) + vec2(0.5)) * float(tile_resolution) - vec2(0.5);
	vec3 light_direction = normalize(cross(params.tangent_near_extent.xyz, params.bitangent_far_extent.xyz));
	float receiver_depth = far_extent - dot(relative, light_direction);
	float radius_voxels = float((packed_filter >> 8u) & 0xfffu) / 16.0;
	float bias_voxels = float((packed_filter >> 20u) & 0xfffu) / 256.0;
	float voxel_size = params.atlas_center_voxel_size.w;
	float radius_texels = radius_voxels * voxel_size * float(tile_resolution) / (2.0 * extent);
	float atlas_texel_world_size = 2.0 * extent / float(tile_resolution);
	float bias_world = bias_voxels * voxel_size;
	if (!soft_shadow) {
		float visibility = nearest_shadow_compare(atlas_position, receiver_depth_slope, atlas_texel_world_size, cascade, tile_resolution, receiver_depth, bias_world);
		imageStore(shadow_mask, pixel, vec4(visibility));
		return;
	}
	float visibility = 0.0;
	for (int sample_index = 0; sample_index < 8; sample_index++) {
		if (sample_index >= sample_count) break;
		vec2 offset = sample_count == 1 ? vec2(0.0) : disk_sample(sample_index) * radius_texels;
		visibility += bilinear_shadow_compare(atlas_position + offset, atlas_position, receiver_depth_slope, atlas_texel_world_size, cascade, tile_resolution, receiver_depth, bias_world);
	}
	visibility /= float(max(sample_count, 1));
	imageStore(shadow_mask, pixel, vec4(visibility));
}
