#[vertex]

#version 450

#VERSION_DEFINES

layout(push_constant, std430) uniform Params {
	mat4 model_view_projection;
	vec4 camera_local;
	ivec4 volume_dimensions;
}
params;

layout(location = 0) out vec3 local_proxy_position;

const int CUBE_INDICES[36] = int[](
	0, 2, 1, 1, 2, 3,
	4, 5, 6, 5, 7, 6,
	0, 4, 2, 4, 6, 2,
	1, 3, 5, 3, 7, 5,
	0, 1, 4, 1, 5, 4,
	2, 6, 3, 3, 6, 7
);

void main() {
	int corner_id = CUBE_INDICES[gl_VertexIndex];
	vec3 corner = vec3(
		float(corner_id & 1),
		float((corner_id >> 1) & 1),
		float((corner_id >> 2) & 1));
	local_proxy_position = corner * vec3(params.volume_dimensions.xyz);
	gl_Position = params.model_view_projection * vec4(local_proxy_position, 1.0);
}

#[fragment]

#version 450

#VERSION_DEFINES

layout(set = 0, binding = 0) uniform sampler3D voxel_texture;
layout(set = 0, binding = 1) uniform sampler3D brick_texture;
layout(set = 0, binding = 2) uniform sampler2D palette_texture;

layout(push_constant, std430) uniform Params {
	mat4 model_view_projection;
	vec4 camera_local;
	ivec4 volume_dimensions;
}
params;

layout(location = 0) in vec3 local_proxy_position;
layout(location = 0) out vec4 frag_color;

const float DIRECTION_EPSILON = 0.0000001;
const float HUGE_DISTANCE = 1e30;
const int BRICK_SIZE = 8;

vec3 safe_inverse(vec3 direction) {
	return vec3(
		abs(direction.x) > DIRECTION_EPSILON ? 1.0 / direction.x : HUGE_DISTANCE,
		abs(direction.y) > DIRECTION_EPSILON ? 1.0 / direction.y : HUGE_DISTANCE,
		abs(direction.z) > DIRECTION_EPSILON ? 1.0 / direction.z : HUGE_DISTANCE);
}

int minimum_axis(vec3 value) {
	if (value.x <= value.y && value.x <= value.z) {
		return 0;
	}
	if (value.y <= value.z) {
		return 1;
	}
	return 2;
}

int maximum_axis(vec3 value) {
	if (value.x >= value.y && value.x >= value.z) {
		return 0;
	}
	if (value.y >= value.z) {
		return 1;
	}
	return 2;
}

int read_voxel(ivec3 cell, uvec4 directory, ivec3 atlas_brick) {
	uint code = directory.r | (directory.g << 8u) | (directory.b << 16u);
	if (code == 0u) {
		return 0;
	}
	if (code == 1u) {
		return int(directory.a);
	}

	ivec3 atlas_cell = atlas_brick * BRICK_SIZE + (cell % BRICK_SIZE);
	return int(round(texelFetch(voxel_texture, atlas_cell, 0).r * 255.0));
}

void main() {
	vec3 ray_origin = params.camera_local.xyz;
	vec3 ray_direction = normalize(local_proxy_position - ray_origin);
	vec3 inverse_direction = safe_inverse(ray_direction);
	vec3 bounds_minimum = vec3(0.0);
	vec3 bounds_maximum = vec3(params.volume_dimensions.xyz);
	vec3 box_t0 = (bounds_minimum - ray_origin) * inverse_direction;
	vec3 box_t1 = (bounds_maximum - ray_origin) * inverse_direction;
	vec3 box_near = min(box_t0, box_t1);
	vec3 box_far = max(box_t0, box_t1);
	float enter_t = max(0.0, max(max(box_near.x, box_near.y), box_near.z));
	float exit_t = min(min(box_far.x, box_far.y), box_far.z);
	if (exit_t < enter_t) {
		discard;
	}

	vec3 entry_position = ray_origin + ray_direction * (enter_t + 0.0001);
	ivec3 cell = ivec3(clamp(floor(entry_position), vec3(0.0), bounds_maximum - vec3(1.0)));
	ivec3 step_direction = ivec3(
		abs(ray_direction.x) > DIRECTION_EPSILON ? (ray_direction.x > 0.0 ? 1 : -1) : 0,
		abs(ray_direction.y) > DIRECTION_EPSILON ? (ray_direction.y > 0.0 ? 1 : -1) : 0,
		abs(ray_direction.z) > DIRECTION_EPSILON ? (ray_direction.z > 0.0 ? 1 : -1) : 0);
	vec3 next_boundary = vec3(cell + max(step_direction, ivec3(0)));
	vec3 next_t = (next_boundary - ray_origin) * inverse_direction;
	if (step_direction.x == 0) {
		next_t.x = HUGE_DISTANCE;
	}
	if (step_direction.y == 0) {
		next_t.y = HUGE_DISTANCE;
	}
	if (step_direction.z == 0) {
		next_t.z = HUGE_DISTANCE;
	}
	vec3 delta_t = abs(inverse_direction);
	float current_t = enter_t;
	int hit_id = 0;
	ivec3 cached_brick = ivec3(-1);
	uvec4 cached_directory = uvec4(0u);
	ivec3 cached_atlas_brick = ivec3(0);
	ivec3 atlas_bricks = max(textureSize(voxel_texture, 0) / BRICK_SIZE, ivec3(1));

	for (int step = 0; step < 768; step++) {
		ivec3 brick = cell / BRICK_SIZE;
		if (any(notEqual(brick, cached_brick))) {
			cached_brick = brick;
			cached_directory = uvec4(round(texelFetch(brick_texture, brick, 0) * 255.0));
			uint code = cached_directory.r | (cached_directory.g << 8u) | (cached_directory.b << 16u);
			if (code > 1u) {
				uint slot = code - 2u;
				cached_atlas_brick = ivec3(
						int(slot % uint(atlas_bricks.x)),
						int((slot / uint(atlas_bricks.x)) % uint(atlas_bricks.y)),
						int(slot / uint(atlas_bricks.x * atlas_bricks.y)));
			}
		}
		hit_id = read_voxel(cell, cached_directory, cached_atlas_brick);
		if (hit_id != 0) {
			break;
		}

		int axis = minimum_axis(next_t);
		current_t = next_t[axis];
		if (current_t > exit_t) {
			hit_id = 0;
			break;
		}
		cell[axis] += step_direction[axis];
		if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, params.volume_dimensions.xyz))) {
			hit_id = 0;
			break;
		}
		next_t[axis] += delta_t[axis];
	}

	if (hit_id == 0) {
		discard;
	}

	vec3 hit_position = ray_origin + ray_direction * current_t;
	vec4 hit_clip = params.model_view_projection * vec4(hit_position, 1.0);
	gl_FragDepth = hit_clip.z / hit_clip.w;
	frag_color = texelFetch(palette_texture, ivec2(clamp(hit_id, 0, 255), 0), 0);
}
