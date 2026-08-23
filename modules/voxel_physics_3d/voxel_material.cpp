#include "voxel_material.h"

#include "core/config/project_settings.h"
#include "core/math/vector4.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "servers/rendering/rendering_method.h"

static Vector4 _make_voxelized_ao_curve(real_t p_hardness, real_t p_strength) {
	const real_t exponent = Math::pow(real_t(2.0), (CLAMP(p_hardness, real_t(0.0), real_t(1.0)) - real_t(0.5)) * real_t(4.0));
	const real_t strength = CLAMP(p_strength, real_t(0.0), real_t(1.0));
	return Vector4(
			Math::pow(real_t(0.25), exponent) * strength,
			Math::pow(real_t(0.5), exponent) * strength,
			Math::pow(real_t(0.75), exponent) * strength,
			strength);
}

static const char *VOXEL_FORWARD_INDIRECT_FUNCTIONS = R"SHADER(
vec3 sample_voxel_forward_indirect_grid(sampler3D grid, vec3 origin, float cell_size, vec3 world_position, out float edge_weight) {
	vec3 uvw = (world_position - origin) / (cell_size * float(voxel_forward_indirect_resolution));
	vec3 edge = min(uvw, vec3(1.0) - uvw);
	float minimum_edge = min(edge.x, min(edge.y, edge.z));
	float transition_width = clamp(voxel_forward_indirect_transition_cells / float(voxel_forward_indirect_resolution), 1.0 / float(voxel_forward_indirect_resolution), 0.45);
	edge_weight = smoothstep(0.0, transition_width, minimum_edge);
	if (minimum_edge <= 0.0) {
		return vec3(0.0);
	}
	return textureLod(grid, uvw, 0.0).rgb;
}

vec3 sample_voxel_forward_indirect(vec3 world_position) {
	if (!voxel_forward_indirect_ready || voxel_forward_indirect_resolution <= 1) {
		return vec3(0.0);
	}
	float distant_weight;
	vec3 distant_light = sample_voxel_forward_indirect_grid(voxel_forward_indirect_distant, voxel_forward_indirect_distant_origin, voxel_forward_indirect_distant_cell_size, world_position, distant_weight);
	vec3 indirect_light = distant_light * distant_weight;
	float far_weight;
	vec3 far_light = sample_voxel_forward_indirect_grid(voxel_forward_indirect_far, voxel_forward_indirect_far_origin, voxel_forward_indirect_far_cell_size, world_position, far_weight);
	indirect_light = mix(indirect_light, far_light, far_weight);
	float near_weight;
	vec3 near_light = sample_voxel_forward_indirect_grid(voxel_forward_indirect_near, voxel_forward_indirect_near_origin, voxel_forward_indirect_near_cell_size, world_position, near_weight);
	return mix(indirect_light, near_light, near_weight);
}
)SHADER";

static const char *VOXEL_OUTLINE_FUNCTION = R"SHADER(
float voxel_normal_outline(ivec3 voxel, vec3 face_position, int normal_axis) {
	if (normal_axis < 0) return 0.0;
	float result = 0.0;
	for (int axis = 0; axis < 3; axis++) {
		if (axis == normal_axis) continue;
		ivec3 direction = ivec3(0);
		direction[axis] = -1;
		if (voxel_id_at(voxel + direction) == 0u) {
			float pixel_distance = fract(face_position[axis]) / max(fwidth(face_position[axis]), 0.00001);
			result = max(result, 1.0 - smoothstep(outline_width, outline_width + 1.0, pixel_distance));
		}
		direction[axis] = 1;
		if (voxel_id_at(voxel + direction) == 0u) {
			float pixel_distance = (1.0 - fract(face_position[axis])) / max(fwidth(face_position[axis]), 0.00001);
			result = max(result, 1.0 - smoothstep(outline_width, outline_width + 1.0, pixel_distance));
		}
	}
	return result;
}
)SHADER";

static const char *VOXEL_AO_BASIS_FUNCTION = R"SHADER(
void voxel_face_ao_basis(int normal_axis, out ivec3 tangent_u, out ivec3 tangent_v) {
	if (normal_axis == 0) {
		tangent_u = ivec3(0, 1, 0);
		tangent_v = ivec3(0, 0, 1);
	} else if (normal_axis == 1) {
		tangent_u = ivec3(1, 0, 0);
		tangent_v = ivec3(0, 0, 1);
	} else {
		tangent_u = ivec3(1, 0, 0);
		tangent_v = ivec3(0, 1, 0);
	}
}
)SHADER";

static const char *VOXEL_AO_CORNER_FUNCTION = R"SHADER(
float voxel_corner_ao(bool side_a, bool side_b, bool corner) {
	if (side_a && side_b) {
		return 0.0;
	}
	float occupied_count =
			(side_a ? 1.0 : 0.0) +
			(side_b ? 1.0 : 0.0) +
			(corner ? 1.0 : 0.0);
	return (3.0 - occupied_count) / 3.0;
}
)SHADER";

static const char *VOXEL_AO_VOXELIZED_FUNCTION = R"SHADER(
float voxel_face_ao(ivec3 voxel, vec3 face_position, int normal_axis, vec3 face_normal) {
	if (normal_axis < 0) {
		return 1.0;
	}
	ivec3 tangent_u;
	ivec3 tangent_v;
	voxel_face_ao_basis(normal_axis, tangent_u, tangent_v);
	ivec3 air_voxel = voxel + ivec3(round(face_normal));
	ivec3 cached_brick = ivec3(-2147483647);
	uvec4 cached_directory_bytes = uvec4(0u);
	float occupied_sides = 0.0;
	occupied_sides += voxel_ao_id_at(air_voxel - tangent_u, cached_brick, cached_directory_bytes) != 0u ? 1.0 : 0.0;
	occupied_sides += voxel_ao_id_at(air_voxel + tangent_u, cached_brick, cached_directory_bytes) != 0u ? 1.0 : 0.0;
	occupied_sides += voxel_ao_id_at(air_voxel - tangent_v, cached_brick, cached_directory_bytes) != 0u ? 1.0 : 0.0;
	occupied_sides += voxel_ao_id_at(air_voxel + tangent_v, cached_brick, cached_directory_bytes) != 0u ? 1.0 : 0.0;
	return 1.0 - occupied_sides * 0.25;
}
)SHADER";

