#include "voxel_material.h"

#include "core/object/class_db.h"

// The draw surface contains no vertex data. These indices synthesize an
// outward-wound unit cube directly from VERTEX_ID.
static const char *VOXEL_RAYMARCH_SHADER_BODY = R"SHADER(
uniform sampler3D u_voxels : filter_nearest, repeat_disable;
uniform sampler3D u_bricks : filter_nearest, repeat_disable;
uniform sampler3D u_neighbor_faces : filter_nearest, repeat_disable;
uniform sampler2D u_palette : source_color, filter_nearest, repeat_disable;
uniform sampler2D u_material : filter_nearest, repeat_disable;
uniform sampler2D u_metallic : filter_nearest, repeat_disable;
uniform sampler2D u_specularity : filter_nearest, repeat_disable;
uniform sampler2D u_emission : filter_nearest, repeat_disable;
uniform bool u_has_metallic;
uniform bool u_has_specularity;
uniform bool u_has_emission;
// TRANSPARENCY_UNIFORM
uniform ivec3 u_volume_dims = ivec3(1);
uniform ivec3 u_brick_dims = ivec3(1);
uniform ivec3 u_atlas_brick_dims = ivec3(1);
uniform int u_neighbor_mask = 0;
uniform vec3 u_volume_size = vec3(0.1);
uniform float u_voxel_size = 0.1;
uniform float emission_energy = 1.0;
uniform int max_outer_steps = 256;
uniform int max_fine_steps = 32;

varying vec3 volume_proxy_position;
varying vec3 volume_ray_origin;
varying vec3 volume_parallel_direction;

const float BRICK_SIZE = 8.0;
const float EPSILON = 0.0002;
const float DIR_EPSILON = 0.00000001;
const float HUGE_DISTANCE = 1e30;
const int CUBE_INDICES[36] = {
	0, 2, 1, 1, 2, 3,
	4, 5, 6, 5, 7, 6,
	0, 4, 2, 4, 6, 2,
	1, 3, 5, 3, 7, 5,
	0, 1, 4, 1, 5, 4,
	2, 6, 3, 3, 6, 7
};

vec3 safe_inverse(vec3 direction) {
	return vec3(
		abs(direction.x) > DIR_EPSILON ? 1.0 / direction.x : (direction.x < 0.0 ? -HUGE_DISTANCE : HUGE_DISTANCE),
		abs(direction.y) > DIR_EPSILON ? 1.0 / direction.y : (direction.y < 0.0 ? -HUGE_DISTANCE : HUGE_DISTANCE),
		abs(direction.z) > DIR_EPSILON ? 1.0 / direction.z : (direction.z < 0.0 ? -HUGE_DISTANCE : HUGE_DISTANCE)
	);
}

int minimum_axis(vec3 value) {
	if (value.x <= value.y && value.x <= value.z) return 0;
	if (value.y <= value.z) return 1;
	return 2;
}

int maximum_axis(vec3 value) {
	if (value.x >= value.y && value.x >= value.z) return 0;
	if (value.y >= value.z) return 1;
	return 2;
}

vec3 disable_parallel_crossings(vec3 crossing_t, ivec3 step_direction) {
	if (step_direction.x == 0) crossing_t.x = HUGE_DISTANCE;
	if (step_direction.y == 0) crossing_t.y = HUGE_DISTANCE;
	if (step_direction.z == 0) crossing_t.z = HUGE_DISTANCE;
	return crossing_t;
}

int boundary_face_for_axis(int axis, ivec3 voxel, ivec3 step_direction) {
	if (axis == 0 && voxel.x == 0 && step_direction.x > 0) return 0;
	if (axis == 0 && voxel.x == u_volume_dims.x - 1 && step_direction.x < 0) return 1;
	if (axis == 1 && voxel.y == 0 && step_direction.y > 0) return 2;
	if (axis == 1 && voxel.y == u_volume_dims.y - 1 && step_direction.y < 0) return 3;
	if (axis == 2 && voxel.z == 0 && step_direction.z > 0) return 4;
	if (axis == 2 && voxel.z == u_volume_dims.z - 1 && step_direction.z < 0) return 5;
	return -1;
}

ivec2 boundary_face_texel(int face, ivec3 voxel) {
	if (face < 2) return voxel.yz;
	if (face < 4) return voxel.xz;
	return voxel.xy;
}

