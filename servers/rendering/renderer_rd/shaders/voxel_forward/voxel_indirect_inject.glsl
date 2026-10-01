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

// The permanent injection field keeps direct/emissive sources separate from
// the Jacobi propagation buffers. Alpha stores the open fraction of the face
// leading toward this lobe's source direction.
layout(rgba16f, set = 0, binding = 4) uniform restrict writeonly image3D injection_grid;
layout(set = 0, binding = 5) uniform sampler3D color_near;
layout(set = 0, binding = 6) uniform sampler3D color_far;
layout(set = 0, binding = 7) uniform sampler3D color_distant;
layout(set = 0, binding = 8) uniform sampler3D material_near;
layout(set = 0, binding = 9) uniform sampler3D material_far;
layout(set = 0, binding = 10) uniform sampler3D material_distant;
layout(set = 0, binding = 11, std140) uniform ColorGridParams {
	vec4 origin_cell_size[3];
	ivec4 resolution;
}
color_params;
layout(set = 0, binding = 12) uniform sampler3D parent_grid;
layout(set = 0, binding = 13, std140) uniform ParentGridParams {
	vec4 origin_cell_size;
	ivec4 state;
}
parent_params;

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

const int DIRECTION_COUNT = 6;
const vec3 DIRECTION_AXES[6] = vec3[6](
		vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0),
		vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
		vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0));

ivec3 directional_texel(ivec3 cell, int direction, int resolution) {
	return ivec3(cell.x + direction * resolution, cell.y, cell.z);
}

vec3 sample_parent_lobe(vec3 world_position, int direction) {
	int resolution = parent_params.state.y;
	vec3 local = (world_position - parent_params.origin_cell_size.xyz) / parent_params.origin_cell_size.w;
	ivec3 cell = ivec3(floor(local));
	if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(resolution)))) {
		return vec3(0.0);
	}
	return texelFetch(parent_grid, directional_texel(cell, direction, resolution), 0).rgb;
}

bool lies_on_parent_source_face(vec3 world_position, int direction) {
	if (parent_params.state.x == 0 || parent_params.origin_cell_size.w <= params.grid_origin_cell_size.w) {
		return false;
	}
	vec3 parent_local = (world_position - parent_params.origin_cell_size.xyz) / parent_params.origin_cell_size.w;
	vec3 within_parent = fract(parent_local);
	float face_layer = min(params.grid_origin_cell_size.w / parent_params.origin_cell_size.w * 0.55, 0.49);
	int axis = direction >> 1;
	return (direction & 1) == 0 ? within_parent[axis] >= 1.0 - face_layer : within_parent[axis] <= face_layer;
}

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

bool sample_clip_cell(sampler3D color_grid, sampler3D material_grid, vec4 origin_cell_size, vec3 world_position, out vec3 albedo, out vec4 material) {
	ivec3 resolution = textureSize(color_grid, 0);
	ivec3 cell = ivec3(floor((world_position - origin_cell_size.xyz) / origin_cell_size.w));
	if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, resolution))) {
		return false;
	}
	vec4 color_sample = texelFetch(color_grid, cell, 0);
	if (color_sample.a < 0.49) {
		return false;
	}
	albedo = color_sample.rgb;
	material = texelFetch(material_grid, cell, 0);
	return true;
}

bool sample_surface_data(vec3 world_position, out vec3 albedo, out vec4 material) {
	if (sample_clip_cell(color_near, material_near, color_params.origin_cell_size[0], world_position, albedo, material)) {
		return true;
	}
	if (sample_clip_cell(color_far, material_far, color_params.origin_cell_size[1], world_position, albedo, material)) {
		return true;
	}
	return sample_clip_cell(color_distant, material_distant, color_params.origin_cell_size[2], world_position, albedo, material);
}

