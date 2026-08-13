#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(rgba16f, set = 0, binding = 0) uniform restrict writeonly image3D irradiance_grid;
layout(set = 0, binding = 1) uniform sampler2D shadow_atlas;

layout(set = 0, binding = 2, std430) readonly buffer WorldDirectory {
	uvec4 entries[];
}
world_directory;

layout(set = 0, binding = 3, std430) readonly buffer MixedBricks {
	uint words[];
}
mixed_bricks;

layout(push_constant, std430) uniform Params {
	vec4 world_origin_voxel_size;
	vec4 grid_origin_cell_size;
	vec4 light_direction_energy;
	vec4 light_color_bias;
	vec4 atlas_center_resolution;
	vec4 tangent_near_extent;
	vec4 bitangent_far_extent;
	ivec4 grid_directory;
}
params;

uint brick_hash(ivec3 position) {
	return uint(position.x) * 73856093u ^ uint(position.y) * 19349663u ^ uint(position.z) * 83492791u;
}

uint find_brick(ivec3 position) {
	uint slot = brick_hash(position) & uint(params.grid_directory.y);
	for (uint probe = 0u; probe < 64u; probe++) {
		uvec4 entry = world_directory.entries[slot];
		if (entry.w == 0u) {
			return 0u;
		}
		if (ivec3(entry.xyz) == position) {
			return entry.w;
		}
		slot = (slot + 1u) & uint(params.grid_directory.y);
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

bool world_occupied(vec3 world_position) {
	vec3 voxel_position = (world_position - params.world_origin_voxel_size.xyz) / params.world_origin_voxel_size.w;
	return voxel_occupied(ivec3(floor(voxel_position)));
}

float shadow_visibility(vec3 world_position) {
	vec3 relative = world_position - params.atlas_center_resolution.xyz;
	float light_x = dot(relative, params.tangent_near_extent.xyz);
	float light_y = dot(relative, params.bitangent_far_extent.xyz);
	float edge_distance = max(abs(light_x), abs(light_y));
	float near_extent = params.tangent_near_extent.w;
	float far_extent = params.bitangent_far_extent.w;
	int cascade = edge_distance <= near_extent * 0.95 ? 0 : 1;
	float extent = cascade == 0 ? near_extent : far_extent;
	if (edge_distance >= extent) {
		return 1.0;
	}
	int resolution = int(params.atlas_center_resolution.w);
	vec2 atlas_position = (vec2(light_x, light_y) / (2.0 * extent) + vec2(0.5)) * float(resolution) - vec2(0.5);
	ivec2 texel = clamp(ivec2(round(atlas_position)), ivec2(0), ivec2(resolution - 1));
	texel.x += cascade * resolution;
	float receiver_depth = far_extent - dot(relative, normalize(params.light_direction_energy.xyz));
	float occluder_depth = texelFetch(shadow_atlas, texel, 0).r;
	return receiver_depth <= occluder_depth + params.light_color_bias.w ? 1.0 : 0.0;
}

void main() {
	ivec3 cell = ivec3(gl_GlobalInvocationID.xyz);
	int resolution = params.grid_directory.x;
	if (any(greaterThanEqual(cell, ivec3(resolution)))) {
		return;
	}

	vec3 cell_center = params.grid_origin_cell_size.xyz + (vec3(cell) + vec3(0.5)) * params.grid_origin_cell_size.w;
	if (world_occupied(cell_center)) {
		imageStore(irradiance_grid, cell, vec4(0.0));
		return;
	}

	const vec3 axes[6] = vec3[6](
		vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0),
		vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
		vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0));
	vec3 injected = vec3(0.0);
	int source_count = 0;
	float voxel_size = params.world_origin_voxel_size.w;
	float sample_spacing = max(voxel_size, params.grid_origin_cell_size.w / float(max(params.grid_directory.z, 1)));
	for (int direction_index = 0; direction_index < 6; direction_index++) {
		vec3 toward_solid = axes[direction_index];
		for (int step_index = 1; step_index <= 8; step_index++) {
			if (step_index > params.grid_directory.z) {
				break;
			}
			vec3 occupied_center = cell_center + toward_solid * (float(step_index) * sample_spacing);
			if (!world_occupied(occupied_center)) {
				continue;
			}
			// Coarse cascades intentionally take large search steps. Refine the
			// empty-to-solid interval so shadow visibility is evaluated at the
			// actual voxel boundary instead of from deep inside a wall.
			vec3 empty_position = cell_center + toward_solid * (float(step_index - 1) * sample_spacing);
			for (int refinement = 0; refinement < 3; refinement++) {
				vec3 midpoint = (empty_position + occupied_center) * 0.5;
				if (world_occupied(midpoint)) {
					occupied_center = midpoint;
				} else {
					empty_position = midpoint;
				}
			}
			vec3 surface_normal = -toward_solid;
			float facing = max(dot(surface_normal, normalize(params.light_direction_energy.xyz)), 0.0);
			if (facing > 0.0) {
				vec3 surface_position = occupied_center + surface_normal * (voxel_size * 0.501);
				float visibility = shadow_visibility(surface_position);
				injected += params.light_color_bias.rgb * (params.light_direction_energy.w * facing * visibility);
				source_count++;
			}
			break;
		}
	}
	if (source_count > 0) {
		injected /= float(source_count);
	}
	imageStore(irradiance_grid, cell, vec4(injected, 1.0));
}
