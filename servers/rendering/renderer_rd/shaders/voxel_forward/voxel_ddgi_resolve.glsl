#[compute]

#version 450

// Full-resolution DDGI gather for visible voxel faces. The depth prepass writes
// one canonical world-space face center and an encoded geometric normal. Every
// matching face in an 8x8 workgroup therefore evaluates once and broadcasts the
// result instead of repeating up to 32 probe samples per covered pixel.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D voxel_face_data;
layout(set = 0, binding = 1) uniform usampler2D voxel_hit_buffer;
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D output_gi;

layout(set = 0, binding = 3) uniform sampler2D irradiance_lod0;
layout(set = 0, binding = 4) uniform sampler2D irradiance_lod1;
layout(set = 0, binding = 5) uniform sampler2D irradiance_lod2;
layout(set = 0, binding = 6) uniform sampler2D irradiance_lod3;
layout(set = 0, binding = 7) uniform sampler2D visibility_lod0;
layout(set = 0, binding = 8) uniform sampler2D visibility_lod1;
layout(set = 0, binding = 9) uniform sampler2D visibility_lod2;
layout(set = 0, binding = 10) uniform sampler2D visibility_lod3;
layout(set = 0, binding = 11) uniform sampler2D metadata_lod0;
layout(set = 0, binding = 12) uniform sampler2D metadata_lod1;
layout(set = 0, binding = 13) uniform sampler2D metadata_lod2;
layout(set = 0, binding = 14) uniform sampler2D metadata_lod3;

layout(set = 0, binding = 15, std140) uniform Params {
	vec4 origins[4];
	vec4 cell_sizes[4];
	ivec4 phase_offsets[4];
	ivec4 logical_origins[4];
	vec4 camera_irradiance_size;
	vec4 atlas_sizes;
	vec4 tuning;
	ivec4 screen_resolution_debug;
} params;

shared uvec4 face_keys[64];
shared vec4 face_results[64];

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

vec3 decode_interpolation_sample(vec3 encoded_irradiance) {
	return pow(max(encoded_irradiance, vec3(0.0)), vec3(2.5));
}

vec3 finish_interpolation(vec3 sqrt_linear_irradiance) {
	return sqrt_linear_irradiance * sqrt_linear_irradiance;
}

vec2 oct_encode(vec3 direction) {
	direction /= max(abs(direction.x) + abs(direction.y) + abs(direction.z), 0.00001);
	vec2 encoded = direction.xz;
	if (direction.y < 0.0) {
		encoded = (1.0 - abs(encoded.yx)) * sign(encoded.xy);
	}
	return encoded * 0.5 + 0.5;
}

vec2 tile_uv(ivec2 tile, int tile_size, int interior_size, vec2 oct, vec2 atlas_size) {
	vec2 pixel = vec2(tile * tile_size) + vec2(1.5) + clamp(oct, vec2(0.0), vec2(1.0)) * float(interior_size - 1);
	return pixel / atlas_size;
}

uint probe_generation(ivec4 logical_cell_lod) {
	uvec4 bits = uvec4(logical_cell_lod);
	uint hash = 2166136261u;
	hash = (hash ^ bits.x) * 16777619u;
	hash = (hash ^ bits.y) * 16777619u;
	hash = (hash ^ bits.z) * 16777619u;
	hash = (hash ^ bits.w) * 16777619u;
	return hash & 0xfffffu;
}