static const char *VOXEL_AO_SMOOTH_FUNCTION = R"SHADER(
float voxel_face_ao(ivec3 voxel, vec3 face_position, int normal_axis, vec3 face_normal) {
	if (normal_axis < 0) {
		return 1.0;
	}
	ivec3 tangent_u;
	ivec3 tangent_v;
	voxel_face_ao_basis(normal_axis, tangent_u, tangent_v);
	ivec3 air_voxel = voxel + ivec3(round(face_normal));
	ivec3 cached_brick = ivec3(-2147483647);
	uvec4 cached_directory_bytes = uvec4(0u);
	bool u_negative = voxel_ao_id_at(air_voxel - tangent_u, cached_brick, cached_directory_bytes) != 0u;
	bool u_positive = voxel_ao_id_at(air_voxel + tangent_u, cached_brick, cached_directory_bytes) != 0u;
	bool v_negative = voxel_ao_id_at(air_voxel - tangent_v, cached_brick, cached_directory_bytes) != 0u;
	bool v_positive = voxel_ao_id_at(air_voxel + tangent_v, cached_brick, cached_directory_bytes) != 0u;
	float ao_negative_negative = voxel_corner_ao(u_negative, v_negative, voxel_ao_id_at(air_voxel - tangent_u - tangent_v, cached_brick, cached_directory_bytes) != 0u);
	float ao_positive_negative = voxel_corner_ao(u_positive, v_negative, voxel_ao_id_at(air_voxel + tangent_u - tangent_v, cached_brick, cached_directory_bytes) != 0u);
	float ao_negative_positive = voxel_corner_ao(u_negative, v_positive, voxel_ao_id_at(air_voxel - tangent_u + tangent_v, cached_brick, cached_directory_bytes) != 0u);
	float ao_positive_positive = voxel_corner_ao(u_positive, v_positive, voxel_ao_id_at(air_voxel + tangent_u + tangent_v, cached_brick, cached_directory_bytes) != 0u);
	vec3 position_in_voxel = clamp(face_position - vec3(voxel), vec3(0.0), vec3(1.0));
	vec2 face_uv;
	if (normal_axis == 0) {
		face_uv = position_in_voxel.yz;
	} else if (normal_axis == 1) {
		face_uv = position_in_voxel.xz;
	} else {
		face_uv = position_in_voxel.xy;
	}
	float negative_edge = mix(ao_negative_negative, ao_positive_negative, face_uv.x);
	float positive_edge = mix(ao_negative_positive, ao_positive_positive, face_uv.x);
	return mix(negative_edge, positive_edge, face_uv.y);
}
)SHADER";

static const char *VOXEL_AO_HARD_CORNER_FUNCTION = R"SHADER(
float voxel_face_ao(ivec3 voxel, vec3 face_position, int normal_axis, vec3 face_normal) {
	if (normal_axis < 0) {
		return 1.0;
	}
	ivec3 tangent_u;
	ivec3 tangent_v;
	voxel_face_ao_basis(normal_axis, tangent_u, tangent_v);
	ivec3 air_voxel = voxel + ivec3(round(face_normal));
	ivec3 cached_brick = ivec3(-2147483647);
	uvec4 cached_directory_bytes = uvec4(0u);
	bool u_negative = voxel_ao_id_at(air_voxel - tangent_u, cached_brick, cached_directory_bytes) != 0u;
	bool u_positive = voxel_ao_id_at(air_voxel + tangent_u, cached_brick, cached_directory_bytes) != 0u;
	bool v_negative = voxel_ao_id_at(air_voxel - tangent_v, cached_brick, cached_directory_bytes) != 0u;
	bool v_positive = voxel_ao_id_at(air_voxel + tangent_v, cached_brick, cached_directory_bytes) != 0u;
	float ao_negative_negative = voxel_corner_ao(u_negative, v_negative, voxel_ao_id_at(air_voxel - tangent_u - tangent_v, cached_brick, cached_directory_bytes) != 0u);
	float ao_positive_negative = voxel_corner_ao(u_positive, v_negative, voxel_ao_id_at(air_voxel + tangent_u - tangent_v, cached_brick, cached_directory_bytes) != 0u);
	float ao_negative_positive = voxel_corner_ao(u_negative, v_positive, voxel_ao_id_at(air_voxel - tangent_u + tangent_v, cached_brick, cached_directory_bytes) != 0u);
	float ao_positive_positive = voxel_corner_ao(u_positive, v_positive, voxel_ao_id_at(air_voxel + tangent_u + tangent_v, cached_brick, cached_directory_bytes) != 0u);
	vec3 position_in_voxel = clamp(face_position - vec3(voxel), vec3(0.0), vec3(1.0));
	vec2 face_uv;
	if (normal_axis == 0) {
		face_uv = position_in_voxel.yz;
	} else if (normal_axis == 1) {
		face_uv = position_in_voxel.xz;
	} else {
		face_uv = position_in_voxel.xy;
	}
	bool positive_u = face_uv.x >= 0.5;
	bool positive_v = face_uv.y >= 0.5;
	if (positive_v) {
		return positive_u ? ao_positive_positive : ao_negative_positive;
	}
	return positive_u ? ao_positive_negative : ao_negative_negative;
}
)SHADER";

// The draw surface contains no vertex data. These indices synthesize an
// outward-wound unit cube directly from VERTEX_ID.
static const char *VOXEL_RAYMARCH_SHADER_PREFIX = R"SHADER(
uniform sampler3D u_voxels : filter_nearest, repeat_disable;
uniform sampler3D u_bricks : filter_nearest, repeat_disable;
uniform sampler3D u_neighbor_faces : filter_nearest, repeat_disable;
uniform sampler2D u_palette : source_color, filter_nearest, repeat_disable;
uniform sampler2D u_material : filter_nearest, repeat_disable;
// OPTIONAL_MATERIAL_TEXTURE_UNIFORMS
// TRANSPARENCY_UNIFORM
uniform ivec3 u_volume_dims = ivec3(1);
uniform ivec3 u_brick_dims = ivec3(1);
uniform ivec3 u_atlas_brick_dims = ivec3(1);
uniform int u_neighbor_mask = 0;
uniform int u_neighbor_diagonal_mask = 0;
uniform vec3 u_volume_size = vec3(0.1);
uniform float u_voxel_size = 0.1;
uniform float emission_energy = 1.0;
uniform vec4 albedo_modulate : source_color = vec4(1.0);
uniform float roughness_multiplier = 1.0;
uniform float metallic_multiplier = 1.0;
uniform float specularity_multiplier = 1.0;
// OUTLINE_UNIFORMS
// AO_UNIFORMS
uniform int max_outer_steps = 256;
uniform int max_fine_steps = 32;

global uniform sampler2D voxel_forward_shadow_mask : filter_nearest, repeat_disable;
global uniform vec3 voxel_forward_shadow_light_direction;
global uniform bool voxel_forward_shadow_ready;
// VOXEL_FORWARD_INDIRECT_UNIFORMS
global uniform vec4 voxel_forward_ambient_color : source_color;
global uniform float voxel_forward_ambient_energy;
// VOXEL_FORWARD_REFLECTION_UNIFORMS

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
// VOXEL_FORWARD_INDIRECT_FUNCTIONS

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
	if (face < 0 || (u_neighbor_mask & (1 << face)) == 0) return false;
	ivec2 face_texel = boundary_face_texel(face, voxel);
	uint packed_faces = uint(round(texelFetch(u_neighbor_faces, ivec3(face_texel.x >> 3, face_texel.y, face), 0).r * 255.0));
	return (packed_faces & (1u << uint(face_texel.x & 7))) != 0u;
}

uint voxel_id_at(ivec3 voxel) {
	if (any(lessThan(voxel, ivec3(0))) || any(greaterThanEqual(voxel, u_volume_dims))) {
		int face = -1;
		ivec3 boundary_voxel = clamp(voxel, ivec3(0), u_volume_dims - ivec3(1));
		if (voxel.x < 0) face = 0;
		else if (voxel.x >= u_volume_dims.x) face = 1;
		else if (voxel.y < 0) face = 2;
		else if (voxel.y >= u_volume_dims.y) face = 3;
		else if (voxel.z < 0) face = 4;
		else if (voxel.z >= u_volume_dims.z) face = 5;
		return has_occupied_neighbor(face, boundary_voxel) ? 1u : 0u;
	}
	ivec3 brick = voxel / int(BRICK_SIZE);
	uvec4 directory_bytes = uvec4(round(texelFetch(u_bricks, brick, 0) * 255.0));
	uint directory_code = directory_bytes.r | (directory_bytes.g << 8u) | (directory_bytes.b << 16u);
	if (directory_code == 0u) return 0u;
	if (directory_code == 1u) return directory_bytes.a;
	uint atlas_slot = directory_code - 2u;
	ivec3 atlas_brick = ivec3(
		int(atlas_slot % uint(u_atlas_brick_dims.x)),
		int((atlas_slot / uint(u_atlas_brick_dims.x)) % uint(u_atlas_brick_dims.y)),
		int(atlas_slot / uint(u_atlas_brick_dims.x * u_atlas_brick_dims.y))
	);
	ivec3 atlas_texel = atlas_brick * int(BRICK_SIZE) + voxel - brick * int(BRICK_SIZE);
	return uint(round(texelFetch(u_voxels, atlas_texel, 0).r * 255.0));
}

