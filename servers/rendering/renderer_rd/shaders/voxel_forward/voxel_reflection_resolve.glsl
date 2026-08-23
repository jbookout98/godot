#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image2D reflection_output;
layout(set = 0, binding = 2, std430) readonly buffer WorldDirectory {
	uvec4 entries[];
}
world_directory;
layout(set = 0, binding = 3, std430) readonly buffer MixedBricks {
	uint words[];
}
mixed_bricks;
layout(set = 0, binding = 4) uniform sampler3D color_near;
layout(set = 0, binding = 5) uniform sampler3D color_far;
layout(set = 0, binding = 6) uniform sampler3D color_distant;
layout(set = 0, binding = 7) uniform sampler3D indirect_near;
layout(set = 0, binding = 8) uniform sampler3D indirect_far;
layout(set = 0, binding = 9) uniform sampler3D indirect_distant;

layout(set = 0, binding = 10, std140) uniform ReflectionData {
	mat4 inv_view_projection;
	vec4 world_origin_voxel_size;
	vec4 camera_position_max_distance;
	vec4 color_grid_origin_cell_size[3];
	vec4 indirect_grid_origin_cell_size[3];
	vec4 ambient_color_energy;
	ivec4 screen_grid_steps;
	ivec4 state;
}
reflection;

const float DIRECTION_EPSILON = 0.0000001;
const float HUGE_DISTANCE = 1e30;

shared ivec4 quad_face_keys[64];
shared vec4 quad_reflections[64];

uint brick_hash(ivec3 position) {
	return uint(position.x) * 73856093u ^ uint(position.y) * 19349663u ^ uint(position.z) * 83492791u;
}

