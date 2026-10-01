#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
layout(set = 0, binding = 1) uniform usampler2D voxel_hit_buffer;
layout(r8, set = 0, binding = 2) uniform restrict writeonly image2DArray local_shadow_masks;
layout(set = 0, binding = 3, std430) readonly buffer WorldDirectory {
	uvec4 entries[];
}
world_directory;
layout(set = 0, binding = 4, std430) readonly buffer MixedBricks {
	uint words[];
}
mixed_bricks;

layout(set = 0, binding = 5, std140) uniform OccupancyData {
	vec4 world_origin_voxel_size;
	ivec4 directory_steps;
	vec4 limits;
}
occupancy;

struct VoxelLocalShadowEntry {
	vec4 position_inv_radius;
	vec4 direction_cone;
	ivec4 indices;
};

layout(set = 0, binding = 6, std140) uniform VoxelLocalShadowData {
	mat4 inv_view_projection;
	ivec4 state;
	vec4 trace_settings;
	VoxelLocalShadowEntry entries[32];
}
local_shadows;
layout(set = 0, binding = 7) uniform sampler2D voxel_face_buffer;

#include "voxel_dynamic_trace_inc.glsl"

const float DIRECTION_EPSILON = 0.0000001;
const float HUGE_DISTANCE = 1e30;

shared uvec4 face_keys[64];
shared float face_visibility[64];

