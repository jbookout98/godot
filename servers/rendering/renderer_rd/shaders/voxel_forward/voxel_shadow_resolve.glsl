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

#ifdef USE_VOXEL_HIT_PAYLOAD
layout(set = 0, binding = 6) uniform usampler2D voxel_hit_buffer;
#else
layout(set = 0, binding = 4, std430) readonly buffer WorldDirectory {
	uvec4 entries[];
}
world_directory;

layout(set = 0, binding = 5, std430) readonly buffer MixedBricks {
	uint words[];
}
mixed_bricks;
#endif

layout(push_constant, std430) uniform Params {
	mat4 inv_view_projection;
	vec4 atlas_center_voxel_size;
	vec4 tangent_near_extent;
	vec4 bitangent_far_extent;
	ivec4 screen_atlas_filter;
}
params;

// Voxel shadow results are deliberately stable across an entire voxel face.
// Reuse one result for every matching face in the complete 8x8 workgroup.
shared ivec4 quad_face_keys[64];
shared float quad_face_visibility[64];

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

#ifndef USE_VOXEL_HIT_PAYLOAD
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

bool receiver_face_data(vec3 world_position, vec3 view_ray_direction, vec3 tangent, vec3 bitangent, vec3 light_direction, out vec3 face_center_world, out vec2 depth_slope, out ivec4 face_key) {
	face_key = ivec4(0);
	vec3 voxel_position = (world_position - occupancy.world_origin_voxel_size.xyz) / occupancy.world_origin_voxel_size.w;
	int axis = -1;
	float normal_sign = 1.0;
	ivec3 occupied_voxel = ivec3(0);
	float best_distance = 1e30;
	float best_facing = -1.0;
	for (int candidate_axis = 0; candidate_axis < 3; candidate_axis++) {
		if (abs(view_ray_direction[candidate_axis]) <= 0.0000001) {
			continue;
		}
		int first_plane = int(floor(voxel_position[candidate_axis]));
		for (int plane_offset = 0; plane_offset < 2; plane_offset++) {
			int plane = first_plane + plane_offset;
			float ray_distance = (float(plane) - voxel_position[candidate_axis]) / view_ray_direction[candidate_axis];
			vec3 plane_hit = voxel_position + view_ray_direction * ray_distance;
			// Step toward the visible solid to make voxel-edge ties deterministic.
			ivec3 entered_voxel = ivec3(floor(plane_hit + view_ray_direction * 0.0001));
			ivec3 negative_voxel = entered_voxel;
			ivec3 positive_voxel = entered_voxel;
			negative_voxel[candidate_axis] = plane - 1;
			positive_voxel[candidate_axis] = plane;
			bool negative_occupied = voxel_occupied(negative_voxel);
			bool positive_occupied = voxel_occupied(positive_voxel);
			if (negative_occupied == positive_occupied) {
				continue;
			}
			float candidate_sign = negative_occupied ? 1.0 : -1.0;
			float facing = -candidate_sign * view_ray_direction[candidate_axis];
			if (facing <= 0.0000001) {
				continue;
			}
			float distance = abs(ray_distance);
			if (distance < best_distance - 0.00001 || (abs(distance - best_distance) <= 0.00001 && facing > best_facing)) {
				axis = candidate_axis;
				normal_sign = candidate_sign;
				occupied_voxel = negative_occupied ? negative_voxel : positive_voxel;
				best_distance = distance;
				best_facing = facing;
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

	vec3 face_center_voxel = vec3(occupied_voxel) + vec3(0.5);
	face_center_voxel[axis] = float(occupied_voxel[axis]) + (normal_sign > 0.0 ? 1.0 : 0.0);
	face_center_world = occupancy.world_origin_voxel_size.xyz + face_center_voxel * occupancy.world_origin_voxel_size.w;
	face_key = ivec4(occupied_voxel, axis * 2 + (normal_sign > 0.0 ? 2 : 1));
	return true;
}
#else
bool payload_receiver_face_data(vec3 world_position, uint hit_payload, vec3 tangent, vec3 bitangent, vec3 light_direction, out vec3 face_center_world, out vec2 depth_slope, out ivec4 face_key) {
	uint world_face_code = (hit_payload >> 11u) & 0x7u;
	if (hit_payload == 0u || world_face_code >= 6u) {
		face_center_world = world_position;
		depth_slope = vec2(0.0);
		face_key = ivec4(0);
		return false;
	}

	int axis = int(world_face_code >> 1u);
	float normal_sign = (world_face_code & 1u) != 0u ? 1.0 : -1.0;
	vec3 receiver_normal = vec3(0.0);
	receiver_normal[axis] = normal_sign;
	vec3 voxel_position = (world_position - occupancy.world_origin_voxel_size.xyz) / occupancy.world_origin_voxel_size.w;
	// Depth is snapped to the exact face plane. Step inward before floor() so
	// both positive and negative faces select the solid voxel deterministically.
	ivec3 occupied_voxel = ivec3(floor(voxel_position - receiver_normal * 0.0001));

	vec3 face_center_voxel = vec3(occupied_voxel) + vec3(0.5);
	face_center_voxel[axis] = float(occupied_voxel[axis]) + (normal_sign > 0.0 ? 1.0 : 0.0);
	face_center_world = occupancy.world_origin_voxel_size.xyz + face_center_voxel * occupancy.world_origin_voxel_size.w;
	face_key = ivec4(occupied_voxel, axis * 2 + (normal_sign > 0.0 ? 2 : 1));

	float light_denominator = dot(receiver_normal, light_direction);
	if (abs(light_denominator) <= 0.0001) {
		depth_slope = vec2(0.0);
	} else {
		depth_slope = vec2(dot(receiver_normal, tangent), dot(receiver_normal, bitangent)) / light_denominator;
	}
	return true;
}
#endif

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
	bool output_pixel = all(lessThan(pixel, output_size));
	bool evaluate_shadow = false;
	bool voxel_receiver = false;
	vec3 receiver_position = vec3(0.0);
	vec2 receiver_depth_slope = vec2(0.0);
	ivec4 face_key = ivec4(0);
	float final_visibility = 1.0;

	if (output_pixel) {
		ivec2 depth_size = textureSize(depth_buffer, 0);
		ivec2 depth_pixel = clamp(ivec2((vec2(pixel) + vec2(0.5)) * vec2(depth_size) / vec2(output_size)), ivec2(0), depth_size - 1);
		float depth = texelFetch(depth_buffer, depth_pixel, 0).r;
		if (depth > 0.000001) {
			vec2 ndc_xy = (vec2(depth_pixel) + vec2(0.5)) / vec2(depth_size) * 2.0 - 1.0;
			vec4 world_h = params.inv_view_projection * vec4(ndc_xy, depth, 1.0);
			vec3 world_position = world_h.xyz / world_h.w;
			vec3 face_center_world;
#ifdef USE_VOXEL_HIT_PAYLOAD
			uint hit_payload = texelFetch(voxel_hit_buffer, depth_pixel, 0).r;
			voxel_receiver = payload_receiver_face_data(world_position, hit_payload, params.tangent_near_extent.xyz, params.bitangent_far_extent.xyz, normalize(cross(params.tangent_near_extent.xyz, params.bitangent_far_extent.xyz)), face_center_world, receiver_depth_slope, face_key);
#else
			vec4 near_h = params.inv_view_projection * vec4(ndc_xy, 1.0, 1.0);
			vec3 near_position = near_h.xyz / near_h.w;
			vec3 view_ray_direction = normalize(world_position - near_position);
			voxel_receiver = receiver_face_data(world_position, view_ray_direction, params.tangent_near_extent.xyz, params.bitangent_far_extent.xyz, normalize(cross(params.tangent_near_extent.xyz, params.bitangent_far_extent.xyz)), face_center_world, receiver_depth_slope, face_key);
#endif
			receiver_position = voxel_receiver ? face_center_world : world_position;
			evaluate_shadow = true;
		}
	}

	uint local_index = gl_LocalInvocationIndex;
	quad_face_keys[local_index] = voxel_receiver ? face_key : ivec4(0);
	barrier();

	int leader = int(local_index);
	if (voxel_receiver) {
		for (int candidate = 0; candidate < int(local_index); candidate++) {
			if (all(equal(quad_face_keys[candidate], face_key))) {
				leader = candidate;
				break;
			}
		}
	}

	if (evaluate_shadow && (!voxel_receiver || leader == int(local_index))) {
		vec3 relative = receiver_position - params.atlas_center_voxel_size.xyz;
		float light_x = dot(relative, params.tangent_near_extent.xyz);
		float light_y = dot(relative, params.bitangent_far_extent.xyz);
		float edge_distance = max(abs(light_x), abs(light_y));
		float near_extent = params.tangent_near_extent.w;
		float far_extent = params.bitangent_far_extent.w;
		int cascade = edge_distance <= near_extent * 0.95 ? 0 : 1;
		float extent = cascade == 0 ? near_extent : far_extent;
		if (edge_distance < extent) {
			uint packed_filter = uint(params.screen_atlas_filter.w);
			bool soft_shadow = (packed_filter & 0x80u) != 0u;
			int sample_count = int(packed_filter & 0x7fu);
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
				final_visibility = nearest_shadow_compare(atlas_position, receiver_depth_slope, atlas_texel_world_size, cascade, tile_resolution, receiver_depth, bias_world);
			} else {
				final_visibility = 0.0;
				for (int sample_index = 0; sample_index < 8; sample_index++) {
					if (sample_index >= sample_count) break;
					vec2 offset = sample_count == 1 ? vec2(0.0) : disk_sample(sample_index) * radius_texels;
					final_visibility += bilinear_shadow_compare(atlas_position + offset, atlas_position, receiver_depth_slope, atlas_texel_world_size, cascade, tile_resolution, receiver_depth, bias_world);
				}
				final_visibility /= float(max(sample_count, 1));
			}
		}
		if (voxel_receiver) {
			quad_face_visibility[local_index] = final_visibility;
		}
	}
	barrier();

	if (voxel_receiver) {
		final_visibility = quad_face_visibility[leader];
	}
	if (output_pixel) {
		imageStore(shadow_mask, pixel, vec4(final_visibility));
	}
}