uint find_brick(ivec3 position) {
	uint slot = brick_hash(position) & uint(reflection.state.x);
	for (uint probe = 0u; probe < 64u; probe++) {
		uvec4 entry = world_directory.entries[slot];
		if (entry.w == 0u) {
			return 0u;
		}
		if (ivec3(entry.xyz) == position) {
			return entry.w;
		}
		slot = (slot + 1u) & uint(reflection.state.x);
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

bool receiver_face_data(vec3 world_position, vec3 view_direction, out vec3 face_center_world, out vec3 receiver_normal, out ivec4 face_key) {
	face_key = ivec4(0);
	vec3 voxel_position = (world_position - reflection.world_origin_voxel_size.xyz) / reflection.world_origin_voxel_size.w;
	int axis = -1;
	float normal_sign = 1.0;
	ivec3 occupied_voxel = ivec3(0);
	float best_distance = HUGE_DISTANCE;
	float best_facing = -1.0;
	for (int candidate_axis = 0; candidate_axis < 3; candidate_axis++) {
		if (abs(view_direction[candidate_axis]) <= DIRECTION_EPSILON) {
			continue;
		}
		int first_plane = int(floor(voxel_position[candidate_axis]));
		for (int plane_offset = 0; plane_offset < 2; plane_offset++) {
			int plane = first_plane + plane_offset;
			float ray_distance = (float(plane) - voxel_position[candidate_axis]) / view_direction[candidate_axis];
			vec3 plane_hit = voxel_position + view_direction * ray_distance;
			ivec3 entered_voxel = ivec3(floor(plane_hit + view_direction * 0.0001));
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
			float facing = -candidate_sign * view_direction[candidate_axis];
			if (facing <= DIRECTION_EPSILON) {
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
		return false;
	}

	receiver_normal = vec3(0.0);
	receiver_normal[axis] = normal_sign;
	vec3 face_center_voxel = vec3(occupied_voxel) + vec3(0.5);
	face_center_voxel[axis] = float(occupied_voxel[axis]) + (normal_sign > 0.0 ? 1.0 : 0.0);
	face_center_world = reflection.world_origin_voxel_size.xyz + face_center_voxel * reflection.world_origin_voxel_size.w;
	face_key = ivec4(occupied_voxel, axis * 2 + (normal_sign > 0.0 ? 2 : 1));
	return true;
}

vec4 sample_grid(sampler3D grid, vec4 origin_cell_size, int resolution, vec3 world_position) {
	vec3 uvw = (world_position - origin_cell_size.xyz) / (origin_cell_size.w * float(resolution));
	if (any(lessThan(uvw, vec3(0.0))) || any(greaterThanEqual(uvw, vec3(1.0)))) {
		return vec4(0.0);
	}
	return textureLod(grid, uvw, 0.0);
}

vec3 sample_hit_albedo(vec3 world_position) {
	vec4 result = sample_grid(color_near, reflection.color_grid_origin_cell_size[0], reflection.screen_grid_steps.z, world_position);
	if (result.a <= 0.0) result = sample_grid(color_far, reflection.color_grid_origin_cell_size[1], reflection.screen_grid_steps.z, world_position);
	if (result.a <= 0.0) result = sample_grid(color_distant, reflection.color_grid_origin_cell_size[2], reflection.screen_grid_steps.z, world_position);
	return result.rgb;
}

vec3 sample_indirect(vec3 world_position) {
	if (reflection.state.y == 0) {
		return vec3(0.0);
	}
	vec4 sample_value = sample_grid(indirect_near, reflection.indirect_grid_origin_cell_size[0], reflection.state.z, world_position);
	if (sample_value.a <= 0.0) sample_value = sample_grid(indirect_far, reflection.indirect_grid_origin_cell_size[1], reflection.state.z, world_position);
	if (sample_value.a <= 0.0) sample_value = sample_grid(indirect_distant, reflection.indirect_grid_origin_cell_size[2], reflection.state.z, world_position);
	return sample_value.rgb;
}

bool trace_reflection(vec3 start_world, vec3 direction, out ivec3 hit_voxel) {
	vec3 origin = (start_world - reflection.world_origin_voxel_size.xyz) / reflection.world_origin_voxel_size.w;
	ivec3 cell = ivec3(floor(origin));
	ivec3 step_direction = ivec3(
			abs(direction.x) > DIRECTION_EPSILON ? (direction.x > 0.0 ? 1 : -1) : 0,
			abs(direction.y) > DIRECTION_EPSILON ? (direction.y > 0.0 ? 1 : -1) : 0,
			abs(direction.z) > DIRECTION_EPSILON ? (direction.z > 0.0 ? 1 : -1) : 0);
	vec3 inverse_direction = vec3(
			step_direction.x != 0 ? 1.0 / direction.x : HUGE_DISTANCE,
			step_direction.y != 0 ? 1.0 / direction.y : HUGE_DISTANCE,
			step_direction.z != 0 ? 1.0 / direction.z : HUGE_DISTANCE);
	vec3 next_boundary = vec3(cell + max(step_direction, ivec3(0)));
	vec3 next_t = (next_boundary - origin) * inverse_direction;
	if (step_direction.x == 0) next_t.x = HUGE_DISTANCE;
	if (step_direction.y == 0) next_t.y = HUGE_DISTANCE;
	if (step_direction.z == 0) next_t.z = HUGE_DISTANCE;
	vec3 delta_t = abs(inverse_direction);
	float max_t = reflection.camera_position_max_distance.w / reflection.world_origin_voxel_size.w;
	ivec3 cached_brick_position = ivec3(0);
	uint cached_brick_code = 0u;
	bool brick_cached = false;

	for (int step = 0; step < 4096; step++) {
		if (step >= reflection.screen_grid_steps.w) {
			break;
		}
		int axis = next_t.x <= next_t.y && next_t.x <= next_t.z ? 0 : (next_t.y <= next_t.z ? 1 : 2);
		float travel_t = next_t[axis];
		if (travel_t > max_t) {
			break;
		}
		cell[axis] += step_direction[axis];
		next_t[axis] += delta_t[axis];

		ivec3 brick_position = ivec3(floor(vec3(cell) / 8.0));
		if (!brick_cached || any(notEqual(brick_position, cached_brick_position))) {
			cached_brick_position = brick_position;
			cached_brick_code = find_brick(brick_position);
			brick_cached = true;
		}
		bool occupied = cached_brick_code == 1u;
		if (cached_brick_code >= 2u) {
			ivec3 local_voxel = cell - cached_brick_position * 8;
			uint local_index = uint(local_voxel.x + local_voxel.y * 8 + local_voxel.z * 64);
			uint word = mixed_bricks.words[(cached_brick_code - 2u) * 16u + (local_index >> 5u)];
			occupied = (word & (1u << (local_index & 31u))) != 0u;
		}
		if (occupied) {
			hit_voxel = cell;
			return true;
		}
	}
	return false;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 output_size = reflection.screen_grid_steps.xy;
	bool output_pixel = all(lessThan(pixel, output_size));
	bool voxel_receiver = false;
	vec3 face_center = vec3(0.0);
	vec3 face_normal = vec3(0.0);
	ivec4 face_key = ivec4(0);
	vec4 final_reflection = vec4(0.0);

	if (output_pixel) {
		ivec2 depth_size = textureSize(depth_buffer, 0);
		ivec2 depth_pixel = clamp(ivec2((vec2(pixel) + vec2(0.5)) * vec2(depth_size) / vec2(output_size)), ivec2(0), depth_size - 1);
		float depth = texelFetch(depth_buffer, depth_pixel, 0).r;
		if (depth <= 0.000001) {
			final_reflection = vec4(reflection.ambient_color_energy.rgb * reflection.ambient_color_energy.a, 0.0);
		} else {
			vec2 ndc_xy = (vec2(depth_pixel) + vec2(0.5)) / vec2(depth_size) * 2.0 - 1.0;
			vec4 world_h = reflection.inv_view_projection * vec4(ndc_xy, depth, 1.0);
			vec3 world_position = world_h.xyz / world_h.w;
			vec3 pixel_view_direction = normalize(world_position - reflection.camera_position_max_distance.xyz);
			voxel_receiver = receiver_face_data(world_position, pixel_view_direction, face_center, face_normal, face_key);
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

	if (voxel_receiver && leader == int(local_index)) {
		// Both the view vector and the ray origin are snapped to the resolved
		// voxel face, so all pixels with this key have the same reflection.
		vec3 face_view_direction = normalize(face_center - reflection.camera_position_max_distance.xyz);
		vec3 reflected_direction = normalize(reflect(face_view_direction, face_normal));
		vec3 start_world = face_center + face_normal * (reflection.world_origin_voxel_size.w * 0.501);
		ivec3 hit_voxel;
		vec3 reflected_radiance = reflection.ambient_color_energy.rgb * reflection.ambient_color_energy.a;
		float hit_alpha = 0.0;
		if (trace_reflection(start_world, reflected_direction, hit_voxel)) {
			vec3 hit_center = reflection.world_origin_voxel_size.xyz + (vec3(hit_voxel) + vec3(0.5)) * reflection.world_origin_voxel_size.w;
			vec3 albedo = sample_hit_albedo(hit_center);
			vec3 illumination = reflection.ambient_color_energy.rgb * reflection.ambient_color_energy.a + sample_indirect(hit_center) / 3.14159265;
			reflected_radiance = albedo * illumination;
			hit_alpha = 1.0;
		}
		quad_reflections[local_index] = vec4(reflected_radiance, hit_alpha);
	}
	barrier();

	if (voxel_receiver) {
		final_reflection = quad_reflections[leader];
	}
	if (output_pixel) {
		imageStore(reflection_output, pixel, final_reflection);
	}
}
