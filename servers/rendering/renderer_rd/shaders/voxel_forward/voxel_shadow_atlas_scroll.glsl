#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_atlas;
layout(r32f, set = 0, binding = 1) uniform restrict writeonly image2D destination_atlas;

layout(push_constant, std430) uniform Params {
	ivec4 atlas_size;
	ivec4 cascade_shifts;
}
params;

void main() {
	ivec2 destination_pixel = ivec2(gl_GlobalInvocationID.xy);
	int tile_resolution = params.atlas_size.x;
	int cascade_count = params.atlas_size.y;
	if (destination_pixel.y >= tile_resolution || destination_pixel.x >= tile_resolution * cascade_count) {
		return;
	}

	int cascade = destination_pixel.x / tile_resolution;
	ivec2 destination_local = ivec2(destination_pixel.x - cascade * tile_resolution, destination_pixel.y);
	ivec2 shift = cascade == 0 ? params.cascade_shifts.xy : params.cascade_shifts.zw;
	// The new texel's world ray was stored at old local coordinate
	// source = destination + (new_center - old_center) / texel_size.
	ivec2 source_local = destination_local + shift;
	float value = 3.402823e38;
	if (all(greaterThanEqual(source_local, ivec2(0))) && all(lessThan(source_local, ivec2(tile_resolution)))) {
		ivec2 source_pixel = ivec2(source_local.x + cascade * tile_resolution, source_local.y);
		value = texelFetch(source_atlas, source_pixel, 0).r;
	}
	imageStore(destination_atlas, destination_pixel, vec4(value));
}
