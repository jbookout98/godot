#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(rgba8, set = 0, binding = 0) uniform restrict writeonly image3D color_grid;
layout(set = 0, binding = 1) uniform sampler3D voxel_texture;
layout(set = 0, binding = 2) uniform sampler3D brick_texture;
layout(set = 0, binding = 3) uniform sampler2D palette_texture;

layout(push_constant, std430) uniform Params {
	mat4 world_to_voxel;
	vec4 grid_origin_cell_size;
	ivec4 dispatch_origin;
	ivec4 volume_dimensions;
	ivec4 grid_resolution;
}
params;

const int BRICK_SIZE = 8;

int read_voxel(ivec3 cell) {
	ivec3 brick = cell / BRICK_SIZE;
	uvec4 directory = uvec4(round(texelFetch(brick_texture, brick, 0) * 255.0));
	uint code = directory.r | (directory.g << 8u) | (directory.b << 16u);
	if (code == 0u) {
		return 0;
	}
	if (code == 1u) {
		return int(directory.a);
	}

	uint slot = code - 2u;
	ivec3 atlas_bricks = max(textureSize(voxel_texture, 0) / BRICK_SIZE, ivec3(1));
	ivec3 atlas_brick = ivec3(
			int(slot % uint(atlas_bricks.x)),
			int((slot / uint(atlas_bricks.x)) % uint(atlas_bricks.y)),
			int(slot / uint(atlas_bricks.x * atlas_bricks.y)));
	ivec3 atlas_cell = atlas_brick * BRICK_SIZE + (cell % BRICK_SIZE);
	return int(round(texelFetch(voxel_texture, atlas_cell, 0).r * 255.0));
}

void main() {
	ivec3 grid_cell = params.dispatch_origin.xyz + ivec3(gl_GlobalInvocationID.xyz);
	if (any(greaterThanEqual(grid_cell, params.grid_resolution.xyz))) {
		return;
	}

	vec3 world_position = params.grid_origin_cell_size.xyz + (vec3(grid_cell) + vec3(0.5)) * params.grid_origin_cell_size.w;
	vec3 local_voxel_position = (params.world_to_voxel * vec4(world_position, 1.0)).xyz;
	ivec3 local_voxel = ivec3(floor(local_voxel_position));
	if (any(lessThan(local_voxel, ivec3(0))) || any(greaterThanEqual(local_voxel, params.volume_dimensions.xyz))) {
		return;
	}

	int voxel_id = read_voxel(local_voxel);
	if (voxel_id == 0) {
		return;
	}
	vec4 albedo = texelFetch(palette_texture, ivec2(clamp(voxel_id, 0, 255), 0), 0);
	imageStore(color_grid, grid_cell, vec4(albedo.rgb, 1.0));
}