uint voxel_ao_id_at(ivec3 voxel, inout ivec3 cached_brick, inout uvec4 cached_directory_bytes) {
	int outside_axis_count = 0;
	ivec3 offset = ivec3(0);
	if (voxel.x < 0) { offset.x = -1; outside_axis_count++; }
	else if (voxel.x >= u_volume_dims.x) { offset.x = 1; outside_axis_count++; }
	if (voxel.y < 0) { offset.y = -1; outside_axis_count++; }
	else if (voxel.y >= u_volume_dims.y) { offset.y = 1; outside_axis_count++; }
	if (voxel.z < 0) { offset.z = -1; outside_axis_count++; }
	else if (voxel.z >= u_volume_dims.z) { offset.z = 1; outside_axis_count++; }
	if (outside_axis_count == 0) {
		ivec3 brick = voxel / int(BRICK_SIZE);
		if (any(notEqual(brick, cached_brick))) {
			cached_brick = brick;
			cached_directory_bytes = uvec4(round(texelFetch(u_bricks, brick, 0) * 255.0));
		}
		uint directory_code = cached_directory_bytes.r | (cached_directory_bytes.g << 8u) | (cached_directory_bytes.b << 16u);
		if (directory_code == 0u) return 0u;
		if (directory_code == 1u) return cached_directory_bytes.a;
		uint atlas_slot = directory_code - 2u;
		ivec3 atlas_brick = ivec3(
			int(atlas_slot % uint(u_atlas_brick_dims.x)),
			int((atlas_slot / uint(u_atlas_brick_dims.x)) % uint(u_atlas_brick_dims.y)),
			int(atlas_slot / uint(u_atlas_brick_dims.x * u_atlas_brick_dims.y))
		);
		ivec3 atlas_texel = atlas_brick * int(BRICK_SIZE) + voxel - brick * int(BRICK_SIZE);
		return uint(round(texelFetch(u_voxels, atlas_texel, 0).r * 255.0));
	}
	if (outside_axis_count == 1) return voxel_id_at(voxel);

	int diagonal = -1;
	int coordinate = 0;
	if (outside_axis_count == 2) {
		if (offset.z == 0) {
			diagonal = (offset.x > 0 ? 2 : 0) + (offset.y > 0 ? 1 : 0);
			coordinate = voxel.z;
		} else if (offset.y == 0) {
			diagonal = 4 + (offset.x > 0 ? 2 : 0) + (offset.z > 0 ? 1 : 0);
			coordinate = voxel.y;
		} else {
			diagonal = 8 + (offset.y > 0 ? 2 : 0) + (offset.z > 0 ? 1 : 0);
			coordinate = voxel.x;
		}
	} else {
		diagonal = 12 + (offset.x > 0 ? 4 : 0) + (offset.y > 0 ? 2 : 0) + (offset.z > 0 ? 1 : 0);
	}
	if ((u_neighbor_diagonal_mask & (1 << diagonal)) == 0) return 0u;
	uint packed_occupancy = uint(round(texelFetch(u_neighbor_faces, ivec3(coordinate >> 3, 0, 6 + diagonal), 0).r * 255.0));
	return (packed_occupancy & (1u << uint(coordinate & 7))) != 0u ? 1u : 0u;
}

// OUTLINE_FUNCTION
// AO_FUNCTIONS

)SHADER";

static const char *VOXEL_FORWARD_LIGHT_BODY = R"SHADER(

float voxel_forward_schlick(float value) {
	float m = 1.0 - value;
	float m2 = m * m;
	return m2 * m2 * m;
}

void light() {
	float normal_dot_light = max(dot(NORMAL, LIGHT), 0.0);
	if (normal_dot_light > 0.0) {
		// The screen mask belongs to one directional light. Area lights retain
		// Godot's LTC path and do not enter this custom point-light BRDF.
		bool is_masked_light = false;
		if (LIGHT_IS_DIRECTIONAL && voxel_forward_shadow_ready) {
			vec3 world_light_direction = normalize(mat3(INV_VIEW_MATRIX) * LIGHT);
			is_masked_light = dot(world_light_direction, voxel_forward_shadow_light_direction) > 0.9999;
		}
		float occupancy_visibility = is_masked_light ? textureLod(voxel_forward_shadow_mask, SCREEN_UV, 0.0).r : 1.0;
		// Occupancy covers voxel casters; the conventional shadow map covers
		// dynamic mesh casters such as the player. Both receiver samples are
		// evaluated at the voxel face center.
		float visibility = ATTENUATION * occupancy_visibility;
		if (visibility > 0.0) {
			vec3 half_vector = normalize(LIGHT + VIEW);
			float normal_dot_view = max(dot(NORMAL, VIEW), 0.0001);
			float light_dot_half = clamp(dot(LIGHT, half_vector), 0.0, 1.0);

			// Match Forward+'s default Burley diffuse BRDF. ALBEDO and the
			// (1 - METALLIC) factor are applied by Godot after light().
			float fd90_minus_1 = 2.0 * light_dot_half * light_dot_half * ROUGHNESS - 0.5;
			float fd_view = 1.0 + fd90_minus_1 * voxel_forward_schlick(normal_dot_view);
			float fd_light = 1.0 + fd90_minus_1 * voxel_forward_schlick(normal_dot_light);
			float diffuse_brdf_nl = (1.0 / PI) * fd_view * fd_light * normal_dot_light;
			DIFFUSE_LIGHT += LIGHT_COLOR * diffuse_brdf_nl * visibility;

			// Point-like lights use one discrete specular response for the complete
			// face. A screen-space GGX lobe makes tiny DDA side faces flare into the
			// chunk-edge lines this renderer is designed to avoid.
			float specular_power = mix(64.0, 4.0, ROUGHNESS);
			float face_specular = pow(normal_dot_light, specular_power);
			vec3 f0 = mix(vec3(0.04), ALBEDO, METALLIC);
			SPECULAR_LIGHT += f0 * face_specular * LIGHT_COLOR * visibility * SPECULAR_AMOUNT;
		}
	}
}

)SHADER";