vec3 probe_sample(sampler2D irradiance_atlas, sampler2D visibility_atlas, sampler2D metadata_atlas,
		ivec3 probe, ivec3 phase_offset, ivec3 logical_origin, int lod, vec3 biased_position,
		vec3 world_normal, float visibility_bias_distance, out float visibility_weight,
		out float support_weight, out float geometric_visibility, out float in_front_weight) {
	visibility_weight = 0.0;
	support_weight = 0.0;
	geometric_visibility = 0.0;
	in_front_weight = 0.0;
	int resolution = params.screen_resolution_debug.z;
	if (any(lessThan(probe, ivec3(0))) || any(greaterThanEqual(probe, ivec3(resolution)))) {
		return vec3(0.0);
	}
	ivec3 physical = ivec3((probe.x + phase_offset.x) % resolution,
			(probe.y + phase_offset.y) % resolution, (probe.z + phase_offset.z) % resolution);
	int probe_index = physical.x + physical.y * resolution + physical.z * resolution * resolution;
	ivec2 tile = ivec2(probe_index % (resolution * resolution), probe_index / (resolution * resolution));
	vec4 metadata = texelFetch(metadata_atlas, tile, 0);
	uint packed_metadata = uint(max(metadata.w, 0.0) + 0.5);
	uint state = packed_metadata & 15u;
	uint generation = packed_metadata >> 4u;
	uint expected_generation = probe_generation(ivec4(logical_origin + probe, lod));
	if (generation != expected_generation || state < 1u || state > 8u) {
		return vec3(0.0);
	}
	vec3 probe_to_surface = biased_position - metadata.xyz;
	float distance_to_surface = length(probe_to_surface);
	vec3 direction_to_surface = distance_to_surface > 0.0001 ? probe_to_surface / distance_to_surface : world_normal;
	vec2 irradiance_uv = tile_uv(tile, 10, 8, oct_encode(world_normal), params.atlas_sizes.xy);
	vec2 visibility_uv = tile_uv(tile, 18, 16, oct_encode(direction_to_surface), params.atlas_sizes.zw);
	vec4 irradiance = textureLod(irradiance_atlas, irradiance_uv, 0.0);
	vec4 moments = textureLod(visibility_atlas, visibility_uv, 0.0);
	if (irradiance.a <= 0.0 || moments.a <= 0.0 || moments.x <= 0.0) {
		return vec3(0.0);
	}
	float confidence = clamp(min(irradiance.a, moments.a), 0.0, 1.0);
	float wrap_shading = (dot(world_normal, -direction_to_surface) + 1.0) * 0.5;
	float directional_weight = wrap_shading * wrap_shading + 0.2;
	support_weight = confidence;
	in_front_weight = directional_weight;
	float mean_depth = moments.x;
	float variance_floor = max(visibility_bias_distance * visibility_bias_distance * 0.0625, 0.000001);
	float variance = max(moments.y - mean_depth * mean_depth, variance_floor);
	float depth_visibility = 1.0;
	if (distance_to_surface > mean_depth) {
		float delta = distance_to_surface - mean_depth;
		depth_visibility = variance / (variance + delta * delta);
		depth_visibility = depth_visibility * depth_visibility * depth_visibility;
	}
	depth_visibility = max(0.05, depth_visibility);
	geometric_visibility = max(directional_weight * depth_visibility, 0.000001);
	const float crush_threshold = 0.2;
	if (geometric_visibility < crush_threshold) {
		geometric_visibility *= geometric_visibility * geometric_visibility / (crush_threshold * crush_threshold);
	}
	visibility_weight = support_weight * geometric_visibility;
	return irradiance.rgb;
}

