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

layout(set = 0, binding = 0, std430) readonly buffer TemporalInput {
	Reservoir values[];
}
temporal_input;
layout(set = 0, binding = 1, std430) writeonly buffer SpatialOutput {
	Reservoir values[];
}
spatial_output;
layout(rgba16f, set = 0, binding = 2) uniform writeonly image2D indirect_output;
layout(set = 0, binding = 3, std430) readonly buffer WorldDirectory {
	uvec4 entries[];
}
world_directory;
layout(set = 0, binding = 4, std430) readonly buffer MixedBricks {
	uint words[];
}
mixed_bricks;
layout(set = 0, binding = 5) uniform sampler3D color_grid_near;
layout(set = 0, binding = 6) uniform sampler3D color_grid_far;
layout(set = 0, binding = 7) uniform sampler3D color_grid_distant;
layout(set = 0, binding = 8, std430) readonly buffer SpatialHistory {
	Reservoir values[];
}
spatial_history;
layout(set = 0, binding = 9, std430) readonly buffer DirtyBricks {
	ivec4 values[];
}
dirty_bricks;

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
}
params;

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
	ivec3 half_voxel = ivec3(round((face_center - params.world_origin_voxel_size.xyz) * (2.0 / params.world_origin_voxel_size.w)));
	uint normal_axis_sign = uint(abs(face_normal.x) > 0.5 ? (face_normal.x > 0.0 ? 1 : 0) : (abs(face_normal.y) > 0.5 ? (face_normal.y > 0.0 ? 3 : 2) : (face_normal.z > 0.0 ? 5 : 4)));
	return hash_u32(uint(half_voxel.x) * 73856093u ^ uint(half_voxel.y) * 19349663u ^ uint(half_voxel.z) * 83492791u ^ normal_axis_sign * 0x9e3779b9u);
}

float random_float(inout uint state) {
	state = hash_u32(state);
	return float(state) * (1.0 / 4294967296.0);
}

