#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

struct Reservoir {
	vec4 sample_position_pdf;
	vec4 sample_normal_target;
	vec4 sample_radiance_weight_sum;
	vec4 visible_position_valid;
	vec4 visible_normal_estimator;
	uvec4 metadata;
};

layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
layout(set = 0, binding = 1) uniform usampler2D voxel_hit_buffer;
layout(set = 0, binding = 2, std430) readonly buffer TemporalHistory { Reservoir values[]; } temporal_history;
layout(set = 0, binding = 3, std430) writeonly buffer TemporalOutput { Reservoir values[]; } temporal_output;
layout(set = 0, binding = 4, std430) readonly buffer WorldDirectory { uvec4 entries[]; } world_directory;
layout(set = 0, binding = 5, std430) readonly buffer MixedBricks { uint words[]; } mixed_bricks;
layout(set = 0, binding = 6) uniform sampler3D color_grid_near;
layout(set = 0, binding = 7) uniform sampler3D color_grid_far;
layout(set = 0, binding = 8) uniform sampler3D color_grid_distant;
layout(set = 0, binding = 9, std430) readonly buffer DirtyBricks { ivec4 values[]; } dirty_bricks;

layout(set = 0, binding = 10, std140) uniform Params {
	mat4 inv_view_projection;
	mat4 previous_view_projection;
	vec4 world_origin_voxel_size;
	vec4 light_direction_energy;
	vec4 light_color_intensity;
	vec4 ambient_color_energy;
	vec4 color_grid_origin_cell_size[3];
	ivec4 screen;
	ivec4 trace;
	uvec4 state;
	ivec4 reuse;
	vec4 limits;
	vec4 validation;
} params;

const float PI = 3.14159265358979323846;
const uint RESERVOIR_VALID = 1u;

uint hash_u32(uint value) {
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	value *= 0x846ca68bu;
	value ^= value >> 16u;
	return value;
}

uint voxel_face_seed(vec3 face_center, vec3 face_normal) {
	// Canonical face centers lie on a half-voxel lattice. This key remains
	// unchanged as the camera moves, so every screen pixel covering one voxel
	// face follows the same candidate sequence instead of exposing unrelated
	// per-pixel histories when the projected center crosses a texel boundary.
	ivec3 half_voxel = ivec3(round((face_center - params.world_origin_voxel_size.xyz) * (2.0 / params.world_origin_voxel_size.w)));
	uint normal_axis_sign = uint(abs(face_normal.x) > 0.5 ? (face_normal.x > 0.0 ? 1 : 0) : (abs(face_normal.y) > 0.5 ? (face_normal.y > 0.0 ? 3 : 2) : (face_normal.z > 0.0 ? 5 : 4)));
	return hash_u32(uint(half_voxel.x) * 73856093u ^ uint(half_voxel.y) * 19349663u ^ uint(half_voxel.z) * 83492791u ^ normal_axis_sign * 0x9e3779b9u);
}

float random_float(inout uint state) {
	state = hash_u32(state);
	return float(state) * (1.0 / 4294967296.0);
}

bool finite_float(float value) {
	return !isnan(value) && !isinf(value);
}

bool finite_vec3(vec3 value) {
	return all(not(isnan(value))) && all(not(isinf(value)));
}

