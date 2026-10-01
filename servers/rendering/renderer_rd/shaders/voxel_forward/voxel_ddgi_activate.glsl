#[compute]

#version 450

#VERSION_DEFINES

// Activation is deliberately separate from placement. It reads cached cell
// occupancy summaries, so ordinary frames never search the voxel hash table.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

const uint PROBE_OFF = 0u;
const uint PROBE_SLEEPING = 1u;
const uint PROBE_NEWLY_AWAKE = 2u;
const uint PROBE_NEWLY_VIGILANT = 3u;
const uint PROBE_AWAKE = 4u;
const uint PROBE_VIGILANT = 5u;
const uint PROBE_DIRTY_AWAKE = 6u;
const uint PROBE_DIRTY_VIGILANT = 7u;
const uint PROBE_RETIRING = 8u;
const uint FLAG_CELL_OCCUPIED = 1u;

struct ProbeRecord {
	ivec4 logical_cell_lod;
	vec4 physical_position_valid;
	uvec4 state_revision_frame_flags;
};

layout(set = 0, binding = 0, std430) buffer ProbeRecords {
	ProbeRecord values[];
}
probe_records;
layout(set = 0, binding = 1, std430) writeonly buffer ActiveProbeIndices {
	uint values[];
}
active_probe_indices;
layout(set = 0, binding = 2, std430) buffer ProbeCounters {
	uint values[6];
}
probe_counters;
layout(set = 0, binding = 3, std430) readonly buffer DynamicProbeMask {
	uint values[];
}
dynamic_probe_mask;
layout(rgba32f, set = 0, binding = 4) uniform restrict writeonly image2D probe_metadata;
layout(set = 0, binding = 6, std430) writeonly buffer DispatchCommands {
	uint values[8];
}
dispatch_commands;