float luminance(vec3 value) {
	return dot(max(value, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
}

bool finite_float(float value) {
	return !isnan(value) && !isinf(value);
}

bool finite_vec3(vec3 value) {
	return all(not(isnan(value))) && all(not(isinf(value)));
}

bool point_is_dirty(vec3 world_position) {
	if ((params.state.w & 1u) != 0u) {
		return true;
	}
	ivec3 voxel_position = ivec3(floor((world_position - params.world_origin_voxel_size.xyz) / params.world_origin_voxel_size.w));
	ivec3 brick = ivec3(floor(vec3(voxel_position) / 8.0));
	int dirty_count = int((params.state.w >> 16u) & 0x1ffu);
	for (int index = 0; index < dirty_count && index < 256; index++) {
		if (all(equal(brick, dirty_bricks.values[index].xyz))) {
			return true;
		}
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
			if (start_position[axis] < brick_min[axis] || start_position[axis] > brick_max[axis]) {
				return false;
			}
			continue;
		}
		float inverse_direction = 1.0 / segment[axis];
		float first = (brick_min[axis] - start_position[axis]) * inverse_direction;
		float second = (brick_max[axis] - start_position[axis]) * inverse_direction;
		enter = max(enter, min(first, second));
		exit = min(exit, max(first, second));
		if (exit < enter) {
			return false;
		}
	}
	return true;
}

bool reservoir_path_is_dirty(Reservoir reservoir) {
	if ((params.state.w & 1u) != 0u) {
		return true;
	}
	int dirty_count = int((params.state.w >> 16u) & 0x1ffu);
	if (dirty_count == 0) {
		return false;
	}
	vec3 sample_position = reservoir.sample_position_pdf.xyz;
	vec3 light_end = sample_position + params.light_direction_energy.xyz * params.limits.x;
	for (int index = 0; index < dirty_count && index < 256; index++) {
		ivec3 brick = dirty_bricks.values[index].xyz;
		if (dirty_box_intersects_segment(brick, reservoir.visible_position_valid.xyz, sample_position) ||
				dirty_box_intersects_segment(brick, sample_position, light_end)) {
			return true;
		}
	}
	return false;
}

uint brick_hash(ivec3 position) {
	return uint(position.x) * 73856093u ^ uint(position.y) * 19349663u ^ uint(position.z) * 83492791u;
}

uint find_brick(ivec3 position) {
	uint slot = brick_hash(position) & uint(params.trace.x);
	for (int probe = 0; probe <= params.trace.y; probe++) {
		uvec4 entry = world_directory.entries[slot];
		if (entry.w == 0u) {
			return 0u;
		}
		if (ivec3(entry.xyz) == position) {
			return entry.w;
		}
		slot = (slot + 1u) & uint(params.trace.x);
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

bool trace_occluded(vec3 start_world, vec3 direction, float maximum_distance) {
	if (maximum_distance <= params.world_origin_voxel_size.w * 0.25) {
		return false;
	}
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
		if (travel_t >= maximum_t) {
			break;
		}
		cell[axis] += step_direction[axis];
		next_t[axis] += delta_t[axis];
		if (voxel_occupied(cell)) {
			return true;
		}
	}
	return false;
}

vec4 sample_color_grid(sampler3D grid, int cascade, vec3 world_position) {
	vec4 definition = params.color_grid_origin_cell_size[cascade];
	vec3 uvw = (world_position - definition.xyz) / (definition.w * float(params.trace.w));
	if (any(lessThan(uvw, vec3(0.0))) || any(greaterThanEqual(uvw, vec3(1.0)))) {
		return vec4(-1.0);
	}
	return textureLod(grid, uvw, 0.0);
}

vec4 sample_surface_color(vec3 world_position) {
	vec4 color = sample_color_grid(color_grid_near, 0, world_position);
	if (color.x < 0.0) {
		color = sample_color_grid(color_grid_far, 1, world_position);
	}
	if (color.x < 0.0) {
		color = sample_color_grid(color_grid_distant, 2, world_position);
	}
	if (color.x < 0.0 || color.a < 0.5) {
		return vec4(0.7, 0.7, 0.7, 0.0);
	}
	return vec4(max(color.rgb, vec3(0.0)), max(color.a * 2.0 - 1.0, 0.0));
}

vec3 evaluate_outgoing_radiance(vec3 hit_position, vec3 hit_normal) {
	float voxel_size = params.world_origin_voxel_size.w;
	vec3 surface_position = hit_position + hit_normal * voxel_size * 0.51;
	vec4 surface_color = sample_surface_color(hit_position - hit_normal * voxel_size * 0.35);
	float facing = max(dot(hit_normal, params.light_direction_energy.xyz), 0.0);
	float visibility = facing > 0.0 && !trace_occluded(surface_position, params.light_direction_energy.xyz, params.limits.x) ? 1.0 : 0.0;
	vec3 direct = params.light_color_intensity.rgb * (params.light_direction_energy.w * facing * visibility);
	return clamp(surface_color.rgb * direct / PI + surface_color.rgb * surface_color.a, vec3(0.0), vec3(params.limits.w));
}

bool reservoir_valid(Reservoir reservoir) {
	return (reservoir.metadata.w & RESERVOIR_VALID) != 0u && reservoir.metadata.x > 0u && reservoir.sample_normal_target.w > 0.0 && reservoir.visible_normal_estimator.w > 0.0;
}

bool similar_surface(Reservoir center, Reservoir neighbor) {
	if (!reservoir_valid(neighbor)) {
		return false;
	}
	if (dot(center.visible_normal_estimator.xyz, neighbor.visible_normal_estimator.xyz) < params.validation.x) {
		return false;
	}
	vec3 delta = neighbor.visible_position_valid.xyz - center.visible_position_valid.xyz;
	return abs(dot(delta, center.visible_normal_estimator.xyz)) <= params.world_origin_voxel_size.w * params.limits.y;
}

bool reconnect_visible(vec3 visible_position, vec3 visible_normal, vec3 sample_position) {
	vec3 vector_to_sample = sample_position - visible_position;
	float distance_to_sample = length(vector_to_sample);
	if (distance_to_sample <= params.world_origin_voxel_size.w * 0.5) {
		return false;
	}
	vec3 direction = vector_to_sample / distance_to_sample;
	if (dot(visible_normal, direction) <= 0.0) {
		return false;
	}
	vec3 start = visible_position + visible_normal * params.world_origin_voxel_size.w * 0.51;
	return !trace_occluded(start, direction, distance_to_sample - params.world_origin_voxel_size.w * 0.75);
}

float reconnection_jacobian(Reservoir source, vec3 destination_position) {
	vec3 sample_position = source.sample_position_pdf.xyz;
	vec3 sample_normal = source.sample_normal_target.xyz;
	vec3 source_vector = source.visible_position_valid.xyz - sample_position;
	vec3 destination_vector = destination_position - sample_position;
	float source_distance_squared = dot(source_vector, source_vector);
	float destination_distance_squared = dot(destination_vector, destination_vector);
	float source_cosine = abs(dot(sample_normal, source_vector / max(sqrt(source_distance_squared), 0.000001)));
	float destination_cosine = abs(dot(sample_normal, destination_vector / max(sqrt(destination_distance_squared), 0.000001)));
	if (source_cosine <= 0.00001 || destination_cosine <= 0.00001 || destination_distance_squared <= 0.000001) {
		return 0.0;
	}
	return destination_cosine / source_cosine * source_distance_squared / destination_distance_squared;
}

ivec2 neighbor_pixel(ivec2 pixel, int iteration, inout uint rng) {
	float angle = random_float(rng) * 6.28318530718;
	float radius = sqrt(random_float(rng)) * float(params.reuse.y);
	ivec2 offset = ivec2(round(vec2(cos(angle), sin(angle)) * radius));
	if (all(equal(offset, ivec2(0)))) {
		offset.x = (iteration & 1) == 0 ? 1 : -1;
	}
	return clamp(pixel + offset, ivec2(0), params.screen.xy - 1);
}

void update_reservoir(inout Reservoir reservoir, Reservoir candidate, float candidate_weight, uint candidate_count, inout uint rng) {
	if (!finite_float(candidate_weight) || candidate_weight <= 0.0 || candidate_count == 0u) {
		return;
	}
	uint remaining = uint(params.reuse.w) > reservoir.metadata.x ? uint(params.reuse.w) - reservoir.metadata.x : 0u;
	uint accepted_count = min(candidate_count, remaining);
	if (accepted_count == 0u) {
		return;
	}
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
	if (any(greaterThanEqual(pixel, params.screen.xy))) {
		return;
	}
	uint index = uint(pixel.x + pixel.y * params.screen.x);
	Reservoir center = temporal_input.values[index];
	if (!reservoir_valid(center)) {
		spatial_output.values[index] = center;
		imageStore(indirect_output, pixel, vec4(0.0));
		return;
	}

	uint rng_seed = hash_u32(voxel_face_seed(center.visible_position_valid.xyz, center.visible_normal_estimator.xyz) ^ (params.state.x * 0xc2b2ae35u));
	// Keep neighborhood selection independent from weighted-reservoir draws.
	// The low-bias support recount below must visit exactly the same source
	// pixels as the merge pass, regardless of how many candidates were accepted.
	uint neighbor_rng = rng_seed;
	uint selection_rng = hash_u32(rng_seed ^ 0x68bc21ebu);
	// ReSTIR GI recommends fewer spatial iterations after reservoirs have
	// accumulated enough input samples. Respect the designer's configured upper
	// bound while converging to three neighbors once M reaches half its cap.
	int iteration_limit = min(params.reuse.x, center.metadata.x < uint(params.reuse.z / 2) ? 9 : 3);
	Reservoir result = center;
	result.metadata.x = 0u;
	result.sample_radiance_weight_sum.w = 0.0;
	float center_weight = center.sample_normal_target.w * center.visible_normal_estimator.w * float(center.metadata.x);
	update_reservoir(result, center, center_weight, center.metadata.x, selection_rng);
	int accepted_neighbors = 0;
	for (int iteration = 0; iteration < iteration_limit && iteration < 32; iteration++) {
		ivec2 neighbor = neighbor_pixel(pixel, iteration, neighbor_rng);
		Reservoir candidate = temporal_input.values[neighbor.x + neighbor.y * params.screen.x];
		if (!similar_surface(center, candidate)) {
			continue;
		}
		if (!reconnect_visible(center.visible_position_valid.xyz, center.visible_normal_estimator.xyz, candidate.sample_position_pdf.xyz)) {
			continue;
		}
		float jacobian = reconnection_jacobian(candidate, center.visible_position_valid.xyz);
		if (!finite_float(jacobian) || jacobian <= 0.00001) {
			continue;
		}
		float destination_target = max(luminance(candidate.sample_radiance_weight_sum.xyz), params.limits.z);
		float merge_weight = (destination_target / jacobian) * candidate.visible_normal_estimator.w * float(candidate.metadata.x);
		update_reservoir(result, candidate, merge_weight, candidate.metadata.x, selection_rng);
		accepted_neighbors++;
	}

	float normalization_count = float(max(result.metadata.x, 1u));
	if (params.validation.w < 0.5) {
		normalization_count = reconnect_visible(center.visible_position_valid.xyz, center.visible_normal_estimator.xyz, result.sample_position_pdf.xyz) ? float(center.metadata.x) : 0.0;
		uint support_rng = rng_seed;
		for (int iteration = 0; iteration < iteration_limit && iteration < 32; iteration++) {
			ivec2 neighbor = neighbor_pixel(pixel, iteration, support_rng);
			Reservoir source = temporal_input.values[neighbor.x + neighbor.y * params.screen.x];
			if (!similar_surface(center, source)) {
				continue;
			}
			if (reconnect_visible(source.visible_position_valid.xyz, source.visible_normal_estimator.xyz, result.sample_position_pdf.xyz)) {
				normalization_count += float(source.metadata.x);
			}
		}
	}

	vec3 outgoing = evaluate_outgoing_radiance(result.sample_position_pdf.xyz, result.sample_normal_target.xyz);
	vec3 to_sample = result.sample_position_pdf.xyz - center.visible_position_valid.xyz;
	float distance_to_sample = length(to_sample);
	float cosine = distance_to_sample > 0.000001 ? max(dot(center.visible_normal_estimator.xyz, to_sample / distance_to_sample), 0.0) : 0.0;
	float target = max(luminance(outgoing), params.limits.z);
	float estimator_weight = normalization_count > 0.0 && target > 0.0 ? result.sample_radiance_weight_sum.w / (normalization_count * target) : 0.0;
	vec3 irradiance = outgoing * (cosine * estimator_weight);
	bool valid = estimator_weight > 0.0 && finite_float(estimator_weight) && all(not(isnan(irradiance))) && all(not(isinf(irradiance)));
	if (!valid) {
		irradiance = vec3(0.0);
	}
	irradiance = clamp(irradiance, vec3(0.0), vec3(params.limits.w));

	uint radiance_history_count = 1u;
	uint maximum_history = max((params.state.w >> 9u) & 0x7fu, 1u);
	if (valid && params.state.y != 0u && !point_is_dirty(center.visible_position_valid.xyz) && !point_is_dirty(result.sample_position_pdf.xyz)) {
		vec4 previous_clip = params.previous_view_projection * vec4(center.visible_position_valid.xyz, 1.0);
		if (previous_clip.w > 0.000001) {
			ivec2 previous_pixel = ivec2(floor((previous_clip.xy / previous_clip.w * 0.5 + 0.5) * vec2(params.screen.xy)));
			if (all(greaterThanEqual(previous_pixel, ivec2(0))) && all(lessThan(previous_pixel, params.screen.xy))) {
				Reservoir history = spatial_history.values[previous_pixel.x + previous_pixel.y * params.screen.x];
				bool compatible = (history.metadata.w & RESERVOIR_VALID) != 0u && finite_vec3(history.sample_radiance_weight_sum.xyz);
				compatible = compatible && distance(history.visible_position_valid.xyz, center.visible_position_valid.xyz) <= params.world_origin_voxel_size.w * params.limits.y;
				compatible = compatible && dot(history.visible_normal_estimator.xyz, center.visible_normal_estimator.xyz) >= params.validation.x;
				compatible = compatible && !point_is_dirty(history.visible_position_valid.xyz) && !point_is_dirty(history.sample_position_pdf.xyz) && !reservoir_path_is_dirty(history);
				if (compatible) {
					uint previous_count = clamp((history.metadata.w >> 8u) & 0x7fu, 1u, maximum_history);
					radiance_history_count = min(previous_count + 1u, maximum_history);
					irradiance = mix(history.sample_radiance_weight_sum.xyz, irradiance, 1.0 / float(radiance_history_count));
				}
			}
		}
	}

	result.sample_radiance_weight_sum.xyz = irradiance;
	result.sample_normal_target.w = target;
	result.visible_position_valid = center.visible_position_valid;
	result.visible_normal_estimator = vec4(center.visible_normal_estimator.xyz, estimator_weight);
	result.metadata.z = params.state.z;
	result.metadata.w = valid ? RESERVOIR_VALID | (radiance_history_count << 8u) : 0u;
	spatial_output.values[index] = result;

	int debug_mode = int((params.state.w >> 1u) & 0xffu);
	vec3 output_value = irradiance;
	if (debug_mode == 1) {
		output_value = outgoing / (vec3(1.0) + outgoing);
	} else if (debug_mode == 2) {
		output_value = vec3(clamp(float(result.metadata.x) / float(max(params.reuse.w, 1)), 0.0, 1.0));
	} else if (debug_mode == 3) {
		output_value = vec3(clamp(estimator_weight * 0.1, 0.0, 1.0));
	} else if (debug_mode == 4) {
		output_value = vec3(clamp(float(result.metadata.y) / float(max(params.reuse.z, 1)), 0.0, 1.0), 0.0, 0.0);
	} else if (debug_mode == 5) {
		output_value = vec3(0.0, clamp(float(accepted_neighbors) / float(max(params.reuse.x, 1)), 0.0, 1.0), 0.0);
	}
	imageStore(indirect_output, pixel, vec4(output_value, valid ? 1.0 : 0.0));
}
