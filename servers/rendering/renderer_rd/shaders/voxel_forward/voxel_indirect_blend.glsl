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

void main() {
	ivec3 target_cell = params.dispatch_origin_resolution.xyz + ivec3(gl_GlobalInvocationID.xyz);
	int resolution = params.dispatch_origin_resolution.w;
	if (any(greaterThanEqual(target_cell, ivec3(resolution)))) {
		return;
	}

	vec3 target = texelFetch(target_grid, target_cell, 0).rgb;
	vec3 history = params.fallback_radiance_blend.rgb;
	if (params.state.x != 0) {
		vec3 world_position = params.target_origin_cell_size.xyz + (vec3(target_cell) + vec3(0.5)) * params.target_origin_cell_size.w;
		vec3 history_uvw = (world_position - params.history_origin_cell_size.xyz) /
				(params.history_origin_cell_size.w * float(resolution));
		// Cell centers at the exact edge are valid. Anything outside the old
		// cascade has no history and begins at the configured ambient fallback.
		vec3 half_texel = vec3(0.5 / float(resolution));
		if (all(greaterThanEqual(history_uvw, half_texel)) && all(lessThanEqual(history_uvw, vec3(1.0) - half_texel))) {
			history = textureLod(history_grid, history_uvw, 0.0).rgb;
		}
	}

	float blend = clamp(params.fallback_radiance_blend.w, 0.0, 1.0);
	imageStore(output_grid, target_cell, vec4(mix(history, target, blend), 1.0));
}