vec3 decode_face_normal(float packed_float) {
	uint packed = uint(max(packed_float, 1.0) + 0.5) - 1u;
	vec2 oct = vec2(float(packed & 0xfffu), float((packed >> 12u) & 0xfffu)) / 4095.0;
	vec2 folded = oct * 2.0 - 1.0;
	vec3 normal = vec3(folded.x, 1.0 - abs(folded.x) - abs(folded.y), folded.y);
	if (normal.y < 0.0) {
		normal.xz = (1.0 - abs(normal.zx)) * sign(normal.xz);
	}
	return normalize(normal);
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

bool mixed_brick_occupied(uint code, ivec3 local_voxel) {
	uint local_index = uint(local_voxel.x + local_voxel.y * 8 + local_voxel.z * 64);
	uint word = mixed_bricks.words[(code - 2u) * 16u + (local_index >> 5u)];
	return (word & (1u << (local_index & 31u))) != 0u;
}

float distance_to_exit(vec3 position, vec3 direction, float cell_size) {
	vec3 cell = floor(position / cell_size);
	vec3 boundary = (cell + step(vec3(0.0), direction)) * cell_size;
	vec3 distance = vec3(HUGE_DISTANCE);
	if (abs(direction.x) > DIRECTION_EPSILON) distance.x = (boundary.x - position.x) / direction.x;
	if (abs(direction.y) > DIRECTION_EPSILON) distance.y = (boundary.y - position.y) / direction.y;
	if (abs(direction.z) > DIRECTION_EPSILON) distance.z = (boundary.z - position.z) / direction.z;
	return max(min(distance.x, min(distance.y, distance.z)), 0.0001);
}

float trace_shadow(vec3 receiver_position, vec3 direction, float maximum_distance) {
	vec3 ray_position = receiver_position + direction * 0.001;
	ivec3 start_brick_position = ivec3(floor(ray_position / 8.0));
	uint start_code = find_brick(start_brick_position);
	bool skipping_receiver = start_code == 1u;
	if (start_code >= 2u) {
		ivec3 start_voxel_position = ivec3(floor(ray_position));
		skipping_receiver = mixed_brick_occupied(start_code, start_voxel_position - start_brick_position * 8);
	}

	float traveled = 0.0;
	for (int step_index = 0; step_index < 4096; step_index++) {
		if (step_index >= occupancy.directory_steps.y || traveled >= maximum_distance) {
			break;
		}
		ivec3 brick_position = ivec3(floor(ray_position / 8.0));
		uint code = find_brick(brick_position);
		if (code == 0u) {
			skipping_receiver = false;
			float advance = distance_to_exit(ray_position, direction, 8.0) + 0.001;
			ray_position += direction * advance;
			traveled += advance;
			continue;
		}
		if (code == 1u) {
			if (!skipping_receiver) {
				return 0.0;
			}
			float advance = distance_to_exit(ray_position, direction, 8.0) + 0.001;
			ray_position += direction * advance;
			traveled += advance;
			continue;
		}

		ivec3 voxel_position = ivec3(floor(ray_position));
		ivec3 local_voxel = voxel_position - brick_position * 8;
		if (mixed_brick_occupied(code, local_voxel)) {
			if (!skipping_receiver) {
				return 0.0;
			}
		} else {
			skipping_receiver = false;
		}
		float advance = distance_to_exit(ray_position, direction, 1.0) + 0.001;
		ray_position += direction * advance;
		traveled += advance;
	}
	return 1.0;
}

float trace_dynamic_shadow(vec3 receiver_world, vec3 direction_world, float maximum_distance_world) {
	if (dynamic_voxel_volumes.state.x == 0u) return 1.0;
	vec3 hit_position;
	vec3 hit_normal;
	float hit_distance;
	bool hit_backface;
	float ray_bias = max(occupancy.world_origin_voxel_size.w * 0.01, 0.0001);
	return dynamic_trace_voxels(receiver_world + direction_world * ray_bias, direction_world, maximum_distance_world,
			hit_position, hit_normal, hit_distance, hit_backface) ? 0.0 : 1.0;
}

vec2 disk_sample(int sample_index) {
	if (sample_index == 0) return vec2(-0.625, -0.250);
	if (sample_index == 1) return vec2(0.250, -0.625);
	if (sample_index == 2) return vec2(0.625, 0.250);
	if (sample_index == 3) return vec2(-0.250, 0.625);
	if (sample_index == 4) return vec2(-0.300, -0.100);
	if (sample_index == 5) return vec2(0.100, -0.300);
	if (sample_index == 6) return vec2(0.300, 0.100);
	return vec2(-0.100, 0.300);
}

bool receiver_face(uint payload, vec4 face_data, out vec3 face_center_world, out vec3 receiver_normal, out uvec4 face_key) {
	if (payload == 0u || face_data.w < 1.0) {
		face_center_world = face_data.xyz;
		receiver_normal = vec3(0.0);
		face_key = uvec4(0u);
		return false;
	}

	face_center_world = face_data.xyz;
	receiver_normal = decode_face_normal(face_data.w);
	face_key = uvec4(floatBitsToUint(face_data.xyz), payload);
	return true;
}

float resolve_visibility(VoxelLocalShadowEntry light, vec3 receiver_world, vec3 receiver_normal) {
	vec3 light_vector_world = light.position_inv_radius.xyz - receiver_world;
	float light_distance = length(light_vector_world);
	if (light_distance <= 0.0001 || light_distance * light.position_inv_radius.w >= 1.0) {
		return 1.0;
	}
	vec3 direction_world = light_vector_world / light_distance;
	if (light.indices.x == 1 && dot(direction_world, -light.direction_cone.xyz) <= light.direction_cone.w) {
		return 1.0;
	}

	float voxel_size = occupancy.world_origin_voxel_size.w;
	vec3 receiver_voxel = (receiver_world - occupancy.world_origin_voxel_size.xyz) / voxel_size;
	float maximum_distance = min(light_distance, local_shadows.trace_settings.x) / voxel_size;
	if (maximum_distance <= 0.001) {
		return 1.0;
	}

	int sample_count = clamp(int(local_shadows.trace_settings.z + 0.5), 1, 8);
	float radius_voxels = local_shadows.trace_settings.w;
	if (local_shadows.trace_settings.y < 0.5 || radius_voxels <= 0.0001 || sample_count == 1) {
		float static_visibility = trace_shadow(receiver_voxel, direction_world, maximum_distance);
		return static_visibility * trace_dynamic_shadow(receiver_world, direction_world, min(light_distance, local_shadows.trace_settings.x));
	}

	vec3 tangent = abs(receiver_normal.x) > 0.5 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 bitangent = normalize(cross(receiver_normal, tangent));
	float visibility = 0.0;
	for (int sample_index = 0; sample_index < 8; sample_index++) {
		if (sample_index >= sample_count) {
			break;
		}
		vec2 face_offset = disk_sample(sample_index) * radius_voxels;
		float largest_axis = max(abs(face_offset.x), abs(face_offset.y));
		if (largest_axis > 0.45) {
			face_offset *= 0.45 / largest_axis;
		}
		vec3 sample_offset_world = (tangent * face_offset.x + bitangent * face_offset.y) * voxel_size;
		vec3 sample_position = receiver_voxel + sample_offset_world / voxel_size;
		vec3 sample_vector_world = light_vector_world - sample_offset_world;
		float sample_distance = min(length(sample_vector_world), local_shadows.trace_settings.x) / voxel_size;
		vec3 sample_direction_world = normalize(sample_vector_world);
		float static_visibility = trace_shadow(sample_position, sample_direction_world, sample_distance);
		vec3 sample_position_world = occupancy.world_origin_voxel_size.xyz + sample_position * voxel_size;
		float dynamic_visibility = trace_dynamic_shadow(sample_position_world, sample_direction_world, min(length(sample_vector_world), local_shadows.trace_settings.x));
		visibility += static_visibility * dynamic_visibility;
	}
	float averaged_visibility = visibility / float(sample_count);
	if (visibility <= 1.0) averaged_visibility = 0.0;
	if (visibility >= float(sample_count - 1)) averaged_visibility = 1.0;
	return averaged_visibility;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	int layer = int(gl_GlobalInvocationID.z);
	ivec2 output_size = local_shadows.state.xy;
	int entry_count = clamp(local_shadows.state.z, 0, 32);
	bool output_pixel = all(lessThan(pixel, output_size)) && layer < entry_count;
	bool voxel_receiver = false;
	vec3 receiver_world = vec3(0.0);
	vec3 receiver_normal = vec3(0.0);
	uvec4 face_key = uvec4(0u);
	float final_visibility = 1.0;

	if (output_pixel) {
		ivec2 depth_size = textureSize(depth_buffer, 0);
		ivec2 depth_pixel = clamp(ivec2((vec2(pixel) + vec2(0.5)) * vec2(depth_size) / vec2(output_size)), ivec2(0), depth_size - 1);
		float depth = texelFetch(depth_buffer, depth_pixel, 0).r;
		uint payload = texelFetch(voxel_hit_buffer, depth_pixel, 0).r;
		vec4 face_data = texelFetch(voxel_face_buffer, depth_pixel, 0);
		if (depth > 0.000001 && payload != 0u && face_data.w >= 1.0) {
			voxel_receiver = receiver_face(payload, face_data, receiver_world, receiver_normal, face_key);
		}
	}

	uint local_index = gl_LocalInvocationIndex;
	face_keys[local_index] = voxel_receiver ? face_key : uvec4(0u);
	barrier();

	int leader = int(local_index);
	if (voxel_receiver) {
		// Share exact face results inside a 2x2 quad.
		ivec2 local_pixel = ivec2(gl_LocalInvocationID.xy);
		ivec2 tile_origin = local_pixel & ivec2(~1);
		for (int tile_y = 0; tile_y < 2; tile_y++) {
			for (int tile_x = 0; tile_x < 2; tile_x++) {
				int candidate = (tile_origin.y + tile_y) * 8 + tile_origin.x + tile_x;
				if (candidate >= int(local_index)) {
					break;
				}
				if (all(equal(face_keys[candidate], face_key))) {
					leader = candidate;
					break;
				}
			}
			if (leader != int(local_index)) {
				break;
			}
		}
	}

	if (voxel_receiver && leader == int(local_index)) {
		final_visibility = resolve_visibility(local_shadows.entries[layer], receiver_world, receiver_normal);
		face_visibility[local_index] = final_visibility;
	}
	barrier();

	if (voxel_receiver) {
		final_visibility = face_visibility[leader];
	}
	if (output_pixel) {
		imageStore(local_shadow_masks, ivec3(pixel, layer), vec4(final_visibility));
	}
}
