#[vertex]

#version 450

#VERSION_DEFINES

const uint PROBE_OFF = 0u;
const uint PROBE_SLEEPING = 1u;
const uint PROBE_NEWLY_AWAKE = 2u;
const uint PROBE_NEWLY_VIGILANT = 3u;
const uint PROBE_AWAKE = 4u;
const uint PROBE_VIGILANT = 5u;

struct ProbeRecord {
	ivec4 logical_cell_lod;
	vec4 physical_position_valid;
	uvec4 state_revision_frame_flags;
};

layout(set = 0, binding = 0, std430) readonly buffer ProbeRecords {
	ProbeRecord values[];
}
probe_records;
layout(set = 0, binding = 1) uniform sampler2D irradiance_atlas;
layout(set = 0, binding = 2) uniform sampler2D depth_atlas;

layout(push_constant, std430) uniform Params {
	mat4 view_projection;
	vec4 grid_origin_marker_size;
	vec4 cell_size;
	vec4 world_origin;
	ivec4 grid_lod_debug;
	ivec4 screen_frame;
}
params;

layout(location = 0) out vec4 marker_color;

const vec2 QUAD[6] = vec2[6](
	vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0),
	vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));

vec3 state_color(uint state) {
	if (state == PROBE_OFF) return vec3(1.0, 0.08, 0.08);
	if (state == PROBE_SLEEPING) return vec3(0.45);
	if (state == PROBE_NEWLY_AWAKE) return vec3(1.0, 0.45, 0.05);
	if (state == PROBE_NEWLY_VIGILANT) return vec3(1.0, 0.82, 0.05);
	if (state == PROBE_AWAKE) return vec3(0.1, 0.55, 1.0);
	if (state == PROBE_VIGILANT) return vec3(0.1, 1.0, 0.25);
	return vec3(0.2);
}

vec3 lod_color(int lod) {
	if (lod == 0) return vec3(0.05, 0.9, 1.0);
	if (lod == 1) return vec3(0.1, 1.0, 0.25);
	if (lod == 2) return vec3(1.0, 0.5, 0.05);
	return vec3(1.0, 0.1, 0.85);
}

ivec3 local_coord(uint index, int resolution) {
	return ivec3(int(index % uint(resolution)), int((index / uint(resolution)) % uint(resolution)), int(index / uint(resolution * resolution)));
}

void main() {
	uint record_index = uint(gl_InstanceIndex);
	ProbeRecord record = probe_records.values[record_index];
	uint state = record.state_revision_frame_flags.x;
	int mode = params.grid_lod_debug.z;
	bool show = true;
	if (mode == 3) show = record.physical_position_valid.w > 0.5;
	if (mode == 4) show = (state == PROBE_AWAKE || state == PROBE_VIGILANT) && record.state_revision_frame_flags.z == uint(params.screen_frame.z);
	if (mode == 5) show = state == PROBE_NEWLY_AWAKE || state == PROBE_NEWLY_VIGILANT;
	if (mode == 6) show = record.physical_position_valid.w > 0.5;
	if (!show) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		marker_color = vec4(0.0);
		return;
	}
	vec3 logical_position = params.world_origin.xyz + (vec3(record.logical_cell_lod.xyz) + vec3(0.5)) * params.cell_size.xyz;
	vec3 marker_position = mode == 2 ? logical_position : record.physical_position_valid.xyz;
	if (record.physical_position_valid.w <= 0.5) marker_position = logical_position;
	vec4 clip = params.view_projection * vec4(marker_position, 1.0);
	if (clip.w <= 0.0) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		marker_color = vec4(0.0);
		return;
	}
	vec2 pixel_scale = vec2(2.0) / vec2(max(params.screen_frame.xy, ivec2(1)));
	clip.xy += QUAD[gl_VertexIndex] * params.grid_origin_marker_size.w * pixel_scale * clip.w;
	gl_Position = clip;
	ivec2 tile = ivec2(int(record_index % uint(params.grid_lod_debug.x * params.grid_lod_debug.x)), int(record_index / uint(params.grid_lod_debug.x * params.grid_lod_debug.x)));
	ivec2 irradiance_center_texel = tile * 10 + ivec2(5);
	ivec2 visibility_center_texel = tile * 18 + ivec2(9);
	vec3 color = state_color(state);
	if (mode == 6 || mode == 10) color = lod_color(params.grid_lod_debug.y);
	if (mode == 7) color = texelFetch(irradiance_atlas, irradiance_center_texel, 0).rgb;
	if (mode == 8) {
		float depth = texelFetch(depth_atlas, visibility_center_texel, 0).r / max(params.cell_size.w, 0.0001);
		color = vec3(clamp(depth, 0.0, 1.0));
	}
	if (mode == 9) {
		vec4 moments = texelFetch(depth_atlas, visibility_center_texel, 0);
		float variance = max(moments.y - moments.x * moments.x, 0.0);
		color = vec3(clamp(sqrt(variance) / max(params.cell_size.w, 0.0001), 0.0, 1.0));
	}
	marker_color = vec4(color, 0.9);
}

#[fragment]

#version 450

#VERSION_DEFINES

layout(location = 0) in vec4 marker_color;

layout(location = 0) out vec4 frag_color;

void main() {
	frag_color = marker_color;
}
