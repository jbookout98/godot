#[compute]

#version 450

#VERSION_DEFINES

#include "../oct_inc.glsl"

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct Surfel {
	vec4 position_distance;
	vec4 normal_valid;
	vec4 radiance;
};

struct ProbeRecord {
	ivec4 logical_cell_lod;
	vec4 physical_position_valid;
	uvec4 state_revision_frame_flags;
};

struct LocalLight {
	vec4 position_range;
	vec4 direction_type;
	vec4 color_energy;
	vec4 attenuation_source_size;
};

layout(set = 0, binding = 0, std430) writeonly buffer Surfels {
	Surfel values[];
}
surfels;
layout(set = 0, binding = 1, std430) readonly buffer ActiveProbeIndices {
	uint values[];
}
active_probe_indices;
layout(set = 0, binding = 2, std430) readonly buffer ProbeRecords {
	ProbeRecord values[];
}
probe_records;
layout(set = 0, binding = 3, std430) readonly buffer ProbeCounters {
	uint values[6];
}
probe_counters;
layout(set = 0, binding = 4, std430) readonly buffer WorldDirectory {
	uvec4 entries[];
}
world_directory;
layout(set = 0, binding = 5, std430) readonly buffer MixedBricks {
	uint words[];
}
mixed_bricks;
layout(set = 0, binding = 6) uniform sampler3D color_grid;
layout(set = 0, binding = 7) uniform sampler2D shadow_atlas;
layout(set = 0, binding = 8) uniform sampler2D irradiance_history_lod0;
layout(set = 0, binding = 9) uniform sampler2D visibility_history_lod0;
layout(set = 0, binding = 10) uniform sampler2D probe_metadata_lod0;
layout(set = 0, binding = 11) uniform sampler2D irradiance_history_lod1;
layout(set = 0, binding = 12) uniform sampler2D visibility_history_lod1;
layout(set = 0, binding = 13) uniform sampler2D probe_metadata_lod1;
layout(set = 0, binding = 14) uniform sampler2D irradiance_history_lod2;
layout(set = 0, binding = 15) uniform sampler2D visibility_history_lod2;
layout(set = 0, binding = 16) uniform sampler2D probe_metadata_lod2;
layout(set = 0, binding = 17) uniform sampler2D irradiance_history_lod3;
layout(set = 0, binding = 18) uniform sampler2D visibility_history_lod3;
layout(set = 0, binding = 19) uniform sampler2D probe_metadata_lod3;
layout(set = 0, binding = 26, std430) readonly buffer LocalLights {
	LocalLight values[];
}
local_lights;

#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 0, binding = 21) uniform sampler2DArray sky_radiance;
#else
layout(set = 0, binding = 21) uniform sampler2D sky_radiance;
#endif

layout(set = 0, binding = 20, std140) uniform Params {
	vec4 world_origin_voxel_size;
	vec4 grid_origin_max_distance;
	vec4 cell_size_lod;
	vec4 color_origin_cell_size;
	vec4 light_direction_energy;
	vec4 light_color_bounce;
	vec4 sky_color_energy;
	vec4 sky_orientation;
	vec4 sky_border_mode;
	vec4 atlas_center_resolution;
	vec4 tangent_near_extent;
	vec4 bitangent_far_extent;
	ivec4 probe_grid;
	ivec4 logical_origin;
	ivec4 directory_trace;
	ivec4 atlas_layout;
	ivec4 visibility_layout;
	ivec4 update_state;
	// Mature, dirty, and camera-exposed probe budgets are independent.
	ivec4 scheduling;
	vec4 query_settings;
	vec4 camera_position_lod_transition;
	vec4 cascade_origin[4];
	vec4 cascade_cell_size[4];
	ivec4 cascade_logical_origin[4];
	uvec4 local_light_state;
}
params;

#include "voxel_dynamic_trace_inc.glsl"

const float PI = 3.14159265358979323846;

vec3 ddgi_decode_interpolation_sample(vec3 encoded_irradiance) {
	return pow(max(encoded_irradiance, vec3(0.0)), vec3(2.5));
}