static const char *VOXEL_RAYMARCH_SHADER_SUFFIX = R"SHADER(

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

	int neighbor_face = boundary_face_for_axis(hit_axis, hit_voxel, step_direction);
	if (has_occupied_neighbor(neighbor_face, hit_voxel)) {
		discard;
	}

	vec3 hit_local_position = hit_voxel_position * u_voxel_size;
	vec4 hit_view_position = VIEW_MATRIX * MODEL_MATRIX * vec4(hit_local_position, 1.0);
	vec4 hit_clip_position = PROJECTION_MATRIX * hit_view_position;

	if (abs(hit_clip_position.w) <= DIR_EPSILON) discard;
	DEPTH = hit_clip_position.z / hit_clip_position.w;

	NORMAL = normalize(mat3(VIEW_MATRIX) * (MODEL_NORMAL_MATRIX * local_normal));

	// AO_OUTPUT



	// Shadow lookup must use the actual DDA surface hit. Using the center of the
	// voxel face moves the receiver by as much as half a voxel in either tangent
	// axis, which shows up as a bright contact gap even with a near-zero light
	// bias. The hit is already snapped exactly onto the crossed face above.
	LIGHT_VERTEX = hit_view_position.xyz;

	// LIGHTING_VERTEX_OUTPUT
	// VOXEL_OCCUPANCY_ENABLE

	vec2 palette_uv = vec2((float(hit_id) + 0.5) / 256.0, 0.5);
	vec3 palette_color = textureLod(u_palette, palette_uv, 0.0).rgb;
	vec4 material_sample = textureLod(u_material, palette_uv, 0.0);
	// MATERIAL_CHANNEL_OUTPUTS
	// ALBEDO_OUTPUT
	ROUGHNESS = clamp((1.0 - specularity) * roughness_multiplier, 0.0, 1.0);
	SPECULAR = clamp(specularity * specularity_multiplier, 0.0, 1.0);
	METALLIC = clamp(metallic * metallic_multiplier, 0.0, 1.0);
	EMISSION = palette_color * emission * emission_energy;
	// INDIRECT_LIGHT_OUTPUT
	// REFLECTION_OUTPUT
	// TRANSPARENCY_OUTPUT
}

// OCCUPANCY_LIGHT
)SHADER";

enum VoxelShaderFeature : uint32_t {
	VOXEL_SHADER_TRANSPARENCY = 1u << 1,
	VOXEL_SHADER_FORWARD_LIGHTING = 1u << 2,
	VOXEL_SHADER_FORWARD_MASK = 1u << 3,
	VOXEL_SHADER_FACE_CENTER_LIGHTING = 1u << 4,
	VOXEL_SHADER_AMBIENT_OCCLUSION = 1u << 5,
	VOXEL_SHADER_OUTLINE = 1u << 8,
	VOXEL_SHADER_INDIRECT = 1u << 9,
	VOXEL_SHADER_REFLECTION = 1u << 10,
	VOXEL_SHADER_METALLIC_TEXTURE = 1u << 11,
	VOXEL_SHADER_SPECULARITY_TEXTURE = 1u << 12,
	VOXEL_SHADER_EMISSION_TEXTURE = 1u << 13,
};

static HashMap<uint32_t, Ref<Shader>> voxel_shader_cache;

void VoxelMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_shading_mode", "mode"), &VoxelMaterial::set_shading_mode);
	ClassDB::bind_method(D_METHOD("get_shading_mode"), &VoxelMaterial::get_shading_mode);
	ClassDB::bind_method(D_METHOD("set_lighting_position_mode", "mode"), &VoxelMaterial::set_lighting_position_mode);
	ClassDB::bind_method(D_METHOD("get_lighting_position_mode"), &VoxelMaterial::get_lighting_position_mode);
	ClassDB::bind_method(D_METHOD("set_emission_energy", "energy"), &VoxelMaterial::set_emission_energy);
	ClassDB::bind_method(D_METHOD("get_emission_energy"), &VoxelMaterial::get_emission_energy);
	ClassDB::bind_method(D_METHOD("set_ambient_occlusion_enabled", "enabled"), &VoxelMaterial::set_ambient_occlusion_enabled);
	ClassDB::bind_method(D_METHOD("is_ambient_occlusion_enabled"), &VoxelMaterial::is_ambient_occlusion_enabled);
	ClassDB::bind_method(D_METHOD("set_ambient_occlusion_color", "color"), &VoxelMaterial::set_ambient_occlusion_color);
	ClassDB::bind_method(D_METHOD("get_ambient_occlusion_color"), &VoxelMaterial::get_ambient_occlusion_color);
	ClassDB::bind_method(D_METHOD("set_ambient_occlusion_strength", "strength"), &VoxelMaterial::set_ambient_occlusion_strength);
	ClassDB::bind_method(D_METHOD("get_ambient_occlusion_strength"), &VoxelMaterial::get_ambient_occlusion_strength);
	ClassDB::bind_method(D_METHOD("set_ambient_occlusion_hardness", "hardness"), &VoxelMaterial::set_ambient_occlusion_hardness);
	ClassDB::bind_method(D_METHOD("get_ambient_occlusion_hardness"), &VoxelMaterial::get_ambient_occlusion_hardness);
	ClassDB::bind_method(D_METHOD("set_ambient_occlusion_mode", "mode"), &VoxelMaterial::set_ambient_occlusion_mode);
	ClassDB::bind_method(D_METHOD("get_ambient_occlusion_mode"), &VoxelMaterial::get_ambient_occlusion_mode);
	ClassDB::bind_method(D_METHOD("set_ambient_occlusion_face_mode", "mode"), &VoxelMaterial::set_ambient_occlusion_face_mode);
	ClassDB::bind_method(D_METHOD("get_ambient_occlusion_face_mode"), &VoxelMaterial::get_ambient_occlusion_face_mode);
	ClassDB::bind_method(D_METHOD("set_albedo_modulate", "color"), &VoxelMaterial::set_albedo_modulate);
	ClassDB::bind_method(D_METHOD("get_albedo_modulate"), &VoxelMaterial::get_albedo_modulate);
	ClassDB::bind_method(D_METHOD("set_palette_texture", "texture"), &VoxelMaterial::set_palette_texture);
	ClassDB::bind_method(D_METHOD("get_palette_texture"), &VoxelMaterial::get_palette_texture);
	ClassDB::bind_method(D_METHOD("set_material_texture", "texture"), &VoxelMaterial::set_material_texture);
	ClassDB::bind_method(D_METHOD("get_material_texture"), &VoxelMaterial::get_material_texture);
	ClassDB::bind_method(D_METHOD("set_metallic_texture", "texture"), &VoxelMaterial::set_metallic_texture);
	ClassDB::bind_method(D_METHOD("get_metallic_texture"), &VoxelMaterial::get_metallic_texture);
	ClassDB::bind_method(D_METHOD("set_transparency_texture", "texture"), &VoxelMaterial::set_transparency_texture);
	ClassDB::bind_method(D_METHOD("get_transparency_texture"), &VoxelMaterial::get_transparency_texture);
	ClassDB::bind_method(D_METHOD("set_specularity_texture", "texture"), &VoxelMaterial::set_specularity_texture);
	ClassDB::bind_method(D_METHOD("get_specularity_texture"), &VoxelMaterial::get_specularity_texture);
	ClassDB::bind_method(D_METHOD("set_emission_texture", "texture"), &VoxelMaterial::set_emission_texture);
	ClassDB::bind_method(D_METHOD("get_emission_texture"), &VoxelMaterial::get_emission_texture);
	ClassDB::bind_method(D_METHOD("set_roughness_multiplier", "multiplier"), &VoxelMaterial::set_roughness_multiplier);
	ClassDB::bind_method(D_METHOD("get_roughness_multiplier"), &VoxelMaterial::get_roughness_multiplier);
	ClassDB::bind_method(D_METHOD("set_metallic_multiplier", "multiplier"), &VoxelMaterial::set_metallic_multiplier);
	ClassDB::bind_method(D_METHOD("get_metallic_multiplier"), &VoxelMaterial::get_metallic_multiplier);
	ClassDB::bind_method(D_METHOD("set_specularity_multiplier", "multiplier"), &VoxelMaterial::set_specularity_multiplier);
	ClassDB::bind_method(D_METHOD("get_specularity_multiplier"), &VoxelMaterial::get_specularity_multiplier);
	ClassDB::bind_method(D_METHOD("set_outline_enabled", "enabled"), &VoxelMaterial::set_outline_enabled);
	ClassDB::bind_method(D_METHOD("is_outline_enabled"), &VoxelMaterial::is_outline_enabled);
	ClassDB::bind_method(D_METHOD("set_outline_color", "color"), &VoxelMaterial::set_outline_color);
	ClassDB::bind_method(D_METHOD("get_outline_color"), &VoxelMaterial::get_outline_color);
	ClassDB::bind_method(D_METHOD("set_outline_width", "width"), &VoxelMaterial::set_outline_width);
	ClassDB::bind_method(D_METHOD("get_outline_width"), &VoxelMaterial::get_outline_width);
	ADD_GROUP("Shading", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shading_mode", PROPERTY_HINT_ENUM, "Unlit,PBR"), "set_shading_mode", "get_shading_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lighting_position_mode", PROPERTY_HINT_ENUM, "Exact Hit,Voxel Face Center"), "set_lighting_position_mode", "get_lighting_position_mode");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "albedo_modulate", PROPERTY_HINT_COLOR_NO_ALPHA), "set_albedo_modulate", "get_albedo_modulate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness_multiplier", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_roughness_multiplier", "get_roughness_multiplier");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "metallic_multiplier", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_metallic_multiplier", "get_metallic_multiplier");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "specularity_multiplier", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_specularity_multiplier", "get_specularity_multiplier");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emission_energy", PROPERTY_HINT_RANGE, "0,64,0.01,or_greater"), "set_emission_energy", "get_emission_energy");
	ADD_GROUP("Ambient Occlusion", "ambient_occlusion_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ambient_occlusion_enabled"), "set_ambient_occlusion_enabled", "is_ambient_occlusion_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "ambient_occlusion_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_ambient_occlusion_color", "get_ambient_occlusion_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ambient_occlusion_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_ambient_occlusion_strength", "get_ambient_occlusion_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ambient_occlusion_hardness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_ambient_occlusion_hardness", "get_ambient_occlusion_hardness");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ambient_occlusion_mode", PROPERTY_HINT_ENUM, "Smooth,Voxelized,Hard Corners"), "set_ambient_occlusion_mode", "get_ambient_occlusion_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ambient_occlusion_face_mode", PROPERTY_HINT_ENUM, "All Faces,Floor Faces Only,Ceiling Faces Only"), "set_ambient_occlusion_face_mode", "get_ambient_occlusion_face_mode");
	ADD_GROUP("Palette Textures", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "palette_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_palette_texture", "get_palette_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_material_texture", "get_material_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "metallic_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_metallic_texture", "get_metallic_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "specularity_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_specularity_texture", "get_specularity_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "emission_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_emission_texture", "get_emission_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "transparency_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_transparency_texture", "get_transparency_texture");
	ADD_GROUP("Normal Outlines", "outline_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "outline_enabled"), "set_outline_enabled", "is_outline_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "outline_color"), "set_outline_color", "get_outline_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "outline_width", PROPERTY_HINT_RANGE, "0.25,4,0.25,suffix:px"), "set_outline_width", "get_outline_width");
	BIND_ENUM_CONSTANT(SHADING_MODE_UNLIT);
	BIND_ENUM_CONSTANT(SHADING_MODE_PBR);
	BIND_ENUM_CONSTANT(LIGHTING_POSITION_EXACT_HIT);
	BIND_ENUM_CONSTANT(LIGHTING_POSITION_VOXEL_FACE_CENTER);
	BIND_ENUM_CONSTANT(AMBIENT_OCCLUSION_MODE_SMOOTH);
	BIND_ENUM_CONSTANT(AMBIENT_OCCLUSION_MODE_VOXELIZED);
	BIND_ENUM_CONSTANT(AMBIENT_OCCLUSION_MODE_HARD_CORNERS);
	BIND_ENUM_CONSTANT(AMBIENT_OCCLUSION_FACE_MODE_ALL);
	BIND_ENUM_CONSTANT(AMBIENT_OCCLUSION_FACE_MODE_FLOORS);
	BIND_ENUM_CONSTANT(AMBIENT_OCCLUSION_FACE_MODE_CEILINGS);
}

