#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0) uniform sampler3D active_grid;
layout(set = 0, binding = 1) uniform sampler3D staging_grid;
layout(set = 0, binding = 2, std430) buffer BoundaryResult {
	uint maximum_delta_bits;
}
boundary_result;

layout(push_constant, std430) uniform Params {
	ivec4 dispatch_origin_resolution;
	ivec4 dispatch_size;
}
params;

void main() {
	ivec3 local_cell = ivec3(gl_GlobalInvocationID.xyz);
	ivec3 region_size = params.dispatch_size.xyz;
	if (any(greaterThanEqual(local_cell, region_size))) {
		return;
	}
	bool boundary = any(equal(local_cell, ivec3(0))) || any(equal(local_cell, region_size - ivec3(1)));
	if (!boundary) {
		return;
	}
	ivec3 cell = params.dispatch_origin_resolution.xyz + local_cell;
	int resolution = params.dispatch_origin_resolution.w;
	float maximum_delta = 0.0;
	for (int direction = 0; direction < 6; direction++) {
		ivec3 texel = ivec3(cell.x + direction * resolution, cell.y, cell.z);
		vec3 active_value = texelFetch(active_grid, texel, 0).rgb;
		vec3 staging_value = texelFetch(staging_grid, texel, 0).rgb;
		vec3 difference = abs(active_value - staging_value);
		maximum_delta = max(maximum_delta, max(difference.x, max(difference.y, difference.z)));
	}
	// Radiance deltas are finite and non-negative, so IEEE-754 uint ordering is
	// monotonic and atomicMax produces the exact maximum without float atomics.
	atomicMax(boundary_result.maximum_delta_bits, floatBitsToUint(maximum_delta));
}
