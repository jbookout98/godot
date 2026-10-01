#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct ProbeRecord {
	ivec4 logical_cell_lod;
	vec4 physical_position_valid;
	uvec4 state_revision_frame_flags;
};

const uint PROBE_OFF = 0u;
const uint PROBE_SLEEPING = 1u;
const uint PROBE_NEWLY_AWAKE = 2u;
const uint PROBE_NEWLY_VIGILANT = 3u;
const uint PROBE_AWAKE = 4u;
const uint PROBE_VIGILANT = 5u;
const uint PROBE_DIRTY_AWAKE = 6u;
const uint PROBE_DIRTY_VIGILANT = 7u;
const uint PROBE_RETIRING = 8u;

layout(set = 0, binding = 0, std430) buffer ProbeRecords {
	ProbeRecord values[];
}
probe_records;

layout(set = 0, binding = 1, std430) readonly buffer UploadedRecords {
	ProbeRecord values[];
}
uploaded_records;

layout(set = 0, binding = 2, std430) readonly buffer ChangedProbeIndices {
	uint values[];
}
changed_probe_indices;

layout(rgba32f, set = 0, binding = 3) uniform restrict writeonly image2D probe_metadata;

layout(push_constant, std430) uniform Params {
	ivec4 grid_resolution_lod;
	ivec4 probe_shift;
}
params;

ivec3 probe_coord(uint index, int resolution) {
	return ivec3(int(index % uint(resolution)), int((index / uint(resolution)) % uint(resolution)), int(index / uint(resolution * resolution)));
}

uint probe_index(ivec3 coord, int resolution) {
	return uint(coord.x + coord.y * resolution + coord.z * resolution * resolution);
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

float packed_metadata(uint state, ivec4 logical_cell_lod) {
	// A float stores every integer through 2^24 exactly. Reserve four low bits
	// for lifecycle state and use the remaining 20 bits as the slot generation.
	return float((probe_generation(logical_cell_lod) << 4u) | (state & 15u));
}

void main() {
	uint changed_index = gl_GlobalInvocationID.x;
	int resolution = params.grid_resolution_lod.x;
	if (changed_index >= uint(params.grid_resolution_lod.y)) {
		return;
	}
	uint destination_index = changed_probe_indices.values[changed_index];
	ProbeRecord previous = probe_records.values[destination_index];
	ProbeRecord record = uploaded_records.values[destination_index];
	uint previous_state = previous.state_revision_frame_flags.x;
	bool previous_usable = previous.physical_position_valid.w > 0.5 && previous_state != PROBE_OFF;
	bool same_identity = all(equal(previous.logical_cell_lod, record.logical_cell_lod));
	// Relocation keeps the logical probe generation. Preserve its radiance and
	// visibility history while the dirty update converges at the new bounded
	// position; only a new logical identity or invalid placement resets history.
	bool compatible_history = previous_usable && record.physical_position_valid.w > 0.5 && same_identity;
	bool retiring_history = previous_usable && record.physical_position_valid.w <= 0.5 && same_identity;
	if (retiring_history) {
		record = previous;
		record.state_revision_frame_flags.x = PROBE_RETIRING;
		record.state_revision_frame_flags.z = 0u;
	} else if (compatible_history) {
		record.state_revision_frame_flags.z = previous.state_revision_frame_flags.z;
		if (previous_state == PROBE_NEWLY_AWAKE || previous_state == PROBE_NEWLY_VIGILANT) {
			record.state_revision_frame_flags.x = previous_state;
		} else if (previous_state == PROBE_DIRTY_AWAKE || previous_state == PROBE_DIRTY_VIGILANT) {
			record.state_revision_frame_flags.x = previous_state;
		} else {
			record.state_revision_frame_flags.x = record.state_revision_frame_flags.x == PROBE_NEWLY_AWAKE ? PROBE_DIRTY_AWAKE : PROBE_DIRTY_VIGILANT;
			// Restart the retrace sequence, but keep the atlas confidence owned by
			// this logical cell. Integration blends the edited result into that
			// authoritative history instead of exposing the ambient fallback.
			record.state_revision_frame_flags.z = 0u;
		}
	}
	probe_records.values[destination_index] = record;
	ivec2 metadata_texel = ivec2(int(destination_index % uint(resolution * resolution)), int(destination_index / uint(resolution * resolution)));
	imageStore(probe_metadata, metadata_texel, vec4(record.physical_position_valid.xyz, packed_metadata(record.state_revision_frame_flags.x, record.logical_cell_lod)));
}
