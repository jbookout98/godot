/**************************************************************************/
/*  voxel_material.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

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
			MIN(Math::pow(real_t(0.25), exponent) * strength, real_t(1.0)),
			MIN(Math::pow(real_t(0.5), exponent) * strength, real_t(1.0)),
			MIN(Math::pow(real_t(0.75), exponent) * strength, real_t(1.0)),
			MIN(strength, real_t(1.0)));
}

static const char *VOXEL_FORWARD_INDIRECT_FUNCTIONS = R"SHADER(
vec3 voxel_gi_directional_uvw(vec3 grid_uvw, int direction) {
	return vec3((float(direction) + grid_uvw.x) / 6.0, grid_uvw.yz);
}

vec4 voxel_gi_debug_grid_value(sampler3D grid, vec3 origin, float cell_size, vec3 world_position, int direction) {
	vec3 uvw = (world_position - origin) / (cell_size * float(voxel_forward_indirect_resolution));
	if (any(lessThan(uvw, vec3(0.0))) || any(greaterThanEqual(uvw, vec3(1.0)))) return vec4(0.0);
	vec3 half_texel = vec3(0.5 / float(voxel_forward_indirect_resolution));
	uvw = clamp(uvw, half_texel, vec3(1.0) - half_texel);
	return textureLod(grid, voxel_gi_directional_uvw(uvw, direction), 0.0);
}

vec4 voxel_gi_debug_value(vec3 world_position, float selected_cascade, int direction) {
	if (selected_cascade < 0.5) return voxel_gi_debug_grid_value(voxel_forward_indirect_near, voxel_forward_indirect_near_origin, voxel_forward_indirect_near_cell_size, world_position, direction);
	if (selected_cascade < 1.5) return voxel_gi_debug_grid_value(voxel_forward_indirect_far, voxel_forward_indirect_far_origin, voxel_forward_indirect_far_cell_size, world_position, direction);
	return voxel_gi_debug_grid_value(voxel_forward_indirect_distant, voxel_forward_indirect_distant_origin, voxel_forward_indirect_distant_cell_size, world_position, direction);
}

// Lobe convention: +X stores radiance arriving from a source on the +X side
// of the sample. Injection, propagation, surface lookup, and debug labels all
// use this same source-relative convention.
vec4 voxel_gi_texel(sampler3D grid, ivec3 cell, int direction, int resolution) {
	return texelFetch(grid, ivec3(cell.x + direction * resolution, cell.y, cell.z), 0);
}

vec3 sample_voxel_forward_indirect_grid(sampler3D grid, vec3 origin, float cell_size, vec3 world_position, vec3 world_normal, out float edge_weight, out float gather_visibility) {
	gather_visibility = 0.0;
	vec3 uvw = (world_position - origin) / (cell_size * float(voxel_forward_indirect_resolution));
	vec3 edge = min(uvw, vec3(1.0) - uvw);
	float minimum_edge = min(edge.x, min(edge.y, edge.z));
	float transition_width = clamp(voxel_forward_indirect_transition_cells / float(voxel_forward_indirect_resolution), 1.0 / float(voxel_forward_indirect_resolution), 0.45);
	edge_weight = smoothstep(0.0, transition_width, minimum_edge);
	if (minimum_edge <= 0.0) {
		return vec3(0.0);
	}
	int resolution = voxel_forward_indirect_resolution;
	vec3 cell_position = (world_position - origin) / cell_size - vec3(0.5);
	ivec3 base_cell = ivec3(floor(cell_position));
	vec3 fraction = fract(cell_position);
	ivec3 anchor_cell = clamp(ivec3(floor((world_position - origin) / cell_size)), ivec3(0), ivec3(resolution - 1));
	vec3 axis_weight = abs(world_normal);
	float direction_weight_sum = max(axis_weight.x + axis_weight.y + axis_weight.z, 0.0001);
	int x_lobe = world_normal.x >= 0.0 ? 0 : 1;
	int y_lobe = world_normal.y >= 0.0 ? 2 : 3;
	int z_lobe = world_normal.z >= 0.0 ? 4 : 5;
	float anchor_faces[6];
	for (int direction = 0; direction < 6; direction++) {
		anchor_faces[direction] = voxel_gi_texel(grid, anchor_cell, direction, resolution).a;
	}
	vec3 irradiance = vec3(0.0);
	float surviving_weight = 0.0;
	float candidate_weight = 0.0;
	for (int corner = 0; corner < 8; corner++) {
		ivec3 offset = ivec3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
		ivec3 cell = base_cell + offset;
		if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(resolution)))) continue;
		vec3 corner_weight = mix(vec3(1.0) - fraction, fraction, vec3(offset));
		float trilinear_weight = corner_weight.x * corner_weight.y * corner_weight.z;
		candidate_weight += trilinear_weight;
		ivec3 delta = cell - anchor_cell;
		float visibility = 1.0;
		// Requiring every crossed anchor face to be open is a conservative
		// supercover rule. A tied diagonal/corner crossing therefore cannot leak
		// merely because one arbitrary axis ordering happened to be open.
		if (delta.x > 0) visibility *= anchor_faces[0];
		if (delta.x < 0) visibility *= anchor_faces[1];
		if (delta.y > 0) visibility *= anchor_faces[2];
		if (delta.y < 0) visibility *= anchor_faces[3];
		if (delta.z > 0) visibility *= anchor_faces[4];
		if (delta.z < 0) visibility *= anchor_faces[5];
		float weight = trilinear_weight * visibility;
		if (weight <= 0.000001) continue;
		vec3 directional = vec3(0.0);
		if (axis_weight.x > 0.0001) directional += voxel_gi_texel(grid, cell, x_lobe, resolution).rgb * axis_weight.x;
		if (axis_weight.y > 0.0001) directional += voxel_gi_texel(grid, cell, y_lobe, resolution).rgb * axis_weight.y;
		if (axis_weight.z > 0.0001) directional += voxel_gi_texel(grid, cell, z_lobe, resolution).rgb * axis_weight.z;
		irradiance += directional * (weight / direction_weight_sum);
		surviving_weight += weight;
	}
	gather_visibility = candidate_weight > 0.000001 ? surviving_weight / candidate_weight : 0.0;
	// Spatial coverage remains valid even when every neighboring contribution is
	// rejected. Returning black here represents known occlusion, not missing GI,
	// so the caller must not replace it with ambient fallback.
	return surviving_weight > 0.000001 ? irradiance / surviving_weight : vec3(0.0);
}

vec3 sample_voxel_forward_indirect(vec3 world_position, vec3 world_normal, out float coverage, out float selected_cascade, out float gather_visibility) {
	coverage = 0.0;
	selected_cascade = -1.0;
	gather_visibility = 0.0;
	if (!voxel_forward_indirect_ready || voxel_forward_indirect_resolution <= 1) {
		return vec3(0.0);
	}
	float distant_weight;
	float distant_visibility;
	vec3 distant_light = sample_voxel_forward_indirect_grid(voxel_forward_indirect_distant, voxel_forward_indirect_distant_origin, voxel_forward_indirect_distant_cell_size, world_position, world_normal, distant_weight, distant_visibility);
	vec3 indirect_light = distant_light * distant_weight;
	coverage = distant_weight;
	gather_visibility = distant_visibility * distant_weight;
	selected_cascade = distant_weight > 0.0 ? 2.0 : -1.0;
	float far_weight;
	float far_visibility;
	vec3 far_light = sample_voxel_forward_indirect_grid(voxel_forward_indirect_far, voxel_forward_indirect_far_origin, voxel_forward_indirect_far_cell_size, world_position, world_normal, far_weight, far_visibility);
	indirect_light = mix(indirect_light, far_light, far_weight);
	coverage = mix(coverage, 1.0, far_weight);
	gather_visibility = mix(gather_visibility, far_visibility, far_weight);
	selected_cascade = far_weight > 0.5 ? 1.0 : selected_cascade;
	float near_weight;
	float near_visibility;
	vec3 near_light = sample_voxel_forward_indirect_grid(voxel_forward_indirect_near, voxel_forward_indirect_near_origin, voxel_forward_indirect_near_cell_size, world_position, world_normal, near_weight, near_visibility);
	indirect_light = mix(indirect_light, near_light, near_weight);
	coverage = mix(coverage, 1.0, near_weight);
	gather_visibility = mix(gather_visibility, near_visibility, near_weight);
	selected_cascade = near_weight > 0.5 ? 0.0 : selected_cascade;
	return indirect_light;
}
)SHADER";

static const char *VOXEL_FORWARD_DDGI_FUNCTIONS = R"SHADER(
vec3 voxel_ddgi_decode_interpolation_sample(vec3 encoded_irradiance) {
	// Irradiance is stored with a fifth-root perceptual encoding. Convert each
	// probe to sqrt(linear) before spatial interpolation, then square the final
	// average. Decoding after interpolation exaggerates dark probe-to-probe
	// differences and exposes the logical lattice as broad blotches.
	return pow(max(encoded_irradiance, vec3(0.0)), vec3(2.5));
}

vec3 voxel_ddgi_finish_interpolation(vec3 sqrt_linear_irradiance) {
	return sqrt_linear_irradiance * sqrt_linear_irradiance;
}

vec2 voxel_ddgi_oct_encode(vec3 direction) {
	direction /= max(abs(direction.x) + abs(direction.y) + abs(direction.z), 0.00001);
	vec2 encoded = direction.xz;
	if (direction.y < 0.0) encoded = (1.0 - abs(encoded.yx)) * sign(encoded.xy);
	return encoded * 0.5 + 0.5;
}

vec2 voxel_ddgi_tile_uv(ivec2 tile, int tile_size, int interior_size, vec2 oct, vec2 atlas_size) {
	// Every probe owns a one-texel guard border. Sample only between interior
	// texel centers; filtering therefore remains inside this probe's tile.
	vec2 pixel = vec2(tile * tile_size) + vec2(1.5) + clamp(oct, vec2(0.0), vec2(1.0)) * float(interior_size - 1);
	return pixel / atlas_size;
}

uint voxel_ddgi_probe_generation(ivec4 logical_cell_lod) {
	uvec4 bits = uvec4(logical_cell_lod);
	uint hash = 2166136261u;
	hash = (hash ^ bits.x) * 16777619u;
	hash = (hash ^ bits.y) * 16777619u;
	hash = (hash ^ bits.z) * 16777619u;
	hash = (hash ^ bits.w) * 16777619u;
	return hash & 0xfffffu;
}

vec3 voxel_ddgi_probe_sample(sampler2D irradiance_atlas, sampler2D visibility_atlas, sampler2D metadata_atlas,
		ivec3 probe, ivec3 phase_offset, ivec3 logical_origin, int lod, vec3 biased_position, vec3 world_normal, float visibility_bias_distance, out float visibility_weight,
		out float support_weight, out float geometric_visibility, out float in_front_weight, out vec3 probe_position) {
	visibility_weight = 0.0;
	support_weight = 0.0;
	geometric_visibility = 0.0;
	in_front_weight = 0.0;
	probe_position = vec3(0.0);
	int resolution = voxel_forward_ddgi_probe_resolution;
	if (any(lessThan(probe, ivec3(0))) || any(greaterThanEqual(probe, ivec3(resolution)))) {
		return vec3(0.0);
	}
	ivec3 physical = ivec3((probe.x + phase_offset.x) % resolution, (probe.y + phase_offset.y) % resolution, (probe.z + phase_offset.z) % resolution);
	int probe_index = physical.x + physical.y * resolution + physical.z * resolution * resolution;
	ivec2 tile = ivec2(probe_index % (resolution * resolution), probe_index / (resolution * resolution));
	vec4 metadata = texelFetch(metadata_atlas, tile, 0);
	probe_position = metadata.xyz;
	uint packed_metadata = uint(max(metadata.w, 0.0) + 0.5);
	uint state = packed_metadata & 15u;
	uint generation = packed_metadata >> 4u;
	uint expected_generation = voxel_ddgi_probe_generation(ivec4(logical_origin + probe, lod));
	// New and retiring records transfer ownership continuously through atlas
	// confidence. The generation check prevents a toroidal slot's former content
	// from becoming visible under its replacement logical identity.
	if (generation != expected_generation || (state != 1u && state != 2u && state != 3u && state != 4u && state != 5u && state != 6u && state != 7u && state != 8u)) {
		return vec3(0.0);
	}
	vec3 probe_to_surface = biased_position - metadata.xyz;
	float distance_to_surface = length(probe_to_surface);
	vec3 direction_to_surface = distance_to_surface > 0.0001 ? probe_to_surface / distance_to_surface : world_normal;
	vec2 irradiance_uv = voxel_ddgi_tile_uv(tile, 10, 8, voxel_ddgi_oct_encode(world_normal), voxel_forward_ddgi_irradiance_atlas_size);
	vec2 visibility_uv = voxel_ddgi_tile_uv(tile, 18, 16, voxel_ddgi_oct_encode(direction_to_surface), voxel_forward_ddgi_visibility_atlas_size);
	vec4 irradiance = textureLod(irradiance_atlas, irradiance_uv, 0.0);
	vec4 moments = textureLod(visibility_atlas, visibility_uv, 0.0);
	if (irradiance.a <= 0.0 || moments.a <= 0.0 || moments.x <= 0.0) {
		return vec3(0.0);
	}
	float confidence = clamp(min(irradiance.a, moments.a), 0.0, 1.0);
	// Keep a continuous contribution from probes near or behind the tangent
	// plane. A clamped cosine makes whole probe groups disappear at zero and
	// exposes the probe grid as moving planes. This is the production RTXGI
	// wrap-shading weight; depth moments remain responsible for occlusion.
	float wrap_shading = (dot(world_normal, -direction_to_surface) + 1.0) * 0.5;
	float directional_weight = wrap_shading * wrap_shading + 0.2;
	support_weight = confidence;
	in_front_weight = directional_weight;

	float mean_depth = moments.x;
	float variance_floor = max(visibility_bias_distance * visibility_bias_distance * 0.0625, 0.000001);
	float variance = max(moments.y - mean_depth * mean_depth, variance_floor);
	float biased_distance = distance_to_surface;
	float depth_visibility = 1.0;
	if (biased_distance > mean_depth) {
		float delta = biased_distance - mean_depth;
		depth_visibility = variance / (variance + delta * delta);
		depth_visibility = depth_visibility * depth_visibility * depth_visibility;
	}
	// Never let every probe reach exactly zero. Besides providing a stable
	// fallback this keeps the interpolation continuous as depth texels change.
	depth_visibility = max(0.05, depth_visibility);
	geometric_visibility = max(directional_weight * depth_visibility, 0.000001);
	const float crush_threshold = 0.2;
	if (geometric_visibility < crush_threshold) {
		geometric_visibility *= geometric_visibility * geometric_visibility / (crush_threshold * crush_threshold);
	}
	visibility_weight = support_weight * geometric_visibility;
	return irradiance.rgb;
}

vec3 voxel_ddgi_lod_sample(sampler2D irradiance_atlas, sampler2D visibility_atlas, sampler2D metadata_atlas,
		vec3 origin, vec3 cell_size, ivec3 phase_offset, ivec3 logical_origin, int lod, vec3 world_position, vec3 world_normal, vec3 view_direction, out float edge_weight,
		out float initialized_support, out float final_visibility, out float final_in_front) {
	view_direction = normalize(view_direction);
	float minimum_spacing = min(cell_size.x, min(cell_size.y, cell_size.z));
	vec3 bias_direction = mix(world_normal, view_direction, voxel_forward_ddgi_view_bias);
	vec3 bias_vector = bias_direction * (0.75 * minimum_spacing) * voxel_forward_ddgi_self_shadow_bias;
	float visibility_bias_distance = length(bias_vector);
	vec3 biased_position = world_position + bias_vector;
	vec3 local = (biased_position - origin) / cell_size - vec3(0.5);
	ivec3 base = ivec3(floor(local));
	vec3 fraction = fract(local);
	vec3 accumulated = vec3(0.0);
	float visible_weight = 0.0;
	float supported_weight = 0.0;
	float geometry_sum = 0.0;
	float in_front_sum = 0.0;
	float basis_weight_sum = 0.0;
	initialized_support = 0.0;
	for (int z = 0; z <= 1; z++) {
		for (int y = 0; y <= 1; y++) {
			for (int x = 0; x <= 1; x++) {
				ivec3 offset = ivec3(x, y, z);
				vec3 axis_weight = mix(vec3(1.0) - fraction, fraction, vec3(offset));
				float ideal_weight = axis_weight.x * axis_weight.y * axis_weight.z;
				float visibility_weight;
				float support_weight;
				float geometry_visibility;
				float in_front_weight;
				vec3 probe_position;
				vec3 value = voxel_ddgi_probe_sample(irradiance_atlas, visibility_atlas, metadata_atlas, base + offset, phase_offset, logical_origin, lod, biased_position, world_normal, visibility_bias_distance, visibility_weight, support_weight, geometry_visibility, in_front_weight, probe_position);
				// Relocation changes visibility and ray origins, not the logical grid
				// basis. Grid trilinear weights form a partition of unity across cell
				// boundaries; rebuilding the basis around relocated positions does not.
				float trilinear_weight = ideal_weight;
				basis_weight_sum += trilinear_weight;
				float combined_weight = trilinear_weight * visibility_weight;
				accumulated += voxel_ddgi_decode_interpolation_sample(value) * combined_weight;
				visible_weight += combined_weight;
				initialized_support += trilinear_weight * support_weight;
				supported_weight += trilinear_weight * support_weight;
				geometry_sum += trilinear_weight * support_weight * geometry_visibility;
				in_front_sum += trilinear_weight * support_weight * in_front_weight;
			}
		}
	}
	float inverse_basis_weight = 1.0 / max(basis_weight_sum, 0.0001);
	initialized_support *= inverse_basis_weight;
	supported_weight *= inverse_basis_weight;
	geometry_sum *= inverse_basis_weight;
	in_front_sum *= inverse_basis_weight;
	vec3 probe_local = (biased_position - origin) / cell_size;
	vec3 sample_edge_distance = min(probe_local - vec3(0.5), vec3(float(voxel_forward_ddgi_probe_resolution) - 0.5) - probe_local);
	float transition_cells = max(voxel_forward_ddgi_lod_transition * float(voxel_forward_ddgi_probe_resolution), 0.25);
	// Circular origins move in whole cells. Deriving the fade only from that
	// origin makes the cascade contribution jump at every phase change. Center
	// the usable region continuously on the camera and keep the outer probe layer
	// at zero so a newly exposed plane matures behind its parent cascade.
	vec3 camera_distance_cells = abs((biased_position - voxel_forward_ddgi_camera_position) / cell_size);
	float camera_edge_distance = float(voxel_forward_ddgi_probe_resolution) * 0.5 - 1.5 - max(camera_distance_cells.x, max(camera_distance_cells.y, camera_distance_cells.z));
	float resident_edge_distance = min(sample_edge_distance.x, min(sample_edge_distance.y, sample_edge_distance.z));
	edge_weight = smoothstep(0.0, transition_cells, min(camera_edge_distance, resident_edge_distance));
	initialized_support = clamp(initialized_support, 0.0, 1.0);
	final_visibility = supported_weight > 0.0001 ? clamp(geometry_sum / supported_weight, 0.0, 1.0) : 0.0;
	final_in_front = supported_weight > 0.0001 ? clamp(in_front_sum / supported_weight, 0.0, 1.0) : 0.0;
	// Normalize exactly once. The former second multiplication by visible_weight
	// projected the trilinear probe lattice onto otherwise flat surfaces.
	return visible_weight > 0.0001 ? voxel_ddgi_finish_interpolation(accumulated / visible_weight) : vec3(0.0);
}

bool voxel_ddgi_lod_contains(vec3 origin, vec3 cell_size, vec3 world_position, vec3 world_normal) {
	vec3 local = (world_position - origin) / cell_size;
	return all(greaterThanEqual(local, vec3(0.5))) && all(lessThanEqual(local, vec3(float(voxel_forward_ddgi_probe_resolution) - 0.5)));
}

)SHADER";

static const char *VOXEL_FORWARD_DDGI_CASCADE_FUNCTIONS = R"SHADER(
vec3 voxel_ddgi_sample_lod_index(int lod, vec3 world_position, vec3 world_normal, vec3 view_direction, out float edge_weight,
		out float support, out float visibility, out float in_front) {
	if (lod == 0) return voxel_ddgi_lod_sample(voxel_forward_ddgi_irradiance_lod0, voxel_forward_ddgi_depth_lod0, voxel_forward_ddgi_metadata_lod0, voxel_forward_ddgi_origin_lod0, voxel_forward_ddgi_cell_size_lod0, voxel_forward_ddgi_phase_lod0, voxel_forward_ddgi_logical_origin_lod0, 0, world_position, world_normal, view_direction, edge_weight, support, visibility, in_front);
	if (lod == 1) return voxel_ddgi_lod_sample(voxel_forward_ddgi_irradiance_lod1, voxel_forward_ddgi_depth_lod1, voxel_forward_ddgi_metadata_lod1, voxel_forward_ddgi_origin_lod1, voxel_forward_ddgi_cell_size_lod1, voxel_forward_ddgi_phase_lod1, voxel_forward_ddgi_logical_origin_lod1, 1, world_position, world_normal, view_direction, edge_weight, support, visibility, in_front);
	if (lod == 2) return voxel_ddgi_lod_sample(voxel_forward_ddgi_irradiance_lod2, voxel_forward_ddgi_depth_lod2, voxel_forward_ddgi_metadata_lod2, voxel_forward_ddgi_origin_lod2, voxel_forward_ddgi_cell_size_lod2, voxel_forward_ddgi_phase_lod2, voxel_forward_ddgi_logical_origin_lod2, 2, world_position, world_normal, view_direction, edge_weight, support, visibility, in_front);
	return voxel_ddgi_lod_sample(voxel_forward_ddgi_irradiance_lod3, voxel_forward_ddgi_depth_lod3, voxel_forward_ddgi_metadata_lod3, voxel_forward_ddgi_origin_lod3, voxel_forward_ddgi_cell_size_lod3, voxel_forward_ddgi_phase_lod3, voxel_forward_ddgi_logical_origin_lod3, 3, world_position, world_normal, view_direction, edge_weight, support, visibility, in_front);
}

bool voxel_ddgi_contains_lod_index(int lod, vec3 world_position, vec3 world_normal) {
	if (lod == 0) return voxel_ddgi_lod_contains(voxel_forward_ddgi_origin_lod0, voxel_forward_ddgi_cell_size_lod0, world_position, world_normal);
	if (lod == 1) return voxel_ddgi_lod_contains(voxel_forward_ddgi_origin_lod1, voxel_forward_ddgi_cell_size_lod1, world_position, world_normal);
	if (lod == 2) return voxel_ddgi_lod_contains(voxel_forward_ddgi_origin_lod2, voxel_forward_ddgi_cell_size_lod2, world_position, world_normal);
	return voxel_ddgi_lod_contains(voxel_forward_ddgi_origin_lod3, voxel_forward_ddgi_cell_size_lod3, world_position, world_normal);
}

vec3 sample_voxel_forward_ddgi(vec3 world_position, vec3 world_normal, vec3 view_direction, out float final_visibility,
		out float data_support, out float selected_lod, out float final_in_front) {
	final_visibility = 0.0;
	data_support = 0.0;
	selected_lod = -1.0;
	final_in_front = 0.0;
	if (!voxel_forward_ddgi_ready || voxel_forward_ddgi_probe_resolution < 2) return vec3(0.0);
	vec3 result = vec3(0.0);
	float remaining = 1.0;
	float visibility_sum = 0.0;
	float in_front_sum = 0.0;
	float lod_sum = 0.0;
	for (int lod = 0; lod < 4 && remaining > 0.001; lod++) {
		if (!voxel_ddgi_contains_lod_index(lod, world_position, world_normal)) continue;
		float edge; float support; float visibility; float in_front;
		vec3 value = voxel_ddgi_sample_lod_index(lod, world_position, world_normal, view_direction, edge, support, visibility, in_front);
		float maturity = smoothstep(0.0, 1.0, support);
		float local_weight = clamp((lod == 3 ? 1.0 : edge) * maturity, 0.0, 1.0);
		float weight = remaining * local_weight;
		result += value * weight;
		visibility_sum += visibility * weight;
		in_front_sum += in_front * weight;
		lod_sum += float(lod) * weight;
		data_support += weight;
		remaining -= weight;
	}
	if (data_support > 0.0001) {
		final_visibility = visibility_sum / data_support;
		final_in_front = in_front_sum / data_support;
		selected_lod = lod_sum / data_support;
	}
	return result;
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
// VOLUME_RESOURCE_UNIFORMS
uniform sampler2D u_palette : source_color, filter_nearest, repeat_disable;
uniform sampler2D u_material : filter_nearest, repeat_disable;
// OPTIONAL_MATERIAL_TEXTURE_UNIFORMS
// TRANSPARENCY_UNIFORM
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
global uniform bool voxel_forward_toon_enabled;
global uniform int voxel_forward_ddgi_toon_band_count;
global uniform float voxel_forward_ddgi_toon_band_softness;
global uniform float voxel_forward_ddgi_toon_band_range;
global uniform bool voxel_forward_toon_specular_enabled;
global uniform float voxel_forward_toon_specular_threshold;
global uniform float voxel_forward_toon_specular_softness;
global uniform float voxel_forward_toon_specular_strength;
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
	uint packed_faces = uint(texelFetch(u_neighbor_faces, ivec3(face_texel.x >> 3, face_texel.y, face), 0).r * 255.0 + 0.5);
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
	uvec4 directory_bytes = uvec4(texelFetch(u_bricks, brick, 0) * 255.0 + vec4(0.5));
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
	return uint(texelFetch(u_voxels, atlas_texel, 0).r * 255.0 + 0.5);
}

bool voxel_id_is_renderable(uint voxel_id) {
	if (voxel_id == 0u) return false;
	// TRANSPARENCY_OCCUPANCY_CHECK
	return true;
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
			cached_directory_bytes = uvec4(texelFetch(u_bricks, brick, 0) * 255.0 + vec4(0.5));
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
		return uint(texelFetch(u_voxels, atlas_texel, 0).r * 255.0 + 0.5);
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
	uint packed_occupancy = uint(texelFetch(u_neighbor_faces, ivec3(coordinate >> 3, 0, 6 + diagonal), 0).r * 255.0 + 0.5);
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

float voxel_forward_toon_quantize(float value, float band_range) {
	float safe_range = max(band_range, 0.0001);
	float band_steps = float(max(voxel_forward_ddgi_toon_band_count - 1, 1));
	float band_coordinate = clamp(value / safe_range, 0.0, 1.0) * band_steps;
	float lower_band = floor(band_coordinate);
	float transition_width = max(fwidth(band_coordinate), max(voxel_forward_ddgi_toon_band_softness * 0.5, 0.0001));
	float upper_band_weight = smoothstep(0.5 - transition_width, 0.5 + transition_width, fract(band_coordinate));
	float band_value = ((lower_band + upper_band_weight) / band_steps) * safe_range;
	return value > safe_range ? value : band_value;
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
			float direct_diffuse_response = normal_dot_light * visibility;
			if (voxel_forward_toon_enabled && voxel_forward_ddgi_toon_band_count >= 2) {
				// Band the complete direct response so both face lighting and soft
				// shadow transitions use the same designer-controlled toon ramp.
				direct_diffuse_response = voxel_forward_toon_quantize(direct_diffuse_response, 1.0);
			}
			float diffuse_brdf = (1.0 / PI) * fd_view * fd_light;
			DIFFUSE_LIGHT += LIGHT_COLOR * diffuse_brdf * direct_diffuse_response;

			// Point-like lights use one discrete specular response for the complete
			// face. A screen-space GGX lobe makes tiny DDA side faces flare into the
			// chunk-edge lines this renderer is designed to avoid.
			float specular_power = mix(64.0, 4.0, ROUGHNESS);
			float face_specular = pow(normal_dot_light, specular_power);
			float specular_response = face_specular * visibility;
			if (voxel_forward_toon_enabled && voxel_forward_toon_specular_enabled) {
				float highlight_width = max(fwidth(specular_response), max(voxel_forward_toon_specular_softness * 0.5, 0.0001));
				specular_response = smoothstep(
						voxel_forward_toon_specular_threshold - highlight_width,
						voxel_forward_toon_specular_threshold + highlight_width,
						specular_response) * voxel_forward_toon_specular_strength;
			}
			vec3 f0 = mix(vec3(0.04), ALBEDO, METALLIC);
			SPECULAR_LIGHT += f0 * specular_response * LIGHT_COLOR * SPECULAR_AMOUNT;
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
	// ARCH_DEPTH_PATH_BEGIN
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
	int outer_limit = min(int(brick_dimensions.x + brick_dimensions.y + brick_dimensions.z) + 3, max_outer_steps);

	for (int outer = 0; outer < outer_limit; outer++) {
		uvec4 directory_bytes = uvec4(texelFetch(u_bricks, brick_coordinate, 0) * 255.0 + vec4(0.5));
		uint directory_code = directory_bytes.r | (directory_bytes.g << 8u) | (directory_bytes.b << 16u);
		if (directory_code == 1u) {
			hit_id = directory_bytes.a;
			hit = voxel_id_is_renderable(hit_id);
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
				hit_id = uint(texelFetch(u_voxels, atlas_texel, 0).r * 255.0 + 0.5);
				if (voxel_id_is_renderable(hit_id)) {
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
	if (hit_axis >= 0) {
		local_normal = vec3(0.0);
		local_normal[hit_axis] = -float(step_direction[hit_axis]);
		// Snap the hit to the exact integer face plane. This makes both proxy
		// triangles and neighboring chunks write the same depth for that face.
		float face_plane = float(hit_voxel[hit_axis]) + (local_normal[hit_axis] > 0.0 ? 1.0 : 0.0);
		hit_t = (face_plane - ray_origin[hit_axis]) * inverse_direction[hit_axis];
	} else {
		vec3 direction_abs = abs(ray_direction);
		if (direction_abs.x >= direction_abs.y && direction_abs.x >= direction_abs.z) { hit_axis = 0; local_normal = vec3(-sign(ray_direction.x), 0.0, 0.0); }
		else if (direction_abs.y >= direction_abs.z) { hit_axis = 1; local_normal = vec3(0.0, -sign(ray_direction.y), 0.0); }
		else { hit_axis = 2; local_normal = vec3(0.0, 0.0, -sign(ray_direction.z)); }
	}
	hit_voxel_position = ray_origin + ray_direction * hit_t;
	if (hit_axis >= 0) {
		hit_voxel_position[hit_axis] = float(hit_voxel[hit_axis]) + (local_normal[hit_axis] > 0.0 ? 1.0 : 0.0);
	}
	// Each volume raycasts its own AABB. A boundary face is internal to the

	int neighbor_face = boundary_face_for_axis(hit_axis, hit_voxel, step_direction);
	if (has_occupied_neighbor(neighbor_face, hit_voxel)) {
		discard;
	}

	hit_local_position = hit_voxel_position * u_voxel_size;
	hit_view_position = VIEW_MATRIX * MODEL_MATRIX * vec4(hit_local_position, 1.0);
	vec4 hit_clip_position = PROJECTION_MATRIX * hit_view_position;

	if (abs(hit_clip_position.w) <= DIR_EPSILON) discard;
	VOXEL_DEPTH = hit_clip_position.z / hit_clip_position.w;
	// ARCH_DEPTH_PATH_END

	NORMAL = normalize(mat3(VIEW_MATRIX) * (MODEL_NORMAL_MATRIX * local_normal));
	vec2 palette_uv = vec2((float(hit_id) + 0.5) / 256.0, 0.5);

	// AO_OUTPUT



	// Shadow lookup must use the actual DDA surface hit. Using the center of the
	// voxel face moves the receiver by as much as half a voxel in either tangent
	// axis, which shows up as a bright contact gap even with a near-zero light
	// bias. The hit is already snapped exactly onto the crossed face above.
	LIGHT_VERTEX = hit_view_position.xyz;

	// LIGHTING_VERTEX_OUTPUT
	// VOXEL_OCCUPANCY_ENABLE

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
	// ARCH_COLOR_PATH_END
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
	VOXEL_SHADER_ARCHITECTURAL_HIT_BUFFER = 1u << 14,
	VOXEL_SHADER_ARCHITECTURAL_HIT_POSITION = 1u << 15,
	VOXEL_SHADER_PRECOMPUTED_INVERSE = 1u << 16,
	VOXEL_SHADER_BATCHED_RESOURCES = 1u << 17,
	VOXEL_SHADER_AO_TINT = 1u << 18,
	VOXEL_SHADER_AO_TINT_PALETTE = 1u << 19,
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
	ClassDB::bind_method(D_METHOD("set_ambient_occlusion_intensity", "intensity"), &VoxelMaterial::set_ambient_occlusion_intensity);
	ClassDB::bind_method(D_METHOD("get_ambient_occlusion_intensity"), &VoxelMaterial::get_ambient_occlusion_intensity);
	ClassDB::bind_method(D_METHOD("set_ambient_occlusion_hardness", "hardness"), &VoxelMaterial::set_ambient_occlusion_hardness);
	ClassDB::bind_method(D_METHOD("get_ambient_occlusion_hardness"), &VoxelMaterial::get_ambient_occlusion_hardness);
	ClassDB::bind_method(D_METHOD("set_ambient_occlusion_contrast", "contrast"), &VoxelMaterial::set_ambient_occlusion_contrast);
	ClassDB::bind_method(D_METHOD("get_ambient_occlusion_contrast"), &VoxelMaterial::get_ambient_occlusion_contrast);
	ClassDB::bind_method(D_METHOD("set_ambient_occlusion_direct_light_influence", "influence"), &VoxelMaterial::set_ambient_occlusion_direct_light_influence);
	ClassDB::bind_method(D_METHOD("get_ambient_occlusion_direct_light_influence"), &VoxelMaterial::get_ambient_occlusion_direct_light_influence);
	ClassDB::bind_method(D_METHOD("set_ambient_occlusion_tint_enabled", "enabled"), &VoxelMaterial::set_ambient_occlusion_tint_enabled);
	ClassDB::bind_method(D_METHOD("is_ambient_occlusion_tint_enabled"), &VoxelMaterial::is_ambient_occlusion_tint_enabled);
	ClassDB::bind_method(D_METHOD("set_ambient_occlusion_tint_strength", "strength"), &VoxelMaterial::set_ambient_occlusion_tint_strength);
	ClassDB::bind_method(D_METHOD("get_ambient_occlusion_tint_strength"), &VoxelMaterial::get_ambient_occlusion_tint_strength);
	ClassDB::bind_method(D_METHOD("set_ambient_occlusion_tint_palette_texture", "texture"), &VoxelMaterial::set_ambient_occlusion_tint_palette_texture);
	ClassDB::bind_method(D_METHOD("get_ambient_occlusion_tint_palette_texture"), &VoxelMaterial::get_ambient_occlusion_tint_palette_texture);
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
	// Keep the original names as storage-only aliases so existing .tres files and
	// scripts continue to load while the Inspector presents accurate terminology.
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "ambient_occlusion_color", PROPERTY_HINT_COLOR_NO_ALPHA, "", PROPERTY_USAGE_STORAGE), "set_ambient_occlusion_color", "get_ambient_occlusion_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ambient_occlusion_strength", PROPERTY_HINT_RANGE, "0,1,0.01", PROPERTY_USAGE_STORAGE), "set_ambient_occlusion_strength", "get_ambient_occlusion_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ambient_occlusion_hardness", PROPERTY_HINT_RANGE, "0,1,0.01", PROPERTY_USAGE_STORAGE), "set_ambient_occlusion_hardness", "get_ambient_occlusion_hardness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ambient_occlusion_intensity", PROPERTY_HINT_RANGE, "0,1,0.01", PROPERTY_USAGE_EDITOR), "set_ambient_occlusion_intensity", "get_ambient_occlusion_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ambient_occlusion_contrast", PROPERTY_HINT_RANGE, "0,1,0.01", PROPERTY_USAGE_EDITOR), "set_ambient_occlusion_contrast", "get_ambient_occlusion_contrast");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ambient_occlusion_mode", PROPERTY_HINT_ENUM, "Smooth,Voxelized,Hard Corners"), "set_ambient_occlusion_mode", "get_ambient_occlusion_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ambient_occlusion_face_mode", PROPERTY_HINT_ENUM, "All Faces,Floor Faces Only,Ceiling Faces Only"), "set_ambient_occlusion_face_mode", "get_ambient_occlusion_face_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ambient_occlusion_direct_light_influence", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_ambient_occlusion_direct_light_influence", "get_ambient_occlusion_direct_light_influence");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ambient_occlusion_tint_enabled"), "set_ambient_occlusion_tint_enabled", "is_ambient_occlusion_tint_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ambient_occlusion_tint_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_ambient_occlusion_tint_strength", "get_ambient_occlusion_tint_strength");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "ambient_occlusion_tint_palette_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_ambient_occlusion_tint_palette_texture", "get_ambient_occlusion_tint_palette_texture");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "ambient_occlusion_fallback_tint", PROPERTY_HINT_COLOR_NO_ALPHA, "", PROPERTY_USAGE_EDITOR), "set_ambient_occlusion_color", "get_ambient_occlusion_color");
	ADD_GROUP("Channel Textures", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "palette_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_palette_texture", "get_palette_texture");
	// Preserve old resources without presenting this ambiguous packed override
	// in the authoring interface. New content uses the channel textures below.
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D", PROPERTY_USAGE_STORAGE), "set_material_texture", "get_material_texture");
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
	// The architectural hit buffer reconstructs opaque voxel surfaces in a
	// separate color pass. Transparent materials can change occupancy through
	// their lookup texture between those passes, so reusing the opaque hit
	// payload can shade the wrong surface (animated effects appeared black).
	const bool use_architectural_hit_buffer = !transparency_enabled &&
			RenderingMethod::is_current_voxel_forward_method() &&
			bool(GLOBAL_GET("rendering/voxel_forward/architectural_hit_buffer/enabled"));
	const bool use_architectural_hit_position = use_architectural_hit_buffer &&
			(bool(GLOBAL_GET("rendering/voxel_forward/architectural_hit_buffer/exact_position_enabled")) || use_voxel_forward_indirect);
	const bool use_precomputed_inverse = use_architectural_hit_buffer && !use_architectural_hit_position &&
			bool(GLOBAL_GET("rendering/voxel_forward/architectural_hit_buffer/precomputed_inverse_enabled"));
	// Voxel Forward outlines are composited from the architectural hit buffer.
	// Keeping this work out of the material variant avoids four occupancy reads,
	// derivatives, and their live values in the register-heavy opaque shader.
	const bool use_material_outline = outline_enabled && !use_architectural_hit_buffer;
	// Voxel Forward is face-shaded by definition. Point and spot lights must use
	// the same face-center receiver as directional visibility, so illumination
	// cannot form a smooth gradient across an individual voxel face.
	const bool use_face_center_lighting = RenderingMethod::is_current_voxel_forward_method() || lighting_position_mode == LIGHTING_POSITION_VOXEL_FACE_CENTER;
	const bool use_ao_tint = ambient_occlusion_enabled && ambient_occlusion_tint_enabled;
	const bool use_ao_tint_palette = use_ao_tint && ambient_occlusion_tint_palette_texture.is_valid();
	const int ao_mode_index = ambient_occlusion_enabled ? int(ambient_occlusion_mode) : 0;
	uint32_t shader_key = uint32_t(shading_mode);
	shader_key |= transparency_enabled ? VOXEL_SHADER_TRANSPARENCY : 0;
	shader_key |= use_voxel_forward_lighting ? VOXEL_SHADER_FORWARD_LIGHTING : 0;
	shader_key |= use_voxel_forward_mask ? VOXEL_SHADER_FORWARD_MASK : 0;
	shader_key |= use_face_center_lighting ? VOXEL_SHADER_FACE_CENTER_LIGHTING : 0;
	shader_key |= ambient_occlusion_enabled ? VOXEL_SHADER_AMBIENT_OCCLUSION : 0;
	shader_key |= uint32_t(ao_mode_index) << 6;
	shader_key |= use_material_outline ? VOXEL_SHADER_OUTLINE : 0;
	shader_key |= use_voxel_forward_indirect ? VOXEL_SHADER_INDIRECT : 0;
	shader_key |= use_voxel_forward_reflection ? VOXEL_SHADER_REFLECTION : 0;
	shader_key |= metallic_texture_enabled ? VOXEL_SHADER_METALLIC_TEXTURE : 0;
	shader_key |= specularity_texture_enabled ? VOXEL_SHADER_SPECULARITY_TEXTURE : 0;
	shader_key |= emission_texture_enabled ? VOXEL_SHADER_EMISSION_TEXTURE : 0;
	shader_key |= use_architectural_hit_buffer ? VOXEL_SHADER_ARCHITECTURAL_HIT_BUFFER : 0;
	shader_key |= use_architectural_hit_position ? VOXEL_SHADER_ARCHITECTURAL_HIT_POSITION : 0;
	shader_key |= use_precomputed_inverse ? VOXEL_SHADER_PRECOMPUTED_INVERSE : 0;
	shader_key |= batched_resources_enabled ? VOXEL_SHADER_BATCHED_RESOURCES : 0;
	shader_key |= use_ao_tint ? VOXEL_SHADER_AO_TINT : 0;
	shader_key |= use_ao_tint_palette ? VOXEL_SHADER_AO_TINT_PALETTE : 0;
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
		if (batched_resources_enabled) {
			body = body.replace("// VOLUME_RESOURCE_UNIFORMS", R"SHADER(
#define u_voxels VOXEL_BATCH_VOXELS
#define u_bricks VOXEL_BATCH_BRICKS
#define u_neighbor_faces VOXEL_BATCH_NEIGHBORS
#define u_volume_dims VOXEL_VOLUME_DIMS
#define u_brick_dims VOXEL_BRICK_DIMS
#define u_atlas_brick_dims VOXEL_ATLAS_BRICK_DIMS
#define u_neighbor_mask VOXEL_NEIGHBOR_MASK
#define u_neighbor_diagonal_mask VOXEL_NEIGHBOR_DIAGONAL_MASK
#define u_voxel_size VOXEL_VOXEL_SIZE
)SHADER");
		} else {
			body = body.replace("// VOLUME_RESOURCE_UNIFORMS", R"SHADER(
uniform sampler3D u_voxels : filter_nearest, repeat_disable;
uniform sampler3D u_bricks : filter_nearest, repeat_disable;
uniform sampler3D u_neighbor_faces : filter_nearest, repeat_disable;
uniform ivec3 u_volume_dims = ivec3(1);
uniform ivec3 u_brick_dims = ivec3(1);
uniform ivec3 u_atlas_brick_dims = ivec3(1);
uniform int u_neighbor_mask = 0;
uniform int u_neighbor_diagonal_mask = 0;
uniform float u_voxel_size = 0.1;
)SHADER");
		}
		if (use_architectural_hit_buffer) {
			body = body.replace("// ARCH_DEPTH_PATH_BEGIN", R"SHADER(
	if (IN_DEPTH_PASS) {
		vec3 local_normal = vec3(0.0);
		vec3 hit_voxel_position = vec3(0.0);
		vec3 hit_local_position = vec3(0.0);
		vec4 hit_view_position = vec4(0.0);
		int hit_axis = -1;
		ivec3 hit_voxel = ivec3(0);
		uint hit_id = 0u;
)SHADER");
			body = body.replace("// ARCH_DEPTH_PATH_END", R"SHADER(
		// The normal is the only surface attribute needed by normal/roughness
		// depth layouts. Material, AO, lighting, and reflection evaluation begin
		// exclusively in the color branch below.
		vec3 world_geometric_normal = normalize(MODEL_NORMAL_MATRIX * local_normal);
		NORMAL = normalize(mat3(VIEW_MATRIX) * world_geometric_normal);
		uint local_face_code = uint(hit_axis * 2 + (local_normal[hit_axis] > 0.0 ? 1 : 0));
		vec3 absolute_world_normal = abs(world_geometric_normal);
		int world_axis = absolute_world_normal.x >= absolute_world_normal.y && absolute_world_normal.x >= absolute_world_normal.z ? 0 : (absolute_world_normal.y >= absolute_world_normal.z ? 1 : 2);
		float off_axis_amount = absolute_world_normal[(world_axis + 1) % 3] + absolute_world_normal[(world_axis + 2) % 3];
		uint world_face_code = 7u;
		if (absolute_world_normal[world_axis] >= 0.9999 && off_axis_amount <= 0.0001) {
			world_face_code = uint(world_axis * 2 + (world_geometric_normal[world_axis] > 0.0 ? 1 : 0));
		}
		// The high 18 bits store owner+1. This covers instance indices 0..262142,
		// far beyond the Voxel Forward resident-volume limit, without permitting
		// a wrapped owner to validate as another volume.
		uint encoded_owner = uint(VOXEL_INSTANCE_ID) + 1u;
		if (encoded_owner > 0x3FFFFu) discard;
		VOXEL_HIT_PAYLOAD = (encoded_owner << 14u) | (world_face_code << 11u) | (local_face_code << 8u) | (hit_id & 0xFFu);
		// Store one canonical world-space face center for every covered pixel. The
		// DDGI screen resolve uses this identity to share one gather across matching
		// lanes, including arbitrarily rotated rigid voxel bodies. W contains a
		// 12+12-bit octahedral world normal plus one (zero remains the clear value).
		// ARCH_HIT_POSITION_WRITE
	} else {
		vec3 local_normal = vec3(0.0);
		vec3 hit_voxel_position = vec3(0.0);
		vec3 hit_local_position = vec3(0.0);
		vec4 hit_view_position = vec4(0.0);
		int hit_axis = -1;
		ivec3 hit_voxel = ivec3(0);
		uint hit_id = 0u;

		uint encoded_owner = VOXEL_HIT_PAYLOAD >> 14u;
		if (encoded_owner == 0u || encoded_owner != uint(VOXEL_INSTANCE_ID) + 1u) discard;
		uint face_code = (VOXEL_HIT_PAYLOAD >> 8u) & 0x7u;
		if (face_code >= 6u) discard;
		hit_axis = int(face_code >> 1u);
		local_normal = vec3(0.0);
		local_normal[hit_axis] = (face_code & 1u) != 0u ? 1.0 : -1.0;
		hit_id = VOXEL_HIT_PAYLOAD & 0xFFu;

		// ARCH_HIT_POSITION_READ
		hit_voxel = ivec3(floor(hit_voxel_position - local_normal * (EPSILON * 4.0)));
		if (any(lessThan(hit_voxel, ivec3(0))) || any(greaterThanEqual(hit_voxel, u_volume_dims))) discard;
)SHADER");
			body = body.replace("// ARCH_HIT_POSITION_WRITE", use_architectural_hit_position ? String(R"SHADER(
		vec3 canonical_local_face = (vec3(hit_voxel) + vec3(0.5) + local_normal * 0.5) * u_voxel_size;
		vec3 canonical_world_face = (MODEL_MATRIX * vec4(canonical_local_face, 1.0)).xyz;
		vec3 encoded_face_normal = world_geometric_normal / max(dot(abs(world_geometric_normal), vec3(1.0)), 0.00001);
		vec2 normal_oct = encoded_face_normal.xz;
		if (encoded_face_normal.y < 0.0) normal_oct = (1.0 - abs(normal_oct.yx)) * sign(normal_oct.xy);
		uvec2 quantized_normal = uvec2(round(clamp(normal_oct * 0.5 + 0.5, vec2(0.0), vec2(1.0)) * 4095.0));
		uint packed_world_normal = quantized_normal.x | (quantized_normal.y << 12u);
		VOXEL_HIT_POSITION = vec4(canonical_world_face, float(packed_world_normal + 1u));
)SHADER")
																							 : String());
			body = body.replace("// ARCH_HIT_POSITION_READ", use_architectural_hit_position ? String(R"SHADER(
		if (VOXEL_HIT_POSITION.w < 1.0) discard;
		hit_local_position = (VOXEL_INV_MODEL_MATRIX * vec4(VOXEL_HIT_POSITION.xyz, 1.0)).xyz;
		hit_voxel_position = hit_local_position / u_voxel_size;
		hit_view_position = VIEW_MATRIX * vec4(VOXEL_HIT_POSITION.xyz, 1.0);
)SHADER")
																							: String(R"SHADER(
		vec2 hit_ndc_xy = SCREEN_UV * 2.0 - vec2(1.0);
		vec4 reconstructed_view = INV_PROJECTION_MATRIX * vec4(hit_ndc_xy, VOXEL_HIT_DEPTH, 1.0);
		if (abs(reconstructed_view.w) <= DIR_EPSILON) discard;
		reconstructed_view /= reconstructed_view.w;
		hit_view_position = reconstructed_view;
		vec3 reconstructed_world = (INV_VIEW_MATRIX * reconstructed_view).xyz;
		// ARCH_INVERSE_MODEL_RECONSTRUCTION
		hit_voxel_position = hit_local_position / u_voxel_size;
		hit_voxel_position[hit_axis] = round(hit_voxel_position[hit_axis]);
)SHADER"));
			body = body.replace("// ARCH_INVERSE_MODEL_RECONSTRUCTION", use_precomputed_inverse ? String("hit_local_position = (VOXEL_INV_MODEL_MATRIX * vec4(reconstructed_world, 1.0)).xyz;") : String("hit_local_position = (inverse(MODEL_MATRIX) * vec4(reconstructed_world, 1.0)).xyz;"));
			body = body.replace("// ARCH_COLOR_PATH_END", "\t}");
		} else {
			body = body.replace("// ARCH_DEPTH_PATH_BEGIN", R"SHADER(
	vec3 local_normal = vec3(0.0);
	vec3 hit_voxel_position = vec3(0.0);
	vec3 hit_local_position = vec3(0.0);
	vec4 hit_view_position = vec4(0.0);
	int hit_axis = -1;
	ivec3 hit_voxel = ivec3(0);
	uint hit_id = 0u;
)SHADER");
			body = body.replace("// ARCH_DEPTH_PATH_END", "");
			body = body.replace("// ARCH_COLOR_PATH_END", "");
		}
		String ao_uniforms;
		String ao_functions;
		String ao_output = R"SHADER(
	float voxel_ao_visibility = 1.0;
	vec3 voxel_ao_indirect_factor = vec3(1.0);
	AO = voxel_ao_visibility;
	AO_LIGHT_AFFECT = 0.0;
)SHADER";
		if (ambient_occlusion_enabled) {
			ao_uniforms = R"SHADER(
uniform vec4 ambient_occlusion_color : source_color = vec4(0.0, 0.0, 0.0, 1.0);
uniform int ambient_occlusion_face_mode = 0;
uniform float ambient_occlusion_direct_light_influence : hint_range(0.0, 1.0) = 0.0;
)SHADER";
			if (use_ao_tint) {
				ao_uniforms += "uniform float ambient_occlusion_tint_strength : hint_range(0.0, 1.0) = 1.0;\n";
			}
			if (use_ao_tint_palette) {
				ao_uniforms += "uniform sampler2D u_ambient_occlusion_tint_palette : source_color, filter_nearest, repeat_disable;\n";
			}
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
	float voxel_ao_visibility = 1.0;
	vec3 voxel_ao_indirect_factor = vec3(1.0);
	bool ambient_occlusion_receives_face =
			ambient_occlusion_face_mode == 0 ||
			(ambient_occlusion_face_mode == 1 && local_normal.y > 0.5) ||
			(ambient_occlusion_face_mode == 2 && local_normal.y < -0.5);
	if (ambient_occlusion_receives_face) {
		float calculated_ao = voxel_face_ao(hit_voxel, hit_voxel_position, hit_axis, local_normal);
		float raw_geometric_occlusion = clamp(1.0 - calculated_ao, 0.0, 1.0);
)SHADER";
			if (ambient_occlusion_mode == AMBIENT_OCCLUSION_MODE_VOXELIZED) {
				ao_output += R"SHADER(
		int curve_index = clamp(int(round(raw_geometric_occlusion * 4.0)) - 1, 0, 3);
		float contrast_shaped_occlusion = raw_geometric_occlusion > 0.0 ? ambient_occlusion_voxelized_curve[curve_index] : 0.0;
)SHADER";
			} else {
				ao_output += R"SHADER(
		float contrast_exponent = exp2((clamp(ambient_occlusion_hardness, 0.0, 1.0) - 0.5) * 4.0);
		float contrast_shaped_occlusion = pow(raw_geometric_occlusion, contrast_exponent) * clamp(ambient_occlusion_strength, 0.0, 1.0);
)SHADER";
			}
			ao_output += R"SHADER(
		float final_occlusion_amount = clamp(contrast_shaped_occlusion, 0.0, 1.0);
		voxel_ao_visibility = 1.0 - final_occlusion_amount;
		voxel_ao_indirect_factor = vec3(voxel_ao_visibility);
)SHADER";
			if (use_ao_tint) {
				ao_output += "\t\tvec3 material_ao_tint = ambient_occlusion_color.rgb;\n";
				if (use_ao_tint_palette) {
					ao_output += "\t\tmaterial_ao_tint = textureLod(u_ambient_occlusion_tint_palette, palette_uv, 0.0).rgb;\n";
				}
				ao_output += R"SHADER(
		// Treat the palette as chroma, not a second darkness control. Intensity
		// remains solely responsible for maximum occlusion.
		float tint_peak = max(material_ao_tint.r, max(material_ao_tint.g, material_ao_tint.b));
		vec3 normalized_ao_tint = tint_peak > 0.0001 ? material_ao_tint / tint_peak : vec3(1.0);
		float applied_tint_strength = ambient_occlusion_tint_strength * final_occlusion_amount;
		voxel_ao_indirect_factor *= mix(vec3(1.0), normalized_ao_tint, applied_tint_strength);
)SHADER";
			}
			ao_output += R"SHADER(
	}
	AO = voxel_ao_visibility;
	AO_LIGHT_AFFECT = ambient_occlusion_direct_light_influence;
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
		body = body.replace("// OUTLINE_UNIFORMS", use_material_outline ? String(R"SHADER(
uniform vec4 outline_color : source_color = vec4(0.0, 0.0, 0.0, 1.0);
uniform float outline_width = 1.0;
)SHADER")
																		: String());
		body = body.replace("// OUTLINE_FUNCTION", use_material_outline ? String(VOXEL_OUTLINE_FUNCTION) : String());
		body = body.replace("// ALBEDO_OUTPUT", use_material_outline ? String(R"SHADER(
	float outline = voxel_normal_outline(hit_voxel, hit_voxel_position, hit_axis) * outline_color.a;
	ALBEDO = mix(palette_color * albedo_modulate.rgb, outline_color.rgb, outline);
)SHADER")
																	 : "ALBEDO = palette_color * albedo_modulate.rgb;");
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
global uniform int voxel_forward_indirect_backend;
global uniform int voxel_forward_indirect_debug_mode;
global uniform sampler2D voxel_forward_restir_gi : filter_linear, repeat_disable;
global uniform bool voxel_forward_restir_ready;
global uniform float voxel_forward_corner_ao_strength;
global uniform vec4 voxel_forward_corner_ao_tint : source_color;
global uniform vec3 voxel_forward_indirect_dirty_min0;
global uniform vec3 voxel_forward_indirect_dirty_min1;
global uniform vec3 voxel_forward_indirect_dirty_min2;
global uniform vec3 voxel_forward_indirect_dirty_max0;
global uniform vec3 voxel_forward_indirect_dirty_max1;
global uniform vec3 voxel_forward_indirect_dirty_max2;
global uniform int voxel_forward_indirect_staging_mask;
global uniform sampler2D voxel_forward_ddgi_irradiance_lod0 : filter_linear, repeat_disable;
global uniform sampler2D voxel_forward_ddgi_irradiance_lod1 : filter_linear, repeat_disable;
global uniform sampler2D voxel_forward_ddgi_irradiance_lod2 : filter_linear, repeat_disable;
global uniform sampler2D voxel_forward_ddgi_irradiance_lod3 : filter_linear, repeat_disable;
global uniform sampler2D voxel_forward_ddgi_depth_lod0 : filter_linear, repeat_disable;
global uniform sampler2D voxel_forward_ddgi_depth_lod1 : filter_linear, repeat_disable;
global uniform sampler2D voxel_forward_ddgi_depth_lod2 : filter_linear, repeat_disable;
global uniform sampler2D voxel_forward_ddgi_depth_lod3 : filter_linear, repeat_disable;
global uniform sampler2D voxel_forward_ddgi_metadata_lod0 : filter_nearest, repeat_disable;
global uniform sampler2D voxel_forward_ddgi_metadata_lod1 : filter_nearest, repeat_disable;
global uniform sampler2D voxel_forward_ddgi_metadata_lod2 : filter_nearest, repeat_disable;
global uniform sampler2D voxel_forward_ddgi_metadata_lod3 : filter_nearest, repeat_disable;
global uniform vec3 voxel_forward_ddgi_origin_lod0;
global uniform vec3 voxel_forward_ddgi_origin_lod1;
global uniform vec3 voxel_forward_ddgi_origin_lod2;
global uniform vec3 voxel_forward_ddgi_origin_lod3;
global uniform vec3 voxel_forward_ddgi_cell_size_lod0;
global uniform vec3 voxel_forward_ddgi_cell_size_lod1;
global uniform vec3 voxel_forward_ddgi_cell_size_lod2;
global uniform vec3 voxel_forward_ddgi_cell_size_lod3;
global uniform ivec3 voxel_forward_ddgi_phase_lod0;
global uniform ivec3 voxel_forward_ddgi_phase_lod1;
global uniform ivec3 voxel_forward_ddgi_phase_lod2;
global uniform ivec3 voxel_forward_ddgi_phase_lod3;
global uniform ivec3 voxel_forward_ddgi_logical_origin_lod0;
global uniform ivec3 voxel_forward_ddgi_logical_origin_lod1;
global uniform ivec3 voxel_forward_ddgi_logical_origin_lod2;
global uniform ivec3 voxel_forward_ddgi_logical_origin_lod3;
global uniform int voxel_forward_ddgi_probe_resolution;
global uniform vec2 voxel_forward_ddgi_irradiance_atlas_size;
global uniform vec2 voxel_forward_ddgi_visibility_atlas_size;
global uniform float voxel_forward_ddgi_self_shadow_bias;
global uniform float voxel_forward_ddgi_view_bias;
global uniform float voxel_forward_ddgi_lod_transition;
global uniform vec3 voxel_forward_ddgi_camera_position;
global uniform bool voxel_forward_ddgi_ready;
global uniform sampler2D voxel_forward_ddgi_resolve : filter_nearest, repeat_disable;
global uniform bool voxel_forward_ddgi_resolve_ready;
global uniform int voxel_forward_ddgi_debug_mode;
global uniform float voxel_forward_ddgi_shadow_fill_strength;
global uniform vec4 voxel_forward_ddgi_shadow_fill_tint : source_color;
global uniform float voxel_forward_ddgi_shadow_fill_reach;
global uniform float voxel_forward_ddgi_color_saturation;
)SHADER")
																							 : String());
		body = body.replace("// VOXEL_FORWARD_INDIRECT_FUNCTIONS", use_voxel_forward_indirect ? String(VOXEL_FORWARD_INDIRECT_FUNCTIONS) : String());
		body = body.replace("// VOXEL_FORWARD_REFLECTION_UNIFORMS", use_voxel_forward_reflection ? String(R"SHADER(
global uniform sampler2D voxel_forward_reflection : filter_nearest, repeat_disable;
global uniform bool voxel_forward_reflection_ready;
global uniform float voxel_forward_reflection_intensity;
)SHADER")
																								 : String());
		body = body.replace("// TRANSPARENCY_UNIFORM", transparency_enabled ? "uniform sampler2D u_transparency : filter_nearest, repeat_disable;" : "");
		body = body.replace("// TRANSPARENCY_OCCUPANCY_CHECK", transparency_enabled ? R"SHADER(
	vec2 transparency_uv = vec2((float(voxel_id) + 0.5) / 256.0, 0.5);
	if (textureLod(u_transparency, transparency_uv, 0.0).r <= 0.001) return false;
)SHADER"
																					: "");
		body = body.replace("// TRANSPARENCY_OUTPUT", transparency_enabled ? "ALPHA = textureLod(u_transparency, palette_uv, 0.0).r;" : "");
		body = body.replace("// LIGHTING_VERTEX_OUTPUT", use_face_center_lighting ? String(R"SHADER(
	// Optional stylized mode: evaluate direct and reflective lighting once from
	// the center of the visible voxel face. Visibility remains at the exact hit.
	vec3 lighting_voxel_position = vec3(hit_voxel) + vec3(0.5);
	if (hit_axis >= 0) {
		lighting_voxel_position[hit_axis] = float(hit_voxel[hit_axis]) + (local_normal[hit_axis] > 0.0 ? 1.0 : 0.0);
	}
	LIGHTING_VERTEX = (VIEW_MATRIX * MODEL_MATRIX * vec4(lighting_voxel_position * u_voxel_size, 1.0)).xyz;
)SHADER")
																				  : String());
		String voxel_forward_enable;
		if (use_voxel_forward_lighting) {
			voxel_forward_enable = "VOXEL_FACE_LIGHTING = true;";
		}
		if (use_voxel_forward_mask) {
			voxel_forward_enable += "\n\tVOXEL_OCCUPANCY_SHADOWS = true;";
		}
		body = body.replace("// VOXEL_OCCUPANCY_ENABLE", voxel_forward_enable);
		body = body.replace("// INDIRECT_LIGHT_OUTPUT", shading_mode == SHADING_MODE_PBR ? (use_voxel_forward_indirect ? String(R"SHADER(
	vec3 indirect_local_position = (vec3(hit_voxel) + vec3(0.5) + local_normal * 0.5) * u_voxel_size;
	vec3 indirect_world_normal = normalize(mat3(MODEL_MATRIX) * local_normal);
	vec3 indirect_world_position = (MODEL_MATRIX * vec4(indirect_local_position, 1.0)).xyz;
	// The lookup begins on the voxel face already. Only move it far enough to
	// resolve floating-point boundary ambiguity. Biasing by a fraction of the GI
	// cell size can move a sample meters away, through the opposite wall of a
	// corridor, and makes the selected irradiance jump as visible faces change.
	float indirect_surface_bias = max(length((MODEL_MATRIX * vec4(local_normal * u_voxel_size, 0.0)).xyz) * 0.02, 0.0001);
	vec3 indirect_sample_position = indirect_world_position + indirect_world_normal * indirect_surface_bias;
	float ddgi_visibility = 0.0;
	float ddgi_data_support = 0.0;
	float ddgi_selected_lod = -1.0;
	float ddgi_in_front = 0.0;
	float voxel_gi_coverage = 0.0;
	float voxel_gi_selected_cascade = -1.0;
	float voxel_gi_gather_visibility = 0.0;
	vec3 indirect_light = vec3(0.0);
	vec4 restir_sample = vec4(0.0);
	if (voxel_forward_indirect_backend == 2 && voxel_forward_ddgi_ready && voxel_forward_ddgi_resolve_ready && voxel_forward_ddgi_debug_mode != 15) {
		vec4 ddgi_resolved = textureLod(voxel_forward_ddgi_resolve, SCREEN_UV, 0.0);
		indirect_light = max(ddgi_resolved.rgb, vec3(0.0));
		ddgi_data_support = clamp(ddgi_resolved.a, 0.0, 1.0);
	} else if (voxel_forward_indirect_backend == 1 && voxel_forward_restir_ready) {
		// Resolve one canonical GI value for the complete voxel face. Sampling at
		// SCREEN_UV exposes the half-resolution stochastic reservoir pixels as
		// tiny squares within a voxel and lets them flicker independently.
		vec4 restir_clip = PROJECTION_MATRIX * VIEW_MATRIX * vec4(indirect_world_position, 1.0);
		// Keep the projection jitter: the ReSTIR buffer is generated from the same
		// jittered depth buffer, so removing it misaligns GI and geometry in motion.
		// Stable voxel-face random keys keep the aligned sample temporally coherent.
		vec2 restir_face_ndc = restir_clip.xy / max(restir_clip.w, 0.000001);
		vec2 restir_face_uv = restir_face_ndc * 0.5 + 0.5;
		vec2 restir_half_texel = 0.5 / vec2(textureSize(voxel_forward_restir_gi, 0));
		vec2 restir_edge_uv = clamp(restir_face_uv, restir_half_texel, vec2(1.0) - restir_half_texel);
		bool restir_face_center_in_front = restir_clip.w > 0.000001;
		vec4 fragment_restir_sample = textureLod(voxel_forward_restir_gi, SCREEN_UV, 0.0);
		// A partially visible face may have its center just outside the viewport.
		// Clamp to the nearest real texel so top/side-edge voxels do not switch
		// between canonical and independent fragment samples as the camera jitters.
		vec4 face_restir_sample = restir_face_center_in_front ? textureLod(voxel_forward_restir_gi, restir_edge_uv, 0.0) : vec4(0.0);
		// A projected face center can be hidden even while part of the face is
		// visible. In that case retain valid local GI instead of exposing an
		// invalid canonical reservoir as an ambient flash.
		restir_sample = face_restir_sample.a > 0.0 ? face_restir_sample : fragment_restir_sample;
		indirect_light = max(restir_sample.rgb, vec3(0.0));
		voxel_gi_coverage = clamp(restir_sample.a, 0.0, 1.0);
	}
	// A tint colors only the occluded portion. Fully open face corners remain
	// neutral, while black preserves the material's exact topology AO unchanged.
	vec3 globally_tinted_corner = voxel_ao_indirect_factor + (vec3(1.0) - voxel_ao_indirect_factor) * voxel_forward_corner_ao_tint.rgb;
	vec3 directional_corner_factor = mix(vec3(1.0), globally_tinted_corner, voxel_forward_corner_ao_strength);
	vec3 ambient_fallback = palette_color * voxel_forward_ambient_color.rgb * voxel_forward_ambient_energy * directional_corner_factor;
	// Missing DDGI data is unknown, not ambient-visible. Starting with uniform
	// ambient made enclosed surfaces light up first and darken only as depth-aware
	// probes matured. Keep the unknown contribution black so valid radiance fades
	// in through atlas confidence; edits retain their previous valid probe history.
	float ambient_fallback_weight = 0.0;
	vec3 fallback_contribution = vec3(0.0);
	if (voxel_forward_indirect_backend == 2 && (voxel_forward_ddgi_debug_mode == 0 || voxel_forward_ddgi_debug_mode == 11) && voxel_forward_ddgi_shadow_fill_strength > 0.0) {
		// This is receiver-shadow fill, not a global DDGI gain. Weight it by the
		// same directional occupancy mask used by direct voxel lighting so lit
		// surfaces retain their sampled result and soft shadow edges blend smoothly.
		float receiver_shadow_visibility = voxel_forward_shadow_ready ? textureLod(voxel_forward_shadow_mask, SCREEN_UV, 0.0).r : 1.0;
		float receiver_light_facing = voxel_forward_shadow_ready ? max(dot(indirect_world_normal, voxel_forward_shadow_light_direction), 0.0) : 1.0;
		// A face can receive no directional light because it is occluded or because
		// it faces away from the light. Both are direct-light deficits that indirect
		// illumination and the scene's artistic shadow fill are expected to soften.
		float receiver_shadow_weight = 1.0 - clamp(receiver_shadow_visibility * receiver_light_facing, 0.0, 1.0);
		// The scene controls how far the artistic fill may depart from sampled DDGI.
		// Reach 0 amplifies valid sampled radiance inside shadows; reach 1 supplies
		// an ambient-colored floor even while probe support is missing.
		vec3 shadow_lift_tint = max(voxel_forward_ddgi_shadow_fill_tint.rgb, vec3(0.0));
		vec3 sampled_lift = indirect_light * shadow_lift_tint * voxel_forward_ddgi_shadow_fill_strength;
		// indirect_light is irradiance and is divided by PI below. Convert the
		// designer's desired diffuse output floor into irradiance here so strength
		// 1.0 reaches the selected ambient color instead of only 1/PI of it.
		vec3 shadow_floor = max(voxel_forward_ambient_color.rgb * voxel_forward_ambient_energy * shadow_lift_tint * voxel_forward_ddgi_shadow_fill_strength * PI, vec3(0.0));
		float floor_luminance = dot(shadow_floor, vec3(0.2126, 0.7152, 0.0722));
		float indirect_luminance = dot(indirect_light, vec3(0.2126, 0.7152, 0.0722));
		float missing_floor = floor_luminance > 0.00001 ? clamp(1.0 - indirect_luminance / floor_luminance, 0.0, 1.0) : 0.0;
		vec3 artistic_floor = shadow_floor * missing_floor;
		indirect_light += receiver_shadow_weight * mix(sampled_lift, artistic_floor, voxel_forward_ddgi_shadow_fill_reach);
	}
	if (voxel_forward_indirect_backend == 2 && (voxel_forward_ddgi_debug_mode == 0 || voxel_forward_ddgi_debug_mode == 11)) {
		float indirect_luminance = dot(indirect_light, vec3(0.2126, 0.7152, 0.0722));
		indirect_light = max(mix(vec3(indirect_luminance), indirect_light, voxel_forward_ddgi_color_saturation), vec3(0.0));
	}
	vec3 indirect_output_light = indirect_light * (voxel_forward_indirect_intensity / PI);
	if (voxel_forward_toon_enabled && voxel_forward_indirect_backend == 2 && (voxel_forward_ddgi_debug_mode == 0 || voxel_forward_ddgi_debug_mode == 11) && voxel_forward_ddgi_toon_band_count >= 2) {
		// Quantize only the final indirect contribution. Probe history remains
		// continuous, so toon styling cannot feed back into multibounce convergence.
		float output_luminance = dot(indirect_output_light, vec3(0.2126, 0.7152, 0.0722));
		float band_range = max(voxel_forward_ddgi_toon_band_range, 0.01);
		float band_steps = float(voxel_forward_ddgi_toon_band_count - 1);
		float band_coordinate = clamp(output_luminance / band_range, 0.0, 1.0) * band_steps;
		float lower_band = floor(band_coordinate);
		float transition_width = max(fwidth(band_coordinate), max(voxel_forward_ddgi_toon_band_softness * 0.5, 0.0001));
		float upper_band_weight = smoothstep(0.5 - transition_width, 0.5 + transition_width, fract(band_coordinate));
		float band_luminance = ((lower_band + upper_band_weight) / band_steps) * band_range;
		// Preserve HDR energy above the selected range instead of turning this
		// style control into an accidental radiance clamp.
		band_luminance += max(output_luminance - band_range, 0.0);
		indirect_output_light *= band_luminance / max(output_luminance, 0.00001);
	}
	// Confidence owns the crossfade: DDGI and fallback sum to one source rather
	// than adding full irradiance while partially retaining constant ambient.
	// The gather has already normalized irradiance using the matching visible
	// weights. Multiplying by the raw weight sum here exposed the probe lattice.
	vec3 indirect_contribution = palette_color * indirect_output_light * directional_corner_factor;
	if (voxel_forward_indirect_backend == 1 && voxel_forward_indirect_debug_mode >= 1 && voxel_forward_indirect_debug_mode <= 5) {
		ALBEDO = vec3(0.0);
		EMISSION = indirect_light;
	} else if (voxel_forward_indirect_backend == 1 && voxel_forward_indirect_debug_mode == 1) {
		ALBEDO = vec3(0.0);
		vec3 cascade_colors[3] = vec3[3](vec3(0.05, 0.9, 1.0), vec3(0.1, 1.0, 0.25), vec3(1.0, 0.5, 0.05));
		int cascade_index = int(clamp(round(voxel_gi_selected_cascade), 0.0, 2.0));
		EMISSION = cascade_colors[cascade_index] * voxel_gi_coverage;
	} else if (voxel_forward_indirect_backend == 1 && (voxel_forward_indirect_debug_mode == 2 || voxel_forward_indirect_debug_mode == 13)) {
		ALBEDO = vec3(0.0);
		bool dirty =
			all(greaterThanEqual(indirect_world_position, voxel_forward_indirect_dirty_min0)) && all(lessThan(indirect_world_position, voxel_forward_indirect_dirty_max0)) ||
			all(greaterThanEqual(indirect_world_position, voxel_forward_indirect_dirty_min1)) && all(lessThan(indirect_world_position, voxel_forward_indirect_dirty_max1)) ||
			all(greaterThanEqual(indirect_world_position, voxel_forward_indirect_dirty_min2)) && all(lessThan(indirect_world_position, voxel_forward_indirect_dirty_max2));
		EMISSION = dirty ? vec3(1.0, 0.22, 0.02) : (voxel_forward_indirect_debug_mode == 13 ? vec3(0.02, 0.35, 0.08) : vec3(0.0));
	} else if (voxel_forward_indirect_backend == 1 && (voxel_forward_indirect_debug_mode == 3 || voxel_forward_indirect_debug_mode == 4)) {
		ALBEDO = vec3(0.0);
		float transmittance_sum = 0.0;
		for (int lobe = 0; lobe < 6; lobe++) transmittance_sum += voxel_gi_debug_value(indirect_world_position, voxel_gi_selected_cascade, lobe).a;
		float mean_transmittance = transmittance_sum / 6.0;
		EMISSION = voxel_forward_indirect_debug_mode == 3 ? mix(vec3(0.9, 0.03, 0.01), vec3(0.02, 0.3, 0.05), step(0.001, transmittance_sum)) : vec3(mean_transmittance);
	} else if (voxel_forward_indirect_backend == 1 && voxel_forward_indirect_debug_mode >= 5 && voxel_forward_indirect_debug_mode <= 10) {
		ALBEDO = vec3(0.0);
		vec3 debug_normals[6] = vec3[6](vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0));
		float debug_coverage;
		float debug_cascade;
		float debug_gather_visibility;
		vec3 debug_normal = debug_normals[voxel_forward_indirect_debug_mode - 5];
		// Match the production voxel-scale surface bias. A GI-cell-scale bias can
		// cross an entire narrow corridor and visualize an unrelated cell.
		EMISSION = sample_voxel_forward_indirect(indirect_world_position + debug_normal * indirect_surface_bias, debug_normal, debug_coverage, debug_cascade, debug_gather_visibility) * voxel_forward_indirect_intensity;
	} else if (voxel_forward_indirect_backend == 1 && voxel_forward_indirect_debug_mode == 11) {
		ALBEDO = vec3(0.0);
		EMISSION = indirect_light * voxel_forward_indirect_intensity;
	} else if (voxel_forward_indirect_backend == 1 && voxel_forward_indirect_debug_mode == 12) {
		ALBEDO = vec3(0.0);
		EMISSION = voxel_ao_indirect_factor;
	} else if (voxel_forward_indirect_backend == 1 && voxel_forward_indirect_debug_mode == 14) {
		ALBEDO = vec3(0.0);
		EMISSION = mix(vec3(0.95, 0.03, 0.02), vec3(0.02, 0.85, 0.15), voxel_gi_gather_visibility);
	} else if (voxel_forward_indirect_backend == 1 && voxel_forward_indirect_debug_mode == 15) {
		// Show exactly what the normal material path adds after albedo, 1/PI,
		// corner occlusion, and missing-data fallback. Unlike raw mode 11 this
		// does not make ordinary HDR irradiance look like saturated white light.
		ALBEDO = vec3(0.0);
		EMISSION = fallback_contribution + indirect_contribution;
	} else if (voxel_forward_indirect_backend == 1 && voxel_forward_indirect_debug_mode == 16) {
		ALBEDO = vec3(0.0);
		EMISSION = vec3(ambient_fallback_weight);
	} else if (voxel_forward_indirect_backend == 2 && voxel_forward_ddgi_debug_mode == 11) {
		ALBEDO = vec3(0.0);
		EMISSION = indirect_contribution;
	} else if (voxel_forward_indirect_backend == 2 && voxel_forward_ddgi_debug_mode == 12) {
		ALBEDO = vec3(0.0);
		EMISSION = fallback_contribution;
	} else if (voxel_forward_indirect_backend == 2 && voxel_forward_ddgi_debug_mode == 13) {
		ALBEDO = vec3(0.0);
		EMISSION = indirect_light;
	} else if (voxel_forward_indirect_backend == 2 && voxel_forward_ddgi_debug_mode == 14) {
		ALBEDO = vec3(0.0);
		EMISSION = indirect_light;
	} else if (voxel_forward_indirect_backend == 2 && voxel_forward_ddgi_debug_mode == 15) {
		// Direct-light-only diagnostic. Medium disables reflections, so no other
		// material-space emission remains in the reference test configuration.
		EMISSION = vec3(0.0);
	} else if (voxel_forward_indirect_backend == 2 && voxel_forward_ddgi_debug_mode == 16) {
		ALBEDO = vec3(0.0);
		EMISSION = indirect_light;
	} else if (voxel_forward_indirect_backend == 2 && voxel_forward_ddgi_debug_mode == 17) {
		ALBEDO = vec3(0.0);
		EMISSION = indirect_light;
	} else if (voxel_forward_indirect_backend == 2 && voxel_forward_ddgi_debug_mode == 18) {
		ALBEDO = vec3(0.0);
		EMISSION = indirect_light;
	} else {
		EMISSION += fallback_contribution + indirect_contribution;
	}
)SHADER")
																													   : String(R"SHADER(
	// Legacy/no-DDGI fallback. DDGI materials instead blend this term per pixel
	// using initialized probe coverage so it cannot illuminate through walls.
	EMISSION += palette_color * voxel_forward_ambient_color.rgb * voxel_forward_ambient_energy * voxel_ao_indirect_factor;
)SHADER"))
																						 : String());
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
)SHADER")
																				 : String());
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
		set_shader_parameter("ambient_occlusion_direct_light_influence", ambient_occlusion_direct_light_influence);
		if (use_ao_tint) {
			set_shader_parameter("ambient_occlusion_tint_strength", ambient_occlusion_tint_strength);
		}
		if (use_ao_tint_palette) {
			set_shader_parameter("u_ambient_occlusion_tint_palette", ambient_occlusion_tint_palette_texture);
		}
		if (ambient_occlusion_mode == AMBIENT_OCCLUSION_MODE_VOXELIZED) {
			set_shader_parameter("ambient_occlusion_voxelized_curve", _make_voxelized_ao_curve(ambient_occlusion_hardness, ambient_occlusion_strength));
		} else {
			set_shader_parameter("ambient_occlusion_strength", ambient_occlusion_strength);
			set_shader_parameter("ambient_occlusion_hardness", ambient_occlusion_hardness);
		}
	}
	if (use_material_outline) {
		set_shader_parameter("outline_color", outline_color);
		set_shader_parameter("outline_width", outline_width);
	}
}

void VoxelMaterial::set_shading_mode(ShadingMode p_mode) {
	ERR_FAIL_INDEX(p_mode, 2);
	if (shading_mode == p_mode) {
		return;
	}
	shading_mode = p_mode;
	if (get_shader().is_valid()) {
		_rebuild_shader();
	}
	emit_changed();
}

VoxelMaterial::ShadingMode VoxelMaterial::get_shading_mode() const {
	return shading_mode;
}

void VoxelMaterial::set_lighting_position_mode(LightingPositionMode p_mode) {
	ERR_FAIL_INDEX(p_mode, 2);
	if (lighting_position_mode == p_mode) {
		return;
	}
	lighting_position_mode = p_mode;
	if (get_shader().is_valid()) {
		_rebuild_shader();
	}
	emit_changed();
}

VoxelMaterial::LightingPositionMode VoxelMaterial::get_lighting_position_mode() const {
	return lighting_position_mode;
}

void VoxelMaterial::set_emission_energy(real_t p_energy) {
	emission_energy = MAX(p_energy, real_t(0.0));
	if (get_shader().is_valid()) {
		set_shader_parameter("emission_energy", emission_energy);
	}
	emit_changed();
}

real_t VoxelMaterial::get_emission_energy() const {
	return emission_energy;
}

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

bool VoxelMaterial::is_ambient_occlusion_enabled() const {
	return ambient_occlusion_enabled;
}

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

Color VoxelMaterial::get_ambient_occlusion_color() const {
	return ambient_occlusion_color;
}

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

real_t VoxelMaterial::get_ambient_occlusion_strength() const {
	return ambient_occlusion_strength;
}

void VoxelMaterial::set_ambient_occlusion_intensity(real_t p_intensity) {
	set_ambient_occlusion_strength(p_intensity);
}

real_t VoxelMaterial::get_ambient_occlusion_intensity() const {
	return get_ambient_occlusion_strength();
}

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

real_t VoxelMaterial::get_ambient_occlusion_hardness() const {
	return ambient_occlusion_hardness;
}

void VoxelMaterial::set_ambient_occlusion_contrast(real_t p_contrast) {
	set_ambient_occlusion_hardness(p_contrast);
}

real_t VoxelMaterial::get_ambient_occlusion_contrast() const {
	return get_ambient_occlusion_hardness();
}

void VoxelMaterial::set_ambient_occlusion_direct_light_influence(real_t p_influence) {
	p_influence = CLAMP(p_influence, real_t(0.0), real_t(1.0));
	if (Math::is_equal_approx(ambient_occlusion_direct_light_influence, p_influence)) {
		return;
	}
	ambient_occlusion_direct_light_influence = p_influence;
	if (get_shader().is_valid() && ambient_occlusion_enabled) {
		set_shader_parameter("ambient_occlusion_direct_light_influence", ambient_occlusion_direct_light_influence);
	}
	_emit_ambient_occlusion_changed();
}

real_t VoxelMaterial::get_ambient_occlusion_direct_light_influence() const {
	return ambient_occlusion_direct_light_influence;
}

void VoxelMaterial::set_ambient_occlusion_tint_enabled(bool p_enabled) {
	if (ambient_occlusion_tint_enabled == p_enabled) {
		return;
	}
	ambient_occlusion_tint_enabled = p_enabled;
	if (get_shader().is_valid() && ambient_occlusion_enabled) {
		_rebuild_shader();
	}
	_emit_ambient_occlusion_changed();
}

bool VoxelMaterial::is_ambient_occlusion_tint_enabled() const {
	return ambient_occlusion_tint_enabled;
}

void VoxelMaterial::set_ambient_occlusion_tint_strength(real_t p_strength) {
	p_strength = CLAMP(p_strength, real_t(0.0), real_t(1.0));
	if (Math::is_equal_approx(ambient_occlusion_tint_strength, p_strength)) {
		return;
	}
	ambient_occlusion_tint_strength = p_strength;
	if (get_shader().is_valid() && ambient_occlusion_enabled && ambient_occlusion_tint_enabled) {
		set_shader_parameter("ambient_occlusion_tint_strength", ambient_occlusion_tint_strength);
	}
	_emit_ambient_occlusion_changed();
}

real_t VoxelMaterial::get_ambient_occlusion_tint_strength() const {
	return ambient_occlusion_tint_strength;
}

void VoxelMaterial::set_ambient_occlusion_tint_palette_texture(const Ref<Texture2D> &p_texture) {
	if (ambient_occlusion_tint_palette_texture == p_texture) {
		return;
	}
	const bool variant_changed = ambient_occlusion_tint_palette_texture.is_valid() != p_texture.is_valid();
	ambient_occlusion_tint_palette_texture = p_texture;
	if (get_shader().is_valid() && ambient_occlusion_enabled && ambient_occlusion_tint_enabled) {
		if (variant_changed) {
			_rebuild_shader();
		} else if (ambient_occlusion_tint_palette_texture.is_valid()) {
			set_shader_parameter("u_ambient_occlusion_tint_palette", ambient_occlusion_tint_palette_texture);
		}
	}
	_emit_ambient_occlusion_changed();
}

Ref<Texture2D> VoxelMaterial::get_ambient_occlusion_tint_palette_texture() const {
	return ambient_occlusion_tint_palette_texture;
}

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

VoxelMaterial::AmbientOcclusionMode VoxelMaterial::get_ambient_occlusion_mode() const {
	return ambient_occlusion_mode;
}

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

VoxelMaterial::AmbientOcclusionFaceMode VoxelMaterial::get_ambient_occlusion_face_mode() const {
	return ambient_occlusion_face_mode;
}

bool VoxelMaterial::is_ambient_occlusion_change_in_progress() const {
	return ambient_occlusion_change_in_progress;
}

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

Color VoxelMaterial::get_albedo_modulate() const {
	return albedo_modulate;
}

#define VOXEL_MATERIAL_TEXTURE_ACCESSORS(m_name) \
	void VoxelMaterial::set_##m_name(const Ref<Texture2D> &p_texture) { \
		if (m_name == p_texture) { \
			return; \
		} \
		m_name = p_texture; \
		emit_changed(); \
	} \
	Ref<Texture2D> VoxelMaterial::get_##m_name() const { \
		return m_name; \
	}

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

real_t VoxelMaterial::get_roughness_multiplier() const {
	return roughness_multiplier;
}

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

real_t VoxelMaterial::get_metallic_multiplier() const {
	return metallic_multiplier;
}

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

real_t VoxelMaterial::get_specularity_multiplier() const {
	return specularity_multiplier;
}

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

bool VoxelMaterial::is_outline_enabled() const {
	return outline_enabled;
}

void VoxelMaterial::set_outline_color(const Color &p_color) {
	if (outline_color == p_color) {
		return;
	}
	outline_color = p_color;
	const bool post_process_outline = RenderingMethod::is_current_voxel_forward_method() &&
			bool(GLOBAL_GET("rendering/voxel_forward/architectural_hit_buffer/enabled"));
	if (get_shader().is_valid() && !post_process_outline) {
		set_shader_parameter("outline_color", outline_color);
	}
	emit_changed();
}

Color VoxelMaterial::get_outline_color() const {
	return outline_color;
}

void VoxelMaterial::set_outline_width(real_t p_width) {
	p_width = CLAMP(p_width, real_t(0.25), real_t(4.0));
	if (Math::is_equal_approx(outline_width, p_width)) {
		return;
	}
	outline_width = p_width;
	const bool post_process_outline = RenderingMethod::is_current_voxel_forward_method() &&
			bool(GLOBAL_GET("rendering/voxel_forward/architectural_hit_buffer/enabled"));
	if (get_shader().is_valid() && !post_process_outline) {
		set_shader_parameter("outline_width", outline_width);
	}
	emit_changed();
}

real_t VoxelMaterial::get_outline_width() const {
	return outline_width;
}

void VoxelMaterial::set_transparency_enabled(bool p_enabled) {
	if (transparency_enabled == p_enabled) {
		return;
	}
	transparency_enabled = p_enabled;
	if (get_shader().is_valid()) {
		_rebuild_shader();
	}
}

bool VoxelMaterial::is_transparency_enabled() const {
	return transparency_enabled;
}

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

void VoxelMaterial::set_batched_resources_enabled(bool p_enabled) {
	if (batched_resources_enabled == p_enabled) {
		return;
	}
	batched_resources_enabled = p_enabled;
	// Like the other feature setters, configure new materials before compiling.
	// ensure_shader() builds the final combination after texture features are set.
	if (get_shader().is_valid()) {
		_rebuild_shader();
	}
}

bool VoxelMaterial::is_batched_resources_enabled() const {
	return batched_resources_enabled;
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
