#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0) uniform sampler3D source_grid;
layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image3D destination_grid;

layout(set = 0, binding = 2, std430) readonly buffer WorldDirectory {
	uvec4 entries[];
}
world_directory;

layout(set = 0, binding = 3, std430) readonly buffer MixedBricks {
	uint words[];
}
mixed_bricks;
layout(set = 0, binding = 4) uniform sampler3D injection_grid;

layout(push_constant, std430) uniform Params {
	vec4 world_origin_voxel_size;
	vec4 grid_origin_cell_size;
	ivec4 grid_directory;
	vec4 propagation;
	ivec4 dispatch_origin;
}
params;

const int DIRECTION_COUNT = 6;
const ivec3 DIRECTION_OFFSETS[6] = ivec3[6](
		ivec3(1, 0, 0), ivec3(-1, 0, 0),
		ivec3(0, 1, 0), ivec3(0, -1, 0),
		ivec3(0, 0, 1), ivec3(0, 0, -1));

ivec3 directional_texel(ivec3 cell, int direction, int resolution) {
	return ivec3(cell.x + direction * resolution, cell.y, cell.z);
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

bool world_occupied(vec3 world_position) {
	vec3 voxel_position_f = (world_position - params.world_origin_voxel_size.xyz) / params.world_origin_voxel_size.w;
	ivec3 voxel_position = ivec3(floor(voxel_position_f));
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

void main() {
	ivec3 cell = params.dispatch_origin.xyz + ivec3(gl_GlobalInvocationID.xyz);
	int resolution = params.grid_directory.x;
	if (any(greaterThanEqual(cell, ivec3(resolution)))) {
		return;
	}
	vec3 cell_center = params.grid_origin_cell_size.xyz + (vec3(cell) + vec3(0.5)) * params.grid_origin_cell_size.w;
	if (world_occupied(cell_center)) {
		for (int direction_index = 0; direction_index < DIRECTION_COUNT; direction_index++) {
			imageStore(destination_grid, directional_texel(cell, direction_index, resolution), vec4(0.0));
		}
		return;
	}

	for (int lobe = 0; lobe < DIRECTION_COUNT; lobe++) {
		ivec3 target_texel = directional_texel(cell, lobe, resolution);
		vec4 injection = texelFetch(injection_grid, target_texel, 0);
		vec3 axial_transport = vec3(0.0);

		ivec3 axial_neighbor = cell + DIRECTION_OFFSETS[lobe];
		if (all(greaterThanEqual(axial_neighbor, ivec3(0))) && all(lessThan(axial_neighbor, ivec3(resolution)))) {
			// This lobe means that the source is in axial_neighbor. Straight-through
			// radiance retains full strength. A six-axis field also has to reproject
			// oblique transport: otherwise energy may spread spatially while keeping
			// its old lobe, and a side wall samples black even though raw irradiance
			// visibly reached it. The field stores the strongest reachable diffuse
			// radiance for each source direction, rather than additive photon energy,
			// so a turn changes the discrete path direction without an extra loss.
			// The opposite lobe is deliberately excluded; accepting it would send
			// energy directly back toward its source and create a two-cell feedback
			// loop. Max, rather than addition, keeps this coarse angular projection
			// bounded and independent of how many lobes contain the same source. The
			// authored propagation decay below is therefore the only per-cell loss;
			// the former 0.25 turn factor caused an undocumented 0.25^N attenuation
			// along the alternating-axis paths needed to reach corridor side walls.
			axial_transport = texelFetch(source_grid, directional_texel(axial_neighbor, lobe, resolution), 0).rgb;
			int opposite_lobe = lobe ^ 1;
			for (int source_lobe = 0; source_lobe < DIRECTION_COUNT; source_lobe++) {
				if (source_lobe == lobe || source_lobe == opposite_lobe) {
					continue;
				}
				vec3 turned = texelFetch(source_grid, directional_texel(axial_neighbor, source_lobe, resolution), 0).rgb;
				axial_transport = max(axial_transport, turned);
			}
			// Face transmittance is already the geometric attenuation between the
			// source neighbor and this cell.
			axial_transport *= injection.a;
		}

		// Injection is a boundary condition, not a source term to accumulate once
		// per iteration. Max keeps the direct value and the transported field stable
		// while making propagation_decay the sole per-cell retention control.
		vec3 result = max(injection.rgb, max(axial_transport, vec3(0.0)) * params.propagation.x);
		imageStore(destination_grid, target_texel, vec4(min(result, vec3(64.0)), injection.a));
	}
}
