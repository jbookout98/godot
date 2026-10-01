#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0) uniform sampler3D history_grid;
layout(set = 0, binding = 1) uniform sampler3D target_grid;
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image3D output_grid;

layout(push_constant, std430) uniform Params {
	vec4 history_origin_cell_size;
	vec4 target_origin_cell_size;
	vec4 fallback_radiance_blend;
	ivec4 dispatch_origin_resolution;
	ivec4 state;
}
params;

const int DIRECTION_COUNT = 6;

ivec3 directional_texel(ivec3 cell, int direction, int resolution) {
	return ivec3(cell.x + direction * resolution, cell.y, cell.z);
}

vec3 directional_uvw(vec3 grid_uvw, int direction) {
	return vec3((float(direction) + grid_uvw.x) / float(DIRECTION_COUNT), grid_uvw.yz);
}

void main() {
	ivec3 target_cell = params.dispatch_origin_resolution.xyz + ivec3(gl_GlobalInvocationID.xyz);
	int resolution = params.dispatch_origin_resolution.w;
	if (any(greaterThanEqual(target_cell, ivec3(resolution)))) {
		return;
	}

	vec3 world_position = params.target_origin_cell_size.xyz + (vec3(target_cell) + vec3(0.5)) * params.target_origin_cell_size.w;
	vec3 history_uvw = (world_position - params.history_origin_cell_size.xyz) /
			(params.history_origin_cell_size.w * float(resolution));
	vec3 half_texel = vec3(0.5 / float(resolution));
	bool history_valid = params.state.x != 0 && all(greaterThanEqual(history_uvw, half_texel)) && all(lessThanEqual(history_uvw, vec3(1.0) - half_texel));
	float blend = clamp(params.fallback_radiance_blend.w, 0.0, 1.0);
	for (int direction = 0; direction < DIRECTION_COUNT; direction++) {
		ivec3 texel = directional_texel(target_cell, direction, resolution);
		vec4 target = texelFetch(target_grid, texel, 0);
		vec3 history = params.fallback_radiance_blend.rgb;
		if (history_valid) {
			history = textureLod(history_grid, directional_uvw(history_uvw, direction), 0.0).rgb;
		}
		imageStore(output_grid, texel, vec4(mix(history, target.rgb, blend), target.a));
	}
}
