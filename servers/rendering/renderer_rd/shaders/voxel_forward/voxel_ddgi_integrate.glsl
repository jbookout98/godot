#[compute]

#version 450

#VERSION_DEFINES

// One 8x8 workgroup owns one probe. Rays are loaded once into shared memory,
// then lanes cooperatively update both angular atlases and their guard texels.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

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

layout(set = 0, binding = 0, std430) readonly buffer Surfels {
	Surfel values[];
}
surfels;
layout(set = 0, binding = 1, std430) readonly buffer ProbeIndices {
	uint values[];
}
probe_indices;
layout(set = 0, binding = 2, std430) readonly buffer ProbeCounters {
	uint values[6];
}
probe_counters;
layout(set = 0, binding = 3, std430) buffer ProbeRecords {
	ProbeRecord values[];
}
probe_records;
layout(rgba16f, set = 0, binding = 4) uniform restrict image2D irradiance_atlas;
layout(rgba16f, set = 0, binding = 5) uniform restrict image2D depth_atlas;
layout(rgba32f, set = 0, binding = 6) uniform restrict writeonly image2D probe_metadata;

layout(push_constant, std430) uniform Params {
	ivec4 probe_grid;
	ivec4 irradiance_layout;
	ivec4 visibility_layout;
	ivec4 update_state;
	ivec4 scheduling;
	// irradiance hysteresis, visibility hysteresis, voxel size, maximum useful depth
	vec4 temporal;
	// Dirty irradiance hysteresis, dirty visibility hysteresis, reserved, reserved.
	vec4 dirty_temporal;
}
params;

uint probe_generation(ivec4 logical_cell_lod) {
	uvec4 bits = uvec4(logical_cell_lod);
	uint hash = 2166136261u;
	hash = (hash ^ bits.x) * 16777619u;
	hash = (hash ^ bits.y) * 16777619u;
	hash = (hash ^ bits.z) * 16777619u;
	hash = (hash ^ bits.w) * 16777619u;
	return hash & 0xfffffu;
}

float packed_metadata(uint state, ivec4 logical_cell_lod) {
	return float((probe_generation(logical_cell_lod) << 4u) | (state & 15u));
}

shared Surfel probe_surfels[64];
const float PI = 3.14159265358979323846;

