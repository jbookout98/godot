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

layout(set = 0, binding = 0, std430) readonly buffer SpatialReservoirs {
	Reservoir values[];
}
spatial_reservoirs;
layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image2D indirect_output;

layout(set = 0, binding = 2, std140) uniform Params {
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

const uint RESERVOIR_VALID = 1u;

bool compatible_face(Reservoir center, Reservoir sample_value) {
	if ((sample_value.metadata.w & RESERVOIR_VALID) == 0u) {
		return false;
	}
	if (any(isnan(sample_value.sample_radiance_weight_sum.xyz)) || any(isinf(sample_value.sample_radiance_weight_sum.xyz))) {
		return false;
	}
	if (dot(center.visible_normal_estimator.xyz, sample_value.visible_normal_estimator.xyz) < 0.9999) {
		return false;
	}
	// Visible positions are canonical voxel-face centers. Permit adjacent voxels
	// only when their faces lie on the same plane. This lets subpixel/single-pixel
	// voxel faces share lighting across a continuous wall while retaining hard
	// boundaries at corners, silhouettes, steps, and disocclusions.
	vec3 position_delta = sample_value.visible_position_valid.xyz - center.visible_position_valid.xyz;
	return abs(dot(position_delta, center.visible_normal_estimator.xyz)) <= params.world_origin_voxel_size.w * 0.01;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen.xy))) {
		return;
	}
	// Debug views are written by the spatial pass and must expose raw reservoir
	// state rather than a filtered visualization.
	if (((params.state.w >> 1u) & 0xffu) != 0u) {
		return;
	}

	uint index = uint(pixel.x + pixel.y * params.screen.x);
	Reservoir center = spatial_reservoirs.values[index];
	if ((center.metadata.w & RESERVOIR_VALID) == 0u) {
		imageStore(indirect_output, pixel, vec4(0.0));
		return;
	}

	vec3 reconstructed = vec3(0.0);
	float sample_count = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 candidate_pixel = pixel + ivec2(x, y);
			if (any(lessThan(candidate_pixel, ivec2(0))) || any(greaterThanEqual(candidate_pixel, params.screen.xy))) {
				continue;
			}
			Reservoir candidate = spatial_reservoirs.values[candidate_pixel.x + candidate_pixel.y * params.screen.x];
			if (!compatible_face(center, candidate)) {
				continue;
			}
			reconstructed += max(candidate.sample_radiance_weight_sum.xyz, vec3(0.0));
			sample_count += 1.0;
		}
	}
	reconstructed /= max(sample_count, 1.0);
	imageStore(indirect_output, pixel, vec4(clamp(reconstructed, vec3(0.0), vec3(params.limits.w)), 1.0));
}