void VoxelMaterial::_rebuild_shader() {
	const bool use_voxel_forward_lighting = shading_mode == SHADING_MODE_PBR && RenderingMethod::is_current_voxel_forward_method();
	const bool use_voxel_forward_mask = use_voxel_forward_lighting &&
			bool(GLOBAL_GET("rendering/voxel_forward/shadow_mask/enabled"));
	const bool use_voxel_forward_indirect = shading_mode == SHADING_MODE_PBR &&
			bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/enabled"));
	const bool use_voxel_forward_reflection = shading_mode == SHADING_MODE_PBR &&
			bool(GLOBAL_GET("rendering/voxel_forward/reflections/enabled"));
	// Voxel Forward is face-shaded by definition. Point and spot lights must use
	// the same face-center receiver as directional visibility, so illumination
	// cannot form a smooth gradient across an individual voxel face.
	const bool use_face_center_lighting = RenderingMethod::is_current_voxel_forward_method() || lighting_position_mode == LIGHTING_POSITION_VOXEL_FACE_CENTER;
	const int ao_mode_index = ambient_occlusion_enabled ? int(ambient_occlusion_mode) : 0;
	uint32_t shader_key = uint32_t(shading_mode);
	shader_key |= transparency_enabled ? VOXEL_SHADER_TRANSPARENCY : 0;
	shader_key |= use_voxel_forward_lighting ? VOXEL_SHADER_FORWARD_LIGHTING : 0;
	shader_key |= use_voxel_forward_mask ? VOXEL_SHADER_FORWARD_MASK : 0;
	shader_key |= use_face_center_lighting ? VOXEL_SHADER_FACE_CENTER_LIGHTING : 0;
	shader_key |= ambient_occlusion_enabled ? VOXEL_SHADER_AMBIENT_OCCLUSION : 0;
	shader_key |= uint32_t(ao_mode_index) << 6;
	shader_key |= outline_enabled ? VOXEL_SHADER_OUTLINE : 0;
	shader_key |= use_voxel_forward_indirect ? VOXEL_SHADER_INDIRECT : 0;
	shader_key |= use_voxel_forward_reflection ? VOXEL_SHADER_REFLECTION : 0;
	shader_key |= metallic_texture_enabled ? VOXEL_SHADER_METALLIC_TEXTURE : 0;
	shader_key |= specularity_texture_enabled ? VOXEL_SHADER_SPECULARITY_TEXTURE : 0;
	shader_key |= emission_texture_enabled ? VOXEL_SHADER_EMISSION_TEXTURE : 0;
	Ref<Shader> &voxel_shader = voxel_shader_cache[shader_key];
	if (voxel_shader.is_null()) {
		voxel_shader.instantiate();
		// One hardware-culled proxy layer covers the projected volume.
		String code = "shader_type spatial;\nrender_mode cull_back, ";
		code += transparency_enabled ? "depth_prepass_alpha" : "depth_draw_opaque";
		if (shading_mode == SHADING_MODE_UNLIT) {
			code += ", unshaded";
		} else if (use_voxel_forward_lighting) {
			// Voxel Forward owns direct and indirect lighting. Do not layer
			// Forward+'s ambient or image-based reflections on top.
			code += ", ambient_light_disabled";
		}
		String body = String(VOXEL_RAYMARCH_SHADER_PREFIX) + String(VOXEL_RAYMARCH_SHADER_SUFFIX);
		String ao_uniforms;
		String ao_functions;
		String ao_output = R"SHADER(
	float voxel_ao = 1.0;
	vec3 voxel_ao_tint = vec3(1.0);
	AO = voxel_ao;
	AO_LIGHT_AFFECT = 0.0;
)SHADER";
		if (ambient_occlusion_enabled) {
			ao_uniforms = R"SHADER(
uniform vec4 ambient_occlusion_color : source_color = vec4(0.0, 0.0, 0.0, 1.0);
uniform int ambient_occlusion_face_mode = 0;
)SHADER";
			ao_functions = String(VOXEL_AO_BASIS_FUNCTION);
			if (ambient_occlusion_mode == AMBIENT_OCCLUSION_MODE_VOXELIZED) {
				ao_uniforms += "uniform vec4 ambient_occlusion_voxelized_curve = vec4(0.25, 0.5, 0.75, 1.0);\n";
				ao_functions += VOXEL_AO_VOXELIZED_FUNCTION;
			} else {
				ao_uniforms += "uniform float ambient_occlusion_strength : hint_range(0.0, 1.0) = 1.0;\n";
				ao_uniforms += "uniform float ambient_occlusion_hardness : hint_range(0.0, 1.0) = 0.5;\n";
				ao_functions += VOXEL_AO_CORNER_FUNCTION;
				ao_functions += ambient_occlusion_mode == AMBIENT_OCCLUSION_MODE_HARD_CORNERS ? VOXEL_AO_HARD_CORNER_FUNCTION : VOXEL_AO_SMOOTH_FUNCTION;
			}
			ao_output = R"SHADER(
	float voxel_ao = 1.0;
	vec3 voxel_ao_tint = vec3(1.0);
	bool ambient_occlusion_receives_face =
			ambient_occlusion_face_mode == 0 ||
			(ambient_occlusion_face_mode == 1 && local_normal.y > 0.5) ||
			(ambient_occlusion_face_mode == 2 && local_normal.y < -0.5);
	if (ambient_occlusion_receives_face) {
		float calculated_ao = voxel_face_ao(hit_voxel, hit_voxel_position, hit_axis, local_normal);
		float raw_occlusion = clamp(1.0 - calculated_ao, 0.0, 1.0);
)SHADER";
			if (ambient_occlusion_mode == AMBIENT_OCCLUSION_MODE_VOXELIZED) {
				ao_output += R"SHADER(
		int curve_index = clamp(int(round(raw_occlusion * 4.0)) - 1, 0, 3);
		float occlusion_amount = raw_occlusion > 0.0 ? ambient_occlusion_voxelized_curve[curve_index] : 0.0;
)SHADER";
			} else {
				ao_output += R"SHADER(
		float hardness_exponent = exp2((clamp(ambient_occlusion_hardness, 0.0, 1.0) - 0.5) * 4.0);
		float occlusion_amount = pow(raw_occlusion, hardness_exponent) * clamp(ambient_occlusion_strength, 0.0, 1.0);
)SHADER";
			}
			ao_output += R"SHADER(
		voxel_ao = 1.0 - occlusion_amount;
		voxel_ao_tint = mix(vec3(1.0), ambient_occlusion_color.rgb, occlusion_amount);
	}
	AO = voxel_ao;
	AO_LIGHT_AFFECT = 0.0;
)SHADER";
		}
		body = body.replace("// AO_UNIFORMS", ao_uniforms);
		body = body.replace("// AO_FUNCTIONS", ao_functions);
		body = body.replace("// AO_OUTPUT", ao_output);
		String optional_texture_uniforms;
		String material_channel_outputs;
		if (metallic_texture_enabled) {
			optional_texture_uniforms += "uniform sampler2D u_metallic : filter_nearest, repeat_disable;\n";
			material_channel_outputs += "float metallic = 1.0 - textureLod(u_metallic, palette_uv, 0.0).r;\n\t";
		} else {
			material_channel_outputs += "float metallic = material_sample.g;\n\t";
		}
		if (specularity_texture_enabled) {
			optional_texture_uniforms += "uniform sampler2D u_specularity : filter_nearest, repeat_disable;\n";
			material_channel_outputs += "float specularity = 1.0 - textureLod(u_specularity, palette_uv, 0.0).r;\n\t";
		} else {
			material_channel_outputs += "float specularity = 1.0 - material_sample.r;\n\t";
		}
		if (emission_texture_enabled) {
			optional_texture_uniforms += "uniform sampler2D u_emission : filter_nearest, repeat_disable;\n";
			material_channel_outputs += "float emission = 1.0 - textureLod(u_emission, palette_uv, 0.0).r;";
		} else {
			material_channel_outputs += "float emission = material_sample.b;";
		}
		body = body.replace("// OPTIONAL_MATERIAL_TEXTURE_UNIFORMS", optional_texture_uniforms);
		body = body.replace("// MATERIAL_CHANNEL_OUTPUTS", material_channel_outputs);
		body = body.replace("// OUTLINE_UNIFORMS", outline_enabled ? String(R"SHADER(
uniform vec4 outline_color : source_color = vec4(0.0, 0.0, 0.0, 1.0);
uniform float outline_width = 1.0;
)SHADER") : String());
		body = body.replace("// OUTLINE_FUNCTION", outline_enabled ? String(VOXEL_OUTLINE_FUNCTION) : String());
		body = body.replace("// ALBEDO_OUTPUT", outline_enabled ? String(R"SHADER(
	float outline = voxel_normal_outline(hit_voxel, hit_voxel_position, hit_axis) * outline_color.a;
	ALBEDO = mix(palette_color * albedo_modulate.rgb, outline_color.rgb, outline);
)SHADER") : "ALBEDO = palette_color * albedo_modulate.rgb;");
		body = body.replace("// VOXEL_FORWARD_INDIRECT_UNIFORMS", use_voxel_forward_indirect ? String(R"SHADER(
global uniform sampler3D voxel_forward_indirect_near : filter_linear, repeat_disable;
global uniform sampler3D voxel_forward_indirect_far : filter_linear, repeat_disable;
global uniform sampler3D voxel_forward_indirect_distant : filter_linear, repeat_disable;
global uniform vec3 voxel_forward_indirect_near_origin;
global uniform vec3 voxel_forward_indirect_far_origin;
global uniform vec3 voxel_forward_indirect_distant_origin;
global uniform float voxel_forward_indirect_near_cell_size;
global uniform float voxel_forward_indirect_far_cell_size;
global uniform float voxel_forward_indirect_distant_cell_size;
global uniform int voxel_forward_indirect_resolution;
global uniform float voxel_forward_indirect_transition_cells;
global uniform float voxel_forward_indirect_intensity;
global uniform bool voxel_forward_indirect_ready;
)SHADER") : String());
		body = body.replace("// VOXEL_FORWARD_INDIRECT_FUNCTIONS", use_voxel_forward_indirect ? String(VOXEL_FORWARD_INDIRECT_FUNCTIONS) : String());
		body = body.replace("// VOXEL_FORWARD_REFLECTION_UNIFORMS", use_voxel_forward_reflection ? String(R"SHADER(
global uniform sampler2D voxel_forward_reflection : filter_nearest, repeat_disable;
global uniform bool voxel_forward_reflection_ready;
global uniform float voxel_forward_reflection_intensity;
)SHADER") : String());
		body = body.replace("// TRANSPARENCY_UNIFORM", transparency_enabled ? "uniform sampler2D u_transparency : filter_nearest, repeat_disable;" : "");
		body = body.replace("// TRANSPARENCY_OUTPUT", transparency_enabled ? "ALPHA = textureLod(u_transparency, palette_uv, 0.0).r;" : "");
		body = body.replace("// LIGHTING_VERTEX_OUTPUT", use_face_center_lighting ? String(R"SHADER(
	// Optional stylized mode: evaluate direct and reflective lighting once from
	// the center of the visible voxel face. Visibility remains at the exact hit.
	vec3 lighting_voxel_position = vec3(hit_voxel) + vec3(0.5);
	if (hit_axis >= 0) {
		lighting_voxel_position[hit_axis] = float(hit_voxel[hit_axis]) + (local_normal[hit_axis] > 0.0 ? 1.0 : 0.0);
	}
	LIGHTING_VERTEX = (VIEW_MATRIX * MODEL_MATRIX * vec4(lighting_voxel_position * u_voxel_size, 1.0)).xyz;
)SHADER") : String());
		String voxel_forward_enable;
		if (use_voxel_forward_lighting) {
			voxel_forward_enable = "VOXEL_FACE_LIGHTING = true;";
		}
		if (use_voxel_forward_mask) {
			voxel_forward_enable += "\n\tVOXEL_OCCUPANCY_SHADOWS = true;";
		}
		body = body.replace("// VOXEL_OCCUPANCY_ENABLE", voxel_forward_enable);
		body = body.replace("// INDIRECT_LIGHT_OUTPUT", shading_mode == SHADING_MODE_PBR ? String(R"SHADER(
	// Ambient belongs to Voxel Forward, not Forward+'s sky/IBL path. It is
	// deliberately constant for every fragment of the voxel material color.
	EMISSION += palette_color * voxel_forward_ambient_color.rgb * voxel_forward_ambient_energy * voxel_ao_tint;
)SHADER") + (use_voxel_forward_indirect ? String(R"SHADER(
	vec3 indirect_local_position = (vec3(hit_voxel) + vec3(0.5) + local_normal * 0.5) * u_voxel_size;
	vec3 indirect_world_normal = normalize(mat3(MODEL_MATRIX) * local_normal);
	vec3 indirect_world_position = (MODEL_MATRIX * vec4(indirect_local_position, 1.0)).xyz + indirect_world_normal * voxel_forward_indirect_near_cell_size * 0.55;
	vec3 indirect_light = sample_voxel_forward_indirect(indirect_world_position);
	EMISSION += palette_color * indirect_light * (voxel_forward_indirect_intensity / PI) * voxel_ao_tint;
)SHADER") : String()) : String());
		body = body.replace("// REFLECTION_OUTPUT", use_voxel_forward_reflection ? String(R"SHADER(
	if (voxel_forward_reflection_ready) {
		// The resolve pass traces one ray from the shared-world face center, so this
		// radiance is constant across the complete voxel face, including where two
		// independently rendered volumes meet.
		vec3 reflected_radiance = textureLod(voxel_forward_reflection, SCREEN_UV, 0.0).rgb;
		vec3 reflection_f0 = mix(vec3(0.16 * SPECULAR * SPECULAR), palette_color * albedo_modulate.rgb, METALLIC);
		float reflection_gloss = 1.0 - ROUGHNESS;
		EMISSION += reflected_radiance * reflection_f0 * reflection_gloss * reflection_gloss * voxel_forward_reflection_intensity;
	}
)SHADER") : String());
		body = body.replace("// OCCUPANCY_LIGHT", use_voxel_forward_lighting ? String(VOXEL_FORWARD_LIGHT_BODY) : String());
		code += ";\n" + body;
		voxel_shader->set_code(code);
	}
	set_shader(voxel_shader);
	set_shader_parameter("emission_energy", emission_energy);
	set_shader_parameter("albedo_modulate", albedo_modulate);
	set_shader_parameter("roughness_multiplier", roughness_multiplier);
	set_shader_parameter("metallic_multiplier", metallic_multiplier);
	set_shader_parameter("specularity_multiplier", specularity_multiplier);
	if (ambient_occlusion_enabled) {
		set_shader_parameter("ambient_occlusion_color", ambient_occlusion_color);
		set_shader_parameter("ambient_occlusion_face_mode", int(ambient_occlusion_face_mode));
		if (ambient_occlusion_mode == AMBIENT_OCCLUSION_MODE_VOXELIZED) {
			set_shader_parameter("ambient_occlusion_voxelized_curve", _make_voxelized_ao_curve(ambient_occlusion_hardness, ambient_occlusion_strength));
		} else {
			set_shader_parameter("ambient_occlusion_strength", ambient_occlusion_strength);
			set_shader_parameter("ambient_occlusion_hardness", ambient_occlusion_hardness);
		}
	}
	if (outline_enabled) {
		set_shader_parameter("outline_color", outline_color);
		set_shader_parameter("outline_width", outline_width);
	}
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

void VoxelMaterial::set_lighting_position_mode(LightingPositionMode p_mode) {
	ERR_FAIL_INDEX(p_mode, 2);
	if (lighting_position_mode == p_mode) return;
	lighting_position_mode = p_mode;
	if (get_shader().is_valid()) {
		_rebuild_shader();
	}
	emit_changed();
}

VoxelMaterial::LightingPositionMode VoxelMaterial::get_lighting_position_mode() const { return lighting_position_mode; }

void VoxelMaterial::set_emission_energy(real_t p_energy) {
	emission_energy = MAX(p_energy, real_t(0.0));
	if (get_shader().is_valid()) {
		set_shader_parameter("emission_energy", emission_energy);
	}
	emit_changed();
}

real_t VoxelMaterial::get_emission_energy() const { return emission_energy; }

void VoxelMaterial::_emit_ambient_occlusion_changed() {
	ambient_occlusion_change_in_progress = true;
	emit_changed();
	ambient_occlusion_change_in_progress = false;
}

void VoxelMaterial::set_ambient_occlusion_enabled(bool p_enabled) {
	if (ambient_occlusion_enabled == p_enabled) {
		return;
	}
	ambient_occlusion_enabled = p_enabled;
	if (get_shader().is_valid()) {
		_rebuild_shader();
	}
	_emit_ambient_occlusion_changed();
}

bool VoxelMaterial::is_ambient_occlusion_enabled() const { return ambient_occlusion_enabled; }

void VoxelMaterial::set_ambient_occlusion_color(const Color &p_color) {
	if (ambient_occlusion_color == p_color) {
		return;
	}
	ambient_occlusion_color = p_color;
	if (get_shader().is_valid() && ambient_occlusion_enabled) {
		set_shader_parameter("ambient_occlusion_color", ambient_occlusion_color);
	}
	_emit_ambient_occlusion_changed();
}

Color VoxelMaterial::get_ambient_occlusion_color() const { return ambient_occlusion_color; }

void VoxelMaterial::set_ambient_occlusion_strength(real_t p_strength) {
	p_strength = CLAMP(p_strength, real_t(0.0), real_t(1.0));
	if (Math::is_equal_approx(ambient_occlusion_strength, p_strength)) {
		return;
	}
	ambient_occlusion_strength = p_strength;
	if (get_shader().is_valid() && ambient_occlusion_enabled) {
		if (ambient_occlusion_mode == AMBIENT_OCCLUSION_MODE_VOXELIZED) {
			set_shader_parameter("ambient_occlusion_voxelized_curve", _make_voxelized_ao_curve(ambient_occlusion_hardness, ambient_occlusion_strength));
		} else {
			set_shader_parameter("ambient_occlusion_strength", ambient_occlusion_strength);
		}
	}
	_emit_ambient_occlusion_changed();
}

real_t VoxelMaterial::get_ambient_occlusion_strength() const { return ambient_occlusion_strength; }

void VoxelMaterial::set_ambient_occlusion_hardness(real_t p_hardness) {
	p_hardness = CLAMP(p_hardness, real_t(0.0), real_t(1.0));
	if (Math::is_equal_approx(ambient_occlusion_hardness, p_hardness)) {
		return;
	}
	ambient_occlusion_hardness = p_hardness;
	if (get_shader().is_valid() && ambient_occlusion_enabled) {
		if (ambient_occlusion_mode == AMBIENT_OCCLUSION_MODE_VOXELIZED) {
			set_shader_parameter("ambient_occlusion_voxelized_curve", _make_voxelized_ao_curve(ambient_occlusion_hardness, ambient_occlusion_strength));
		} else {
			set_shader_parameter("ambient_occlusion_hardness", ambient_occlusion_hardness);
		}
	}
	_emit_ambient_occlusion_changed();
}

real_t VoxelMaterial::get_ambient_occlusion_hardness() const { return ambient_occlusion_hardness; }

void VoxelMaterial::set_ambient_occlusion_mode(AmbientOcclusionMode p_mode) {
	ERR_FAIL_INDEX(p_mode, 3);
	if (ambient_occlusion_mode == p_mode) {
		return;
	}
	ambient_occlusion_mode = p_mode;
	if (get_shader().is_valid()) {
		_rebuild_shader();
	}
	_emit_ambient_occlusion_changed();
}

VoxelMaterial::AmbientOcclusionMode VoxelMaterial::get_ambient_occlusion_mode() const { return ambient_occlusion_mode; }

void VoxelMaterial::set_ambient_occlusion_face_mode(AmbientOcclusionFaceMode p_mode) {
	ERR_FAIL_INDEX(p_mode, 3);
	if (ambient_occlusion_face_mode == p_mode) {
		return;
	}
	ambient_occlusion_face_mode = p_mode;
	if (get_shader().is_valid() && ambient_occlusion_enabled) {
		set_shader_parameter("ambient_occlusion_face_mode", int(ambient_occlusion_face_mode));
	}
	_emit_ambient_occlusion_changed();
}

VoxelMaterial::AmbientOcclusionFaceMode VoxelMaterial::get_ambient_occlusion_face_mode() const { return ambient_occlusion_face_mode; }

bool VoxelMaterial::is_ambient_occlusion_change_in_progress() const { return ambient_occlusion_change_in_progress; }

void VoxelMaterial::set_albedo_modulate(const Color &p_color) {
	if (albedo_modulate == p_color) {
		return;
	}
	albedo_modulate = p_color;
	if (get_shader().is_valid()) {
		set_shader_parameter("albedo_modulate", albedo_modulate);
	}
	emit_changed();
}

Color VoxelMaterial::get_albedo_modulate() const { return albedo_modulate; }

#define VOXEL_MATERIAL_TEXTURE_ACCESSORS(m_name) \
	void VoxelMaterial::set_##m_name(const Ref<Texture2D> &p_texture) { \
		if (m_name == p_texture) { \
			return; \
		} \
		m_name = p_texture; \
		emit_changed(); \
	} \
	Ref<Texture2D> VoxelMaterial::get_##m_name() const { return m_name; }

VOXEL_MATERIAL_TEXTURE_ACCESSORS(palette_texture)
VOXEL_MATERIAL_TEXTURE_ACCESSORS(material_texture)
VOXEL_MATERIAL_TEXTURE_ACCESSORS(metallic_texture)
VOXEL_MATERIAL_TEXTURE_ACCESSORS(transparency_texture)
VOXEL_MATERIAL_TEXTURE_ACCESSORS(specularity_texture)
VOXEL_MATERIAL_TEXTURE_ACCESSORS(emission_texture)

#undef VOXEL_MATERIAL_TEXTURE_ACCESSORS

void VoxelMaterial::set_roughness_multiplier(real_t p_multiplier) {
	p_multiplier = MAX(p_multiplier, real_t(0.0));
	if (Math::is_equal_approx(roughness_multiplier, p_multiplier)) {
		return;
	}
	roughness_multiplier = p_multiplier;
	if (get_shader().is_valid()) {
		set_shader_parameter("roughness_multiplier", roughness_multiplier);
	}
	emit_changed();
}

real_t VoxelMaterial::get_roughness_multiplier() const { return roughness_multiplier; }

void VoxelMaterial::set_metallic_multiplier(real_t p_multiplier) {
	p_multiplier = MAX(p_multiplier, real_t(0.0));
	if (Math::is_equal_approx(metallic_multiplier, p_multiplier)) {
		return;
	}
	metallic_multiplier = p_multiplier;
	if (get_shader().is_valid()) {
		set_shader_parameter("metallic_multiplier", metallic_multiplier);
	}
	emit_changed();
}

real_t VoxelMaterial::get_metallic_multiplier() const { return metallic_multiplier; }

void VoxelMaterial::set_specularity_multiplier(real_t p_multiplier) {
	p_multiplier = MAX(p_multiplier, real_t(0.0));
	if (Math::is_equal_approx(specularity_multiplier, p_multiplier)) {
		return;
	}
	specularity_multiplier = p_multiplier;
	if (get_shader().is_valid()) {
		set_shader_parameter("specularity_multiplier", specularity_multiplier);
	}
	emit_changed();
}

real_t VoxelMaterial::get_specularity_multiplier() const { return specularity_multiplier; }

void VoxelMaterial::set_outline_enabled(bool p_enabled) {
	if (outline_enabled == p_enabled) {
		return;
	}
	outline_enabled = p_enabled;
	if (get_shader().is_valid()) {
		_rebuild_shader();
	}
	emit_changed();
}

bool VoxelMaterial::is_outline_enabled() const { return outline_enabled; }

void VoxelMaterial::set_outline_color(const Color &p_color) {
	if (outline_color == p_color) {
		return;
	}
	outline_color = p_color;
	if (get_shader().is_valid()) {
		set_shader_parameter("outline_color", outline_color);
	}
	emit_changed();
}

Color VoxelMaterial::get_outline_color() const { return outline_color; }

void VoxelMaterial::set_outline_width(real_t p_width) {
	p_width = CLAMP(p_width, real_t(0.25), real_t(4.0));
	if (Math::is_equal_approx(outline_width, p_width)) {
		return;
	}
	outline_width = p_width;
	if (get_shader().is_valid()) {
		set_shader_parameter("outline_width", outline_width);
	}
	emit_changed();
}

real_t VoxelMaterial::get_outline_width() const { return outline_width; }

void VoxelMaterial::set_transparency_enabled(bool p_enabled) {
	if (transparency_enabled == p_enabled) return;
	transparency_enabled = p_enabled;
	if (get_shader().is_valid()) _rebuild_shader();
}

bool VoxelMaterial::is_transparency_enabled() const { return transparency_enabled; }

void VoxelMaterial::set_texture_features(bool p_metallic_enabled, bool p_specularity_enabled, bool p_emission_enabled) {
	if (metallic_texture_enabled == p_metallic_enabled &&
			specularity_texture_enabled == p_specularity_enabled &&
			emission_texture_enabled == p_emission_enabled) {
		return;
	}
	metallic_texture_enabled = p_metallic_enabled;
	specularity_texture_enabled = p_specularity_enabled;
	emission_texture_enabled = p_emission_enabled;
	if (get_shader().is_valid()) {
		_rebuild_shader();
	}
}

void VoxelMaterial::ensure_shader() {
	if (get_shader().is_null()) {
		_rebuild_shader();
	}
}

void VoxelMaterial::clear_shader_cache() {
	voxel_shader_cache.clear();
}

VoxelMaterial::VoxelMaterial() {}
