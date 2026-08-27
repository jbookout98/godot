#[vertex]

#version 450

#VERSION_DEFINES

void main() {
	vec2 vertex = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(vertex * 2.0 - 1.0, 0.0, 1.0);
}

#[fragment]

#version 450

#VERSION_DEFINES

layout(set = 0, binding = 0) uniform usampler2D voxel_hit_buffer;
layout(set = 0, binding = 1) uniform sampler2D voxel_depth_buffer;
layout(set = 0, binding = 2, std430) readonly buffer InstanceWords {
	uint words[];
}
instance_data;

layout(push_constant, std430) uniform Params {
	ivec4 screen_instances;
	ivec4 instance_layout;
	vec4 thresholds;
}
params;

layout(location = 0) out vec4 frag_color;

const ivec2 CARDINAL_DIRECTIONS[4] = ivec2[](
	ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));

bool inside_screen(ivec2 pixel) {
	return all(greaterThanEqual(pixel, ivec2(0))) && all(lessThan(pixel, params.screen_instances.xy));
}

uint hit_at(ivec2 pixel) {
	return inside_screen(pixel) ? texelFetch(voxel_hit_buffer, pixel, 0).r : 0u;
}

float depth_at(ivec2 pixel) {
	return inside_screen(pixel) ? texelFetch(voxel_depth_buffer, pixel, 0).r : 0.0;
}

bool load_style(uint payload, out vec4 color, out float width) {
	uint encoded_owner = payload >> 14u;
	if (encoded_owner == 0u || encoded_owner > uint(params.screen_instances.z)) {
		return false;
	}
	uint word = (encoded_owner - 1u) * uint(params.instance_layout.x) + uint(params.instance_layout.y);
	width = uintBitsToFloat(instance_data.words[word + 1u]);
	uint packed = instance_data.words[word + 2u];
	color = vec4(
		float((packed >> 24u) & 0xFFu),
		float((packed >> 16u) & 0xFFu),
		float((packed >> 8u) & 0xFFu),
		float(packed & 0xFFu)) / 255.0;
	return width > 0.0 && color.a > 0.0;
}

bool normals_discontinuous(uint center_payload, uint neighbor_payload) {
	uint center_world_face = (center_payload >> 11u) & 0x7u;
	uint neighbor_world_face = (neighbor_payload >> 11u) & 0x7u;
	if (center_world_face < 6u && neighbor_world_face < 6u) {
		return center_world_face != neighbor_world_face;
	}
	// Arbitrarily rotated volumes do not have a cardinal world-face code. Local
	// face codes remain comparable within one owner, but never across owners.
	if ((center_payload >> 14u) != (neighbor_payload >> 14u)) {
		return false;
	}
	return ((center_payload >> 8u) & 0x7u) != ((neighbor_payload >> 8u) & 0x7u);
}

void main() {
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	uint center_payload = hit_at(pixel);
	vec4 outline_color;
	float outline_width;
	if (center_payload == 0u || !load_style(center_payload, outline_color, outline_width)) {
		discard;
	}

	float center_depth = depth_at(pixel);
	float planar_depth_delta = fwidth(center_depth);
	int radius = clamp(int(ceil(outline_width)), 1, 4);
	bool edge = false;
	for (int direction = 0; direction < 4; direction++) {
		ivec2 neighbor_pixel = pixel + CARDINAL_DIRECTIONS[direction] * radius;
		uint neighbor_payload = hit_at(neighbor_pixel);
		if (neighbor_payload == 0u || normals_discontinuous(center_payload, neighbor_payload)) {
			edge = true;
			break;
		}
		float neighbor_depth = depth_at(neighbor_pixel);
		float expected_planar_delta = planar_depth_delta * float(radius) * params.thresholds.y;
		float depth_threshold = max(params.thresholds.x, expected_planar_delta);
		if (abs(center_depth - neighbor_depth) > depth_threshold) {
			edge = true;
			break;
		}
	}

	if (!edge) {
		discard;
	}
	// Sub-pixel widths fade coverage; widths of one pixel or more remain crisp.
	frag_color = vec4(outline_color.rgb, outline_color.a * min(outline_width, 1.0));
}