float connection_transmittance(vec3 cell_center, int direction_index) {
	vec3 direction = DIRECTION_AXES[direction_index];
	ivec3 voxel_step = ivec3(round(direction));
	vec3 tangent_u = abs(direction.x) > 0.5 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent_v = normalize(cross(direction, tangent_u));
	float radius = params.grid_origin_cell_size.w * 0.24;
	const vec2 offsets[5] = vec2[5](vec2(0.0), vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
	float transmitted_rays = 0.0;
	for (int ray_index = 0; ray_index < 5; ray_index++) {
		vec3 transverse = (tangent_u * offsets[ray_index].x + tangent_v * offsets[ray_index].y) * radius;
		vec3 ray_start = cell_center + transverse + direction * (params.world_origin_voxel_size.w * 0.001);
		vec3 ray_end = cell_center + transverse + direction * (params.grid_origin_cell_size.w - params.world_origin_voxel_size.w * 0.001);
		ivec3 voxel_position = ivec3(floor((ray_start - params.world_origin_voxel_size.xyz) / params.world_origin_voxel_size.w));
		ivec3 end_voxel = ivec3(floor((ray_end - params.world_origin_voxel_size.xyz) / params.world_origin_voxel_size.w));
		float ray_transmittance = 1.0;
		// The segment is axis aligned, so walking integer voxel coordinates is an
		// exact DDA. Unlike the old eight point samples, this cannot step over a
		// one-voxel-thick wall in the far or distant cascades.
		for (int voxel_index = 0; voxel_index < 256; voxel_index++) {
			if (voxel_occupied(voxel_position)) {
				vec3 sample_position = params.world_origin_voxel_size.xyz + (vec3(voxel_position) + vec3(0.5)) * params.world_origin_voxel_size.w;
				vec3 ignored_albedo;
				vec4 material = vec4(0.0);
				if (sample_surface_data(sample_position, ignored_albedo, material)) {
					ray_transmittance *= clamp(material.a, 0.0, 1.0);
				} else {
					ray_transmittance = 0.0;
				}
				if (ray_transmittance <= 0.001) {
					break;
				}
			}
			if (all(equal(voxel_position, end_voxel))) {
				break;
			}
			voxel_position += voxel_step;
		}
		transmitted_rays += ray_transmittance;
	}
	return transmitted_rays * 0.2;
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
	float receiver_depth = far_extent - dot(relative, params.light_direction_energy.xyz);
	float occluder_depth = texelFetch(shadow_atlas, texel, 0).r;
	return receiver_depth <= occluder_depth + params.light_color_bias.w ? 1.0 : 0.0;
}

void main() {
	uint packed_dispatch_origin = uint(params.grid_directory.w);
	ivec3 dispatch_origin = ivec3(packed_dispatch_origin & 0xffu, (packed_dispatch_origin >> 8u) & 0xffu, (packed_dispatch_origin >> 16u) & 0xffu);
	ivec3 cell = dispatch_origin + ivec3(gl_GlobalInvocationID.xyz);
	int resolution = params.grid_directory.x;
	if (any(greaterThanEqual(cell, ivec3(resolution)))) {
		return;
	}

	vec3 cell_center = params.grid_origin_cell_size.xyz + (vec3(cell) + vec3(0.5)) * params.grid_origin_cell_size.w;
	if (world_occupied(cell_center)) {
		vec3 ignored_albedo;
		vec4 center_material = vec4(0.0);
		float center_transmittance = sample_surface_data(cell_center, ignored_albedo, center_material) ? clamp(center_material.a, 0.0, 1.0) : 0.0;
		if (center_transmittance <= 0.001) {
			for (int direction_index = 0; direction_index < DIRECTION_COUNT; direction_index++) {
				ivec3 texel = directional_texel(cell, direction_index, resolution);
				imageStore(irradiance_grid, texel, vec4(0.0));
				imageStore(injection_grid, texel, vec4(0.0));
			}
			return;
		}
	}

	float voxel_size = params.world_origin_voxel_size.w;
	for (int direction_index = 0; direction_index < 6; direction_index++) {
		vec3 toward_solid = DIRECTION_AXES[direction_index];
		ivec3 voxel_step = ivec3(round(toward_solid));
		vec3 search_start = cell_center + toward_solid * (voxel_size * 0.501);
		vec3 search_end = cell_center + toward_solid * params.grid_origin_cell_size.w;
		ivec3 search_voxel = ivec3(floor((search_start - params.world_origin_voxel_size.xyz) / voxel_size));
		ivec3 end_voxel = ivec3(floor((search_end - params.world_origin_voxel_size.xyz) / voxel_size));
		vec3 injected = vec3(0.0);
		for (int voxel_index = 0; voxel_index < 256; voxel_index++) {
			if (!voxel_occupied(search_voxel)) {
				if (all(equal(search_voxel, end_voxel))) {
					break;
				}
				search_voxel += voxel_step;
				continue;
			}
			vec3 occupied_center = params.world_origin_voxel_size.xyz + (vec3(search_voxel) + vec3(0.5)) * voxel_size;
			vec3 surface_normal = -toward_solid;
			float facing = max(dot(surface_normal, params.light_direction_energy.xyz), 0.0);
			vec3 surface_position = occupied_center + surface_normal * (voxel_size * 0.501);
			vec3 albedo = vec3(0.8);
			vec4 material = vec4(0.0);
			sample_surface_data(occupied_center, albedo, material);
			float metallic = clamp(material.g, 0.0, 1.0);
			float emission = clamp(material.b, 0.0, 1.0);
			float transparency = clamp(material.a, 0.0, 1.0);
			float diffuse_response = (1.0 - metallic) * (1.0 - transparency);
			float visibility = facing > 0.0 ? shadow_visibility(surface_position) : 0.0;
			injected = albedo * (params.light_color_bias.rgb * (params.light_direction_energy.w * facing * visibility * diffuse_response) + vec3(emission * 2.0));
			break;
		}
		float transmittance = connection_transmittance(cell_center, direction_index);
		// The three clipmaps form one hierarchical solve. A coarser lobe enters the
		// finer grid only on the matching parent-cell face, and only through the
		// exact fine-grid connection for that face. Fine propagation then carries it
		// across the parent cell. This supplies long-range energy without allowing a
		// coarse per-pixel estimate to replace known fine-grid occlusion.
		if (transmittance > 0.001 && lies_on_parent_source_face(cell_center, direction_index)) {
			vec3 parent_light = sample_parent_lobe(cell_center - DIRECTION_AXES[direction_index] * params.grid_origin_cell_size.w * 0.01, direction_index);
			injected = max(injected, parent_light * transmittance);
		}
		ivec3 texel = directional_texel(cell, direction_index, resolution);
		vec4 directional_value = vec4(max(injected, vec3(0.0)), transmittance);
		imageStore(irradiance_grid, texel, directional_value);
		imageStore(injection_grid, texel, directional_value);
	}
}