layout(push_constant, std430) uniform Params {
	vec4 world_origin_voxel_size;
	ivec4 grid_origin_lod;
	ivec4 grid_resolution_phase;
	ivec4 cell_size_voxels;
	uvec4 directory_revision;
	uvec4 schedule;
	uvec4 changes;
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

ivec3 local_coord(uint index, int resolution) {
	return ivec3(int(index % uint(resolution)), int((index / uint(resolution)) % uint(resolution)), int(index / uint(resolution * resolution)));
}

// Enumerate the circular grid in Morton order so an atomic-compacted budget
// updates spatial neighborhoods instead of unrelated scanline fragments.
ivec3 morton_coord(uint code) {
	uvec3 result = uvec3(0u);
	for (uint bit = 0u; bit < 10u; bit++) {
		result.x |= ((code >> (bit * 3u)) & 1u) << bit;
		result.y |= ((code >> (bit * 3u + 1u)) & 1u) << bit;
		result.z |= ((code >> (bit * 3u + 2u)) & 1u) << bit;
	}
	return ivec3(result);
}

uint probe_index(ivec3 coord, int resolution) {
	return uint(coord.x + coord.y * resolution + coord.z * resolution * resolution);
}

ivec3 physical_coord(ivec3 logical_cell, int resolution) {
	return ivec3(
			(logical_cell.x % resolution + resolution) % resolution,
			(logical_cell.y % resolution + resolution) % resolution,
			(logical_cell.z % resolution + resolution) % resolution);
}

ivec2 metadata_texel(uint index, int resolution) {
	return ivec2(int(index % uint(resolution * resolution)), int(index / uint(resolution * resolution)));
}

void append_probe(uint index, uint scheduler_class) {
	uint counter_index = scheduler_class == 1u ? 3u : (scheduler_class == 2u ? 4u : 5u);
	uint segment = scheduler_class == 1u ? 0u : (scheduler_class == 2u ? 1u : 2u);
	uint output_index = atomicAdd(probe_counters.values[counter_index], 1u);
	atomicAdd(probe_counters.values[0], 1u);
	if (output_index < params.schedule.y) {
		active_probe_indices.values[segment * params.schedule.y + output_index] = index;
	}
}

void main() {
	uint traversal_index = gl_GlobalInvocationID.x;
	if (traversal_index >= params.schedule.y) {
		return;
	}
	uint phase = uint(params.grid_resolution_phase.w);
	if (phase == 1u) {
		if (traversal_index != 0u) {
			return;
		}
		uint dirty_count = min(probe_counters.values[3], params.schedule.y);
		uint new_count = min(probe_counters.values[4], params.schedule.y);
		uint mature_count = min(probe_counters.values[5], params.schedule.y);
		uint update_count = min(dirty_count, params.schedule.z) + min(new_count, params.schedule.w) + min(mature_count, params.schedule.x);
		uint trace_threads = update_count * params.directory_revision.w;
		dispatch_commands.values[0] = (trace_threads + 63u) / 64u;
		dispatch_commands.values[1] = 1u;
		dispatch_commands.values[2] = 1u;
		dispatch_commands.values[3] = 0u;
		dispatch_commands.values[4] = 1u;
		dispatch_commands.values[5] = 1u;
		dispatch_commands.values[6] = update_count;
		dispatch_commands.values[7] = 0u;
		return;
	}
	int resolution = params.grid_resolution_phase.x;
	ivec3 local = morton_coord(traversal_index);
	if (any(greaterThanEqual(local, ivec3(resolution)))) {
		return;
	}
	ivec3 logical_cell = params.grid_origin_lod.xyz + local;
	uint invocation = probe_index(physical_coord(logical_cell, resolution), resolution);
	ProbeRecord record = probe_records.values[invocation];
	uint state = record.state_revision_frame_flags.x;
	if (state == PROBE_OFF || record.physical_position_valid.w <= 0.5) {
		return;
	}
	logical_cell = record.logical_cell_lod.xyz;
	local = logical_cell - params.grid_origin_lod.xyz;
	if (any(lessThan(local, ivec3(0))) || any(greaterThanEqual(local, ivec3(resolution)))) {
		return;
	}
	if (probe_index(physical_coord(logical_cell, resolution), resolution) != invocation) {
		return;
	}
	if (any(notEqual(record.logical_cell_lod, ivec4(logical_cell, params.grid_origin_lod.w)))) {
		return;
	}
	if (state == PROBE_RETIRING) {
		append_probe(invocation, 2u);
		imageStore(probe_metadata, metadata_texel(invocation, resolution), vec4(record.physical_position_valid.xyz, packed_metadata(state, record.logical_cell_lod)));
		return;
	}
	// A lighting revision invalidates irradiance without discarding placement.
	// Promote mature records to Reconverging and restart their retrace sequence;
	// integration keeps the same-cell history visible while blending the change.
	bool dynamic_changed = params.changes.z != 0u && (dynamic_probe_mask.values[invocation] & 2u) != 0u;
	if (params.changes.x != 0u || dynamic_changed) {
		if (state == PROBE_AWAKE) {
			state = PROBE_DIRTY_AWAKE;
		} else if (state == PROBE_VIGILANT) {
			state = PROBE_DIRTY_VIGILANT;
		}
		if (state == PROBE_DIRTY_AWAKE || state == PROBE_DIRTY_VIGILANT) {
			record.state_revision_frame_flags.z = 0u;
		}
	}
	bool static_surface = (record.state_revision_frame_flags.w & FLAG_CELL_OCCUPIED) != 0u;
	const ivec3 neighbor_axes[6] = ivec3[6](
			ivec3(1, 0, 0), ivec3(-1, 0, 0), ivec3(0, 1, 0),
			ivec3(0, -1, 0), ivec3(0, 0, 1), ivec3(0, 0, -1));
	for (int neighbor = 0; neighbor < 6 && !static_surface; neighbor++) {
		ivec3 neighbor_local = local + neighbor_axes[neighbor];
		if (any(lessThan(neighbor_local, ivec3(0))) || any(greaterThanEqual(neighbor_local, ivec3(resolution)))) {
			continue;
		}
		ivec3 expected_cell = logical_cell + neighbor_axes[neighbor];
		ProbeRecord neighbor_record = probe_records.values[probe_index(physical_coord(expected_cell, resolution), resolution)];
		static_surface = all(equal(neighbor_record.logical_cell_lod, ivec4(expected_cell, params.grid_origin_lod.w))) &&
				(neighbor_record.state_revision_frame_flags.w & FLAG_CELL_OCCUPIED) != 0u;
	}
	vec3 world_begin = params.world_origin_voxel_size.xyz + vec3(logical_cell * params.cell_size_voxels.xyz) * params.world_origin_voxel_size.w;
	vec3 world_end = world_begin + vec3(params.cell_size_voxels.xyz) * params.world_origin_voxel_size.w;
	bool dynamic_surface = (dynamic_probe_mask.values[invocation] & 1u) != 0u;
	if (!static_surface && !dynamic_surface) {
		state = PROBE_SLEEPING;
	} else if (static_surface) {
		if (state == PROBE_NEWLY_AWAKE) {
			state = PROBE_NEWLY_VIGILANT;
		} else if (state == PROBE_DIRTY_AWAKE) {
			state = PROBE_DIRTY_VIGILANT;
		} else if (state != PROBE_NEWLY_VIGILANT && state != PROBE_DIRTY_VIGILANT) {
			state = PROBE_VIGILANT;
		}
	} else {
		if (state == PROBE_NEWLY_VIGILANT) {
			state = PROBE_NEWLY_AWAKE;
		} else if (state == PROBE_DIRTY_VIGILANT) {
			state = PROBE_DIRTY_AWAKE;
		} else if (state != PROBE_NEWLY_AWAKE && state != PROBE_DIRTY_AWAKE) {
			state = PROBE_AWAKE;
		}
	}
	record.state_revision_frame_flags.x = state;
	probe_records.values[invocation] = record;
	atomicAdd(probe_counters.values[1], 1u);
	if (state == PROBE_SLEEPING) {
		atomicAdd(probe_counters.values[2], 1u);
	} else if (state == PROBE_DIRTY_AWAKE || state == PROBE_DIRTY_VIGILANT) {
		append_probe(invocation, 1u);
	} else if (state == PROBE_NEWLY_AWAKE || state == PROBE_NEWLY_VIGILANT) {
		append_probe(invocation, 2u);
	} else if (state == PROBE_AWAKE || state == PROBE_VIGILANT) {
		append_probe(invocation, 0u);
	}
	imageStore(probe_metadata, metadata_texel(invocation, resolution), vec4(record.physical_position_valid.xyz, packed_metadata(state, record.logical_cell_lod)));
}