vec3 lod_sample(sampler2D irradiance_atlas, sampler2D visibility_atlas, sampler2D metadata_atlas,
		int lod, vec3 world_position, vec3 world_normal, vec3 view_direction, out float edge_weight,
		out float initialized_support, out float final_visibility, out float final_in_front) {
	vec3 origin = params.origins[lod].xyz;
	vec3 cell_size = params.cell_sizes[lod].xyz;
	ivec3 phase_offset = params.phase_offsets[lod].xyz;
	ivec3 logical_origin = params.logical_origins[lod].xyz;
	int resolution = params.screen_resolution_debug.z;
	view_direction = normalize(view_direction);
	float minimum_spacing = min(cell_size.x, min(cell_size.y, cell_size.z));
	vec3 bias_direction = mix(world_normal, view_direction, params.tuning.y);
	vec3 bias_vector = bias_direction * (0.75 * minimum_spacing) * params.tuning.x;
	float visibility_bias_distance = length(bias_vector);
	vec3 biased_position = world_position + bias_vector;
	vec3 local = (biased_position - origin) / cell_size - vec3(0.5);
	// A higher-density cascade owns every point inside its resident probe
	// volume. Clamp only the interpolation coordinate so edge probes can extend
	// their low-frequency field through the configured transition outside the
	// volume. Fading inside the volume exposes coarser probes that cannot resolve
	// thin interior geometry and produces visible nested bands.
	vec3 interpolation_local = clamp(local, vec3(0.0), vec3(float(resolution) - 1.0001));
	ivec3 base = ivec3(floor(interpolation_local));
	vec3 fraction = fract(interpolation_local);
	vec3 accumulated = vec3(0.0);
	float visible_weight = 0.0;
	float supported_weight = 0.0;
	float geometry_sum = 0.0;
	float in_front_sum = 0.0;
	float basis_weight_sum = 0.0;
	initialized_support = 0.0;
	for (int z = 0; z <= 1; z++) {
		for (int y = 0; y <= 1; y++) {
			for (int x = 0; x <= 1; x++) {
				ivec3 offset = ivec3(x, y, z);
				vec3 axis_weight = mix(vec3(1.0) - fraction, fraction, vec3(offset));
				float ideal_weight = axis_weight.x * axis_weight.y * axis_weight.z;
				float visibility_weight;
				float support_weight;
				float geometry_visibility;
				float in_front_weight;
				vec3 value = probe_sample(irradiance_atlas, visibility_atlas, metadata_atlas,
						base + offset, phase_offset, logical_origin, lod, biased_position, world_normal,
						visibility_bias_distance, visibility_weight, support_weight, geometry_visibility, in_front_weight);
				basis_weight_sum += ideal_weight;
				float combined_weight = ideal_weight * visibility_weight;
				accumulated += decode_interpolation_sample(value) * combined_weight;
				visible_weight += combined_weight;
				initialized_support += ideal_weight * support_weight;
				supported_weight += ideal_weight * support_weight;
				geometry_sum += ideal_weight * support_weight * geometry_visibility;
				in_front_sum += ideal_weight * support_weight * in_front_weight;
			}
		}
	}
	float inverse_basis_weight = 1.0 / max(basis_weight_sum, 0.0001);
	initialized_support *= inverse_basis_weight;
	supported_weight *= inverse_basis_weight;
	geometry_sum *= inverse_basis_weight;
	in_front_sum *= inverse_basis_weight;
	vec3 probe_local = (biased_position - origin) / cell_size;
	float transition_cells = max(params.tuning.z * float(resolution), 0.25);
	vec3 outside_cells = max(max(vec3(0.5) - probe_local,
			probe_local - vec3(float(resolution) - 0.5)), vec3(0.0));
	float outside_distance = max(outside_cells.x, max(outside_cells.y, outside_cells.z));
	edge_weight = 1.0 - smoothstep(0.0, transition_cells, outside_distance);
	initialized_support = clamp(initialized_support, 0.0, 1.0);
	final_visibility = supported_weight > 0.0001 ? clamp(geometry_sum / supported_weight, 0.0, 1.0) : 0.0;
	final_in_front = supported_weight > 0.0001 ? clamp(in_front_sum / supported_weight, 0.0, 1.0) : 0.0;
	return visible_weight > 0.0001 ? finish_interpolation(accumulated / visible_weight) : vec3(0.0);
}

bool lod_contains(int lod, vec3 world_position) {
	vec3 local = (world_position - params.origins[lod].xyz) / params.cell_sizes[lod].xyz;
	float resolution = float(params.screen_resolution_debug.z);
	float transition_cells = max(params.tuning.z * resolution, 0.25);
	return all(greaterThanEqual(local, vec3(0.5 - transition_cells))) &&
			all(lessThanEqual(local, vec3(resolution - 0.5 + transition_cells)));
}

vec3 sample_lod_index(int lod, vec3 world_position, vec3 world_normal, vec3 view_direction,
		out float edge, out float support, out float visibility, out float in_front) {
	if (lod == 0) return lod_sample(irradiance_lod0, visibility_lod0, metadata_lod0, lod, world_position, world_normal, view_direction, edge, support, visibility, in_front);
	if (lod == 1) return lod_sample(irradiance_lod1, visibility_lod1, metadata_lod1, lod, world_position, world_normal, view_direction, edge, support, visibility, in_front);
	if (lod == 2) return lod_sample(irradiance_lod2, visibility_lod2, metadata_lod2, lod, world_position, world_normal, view_direction, edge, support, visibility, in_front);
	return lod_sample(irradiance_lod3, visibility_lod3, metadata_lod3, lod, world_position, world_normal, view_direction, edge, support, visibility, in_front);
}