bool has_occupied_neighbor(int face, ivec3 voxel) {
	return face >= 0 && (u_neighbor_mask & (1 << face)) != 0 &&
			texelFetch(u_neighbor_faces, ivec3(boundary_face_texel(face, voxel), face), 0).r > 0.5;
}

void vertex() {
	vec3 pass_world_position = INV_VIEW_MATRIX[3].xyz;
	vec3 pass_local_position = (inverse(MODEL_MATRIX) * vec4(pass_world_position, 1.0)).xyz;
	volume_ray_origin = pass_local_position / u_voxel_size;

	int corner_id = CUBE_INDICES[VERTEX_ID];
	vec3 corner = vec3(
		float(corner_id & 1),
		float((corner_id >> 1) & 1),
		float((corner_id >> 2) & 1)
	);
	volume_proxy_position = corner * vec3(u_volume_dims);
	VERTEX = volume_proxy_position * u_voxel_size;

	vec3 pass_world_forward = normalize(-INV_VIEW_MATRIX[2].xyz);
	volume_parallel_direction = normalize((inverse(MODEL_MATRIX) * vec4(pass_world_forward, 0.0)).xyz);
}

void fragment() {
	vec3 dimensions = vec3(u_volume_dims);
	vec3 brick_dimensions = vec3(u_brick_dims);
	bool parallel_projection = abs(PROJECTION_MATRIX[3][3]) > 0.5;
	vec3 ray_direction = parallel_projection ? normalize(volume_parallel_direction) : normalize(volume_proxy_position - volume_ray_origin);
	vec3 ray_origin = volume_ray_origin;
	if (parallel_projection) {
		ray_origin = volume_proxy_position - ray_direction * (length(dimensions) + BRICK_SIZE * 2.0);
	}
	vec3 inverse_direction = safe_inverse(ray_direction);
	ivec3 step_direction = ivec3(
		abs(ray_direction.x) > DIR_EPSILON ? (ray_direction.x > 0.0 ? 1 : -1) : 0,
		abs(ray_direction.y) > DIR_EPSILON ? (ray_direction.y > 0.0 ? 1 : -1) : 0,
		abs(ray_direction.z) > DIR_EPSILON ? (ray_direction.z > 0.0 ? 1 : -1) : 0
	);
	vec3 box_t0 = -ray_origin * inverse_direction;
	vec3 box_t1 = (dimensions - ray_origin) * inverse_direction;
	vec3 box_near = min(box_t0, box_t1);
	vec3 box_far = max(box_t0, box_t1);
	float enter_t = max(max(box_near.x, box_near.y), box_near.z);
	float exit_t = min(min(box_far.x, box_far.y), box_far.z);
	if (exit_t <= max(enter_t, 0.0)) discard;

	bool origin_inside = all(greaterThanEqual(ray_origin, vec3(0.0))) && all(lessThan(ray_origin, dimensions));
	float start_t = origin_inside ? 0.0 : max(enter_t, 0.0);
	float sample_t = start_t + EPSILON;
	vec3 start_position = clamp(ray_origin + ray_direction * sample_t, vec3(0.0), dimensions - vec3(EPSILON));
	int entry_axis = origin_inside ? -1 : maximum_axis(box_near);

	ivec3 brick_coordinate = ivec3(clamp(floor(start_position / BRICK_SIZE), vec3(0.0), brick_dimensions - vec3(1.0)));
	vec3 next_brick_boundary = vec3(brick_coordinate + max(step_direction, ivec3(0))) * BRICK_SIZE;
	// Recalculate crossings from the integer grid boundary. Repeatedly adding
	// delta-t accumulates enough error to disagree along chunk/proxy edges.
	vec3 next_brick_t = disable_parallel_crossings(
			(next_brick_boundary - ray_origin) * inverse_direction,
			step_direction);
	if (step_direction.x == 0) next_brick_t.x = HUGE_DISTANCE;
	if (step_direction.y == 0) next_brick_t.y = HUGE_DISTANCE;
	if (step_direction.z == 0) next_brick_t.z = HUGE_DISTANCE;
	float brick_enter_t = start_t;
	int brick_entry_axis = entry_axis;
	bool hit = false;
	float hit_t = 0.0;
	int hit_axis = -1;
	ivec3 hit_voxel = ivec3(0);
	uint hit_id = 0u;
	int outer_limit = min(int(brick_dimensions.x + brick_dimensions.y + brick_dimensions.z) + 3, max_outer_steps);

	for (int outer = 0; outer < outer_limit; outer++) {
		uvec4 directory_bytes = uvec4(round(texelFetch(u_bricks, brick_coordinate, 0) * 255.0));
		uint directory_code = directory_bytes.r | (directory_bytes.g << 8u) | (directory_bytes.b << 16u);
		if (directory_code == 1u) {
			hit_id = directory_bytes.a;
			hit = hit_id > 0u;
			hit_t = max(brick_enter_t, start_t);
			hit_axis = brick_entry_axis;
			hit_voxel = ivec3(clamp(floor(ray_origin + ray_direction * (hit_t + EPSILON)), vec3(0.0), dimensions - vec3(1.0)));
		} else if (directory_code > 1u) {
			uint atlas_slot = directory_code - 2u;
			ivec3 atlas_brick = ivec3(
				int(atlas_slot % uint(u_atlas_brick_dims.x)),
				int((atlas_slot / uint(u_atlas_brick_dims.x)) % uint(u_atlas_brick_dims.y)),
				int(atlas_slot / uint(u_atlas_brick_dims.x * u_atlas_brick_dims.y))
			);
			ivec3 brick_minimum_i = brick_coordinate * int(BRICK_SIZE);
			vec3 brick_minimum = vec3(brick_minimum_i);
			vec3 brick_maximum = min(brick_minimum + vec3(BRICK_SIZE), dimensions);
			float fine_t = max(brick_enter_t, start_t) + EPSILON;
			vec3 fine_position = clamp(ray_origin + ray_direction * fine_t, brick_minimum, brick_maximum - vec3(EPSILON));
			ivec3 voxel_coordinate = ivec3(clamp(floor(fine_position), brick_minimum, brick_maximum - vec3(1.0)));
			vec3 next_voxel_boundary = vec3(voxel_coordinate + max(step_direction, ivec3(0)));
			vec3 next_voxel_t = disable_parallel_crossings(
					(next_voxel_boundary - ray_origin) * inverse_direction,
					step_direction);
			if (step_direction.x == 0) next_voxel_t.x = HUGE_DISTANCE;
			if (step_direction.y == 0) next_voxel_t.y = HUGE_DISTANCE;
			if (step_direction.z == 0) next_voxel_t.z = HUGE_DISTANCE;
			float voxel_enter_t = max(brick_enter_t, start_t);
			int voxel_entry_axis = brick_entry_axis;

			for (int fine = 0; fine < max_fine_steps; fine++) {
				ivec3 atlas_texel = atlas_brick * int(BRICK_SIZE) + voxel_coordinate - brick_minimum_i;
				hit_id = uint(round(texelFetch(u_voxels, atlas_texel, 0).r * 255.0));
				if (hit_id > 0u) {
					hit = true;
					hit_t = max(voxel_enter_t, start_t);
					hit_axis = voxel_entry_axis;
					hit_voxel = voxel_coordinate;
					break;
				}
				int axis = minimum_axis(next_voxel_t);
				voxel_enter_t = next_voxel_t[axis];
				voxel_coordinate[axis] += step_direction[axis];
				next_voxel_t[axis] = (float(voxel_coordinate[axis] + max(step_direction[axis], 0)) - ray_origin[axis]) * inverse_direction[axis];
				voxel_entry_axis = axis;
				if (voxel_enter_t > exit_t || any(lessThan(voxel_coordinate, brick_minimum_i)) || any(greaterThanEqual(vec3(voxel_coordinate), brick_maximum))) break;
			}
		}
		if (hit) break;
		int axis = minimum_axis(next_brick_t);
		brick_enter_t = next_brick_t[axis];
		brick_coordinate[axis] += step_direction[axis];
		next_brick_t[axis] = (float(brick_coordinate[axis] + max(step_direction[axis], 0)) * BRICK_SIZE - ray_origin[axis]) * inverse_direction[axis];
		brick_entry_axis = axis;
		if (brick_enter_t > exit_t || any(lessThan(brick_coordinate, ivec3(0))) || any(greaterThanEqual(vec3(brick_coordinate), brick_dimensions))) break;
	}

	if (!hit) discard;
	// A ray can enter exactly through two or three volume faces. If the
	// deterministic DDA tie picked an internal neighbor wall, prefer a tied
	// exposed face instead of discarding the exterior surface with the wall.
	if (hit_axis >= 0 && abs(hit_t - enter_t) <= EPSILON * 4.0 &&
			has_occupied_neighbor(boundary_face_for_axis(hit_axis, hit_voxel, step_direction), hit_voxel)) {
		int exposed_axis = -1;
		float exposed_score = -1.0;
		for (int candidate_axis = 0; candidate_axis < 3; candidate_axis++) {
			int candidate_face = boundary_face_for_axis(candidate_axis, hit_voxel, step_direction);
			if (candidate_axis != hit_axis && candidate_face >= 0 &&
					abs(box_near[candidate_axis] - hit_t) <= EPSILON * 4.0 &&
					!has_occupied_neighbor(candidate_face, hit_voxel) &&
					abs(ray_direction[candidate_axis]) > exposed_score) {
				exposed_axis = candidate_axis;
				exposed_score = abs(ray_direction[candidate_axis]);
			}
		}
		if (exposed_axis >= 0) hit_axis = exposed_axis;
	}
	vec3 local_normal;
	if (hit_axis >= 0) {
		local_normal = vec3(0.0);
		local_normal[hit_axis] = -float(step_direction[hit_axis]);
		// Snap the hit to the exact integer face plane. This makes both proxy
		// triangles and neighboring chunks write the same depth for that face.
		float face_plane = float(hit_voxel[hit_axis]) + (local_normal[hit_axis] > 0.0 ? 1.0 : 0.0);
		hit_t = (face_plane - ray_origin[hit_axis]) * inverse_direction[hit_axis];
	} else {
		vec3 direction_abs = abs(ray_direction);
		if (direction_abs.x >= direction_abs.y && direction_abs.x >= direction_abs.z) local_normal = vec3(-sign(ray_direction.x), 0.0, 0.0);
		else if (direction_abs.y >= direction_abs.z) local_normal = vec3(0.0, -sign(ray_direction.y), 0.0);
		else local_normal = vec3(0.0, 0.0, -sign(ray_direction.z));
	}
	vec3 hit_voxel_position = ray_origin + ray_direction * hit_t;
	if (hit_axis >= 0) {
		hit_voxel_position[hit_axis] = float(hit_voxel[hit_axis]) + (local_normal[hit_axis] > 0.0 ? 1.0 : 0.0);
	}
	// Each volume raycasts its own AABB. A boundary face is internal to the
	// combined solid when the adjacent volume's matching voxel is occupied.
	// Discard that volume-local wall so it cannot enter the camera or shadow
	// depth buffer as a seam.
	int neighbor_face = boundary_face_for_axis(hit_axis, hit_voxel, step_direction);
	if (has_occupied_neighbor(neighbor_face, hit_voxel)) {
		discard;
	}
	vec3 hit_local_position = hit_voxel_position * u_voxel_size;
	vec4 hit_view_position = VIEW_MATRIX * MODEL_MATRIX * vec4(hit_local_position, 1.0);
	vec4 hit_clip_position = PROJECTION_MATRIX * hit_view_position;
	if (abs(hit_clip_position.w) <= DIR_EPSILON) discard;
	DEPTH = hit_clip_position.z / hit_clip_position.w;

	NORMAL = normalize(transpose(inverse(mat3(VIEW_MATRIX * MODEL_MATRIX))) * local_normal);

	// Keep one lighting sample for the complete voxel face. Sampling from the
	// exact ray hit makes point/specular lighting vary continuously across a
	// voxel and loses the intended block-locked appearance. Sampling from the
	// voxel center is also wrong because it lies inside the solid and can cause
	// self-shadow/checker artifacts. Push the boundary hit into the occupied
	// cell to identify it robustly, then place the sample at that face's center.
	vec3 light_voxel_position = vec3(hit_voxel) + vec3(0.5) + local_normal * 0.5;
	vec3 light_local_position = light_voxel_position * u_voxel_size;
	LIGHT_VERTEX = (VIEW_MATRIX * MODEL_MATRIX * vec4(light_local_position, 1.0)).xyz;

	vec2 palette_uv = vec2((float(hit_id) + 0.5) / 256.0, 0.5);
	vec3 palette_color = textureLod(u_palette, palette_uv, 0.0).rgb;
	vec4 material_sample = textureLod(u_material, palette_uv, 0.0);
	float metallic = u_has_metallic ? 1.0 - textureLod(u_metallic, palette_uv, 0.0).r : material_sample.g;
	float specularity = u_has_specularity ? 1.0 - textureLod(u_specularity, palette_uv, 0.0).r : 1.0 - material_sample.r;
	float emission = u_has_emission ? 1.0 - textureLod(u_emission, palette_uv, 0.0).r : material_sample.b;
	ALBEDO = palette_color;
	ROUGHNESS = 1.0 - specularity;
	SPECULAR = specularity;
	METALLIC = metallic;
	EMISSION = palette_color * emission * emission_energy;
	// TRANSPARENCY_OUTPUT
}
)SHADER";