vec3 ddgi_finish_interpolation(vec3 sqrt_linear_irradiance) {
	return sqrt_linear_irradiance * sqrt_linear_irradiance;
}

uint brick_hash(ivec3 position) {
	return uint(position.x) * 73856093u ^ uint(position.y) * 19349663u ^ uint(position.z) * 83492791u;
}

uint find_brick(ivec3 position) {
	uint slot = brick_hash(position) & uint(params.directory_trace.x);
	for (int probe = 0; probe <= params.directory_trace.y; probe++) {
		uvec4 entry = world_directory.entries[slot];
		if (entry.w == 0u) {
			return 0u;
		}
		if (ivec3(entry.xyz) == position) {
			return entry.w;
		}
		slot = (slot + 1u) & uint(params.directory_trace.x);
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

uint probe_direction_hash(ivec4 logical_cell_lod) {
	uvec4 value = uvec4(logical_cell_lod);
	uint seed = value.x * 73856093u ^ value.y * 19349663u ^ value.z * 83492791u ^ value.w * 2654435761u;
	seed ^= seed >> 16u;
	seed *= 2246822519u;
	seed ^= seed >> 13u;
	return seed;
}

vec3 fibonacci_direction(uint ray_index, uint ray_count, ivec4 logical_cell_lod, uint update_sequence) {
	float count = float(max(ray_count, 1u));
	float z = 1.0 - 2.0 * (float(ray_index) + 0.5) / count;
	float radius = sqrt(max(0.0, 1.0 - z * z));
	uint rotation_hash = probe_direction_hash(logical_cell_lod) ^ (update_sequence * 2246822519u);
	float probe_rotation = float(rotation_hash & 65535u) * (2.0 * PI / 65536.0);
	float angle = float(ray_index) * 2.399963229728653 + probe_rotation;
	return vec3(cos(angle) * radius, z, sin(angle) * radius);
}

vec2 oct_encode(vec3 direction) {
	direction /= max(abs(direction.x) + abs(direction.y) + abs(direction.z), 0.00001);
	vec2 encoded = direction.xz;
	if (direction.y < 0.0) {
		encoded = (1.0 - abs(encoded.yx)) * sign(encoded.xy);
	}
	return encoded * 0.5 + 0.5;
}

vec3 sample_environment_radiance(vec3 direction) {
	if (params.sky_border_mode.z > 1.5) {
		vec3 rotated = cross(params.sky_orientation.xyz, direction);
		vec3 sky_direction = direction + ((rotated * params.sky_orientation.w) + cross(params.sky_orientation.xyz, rotated)) * 2.0;
		vec2 sky_uv = vec3_to_oct_with_border(sky_direction, params.sky_border_mode.xy);
#ifdef USE_RADIANCE_OCTMAP_ARRAY
		return textureLod(sky_radiance, vec3(sky_uv, 0.0), 2.0).rgb * params.sky_color_energy.w;
#else
		return textureLod(sky_radiance, sky_uv, 2.0).rgb * params.sky_color_energy.w;
#endif
	}
	if (params.sky_border_mode.z > 0.5) {
		return params.sky_color_energy.rgb * params.sky_color_energy.w;
	}
	return vec3(0.0);
}

ivec2 probe_tile(uint probe_index) {
	uint row_width = uint(params.probe_grid.x * params.probe_grid.x);
	return ivec2(int(probe_index % row_width), int(probe_index / row_width));
}

vec4 ddgi_fetch_metadata(int lod, ivec2 pixel) {
	if (lod == 0) {
		return texelFetch(probe_metadata_lod0, pixel, 0);
	}
	if (lod == 1) {
		return texelFetch(probe_metadata_lod1, pixel, 0);
	}
	if (lod == 2) {
		return texelFetch(probe_metadata_lod2, pixel, 0);
	}
	return texelFetch(probe_metadata_lod3, pixel, 0);
}

uint ddgi_probe_generation(ivec4 logical_cell_lod) {
	uvec4 bits = uvec4(logical_cell_lod);
	uint hash = 2166136261u;
	hash = (hash ^ bits.x) * 16777619u;
	hash = (hash ^ bits.y) * 16777619u;
	hash = (hash ^ bits.z) * 16777619u;
	hash = (hash ^ bits.w) * 16777619u;
	return hash & 0xfffffu;
}

bool ddgi_metadata_usable(float packed_value, ivec4 expected_logical_cell_lod) {
	uint packed = uint(max(packed_value, 0.0) + 0.5);
	uint state = packed & 15u;
	uint generation = packed >> 4u;
	bool usable_state = state == 1u || state == 2u || state == 3u || state == 4u || state == 5u || state == 6u || state == 7u || state == 8u;
	return usable_state && generation == ddgi_probe_generation(expected_logical_cell_lod);
}

vec4 ddgi_sample_visibility(int lod, vec2 uv) {
	if (lod == 0) {
		return textureLod(visibility_history_lod0, uv, 0.0);
	}
	if (lod == 1) {
		return textureLod(visibility_history_lod1, uv, 0.0);
	}
	if (lod == 2) {
		return textureLod(visibility_history_lod2, uv, 0.0);
	}
	return textureLod(visibility_history_lod3, uv, 0.0);
}

vec4 ddgi_sample_irradiance(int lod, vec2 uv) {
	if (lod == 0) {
		return textureLod(irradiance_history_lod0, uv, 0.0);
	}
	if (lod == 1) {
		return textureLod(irradiance_history_lod1, uv, 0.0);
	}
	if (lod == 2) {
		return textureLod(irradiance_history_lod2, uv, 0.0);
	}
	return textureLod(irradiance_history_lod3, uv, 0.0);
}

bool ddgi_cascade_contains(int lod, vec3 world_position) {
	vec3 local = (world_position - params.cascade_origin[lod].xyz) / params.cascade_cell_size[lod].xyz;
	float transition_cells = max(params.camera_position_lod_transition.w * float(params.probe_grid.x), 0.25);
	return all(greaterThanEqual(local, vec3(0.5 - transition_cells))) &&
			all(lessThanEqual(local, vec3(float(params.probe_grid.x) - 0.5 + transition_cells)));
}

vec3 ddgi_sample_previous_lod(int lod, vec3 world_position, vec3 normal, vec3 view_direction,
		out float edge_weight, out float initialized_support) {
	vec3 cell_size = params.cascade_cell_size[lod].xyz;
	float minimum_spacing = min(cell_size.x, min(cell_size.y, cell_size.z));
	vec3 bias_direction = mix(normal, view_direction, clamp(params.query_settings.y, 0.0, 1.0));
	vec3 bias = bias_direction * (0.75 * minimum_spacing) * params.query_settings.x;
	vec3 biased_position = world_position + bias;
	vec3 local = (biased_position - params.cascade_origin[lod].xyz) / cell_size - vec3(0.5);
	int resolution = params.probe_grid.x;
	vec3 interpolation_local = clamp(local, vec3(0.0), vec3(float(resolution) - 1.0001));
	ivec3 base = ivec3(floor(interpolation_local));
	vec3 fraction = fract(interpolation_local);
	vec3 sqrt_linear_sum = vec3(0.0);
	float weight_sum = 0.0;
	float basis_weight_sum = 0.0;
	float initialized_weight_sum = 0.0;
	for (int z = 0; z < 2; z++) {
		for (int y = 0; y < 2; y++) {
			for (int x = 0; x < 2; x++) {
				ivec3 local_probe = base + ivec3(x, y, z);
				if (any(lessThan(local_probe, ivec3(0))) || any(greaterThanEqual(local_probe, ivec3(resolution)))) {
					continue;
				}
				ivec3 logical_cell = params.cascade_logical_origin[lod].xyz + local_probe;
				ivec3 physical = ivec3((logical_cell.x % resolution + resolution) % resolution, (logical_cell.y % resolution + resolution) % resolution, (logical_cell.z % resolution + resolution) % resolution);
				uint index = uint(physical.x + physical.y * resolution + physical.z * resolution * resolution);
				ivec2 metadata_pixel = ivec2(int(index % uint(resolution * resolution)), int(index / uint(resolution * resolution)));
				vec4 metadata = ddgi_fetch_metadata(lod, metadata_pixel);
				vec3 corner_weight = mix(vec3(1.0) - fraction, fraction, vec3(x, y, z));
				float ideal_weight = corner_weight.x * corner_weight.y * corner_weight.z;
				ivec4 expected_logical = ivec4(params.cascade_logical_origin[lod].xyz + base + ivec3(x, y, z), lod);
				if (!ddgi_metadata_usable(metadata.w, expected_logical)) {
					basis_weight_sum += ideal_weight;
					continue;
				}
				// Relocation changes ray origins and geometric weighting, never the
				// logical trilinear basis. This is the same contract used by surface
				// shading and preserves a partition of unity at cell boundaries.
				float trilinear = ideal_weight;
				basis_weight_sum += trilinear;
				vec3 probe_to_surface = biased_position - metadata.xyz;
				float distance_to_surface = length(probe_to_surface);
				vec3 direction_to_surface = distance_to_surface > 0.0001 ? probe_to_surface / distance_to_surface : normal;
				float wrap_shading = (dot(normal, -direction_to_surface) + 1.0) * 0.5;
				float directional_weight = wrap_shading * wrap_shading + 0.2;
				ivec2 tile = probe_tile(index);
				vec2 visibility_pixel = vec2(tile * params.visibility_layout.x) + vec2(1.5) + oct_encode(direction_to_surface) * float(params.visibility_layout.w - 1);
				vec4 moments = ddgi_sample_visibility(lod, visibility_pixel / vec2(params.visibility_layout.yz));
				vec2 irradiance_pixel = vec2(tile * params.atlas_layout.x) + vec2(1.5) + oct_encode(normal) * float(params.atlas_layout.w - 1);
				vec4 irradiance = ddgi_sample_irradiance(lod, irradiance_pixel / vec2(params.atlas_layout.yz));
				if (irradiance.a <= 0.0 || moments.a <= 0.0 || moments.x <= 0.0) {
					continue;
				}
				float confidence = min(irradiance.a, moments.a);
				initialized_weight_sum += trilinear * confidence;
				float visibility_bias_distance = length(bias);
				float variance_floor = max(visibility_bias_distance * visibility_bias_distance * 0.0625, 0.000001);
				float variance = max(moments.y - moments.x * moments.x, variance_floor);
				float depth_visibility = 1.0;
				if (distance_to_surface > moments.x) {
					float delta = distance_to_surface - moments.x;
					depth_visibility = variance / (variance + delta * delta);
					depth_visibility = depth_visibility * depth_visibility * depth_visibility;
				}
				depth_visibility = max(0.05, depth_visibility);
				float geometric_visibility = max(directional_weight * depth_visibility, 0.000001);
				const float crush_threshold = 0.2;
				if (geometric_visibility < crush_threshold) {
					geometric_visibility *= geometric_visibility * geometric_visibility / (crush_threshold * crush_threshold);
				}
				float weight = trilinear * confidence * geometric_visibility;
				sqrt_linear_sum += ddgi_decode_interpolation_sample(irradiance.rgb) * weight;
				weight_sum += weight;
			}
		}
	}
	initialized_support = clamp(initialized_weight_sum / max(basis_weight_sum, 0.0001), 0.0, 1.0);
	vec3 probe_local = (biased_position - params.cascade_origin[lod].xyz) / cell_size;
	float transition_cells = max(params.camera_position_lod_transition.w * float(resolution), 0.25);
	vec3 outside_cells = max(max(vec3(0.5) - probe_local,
									 probe_local - vec3(float(resolution) - 0.5)),
			vec3(0.0));
	float outside_distance = max(outside_cells.x, max(outside_cells.y, outside_cells.z));
	edge_weight = 1.0 - smoothstep(0.0, transition_cells, outside_distance);
	return weight_sum > 0.00001 ? ddgi_finish_interpolation(sqrt_linear_sum / weight_sum) : vec3(0.0);
}

vec3 sample_previous_irradiance(vec3 world_position, vec3 normal, vec3 view_direction) {
	vec3 result = vec3(0.0);
	float remaining = 1.0;
	for (int lod = 0; lod < 4 && remaining > 0.001; lod++) {
		if (!ddgi_cascade_contains(lod, world_position)) {
			continue;
		}
		float edge_weight;
		float support;
		vec3 value = ddgi_sample_previous_lod(lod, world_position, normal, view_direction, edge_weight, support);
		float local_weight = clamp((lod == 3 ? 1.0 : edge_weight) * smoothstep(0.0, 1.0, support), 0.0, 1.0);
		float weight = remaining * local_weight;
		result += value * weight;
		remaining -= weight;
	}
	return result;
}

bool trace_voxels(vec3 probe_position, vec3 direction, out vec3 hit_position, out vec3 hit_normal, out float hit_distance, out bool hit_backface);

float shadow_visibility(vec3 world_position) {
	vec3 relative = world_position - params.atlas_center_resolution.xyz;
	float light_x = dot(relative, params.tangent_near_extent.xyz);
	float light_y = dot(relative, params.bitangent_far_extent.xyz);
	float edge_distance = max(abs(light_x), abs(light_y));
	float near_extent = params.tangent_near_extent.w;
	float far_extent = params.bitangent_far_extent.w;
	bool atlas_covers_receiver = params.query_settings.z >= 0.5 && edge_distance < far_extent;
	if (!atlas_covers_receiver) {
		// Missing or out-of-bounds shadow data is unknown, never proof that the
		// directional light is visible. Resolve the same world occupancy used by
		// DDGI rays so an unavailable atlas cannot inject unshadowed radiance into
		// enclosed surfaces. This path is bounded by max_trace_steps and executes
		// only where the cached atlas cannot answer the query.
		vec3 hit_position;
		vec3 hit_normal;
		float hit_distance;
		bool hit_backface;
		return trace_voxels(world_position, params.light_direction_energy.xyz, hit_position, hit_normal, hit_distance, hit_backface) ? 0.0 : 1.0;
	}
	float extent = edge_distance <= near_extent ? near_extent : far_extent;
	int resolution = int(params.atlas_center_resolution.w);
	ivec2 texel = ivec2(clamp((vec2(light_x, light_y) / (2.0 * extent) + vec2(0.5)) * float(resolution), vec2(0.0), vec2(float(resolution - 1))));
	if (extent > near_extent) {
		texel.x += resolution;
	}
	float receiver_depth = far_extent - dot(relative, params.light_direction_energy.xyz);
	float occluder_depth = texelFetch(shadow_atlas, texel, 0).r;
	float atlas_visibility = receiver_depth <= occluder_depth + params.world_origin_voxel_size.w ? 1.0 : 0.0;
	if (atlas_visibility <= 0.0 || dynamic_voxel_volumes.state.x == 0u) {
		return atlas_visibility;
	}
	// The directional atlas owns static world occupancy. Moving/rotated voxel
	// bodies are not baked into it, so test only that bounded dynamic BVH here.
	vec3 dynamic_hit_position;
	vec3 dynamic_hit_normal;
	float dynamic_hit_distance;
	bool dynamic_hit_backface;
	bool dynamic_blocked = dynamic_trace_voxels(world_position, params.light_direction_energy.xyz, params.grid_origin_max_distance.w,
			dynamic_hit_position, dynamic_hit_normal, dynamic_hit_distance, dynamic_hit_backface);
	return dynamic_blocked ? 0.0 : atlas_visibility;
}

vec4 sample_source_color(vec3 world_position) {
	vec3 uvw = (world_position - params.color_origin_cell_size.xyz) /
			(params.color_origin_cell_size.w * float(params.directory_trace.w));
	if (any(lessThan(uvw, vec3(0.0))) || any(greaterThanEqual(uvw, vec3(1.0)))) {
		return vec4(0.7, 0.7, 0.7, 0.0);
	}
	vec4 color = textureLod(color_grid, uvw, 0.0);
	return color.a < 0.5 ? vec4(0.7, 0.7, 0.7, 0.0) : vec4(color.rgb, max(color.a * 2.0 - 1.0, 0.0));
}

bool trace_static_voxels(vec3 start_world, vec3 direction, out vec3 hit_position, out vec3 hit_normal, out float hit_distance, out bool hit_backface) {
	vec3 origin = (start_world - params.world_origin_voxel_size.xyz) / params.world_origin_voxel_size.w;
	ivec3 cell = ivec3(floor(origin));
	bool previous_occupied = voxel_occupied(cell);
	hit_backface = false;
	ivec3 step_direction = ivec3(direction.x >= 0.0 ? 1 : -1, direction.y >= 0.0 ? 1 : -1, direction.z >= 0.0 ? 1 : -1);
	vec3 inverse_direction = 1.0 / max(abs(direction), vec3(0.000001)) * vec3(step_direction);
	vec3 next_boundary = vec3(cell + max(step_direction, ivec3(0)));
	vec3 next_t = (next_boundary - origin) * inverse_direction;
	vec3 delta_t = abs(inverse_direction);
	float max_t = params.grid_origin_max_distance.w / params.world_origin_voxel_size.w;
	for (int step = 0; step < 4096 && step < params.directory_trace.z; step++) {
		int axis = next_t.x <= next_t.y && next_t.x <= next_t.z ? 0 : (next_t.y <= next_t.z ? 1 : 2);
		float travel_t = next_t[axis];
		if (travel_t > max_t) {
			break;
		}
		cell[axis] += step_direction[axis];
		next_t[axis] += delta_t[axis];
		bool occupied = voxel_occupied(cell);
		if (occupied != previous_occupied) {
			hit_backface = previous_occupied && !occupied;
			hit_normal = vec3(0.0);
			hit_normal[axis] = hit_backface ? float(step_direction[axis]) : -float(step_direction[axis]);
			hit_distance = travel_t * params.world_origin_voxel_size.w;
			hit_position = start_world + direction * hit_distance;
			return true;
		}
		previous_occupied = occupied;
	}
	return false;
}

bool trace_voxels(vec3 start_world, vec3 direction, out vec3 hit_position, out vec3 hit_normal, out float hit_distance, out bool hit_backface) {
	bool found = trace_static_voxels(start_world, direction, hit_position, hit_normal, hit_distance, hit_backface);
	float maximum_distance = found ? hit_distance : params.grid_origin_max_distance.w;
	vec3 dynamic_position;
	vec3 dynamic_normal;
	float dynamic_distance;
	bool dynamic_backface;
	if (dynamic_trace_voxels(start_world, direction, maximum_distance, dynamic_position, dynamic_normal, dynamic_distance, dynamic_backface)) {
		found = true;
		hit_position = dynamic_position;
		hit_normal = dynamic_normal;
		hit_distance = dynamic_distance;
		hit_backface = dynamic_backface;
	}
	return found;
}

float local_light_attenuation(float distance_to_light, float range, float decay) {
	float normalized_distance = distance_to_light / max(range, params.world_origin_voxel_size.w);
	float smooth_range = max(1.0 - pow(normalized_distance, 4.0), 0.0);
	return smooth_range * smooth_range * pow(max(distance_to_light, params.world_origin_voxel_size.w), -max(decay, 0.0));
}

vec3 local_light_candidate(LocalLight light, vec3 source_position, vec3 hit_normal) {
	vec3 to_light = light.position_range.xyz - source_position;
	float distance_to_light = length(to_light);
	if (distance_to_light <= 0.00001 || distance_to_light >= light.position_range.w) {
		return vec3(0.0);
	}
	vec3 light_direction = to_light / distance_to_light;
	float facing = max(dot(hit_normal, light_direction), 0.0);
	if (facing <= 0.0) {
		return vec3(0.0);
	}
	// Area lights use a bounded center-sample approximation and preserve their
	// one-sided emission. Their exact dimensions remain available in zw for a
	// future multi-sample quality mode without changing this typed layout.
	if (light.direction_type.w > 1.5 && dot(light.direction_type.xyz, -light_direction) <= 0.0) {
		return vec3(0.0);
	}
	float attenuation = local_light_attenuation(distance_to_light, light.position_range.w, light.attenuation_source_size.x);
	return light.color_energy.rgb * (light.color_energy.w * facing * attenuation);
}

vec3 evaluate_local_lights(vec3 source_position, vec3 hit_normal, float selection_noise) {
	// Importance-sample one bounded local light per probe ray. This is an
	// unbiased estimator of the complete compact light list, but performs at most
	// one expensive voxel visibility trace instead of one per overlapping light.
	float total_weight = 0.0;
	uint candidate_count = 0u;
	uint selected_index = 0u;
	vec3 selected_candidate = vec3(0.0);
	for (uint light_index = 0u; light_index < params.local_light_state.x; light_index++) {
		vec3 candidate = local_light_candidate(local_lights.values[light_index], source_position, hit_normal);
		float weight = dot(candidate, vec3(0.2126, 0.7152, 0.0722));
		if (weight <= 0.000001) {
			continue;
		}
		total_weight += weight;
		candidate_count++;
	}
	if (candidate_count == 0u || total_weight <= 0.000001) {
		return vec3(0.0);
	}
	float target = clamp(selection_noise, 0.0, 0.999999) * total_weight;
	float accumulated = 0.0;
	float selected_weight = 0.0;
	for (uint light_index = 0u; light_index < params.local_light_state.x; light_index++) {
		vec3 candidate = local_light_candidate(local_lights.values[light_index], source_position, hit_normal);
		float weight = dot(candidate, vec3(0.2126, 0.7152, 0.0722));
		if (weight <= 0.000001) {
			continue;
		}
		accumulated += weight;
		if (accumulated >= target) {
			selected_index = light_index;
			selected_candidate = candidate;
			selected_weight = weight;
			break;
		}
	}
	LocalLight selected_light = local_lights.values[selected_index];
	vec3 to_light = selected_light.position_range.xyz - source_position;
	float distance_to_light = length(to_light);
	vec3 light_direction = to_light / max(distance_to_light, 0.00001);
	float visibility = 1.0;
	if (selected_light.attenuation_source_size.y > 0.5) {
		vec3 shadow_hit_position;
		vec3 shadow_hit_normal;
		float shadow_hit_distance;
		bool shadow_hit_backface;
		if (trace_voxels(source_position, light_direction, shadow_hit_position, shadow_hit_normal, shadow_hit_distance, shadow_hit_backface) &&
				shadow_hit_distance < distance_to_light - params.world_origin_voxel_size.w * 0.5) {
			visibility = 0.0;
		}
	}
	float selection_probability = selected_weight / total_weight;
	return selected_candidate * (visibility / max(selection_probability, 0.000001));
}

uint scheduled_probe_list_index(uint update_index) {
	uint segment_size = uint(params.probe_grid.x * params.probe_grid.x * params.probe_grid.x);
	uint dirty_count = min(probe_counters.values[3], segment_size);
	uint new_count = min(probe_counters.values[4], segment_size);
	uint mature_count = min(probe_counters.values[5], segment_size);
	uint dirty_updates = min(dirty_count, uint(params.scheduling.y));
	if (update_index < dirty_updates) {
		return (uint(params.update_state.w) + update_index) % max(dirty_count, 1u);
	}
	uint camera_update_index = update_index - dirty_updates;
	uint new_updates = min(new_count, uint(params.scheduling.z));
	if (camera_update_index < new_updates) {
		return segment_size + (uint(params.update_state.z) + camera_update_index) % max(new_count, 1u);
	}
	uint mature_update_index = camera_update_index - new_updates;
	uint mature_offset = (uint(params.update_state.z) + mature_update_index) % max(mature_count, 1u);
	return segment_size * 2u + mature_offset;
}

uint scheduled_probe_count() {
	uint segment_size = uint(params.probe_grid.x * params.probe_grid.x * params.probe_grid.x);
	uint dirty_count = min(probe_counters.values[3], segment_size);
	uint new_count = min(probe_counters.values[4], segment_size);
	uint mature_count = min(probe_counters.values[5], segment_size);
	uint dirty_updates = min(dirty_count, uint(params.scheduling.y));
	uint new_updates = min(new_count, uint(params.scheduling.z));
	uint mature_updates = min(mature_count, uint(params.scheduling.x));
	return dirty_updates + new_updates + mature_updates;
}

void main() {
	uint invocation = gl_GlobalInvocationID.x;
	uint ray_stride = uint(params.probe_grid.y);
	uint update_count = scheduled_probe_count();
	if (invocation >= ray_stride * update_count) {
		return;
	}
	uint update_index = invocation / ray_stride;
	uint ray_index = invocation - update_index * ray_stride;
	uint probe_index = active_probe_indices.values[scheduled_probe_list_index(update_index)];
	ProbeRecord record = probe_records.values[probe_index];
	uint state = record.state_revision_frame_flags.x;
	bool converging = state == 2u || state == 3u || state == 6u || state == 7u;
	uint ray_count = converging ? uint(params.update_state.y) : uint(params.update_state.x);
	if (ray_index >= ray_count) {
		return;
	}
	vec3 probe_position = record.physical_position_valid.xyz;
	// Rotation follows this stable logical probe's own update generation. Global
	// frame time made the sampling history depend on the path taken by the camera.
	uint update_sequence = record.state_revision_frame_flags.z + 1u;
	vec3 direction = fibonacci_direction(ray_index, ray_count, record.logical_cell_lod, update_sequence);
	vec3 hit_position;
	vec3 hit_normal;
	float hit_distance;
	bool hit_backface;
	Surfel result;
	if (!trace_voxels(probe_position, direction, hit_position, hit_normal, hit_distance, hit_backface)) {
		float miss_distance = params.grid_origin_max_distance.w;
		result.position_distance = vec4(probe_position + direction * miss_distance, miss_distance);
		result.normal_valid = vec4(-direction, 0.0);
		result.radiance = vec4(max(sample_environment_radiance(direction), vec3(0.0)), 1.0);
	} else if (hit_backface) {
		// Backfaces indicate the probe is inside or too close to geometry. They
		// contribute no light and use 20% distance to strongly reject leaking.
		result.position_distance = vec4(hit_position, hit_distance * 0.2);
		result.normal_valid = vec4(hit_normal, -1.0);
		result.radiance = vec4(0.0, 0.0, 0.0, 1.0);
	} else {
		vec3 source_position = hit_position + hit_normal * params.world_origin_voxel_size.w * 0.51;
		vec4 source_color = sample_source_color(hit_position - hit_normal * params.world_origin_voxel_size.w * 0.35);
		float facing = max(dot(hit_normal, params.light_direction_energy.xyz), 0.0);
		vec3 direct = params.light_color_bounce.rgb * (params.light_direction_energy.w * facing * shadow_visibility(source_position));
		uint local_light_hash = probe_direction_hash(record.logical_cell_lod) ^ (ray_index * 747796405u) ^ (update_sequence * 2891336453u);
		float local_light_noise = float(local_light_hash & 0x00ffffffu) / 16777216.0;
		direct += evaluate_local_lights(source_position, hit_normal, local_light_noise);
		vec3 bounced = sample_previous_irradiance(source_position, hit_normal, normalize(probe_position - source_position)) * params.light_color_bounce.w / PI;
		vec3 emissive = source_color.rgb * source_color.a;
		result.position_distance = vec4(hit_position, hit_distance);
		result.normal_valid = vec4(hit_normal, 1.0);
		result.radiance = vec4(clamp(source_color.rgb * (direct + bounced) + emissive, vec3(0.0), vec3(64.0)), 1.0);
	}
	surfels.values[invocation] = result;
}