vec4 gather_ddgi(vec3 world_position, vec3 world_normal, out float visibility, out float selected_lod, out float in_front) {
	vec3 result = vec3(0.0);
	float support = 0.0;
	float remaining = 1.0;
	float visibility_sum = 0.0;
	float in_front_sum = 0.0;
	float lod_sum = 0.0;
	vec3 view_direction = normalize(params.camera_irradiance_size.xyz - world_position);
	for (int lod = 0; lod < 4 && remaining > 0.001; lod++) {
		if (!lod_contains(lod, world_position)) continue;
		float edge;
		float local_support;
		float local_visibility;
		float local_in_front;
		vec3 value = sample_lod_index(lod, world_position, world_normal, view_direction, edge, local_support, local_visibility, local_in_front);
		float maturity = smoothstep(0.0, 1.0, local_support);
		float local_weight = clamp((lod == 3 ? 1.0 : edge) * maturity, 0.0, 1.0);
		float weight = remaining * local_weight;
		result += value * weight;
		visibility_sum += local_visibility * weight;
		in_front_sum += local_in_front * weight;
		lod_sum += float(lod) * weight;
		support += weight;
		remaining -= weight;
	}
	visibility = support > 0.0001 ? visibility_sum / support : 0.0;
	in_front = support > 0.0001 ? in_front_sum / support : 0.0;
	selected_lod = support > 0.0001 ? lod_sum / support : -1.0;
	return vec4(result, support);
}

vec4 resolve_face(ivec2 pixel, vec4 face_data, uint payload) {
	if (payload == 0u || face_data.w < 1.0) return vec4(0.0);
	vec3 world_position = face_data.xyz;
	vec3 world_normal = decode_face_normal(face_data.w);
	float visibility;
	float selected_lod;
	float in_front;
	vec4 gathered = gather_ddgi(world_position, world_normal, visibility, selected_lod, in_front);
	int debug_mode = params.screen_resolution_debug.w;
	if (debug_mode == 13) return vec4(vec3(visibility), gathered.a);
	if (debug_mode == 14) {
		vec3 lod_colors[4] = vec3[4](vec3(0.05, 0.9, 1.0), vec3(0.1, 1.0, 0.25), vec3(1.0, 0.5, 0.05), vec3(1.0, 0.1, 0.85));
		float visual_lod = clamp(selected_lod, 0.0, 3.0);
		int lower_lod = int(floor(visual_lod));
		return vec4(mix(lod_colors[lower_lod], lod_colors[min(lower_lod + 1, 3)], fract(visual_lod)), gathered.a);
	}
	if (debug_mode == 16) return vec4(vec3(gathered.a), gathered.a);
	if (debug_mode == 17) return vec4(vec3(in_front), gathered.a);
	if (debug_mode == 18) return vec4(vec3(in_front > 0.0001 ? clamp(visibility / in_front, 0.0, 1.0) : 0.0), gathered.a);
	return gathered;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 screen_size = params.screen_resolution_debug.xy;
	uint local_index = gl_LocalInvocationIndex;
	bool in_bounds = all(lessThan(pixel, screen_size));
	vec4 face_data = in_bounds ? texelFetch(voxel_face_data, pixel, 0) : vec4(0.0);
	uint payload = in_bounds ? texelFetch(voxel_hit_buffer, pixel, 0).r : 0u;
	face_keys[local_index] = payload != 0u && face_data.w >= 1.0 ? uvec4(floatBitsToUint(face_data.xyz), payload) : uvec4(0u);
	face_results[local_index] = vec4(0.0);
	barrier();

	uint leader = local_index;
	for (uint candidate = 0u; candidate < 64u; candidate++) {
		if (all(equal(face_keys[candidate], face_keys[local_index]))) {
			leader = candidate;
			break;
		}
	}
	if (local_index == leader) {
		face_results[local_index] = resolve_face(pixel, face_data, payload);
	}
	barrier();
	if (in_bounds) {
		imageStore(output_gi, pixel, face_results[leader]);
	}
}