static Ref<Shader> voxel_shader_cache[2][2];

void VoxelMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_shading_mode", "mode"), &VoxelMaterial::set_shading_mode);
	ClassDB::bind_method(D_METHOD("get_shading_mode"), &VoxelMaterial::get_shading_mode);
	ClassDB::bind_method(D_METHOD("set_emission_energy", "energy"), &VoxelMaterial::set_emission_energy);
	ClassDB::bind_method(D_METHOD("get_emission_energy"), &VoxelMaterial::get_emission_energy);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shading_mode", PROPERTY_HINT_ENUM, "Unlit,PBR"), "set_shading_mode", "get_shading_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emission_energy", PROPERTY_HINT_RANGE, "0,64,0.01,or_greater"), "set_emission_energy", "get_emission_energy");
	BIND_ENUM_CONSTANT(SHADING_MODE_UNLIT);
	BIND_ENUM_CONSTANT(SHADING_MODE_PBR);
}

void VoxelMaterial::_rebuild_shader() {
	Ref<Shader> &voxel_shader = voxel_shader_cache[int(shading_mode)][transparency_enabled ? 1 : 0];
	if (voxel_shader.is_null()) {
		voxel_shader.instantiate();
		// One hardware-culled proxy layer covers the projected volume.
		String code = "shader_type spatial;\nrender_mode cull_back, ";
		code += transparency_enabled ? "depth_prepass_alpha" : "depth_draw_opaque";
		if (shading_mode == SHADING_MODE_UNLIT) {
			code += ", unshaded";
		}
		String body = String(VOXEL_RAYMARCH_SHADER_BODY);
		body = body.replace("// TRANSPARENCY_UNIFORM", transparency_enabled ? "uniform sampler2D u_transparency : filter_nearest, repeat_disable;" : "");
		body = body.replace("// TRANSPARENCY_OUTPUT", transparency_enabled ? "ALPHA = textureLod(u_transparency, palette_uv, 0.0).r;" : "");
		code += ";\n" + body;
		voxel_shader->set_code(code);
	}
	set_shader(voxel_shader);
	set_shader_parameter("emission_energy", emission_energy);
}