vec3 oct_decode(vec2 encoded) {
	vec2 f = encoded * 2.0 - 1.0;
	vec3 direction = vec3(f.x, 1.0 - abs(f.x) - abs(f.y), f.y);
	if (direction.y < 0.0) {
		direction.xz = (1.0 - abs(direction.zx)) * sign(direction.xz);
	}
	return normalize(direction);
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
	float rotation = float(rotation_hash & 65535u) * (2.0 * PI / 65536.0);
	float angle = float(ray_index) * 2.399963229728653 + rotation;
	return vec3(cos(angle) * radius, z, sin(angle) * radius);
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

ivec2 oct_border_source(ivec2 texel, int interior_size) {
	int last = interior_size + 1;
	if (texel.x == 0 && texel.y == 0) {
		return ivec2(interior_size, interior_size);
	}
	if (texel.x == last && texel.y == 0) {
		return ivec2(1, interior_size);
	}
	if (texel.x == 0 && texel.y == last) {
		return ivec2(interior_size, 1);
	}
	if (texel.x == last && texel.y == last) {
		return ivec2(1, 1);
	}
	if (texel.y == 0) {
		return ivec2(interior_size - texel.x + 1, 1);
	}
	if (texel.y == last) {
		return ivec2(interior_size - texel.x + 1, interior_size);
	}
	if (texel.x == 0) {
		return ivec2(1, interior_size - texel.y + 1);
	}
	if (texel.x == last) {
		return ivec2(interior_size, interior_size - texel.y + 1);
	}
	return texel;
}

void main() {
	uint update_index = gl_WorkGroupID.z;
	uint update_count = scheduled_probe_count();
	if (update_index >= update_count) {
		return;
	}
	uint probe_index = probe_indices.values[scheduled_probe_list_index(update_index)];
	ProbeRecord record = probe_records.values[probe_index];
	uint row_width = uint(params.probe_grid.x * params.probe_grid.x);
	ivec2 tile = ivec2(int(probe_index % row_width), int(probe_index / row_width));
	uint update_sequence = record.state_revision_frame_flags.z + 1u;
	uint state = record.state_revision_frame_flags.x;
	bool new_probe = state == 2u || state == 3u;
	bool dirty_probe = state == 6u || state == 7u;
	bool retiring_probe = state == 8u;
	bool converging_probe = new_probe || dirty_probe;
	uint ray_stride = uint(params.probe_grid.y);
	uint ray_count = converging_probe ? uint(params.update_state.y) : uint(params.update_state.x);
	uint convergence_updates = uint(max(params.dirty_temporal.z, 1.0));
	uint lane = gl_LocalInvocationIndex;
	for (uint ray = lane; ray < ray_count; ray += 64u) {
		probe_surfels[ray] = surfels.values[update_index * ray_stride + ray];
	}
	barrier();

	float confidence_step = 1.0 / float(convergence_updates);
	float integrated_confidence = converging_probe ? confidence_step : 0.0;
	for (int y = int(gl_LocalInvocationID.y) + 1; y <= params.irradiance_layout.w; y += 8) {
		for (int x = int(gl_LocalInvocationID.x) + 1; x <= params.irradiance_layout.w; x += 8) {
			ivec2 local_texel = ivec2(x, y);
			ivec2 interior_texel = local_texel - ivec2(1);
			vec3 output_direction = oct_decode(vec2(interior_texel) / float(max(params.irradiance_layout.w - 1, 1)));
			vec3 irradiance_sum = vec3(0.0);
			float irradiance_weight = 0.0;
			for (uint ray = 0u; ray < ray_count; ray++) {
				vec3 ray_direction = fibonacci_direction(ray, ray_count, record.logical_cell_lod, update_sequence);
				float weight = max(dot(output_direction, ray_direction), 0.0);
				irradiance_sum += probe_surfels[ray].radiance.rgb * weight;
				irradiance_weight += weight;
			}
			vec3 target_linear = irradiance_sum * (PI / max(irradiance_weight, 0.00001));
			vec3 target_encoded = pow(max(target_linear, vec3(0.0)), vec3(0.2));
			ivec2 atlas_texel = tile * params.irradiance_layout.x + local_texel;
			vec4 history = imageLoad(irradiance_atlas, atlas_texel);
			if (retiring_probe) {
				imageStore(irradiance_atlas, atlas_texel, vec4(history.rgb, max(history.a - confidence_step, 0.0)));
				continue;
			}
			// The first update discards any atlas value left by the former toroidal
			// occupant. Later New updates retain the accumulated confidence until the
			// probe is mature enough to join the steady round-robin set.
			// A dirty probe still owns valid history for the same logical cell.
			// Preserve that confidence while its retraced result converges; dropping
			// it to zero exposes ambient fallback as an edit-wide lighting flash.
			// Only a genuinely new logical identity must replace stale atlas data.
			float history_confidence = new_probe && update_sequence == 1u ? 0.0 : clamp(history.a, 0.0, 1.0);
			vec3 encoded_delta = abs(target_encoded - history.rgb);
			float change = max(encoded_delta.r, max(encoded_delta.g, encoded_delta.b));
			float base_hysteresis = dirty_probe ? params.dirty_temporal.x : params.temporal.x;
			// Dirty edits use their explicit transition rate even for a large
			// radiance delta. A hard replacement is correct only for new probes;
			// mature background updates may retain the fast large-change response.
			float hysteresis = dirty_probe ? base_hysteresis : (change > 0.8 ? 0.0 : (change > 0.25 ? max(base_hysteresis - 0.15, 0.0) : base_hysteresis));
			// A new probe accumulates its bounded convergence sequence as a running
			// mean. Replacing it with each rotated ray set made the last random set
			// win and visibly rippled before the probe became mature.
			float blend = history_confidence <= 0.0 ? 1.0 : (new_probe ? 1.0 / float(update_sequence) : 1.0 - clamp(hysteresis, 0.0, 0.999));
			imageStore(irradiance_atlas, atlas_texel, vec4(mix(history.rgb, target_encoded, blend), min(1.0, history_confidence + integrated_confidence)));
		}
	}

	for (int y = int(gl_LocalInvocationID.y) + 1; y <= params.visibility_layout.w; y += 8) {
		for (int x = int(gl_LocalInvocationID.x) + 1; x <= params.visibility_layout.w; x += 8) {
			ivec2 local_texel = ivec2(x, y);
			ivec2 interior_texel = local_texel - ivec2(1);
			vec3 output_direction = oct_decode(vec2(interior_texel) / float(max(params.visibility_layout.w - 1, 1)));
			float mean_sum = 0.0;
			float second_sum = 0.0;
			float weight_sum = 0.0;
			float maximum_useful_depth = max(params.temporal.w, params.temporal.z);
			for (uint ray = 0u; ray < ray_count; ray++) {
				vec3 ray_direction = fibonacci_direction(ray, ray_count, record.logical_cell_lod, update_sequence);
				float weight = pow(max(dot(output_direction, ray_direction), 0.0), 16.0);
				float distance = probe_surfels[ray].position_distance.w;
				// A query owned by this cage cannot lie beyond its enlarged diagonal.
				// Rejecting farther samples prevents a nearly parallel ray from
				// smearing an extreme depth into adjacent octahedral texels.
				if (distance > maximum_useful_depth) {
					continue;
				}
				mean_sum += distance * weight;
				second_sum += distance * distance * weight;
				weight_sum += weight;
			}
			// No nearby hit means the whole usable cage direction is visible.
			float target_mean = weight_sum > 0.00001 ? mean_sum / weight_sum : maximum_useful_depth;
			float target_second = weight_sum > 0.00001 ? second_sum / weight_sum : maximum_useful_depth * maximum_useful_depth;
			float minimum_variance = max(params.temporal.z * params.temporal.z * 0.0625, 0.000001);
			float variance = max(target_second - target_mean * target_mean, minimum_variance);
			vec3 target = vec3(target_mean, target_second, target_mean + 2.0 * sqrt(variance));
			ivec2 atlas_texel = tile * params.visibility_layout.x + local_texel;
			vec4 history = imageLoad(depth_atlas, atlas_texel);
			if (retiring_probe) {
				imageStore(depth_atlas, atlas_texel, vec4(history.rgb, max(history.a - confidence_step, 0.0)));
				continue;
			}
			// Visibility history follows the same ownership rule as irradiance:
			// edits update an existing logical probe instead of making it disappear.
			float history_confidence = new_probe && update_sequence == 1u ? 0.0 : clamp(history.a, 0.0, 1.0);
			float visibility_hysteresis = dirty_probe ? params.dirty_temporal.y : params.temporal.y;
			float blend = history_confidence <= 0.0 ? 1.0 : (new_probe ? 1.0 / float(update_sequence) : 1.0 - clamp(visibility_hysteresis, 0.0, 0.999));
			imageStore(depth_atlas, atlas_texel, vec4(mix(history.rgb, target, blend), min(1.0, history_confidence + integrated_confidence)));
		}
	}
	memoryBarrierImage();
	barrier();

	for (int y = int(gl_LocalInvocationID.y); y < params.irradiance_layout.x; y += 8) {
		for (int x = int(gl_LocalInvocationID.x); x < params.irradiance_layout.x; x += 8) {
			if (x == 0 || y == 0 || x == params.irradiance_layout.x - 1 || y == params.irradiance_layout.x - 1) {
				ivec2 local_texel = ivec2(x, y);
				ivec2 origin = tile * params.irradiance_layout.x;
				imageStore(irradiance_atlas, origin + local_texel, imageLoad(irradiance_atlas, origin + oct_border_source(local_texel, params.irradiance_layout.w)));
			}
		}
	}
	for (int y = int(gl_LocalInvocationID.y); y < params.visibility_layout.x; y += 8) {
		for (int x = int(gl_LocalInvocationID.x); x < params.visibility_layout.x; x += 8) {
			if (x == 0 || y == 0 || x == params.visibility_layout.x - 1 || y == params.visibility_layout.x - 1) {
				ivec2 local_texel = ivec2(x, y);
				ivec2 origin = tile * params.visibility_layout.x;
				imageStore(depth_atlas, origin + local_texel, imageLoad(depth_atlas, origin + oct_border_source(local_texel, params.visibility_layout.w)));
			}
		}
	}

	if (lane == 0u) {
		if (retiring_probe) {
			record.state_revision_frame_flags.x = update_sequence >= convergence_updates ? 0u : 8u;
			if (record.state_revision_frame_flags.x == 0u) {
				record.physical_position_valid.w = 0.0;
			}
		} else if (converging_probe && update_sequence >= convergence_updates) {
			bool awake = state == 2u || state == 6u;
			record.state_revision_frame_flags.x = awake ? 4u : 5u;
		}
		record.state_revision_frame_flags.z = update_sequence;
		probe_records.values[probe_index] = record;
		imageStore(probe_metadata, tile, vec4(record.physical_position_valid.xyz, packed_metadata(record.state_revision_frame_flags.x, record.logical_cell_lod)));
	}
}
