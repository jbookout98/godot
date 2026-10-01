#[compute]
#version 450
#VERSION_DEFINES
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
struct ProbeRecord {
	ivec4 cell;
	vec4 position;
	uvec4 state;
};
layout(set = 0, binding = 0, std430) readonly buffer Seeds {
	uint words[];
}
seeds;
layout(set = 0, binding = 1, std430) buffer Records {
	ProbeRecord values[];
}
records;
layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D irradiance;
layout(rgba16f, set = 0, binding = 3) uniform restrict writeonly image2D visibility;
layout(rgba32f, set = 0, binding = 4) uniform restrict writeonly image2D metadata;
layout(push_constant, std430) uniform Params {
	uint resolution;
	uint count;
	uint revision;
	uint padding;
}
params;
void main() {
	uint index = gl_WorkGroupID.x;
	if (index >= params.count) {
		return;
	}
	uint base = index * 861u;
	uint slot = seeds.words[base++];
	ivec2 tile = ivec2(int(slot % (params.resolution * params.resolution)), int(slot / (params.resolution * params.resolution)));
	for (uint pixel = gl_LocalInvocationID.x; pixel < 100u; pixel += 64u) {
		uint at = base + 12u + pixel * 2u;
		imageStore(irradiance, tile * 10 + ivec2(int(pixel % 10u), int(pixel / 10u)), vec4(unpackHalf2x16(seeds.words[at]), unpackHalf2x16(seeds.words[at + 1u])));
	}
	for (uint pixel = gl_LocalInvocationID.x; pixel < 324u; pixel += 64u) {
		uint at = base + 212u + pixel * 2u;
		imageStore(visibility, tile * 18 + ivec2(int(pixel % 18u), int(pixel / 18u)), vec4(unpackHalf2x16(seeds.words[at]), unpackHalf2x16(seeds.words[at + 1u])));
	}
	if (gl_LocalInvocationID.x == 0u) {
		ProbeRecord record;
		record.cell = ivec4(seeds.words[base], seeds.words[base + 1u], seeds.words[base + 2u], seeds.words[base + 3u]);
		record.position = uintBitsToFloat(uvec4(seeds.words[base + 4u], seeds.words[base + 5u], seeds.words[base + 6u], seeds.words[base + 7u]));
		record.state = uvec4(seeds.words[base + 8u], params.revision, seeds.words[base + 10u], seeds.words[base + 11u]);
		// Runtime inputs excluded from the authored bake warm-start from saved
		// samples, then use the existing dirty-probe convergence scheduler.
		if (params.padding != 0u && (record.state.x == 4u || record.state.x == 5u)) {
			record.state.x += 2u;
			record.state.z = 0u;
		}
		records.values[slot] = record;
		uint hash = 2166136261u;
		for (uint i = 0u; i < 4u; i++) {
			hash = (hash ^ uint(record.cell[i])) * 16777619u;
		}
		float packed = float(((hash & 0xfffffu) << 4u) | (record.state.x & 15u));
		imageStore(metadata, tile, vec4(record.position.xyz, packed));
	}
}