void VoxelMaterial::set_shading_mode(ShadingMode p_mode) {
	ERR_FAIL_INDEX(p_mode, 2);
	if (shading_mode == p_mode) return;
	shading_mode = p_mode;
	if (get_shader().is_valid()) {
		_rebuild_shader();
	}
	emit_changed();
}

VoxelMaterial::ShadingMode VoxelMaterial::get_shading_mode() const { return shading_mode; }

void VoxelMaterial::set_emission_energy(real_t p_energy) {
	emission_energy = MAX(p_energy, real_t(0.0));
	if (get_shader().is_valid()) {
		set_shader_parameter("emission_energy", emission_energy);
	}
	emit_changed();
}

real_t VoxelMaterial::get_emission_energy() const { return emission_energy; }

void VoxelMaterial::set_transparency_enabled(bool p_enabled) {
	if (transparency_enabled == p_enabled) return;
	transparency_enabled = p_enabled;
	if (get_shader().is_valid()) _rebuild_shader();
}

bool VoxelMaterial::is_transparency_enabled() const { return transparency_enabled; }

void VoxelMaterial::ensure_shader() {
	if (get_shader().is_null()) {
		_rebuild_shader();
	}
}

void VoxelMaterial::clear_shader_cache() {
	for (int shading = 0; shading < 2; shading++) {
		for (int transparency = 0; transparency < 2; transparency++) {
			voxel_shader_cache[shading][transparency].unref();
		}
	}
}

VoxelMaterial::VoxelMaterial() {}