float luminance(vec3 value) {
	return dot(max(value, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
}

uint brick_hash(ivec3 position) {
	return uint(position.x) * 73856093u ^ uint(position.y) * 19349663u ^ uint(position.z) * 83492791u;
}

uint find_brick(ivec3 position) {
	uint slot = brick_hash(position) & uint(params.trace.x);
	for (int probe = 0; probe <= params.trace.y; probe++) {
		uvec4 entry = world_directory.entries[slot];
		if (entry.w == 0u) return 0u;
		if (ivec3(entry.xyz) == position) return entry.w;
		slot = (slot + 1u) & uint(params.trace.x);
	}
	return 0u;
}

bool voxel_occupied(ivec3 voxel_position) {
	ivec3 brick_position = ivec3(floor(vec3(voxel_position) / 8.0));
	uint code = find_brick(brick_position);
	if (code == 0u) return false;
	if (code == 1u) return true;
	ivec3 local_voxel = voxel_position - brick_position * 8;
	uint local_index = uint(local_voxel.x + local_voxel.y * 8 + local_voxel.z * 64);
	uint word = mixed_bricks.words[(code - 2u) * 16u + (local_index >> 5u)];
	return (word & (1u << (local_index & 31u))) != 0u;
}

bool trace_surface(vec3 start_world, vec3 direction, out vec3 hit_position, out vec3 hit_normal, out float hit_distance) {
	vec3 origin = (start_world - params.world_origin_voxel_size.xyz) / params.world_origin_voxel_size.w;
	ivec3 cell = ivec3(floor(origin));
	bool previous_occupied = voxel_occupied(cell);
	ivec3 step_direction = ivec3(direction.x >= 0.0 ? 1 : -1, direction.y >= 0.0 ? 1 : -1, direction.z >= 0.0 ? 1 : -1);
	vec3 inverse_direction = vec3(step_direction) / max(abs(direction), vec3(0.000001));
	vec3 next_boundary = vec3(cell + max(step_direction, ivec3(0)));
	vec3 next_t = (next_boundary - origin) * inverse_direction;
	vec3 delta_t = abs(inverse_direction);
	float maximum_t = params.limits.x / params.world_origin_voxel_size.w;
	for (int step = 0; step < 4096 && step < params.trace.z; step++) {
		int axis = next_t.x <= next_t.y && next_t.x <= next_t.z ? 0 : (next_t.y <= next_t.z ? 1 : 2);
		float travel_t = next_t[axis];
		if (travel_t > maximum_t) break;
		cell[axis] += step_direction[axis];
		next_t[axis] += delta_t[axis];
		bool occupied = voxel_occupied(cell);
		if (occupied != previous_occupied) {
			if (previous_occupied && !occupied) return false;
			hit_normal = vec3(0.0);
			hit_normal[axis] = -float(step_direction[axis]);
			hit_distance = travel_t * params.world_origin_voxel_size.w;
			hit_position = start_world + direction * hit_distance;
			return true;
		}
		previous_occupied = occupied;
	}
	return false;
}

bool trace_occluded(vec3 start_world, vec3 direction, float maximum_distance) {
	if (maximum_distance <= params.world_origin_voxel_size.w * 0.25) return false;
	vec3 origin = (start_world - params.world_origin_voxel_size.xyz) / params.world_origin_voxel_size.w;
	ivec3 cell = ivec3(floor(origin));
	ivec3 step_direction = ivec3(direction.x >= 0.0 ? 1 : -1, direction.y >= 0.0 ? 1 : -1, direction.z >= 0.0 ? 1 : -1);
	vec3 inverse_direction = vec3(step_direction) / max(abs(direction), vec3(0.000001));
	vec3 next_boundary = vec3(cell + max(step_direction, ivec3(0)));
	vec3 next_t = (next_boundary - origin) * inverse_direction;
	vec3 delta_t = abs(inverse_direction);
	float maximum_t = maximum_distance / params.world_origin_voxel_size.w;
	for (int step = 0; step < 4096 && step < params.trace.z; step++) {
		int axis = next_t.x <= next_t.y && next_t.x <= next_t.z ? 0 : (next_t.y <= next_t.z ? 1 : 2);
		float travel_t = next_t[axis];
		if (travel_t >= maximum_t) break;
		cell[axis] += step_direction[axis];
		next_t[axis] += delta_t[axis];
		if (voxel_occupied(cell)) return true;
	}
	return false;
}

vec4 sample_color_grid(sampler3D grid, int cascade, vec3 world_position) {
	vec4 definition = params.color_grid_origin_cell_size[cascade];
	vec3 uvw = (world_position - definition.xyz) / (definition.w * float(params.trace.w));
	if (any(lessThan(uvw, vec3(0.0))) || any(greaterThanEqual(uvw, vec3(1.0)))) return vec4(-1.0);
	return textureLod(grid, uvw, 0.0);
}

vec4 sample_surface_color(vec3 world_position) {
	vec4 color = sample_color_grid(color_grid_near, 0, world_position);
	if (color.x < 0.0) color = sample_color_grid(color_grid_far, 1, world_position);
	if (color.x < 0.0) color = sample_color_grid(color_grid_distant, 2, world_position);
	if (color.x < 0.0 || color.a < 0.5) return vec4(0.7, 0.7, 0.7, 0.0);
	return vec4(max(color.rgb, vec3(0.0)), max(color.a * 2.0 - 1.0, 0.0));
}

vec3 evaluate_outgoing_radiance(vec3 hit_position, vec3 hit_normal) {
	float voxel_size = params.world_origin_voxel_size.w;
	vec3 surface_position = hit_position + hit_normal * voxel_size * 0.51;
	vec4 surface_color = sample_surface_color(hit_position - hit_normal * voxel_size * 0.35);
	float facing = max(dot(hit_normal, params.light_direction_energy.xyz), 0.0);
	float visibility = facing > 0.0 && !trace_occluded(surface_position, params.light_direction_energy.xyz, params.limits.x) ? 1.0 : 0.0;
	vec3 direct = params.light_color_intensity.rgb * (params.light_direction_energy.w * facing * visibility);
	vec3 emissive = surface_color.rgb * surface_color.a;
	return clamp(surface_color.rgb * direct / PI + emissive, vec3(0.0), vec3(params.limits.w));
}

vec3 receiver_normal(uint payload) {
	uint face_code = (payload >> 11u) & 0x7u;
	if (face_code >= 6u) return vec3(0.0);
	vec3 normal = vec3(0.0);
	int axis = int(face_code >> 1u);
	normal[axis] = (face_code & 1u) != 0u ? 1.0 : -1.0;
	return normal;
}

vec3 receiver_face_center(vec3 world_position, vec3 normal) {
	vec3 voxel_position = (world_position - params.world_origin_voxel_size.xyz) / params.world_origin_voxel_size.w;
	ivec3 occupied_voxel = ivec3(floor(voxel_position - normal * 0.0001));
	vec3 center = vec3(occupied_voxel) + vec3(0.5);
	int axis = abs(normal.x) > 0.5 ? 0 : (abs(normal.y) > 0.5 ? 1 : 2);
	center[axis] = float(occupied_voxel[axis]) + (normal[axis] > 0.0 ? 1.0 : 0.0);
	return params.world_origin_voxel_size.xyz + center * params.world_origin_voxel_size.w;
}

vec3 uniform_hemisphere(vec3 normal, inout uint rng) {
	float u = random_float(rng);
	float phi = 2.0 * PI * random_float(rng);
	float radius = sqrt(max(0.0, 1.0 - u * u));
	vec3 tangent = abs(normal.z) < 0.999 ? normalize(cross(normal, vec3(0.0, 0.0, 1.0))) : vec3(1.0, 0.0, 0.0);
	vec3 bitangent = cross(normal, tangent);
	return normalize(tangent * (radius * cos(phi)) + bitangent * (radius * sin(phi)) + normal * u);
}

bool point_is_dirty(vec3 world_position) {
	if ((params.state.w & 1u) != 0u) return true;
	ivec3 voxel_position = ivec3(floor((world_position - params.world_origin_voxel_size.xyz) / params.world_origin_voxel_size.w));
	ivec3 brick = ivec3(floor(vec3(voxel_position) / 8.0));
	int dirty_count = int((params.state.w >> 16u) & 0x1ffu);
	for (int index = 0; index < dirty_count && index < 256; index++) {
		if (all(equal(brick, dirty_bricks.values[index].xyz))) return true;
	}
	return false;
}

bool dirty_box_intersects_segment(ivec3 brick, vec3 start_position, vec3 end_position) {
	vec3 brick_min = params.world_origin_voxel_size.xyz + vec3(brick * 8) * params.world_origin_voxel_size.w;
	vec3 brick_max = brick_min + vec3(8.0 * params.world_origin_voxel_size.w);
	vec3 segment = end_position - start_position;
	float enter = 0.0;
	float exit = 1.0;
	for (int axis = 0; axis < 3; axis++) {
		if (abs(segment[axis]) <= 0.000001) {
			if (start_position[axis] < brick_min[axis] || start_position[axis] > brick_max[axis]) return false;
			continue;
		}
		float inverse_direction = 1.0 / segment[axis];
		float first = (brick_min[axis] - start_position[axis]) * inverse_direction;
		float second = (brick_max[axis] - start_position[axis]) * inverse_direction;
		enter = max(enter, min(first, second));
		exit = min(exit, max(first, second));
		if (exit < enter) return false;
	}
	return true;
}

bool reservoir_path_is_dirty(Reservoir reservoir) {
	if ((params.state.w & 1u) != 0u) return true;
	int dirty_count = int((params.state.w >> 16u) & 0x1ffu);
	if (dirty_count == 0) return false;
	vec3 sample_position = reservoir.sample_position_pdf.xyz;
	vec3 light_end = sample_position + params.light_direction_energy.xyz * params.limits.x;
	for (int index = 0; index < dirty_count && index < 256; index++) {
		ivec3 brick = dirty_bricks.values[index].xyz;
		if (dirty_box_intersects_segment(brick, reservoir.visible_position_valid.xyz, sample_position) ||
				dirty_box_intersects_segment(brick, sample_position, light_end)) return true;
	}
	return false;
}

Reservoir empty_reservoir() {
	Reservoir reservoir;
	reservoir.sample_position_pdf = vec4(0.0);
	reservoir.sample_normal_target = vec4(0.0);
	reservoir.sample_radiance_weight_sum = vec4(0.0);
	reservoir.visible_position_valid = vec4(0.0);
	reservoir.visible_normal_estimator = vec4(0.0);
	reservoir.metadata = uvec4(0u);
	return reservoir;
}

void update_reservoir(inout Reservoir reservoir, Reservoir candidate, float candidate_weight, uint candidate_count, inout uint rng) {
	if (!finite_float(candidate_weight) || candidate_weight <= 0.0 || candidate_count == 0u) return;
	uint remaining = uint(params.reuse.z) > reservoir.metadata.x ? uint(params.reuse.z) - reservoir.metadata.x : 0u;
	uint accepted_count = min(candidate_count, remaining);
	if (accepted_count == 0u) return;
	candidate_weight *= float(accepted_count) / float(candidate_count);
	float previous_sum = reservoir.sample_radiance_weight_sum.w;
	float next_sum = min(previous_sum + candidate_weight, 1e30);
	reservoir.metadata.x += accepted_count;
	if (previous_sum <= 0.0 || random_float(rng) * next_sum < candidate_weight) {
		reservoir.sample_position_pdf = candidate.sample_position_pdf;
		reservoir.sample_normal_target = candidate.sample_normal_target;
		reservoir.sample_radiance_weight_sum.xyz = candidate.sample_radiance_weight_sum.xyz;
		reservoir.metadata.y = candidate.metadata.y;
		reservoir.metadata.w = candidate.metadata.w;
	}
	reservoir.sample_radiance_weight_sum.w = next_sum;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen.xy))) return;
	uint output_index = uint(pixel.x + pixel.y * params.screen.x);
	ivec2 depth_pixel = clamp(ivec2((vec2(pixel) + vec2(0.5)) * vec2(params.screen.zw) / vec2(params.screen.xy)), ivec2(0), params.screen.zw - 1);
	float depth = texelFetch(depth_buffer, depth_pixel, 0).r;
	uint payload = texelFetch(voxel_hit_buffer, depth_pixel, 0).r;
	vec3 normal = receiver_normal(payload);
	if (depth <= 0.000001 || payload == 0u || dot(normal, normal) < 0.5) {
		temporal_output.values[output_index] = empty_reservoir();
		return;
	}

	vec2 ndc = (vec2(depth_pixel) + vec2(0.5)) / vec2(params.screen.zw) * 2.0 - 1.0;
	vec4 world_h = params.inv_view_projection * vec4(ndc, depth, 1.0);
	if (abs(world_h.w) <= 0.000001) {
		temporal_output.values[output_index] = empty_reservoir();
		return;
	}
	vec3 visible_position = receiver_face_center(world_h.xyz / world_h.w, normal);
	uint rng = hash_u32(voxel_face_seed(visible_position, normal) ^ (params.state.x * 0x85ebca6bu));

	Reservoir result = empty_reservoir();
	Reservoir current = empty_reservoir();
	vec3 direction = uniform_hemisphere(normal, rng);
	vec3 sample_position;
	vec3 sample_normal;
	float sample_distance;
	if (trace_surface(visible_position + normal * params.world_origin_voxel_size.w * 0.51, direction, sample_position, sample_normal, sample_distance)) {
		vec3 radiance = evaluate_outgoing_radiance(sample_position, sample_normal);
		float target = max(luminance(radiance), params.limits.z);
		current.sample_position_pdf = vec4(sample_position, 1.0 / (2.0 * PI));
		current.sample_normal_target = vec4(sample_normal, target);
		current.sample_radiance_weight_sum = vec4(radiance, 0.0);
		current.metadata = uvec4(1u, 0u, params.state.z, RESERVOIR_VALID);
		update_reservoir(result, current, target / current.sample_position_pdf.w, 1u, rng);
	}

	if (params.state.y != 0u) {
		vec4 previous_clip = params.previous_view_projection * vec4(visible_position, 1.0);
		if (previous_clip.w > 0.000001) {
			vec2 previous_uv = previous_clip.xy / previous_clip.w * 0.5 + 0.5;
			ivec2 previous_pixel = ivec2(floor(previous_uv * vec2(params.screen.xy)));
			if (all(greaterThanEqual(previous_pixel, ivec2(0))) && all(lessThan(previous_pixel, params.screen.xy))) {
				Reservoir history = temporal_history.values[previous_pixel.x + previous_pixel.y * params.screen.x];
				// The temporal limit bounds the effective stream length, not the
				// lifetime of a selected sample. Expiring a reservoir solely because
				// of age creates periodic holes whenever the current hemisphere ray
				// misses. Keeping it eligible is still bounded: one slot is reserved
				// below for the current candidate and retained_count is capped.
				bool valid = (history.metadata.w & RESERVOIR_VALID) != 0u && history.metadata.x > 0u;
				valid = valid && distance(history.visible_position_valid.xyz, visible_position) <= params.world_origin_voxel_size.w * params.limits.y;
				valid = valid && dot(history.visible_normal_estimator.xyz, normal) >= params.validation.x;
				valid = valid && !point_is_dirty(history.visible_position_valid.xyz) && !point_is_dirty(history.sample_position_pdf.xyz) && !reservoir_path_is_dirty(history);
				if (valid && params.validation.y > 0.0 && (params.state.x % uint(params.validation.y)) == 0u) {
					vec3 refreshed = evaluate_outgoing_radiance(history.sample_position_pdf.xyz, history.sample_normal_target.xyz);
					float previous_luminance = luminance(history.sample_radiance_weight_sum.xyz);
					float relative_delta = abs(luminance(refreshed) - previous_luminance) / max(previous_luminance, 0.01);
					valid = finite_vec3(refreshed) && relative_delta <= params.validation.z;
					if (valid) {
						history.sample_radiance_weight_sum.xyz = refreshed;
						history.sample_normal_target.w = max(luminance(refreshed), params.limits.z);
					}
				}
				if (valid) {
					uint retained_count = min(history.metadata.x, uint(max(params.reuse.z - 1, 0)));
					float merge_weight = history.sample_normal_target.w * history.visible_normal_estimator.w * float(retained_count);
					history.metadata.x = retained_count;
					history.metadata.y = min(history.metadata.y + 1u, uint(params.reuse.z));
					update_reservoir(result, history, merge_weight, retained_count, rng);
				}
			}
		}
	}

	result.visible_position_valid = vec4(visible_position, 1.0);
	result.visible_normal_estimator.xyz = normal;
	result.metadata.z = params.state.z;
	if (result.metadata.x > 0u && result.sample_normal_target.w > 0.0 && result.sample_radiance_weight_sum.w > 0.0) {
		result.visible_normal_estimator.w = result.sample_radiance_weight_sum.w / (float(result.metadata.x) * result.sample_normal_target.w);
		result.metadata.w |= RESERVOIR_VALID;
	} else {
		result = empty_reservoir();
	}
	temporal_output.values[output_index] = result;
}
