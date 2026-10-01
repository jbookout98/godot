#[compute]

#version 450

// The probe atlas is updated under a bounded budget, so its raw screen resolve
// changes on probe-update frames rather than every rendered frame. Reproject a
// canonical voxel face into the previous frame and continuously publish the new
// value only when the exact world-space face and normal still match. This keeps
// the filter attached to geometry instead of smearing lighting across edges.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D raw_gi;
layout(set = 0, binding = 1) uniform sampler2D current_face_data;
layout(set = 0, binding = 2) uniform sampler2D previous_gi;
layout(set = 0, binding = 3) uniform sampler2D previous_face_data;
layout(rgba16f, set = 0, binding = 4) uniform restrict writeonly image2D published_gi;
layout(rgba32f, set = 0, binding = 5) uniform restrict writeonly image2D published_face_data;
layout(rgba16f, set = 0, binding = 6) uniform restrict writeonly image2D stable_published_gi;

layout(set = 0, binding = 7, std140) uniform Params {
	mat4 previous_view_projection;
	ivec4 screen_history;
	vec4 temporal;
} params;

vec3 decode_face_normal(float packed_float) {
	uint packed = uint(max(packed_float, 1.0) + 0.5) - 1u;
	vec2 oct = vec2(float(packed & 0xfffu), float((packed >> 12u) & 0xfffu)) / 4095.0;
	vec2 folded = oct * 2.0 - 1.0;
	vec3 normal = vec3(folded.x, 1.0 - abs(folded.x) - abs(folded.y), folded.y);
	if (normal.y < 0.0) {
		normal.xz = (1.0 - abs(normal.zx)) * sign(normal.xz);
	}
	return normalize(normal);
}

bool matching_history(vec4 current_face, out vec4 history) {
	history = vec4(0.0);
	if (params.screen_history.z == 0 || current_face.w < 1.0) return false;
	vec4 previous_clip = params.previous_view_projection * vec4(current_face.xyz, 1.0);
	if (previous_clip.w <= 0.000001) return false;
	vec2 previous_uv = previous_clip.xy / previous_clip.w * 0.5 + 0.5;
	if (any(lessThan(previous_uv, vec2(0.0))) || any(greaterThanEqual(previous_uv, vec2(1.0)))) return false;
	ivec2 previous_pixel = ivec2(clamp(previous_uv * vec2(params.screen_history.xy), vec2(0.0), vec2(params.screen_history.xy - 1)));
	vec4 previous_face = texelFetch(previous_face_data, previous_pixel, 0);
	if (previous_face.w < 1.0) return false;
	float position_error = length(previous_face.xyz - current_face.xyz);
	float normal_alignment = dot(decode_face_normal(previous_face.w), decode_face_normal(current_face.w));
	if (position_error > params.temporal.y || normal_alignment < params.temporal.z) return false;
	history = texelFetch(previous_gi, previous_pixel, 0);
	return all(not(isnan(history))) && all(not(isinf(history)));
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_history.xy))) return;
	vec4 face = texelFetch(current_face_data, pixel, 0);
	vec4 current = max(texelFetch(raw_gi, pixel, 0), vec4(0.0));
	vec4 history;
	vec4 published = matching_history(face, history) ? mix(history, current, params.temporal.x) : current;
	imageStore(published_gi, pixel, max(published, vec4(0.0)));
	imageStore(stable_published_gi, pixel, max(published, vec4(0.0)));
	imageStore(published_face_data, pixel, face);
}
