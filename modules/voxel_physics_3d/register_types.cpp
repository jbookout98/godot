/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "godot_physics_server_3d.h"
#include "voxel_lighting_bake_3d.h"
#include "voxel_material.h"
#include "voxel_shape_3d.h"
#include "voxel_shape_data.h"
#include "voxel_volume_3d.h"
#include "voxel_volume_streaming_manager.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/physics_3d/physics_server_3d_wrap_mt.h"

#ifdef TOOLS_ENABLED
#include "editor/voxel_lighting_bake_editor_plugin.h"
#include "editor/voxel_volume_3d_editor_plugin.h"
#endif

static PhysicsServer3D *_create_voxel_physics_3d_callback() {
#ifdef THREADS_ENABLED
	bool using_threads = GLOBAL_GET("physics/3d/run_on_separate_thread");
#else
	bool using_threads = false;
#endif

	PhysicsServer3D *physics_server_3d = memnew(GodotPhysicsServer3D(using_threads));

	return memnew(PhysicsServer3DWrapMT(physics_server_3d, using_threads));
}

static VoxelVolumeStreamingManager *voxel_volume_streaming_manager = nullptr;
static Ref<ResourceFormatLoaderVoxelLighting> voxel_lighting_loader;
static Ref<ResourceFormatSaverVoxelLighting> voxel_lighting_saver;

void initialize_voxel_physics_3d_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		// This is consumed while the Voxel Forward renderer is constructed, before
		// scene-level voxel material settings are registered.
		GLOBAL_DEF("rendering/voxel_forward/architectural_hit_buffer/exact_position_enabled", true);
		GLOBAL_DEF("rendering/voxel_forward/architectural_hit_buffer/precomputed_inverse_enabled", true);
		PhysicsServer3DManager::get_singleton()->register_server("VoxelPhysics3D", callable_mp_static(_create_voxel_physics_3d_callback));
	}

	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GLOBAL_DEF("rendering/voxel_volume/max_resident_volumes", 256);
		GLOBAL_DEF("rendering/voxel_volume/max_loads_per_frame", 8);
		GLOBAL_DEF("rendering/voxel_volume/streaming_distance", 512.0);
		GLOBAL_DEF("rendering/voxel_volume/streaming_hysteresis", 8.0);
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_volume/streaming_hysteresis", PROPERTY_HINT_RANGE, "0,256,0.5,or_greater,suffix:m"));
		// The native RD visibility pass is deliberately opt-in until its output
		// matches the established DDA path for multi-volume boundaries.
		GLOBAL_DEF("rendering/voxel_forward/experimental_custom_visibility", false);
		// Run exact voxel traversal in the depth prepass and reuse its compact
		// owner/face payload from the materially heavier opaque color pass.
		GLOBAL_DEF("rendering/voxel_forward/architectural_hit_buffer/enabled", true);
		// Screen-space silhouettes and face/depth creases are composited after the
		// opaque pass. Per-material color and width remain on VoxelMaterial and are
		// carried by the existing instance buffer.
		GLOBAL_DEF("rendering/voxel_forward/outline/enabled", true);
		GLOBAL_DEF("rendering/voxel_forward/outline/depth_threshold", 0.00005);
		GLOBAL_DEF("rendering/voxel_forward/outline/planar_depth_tolerance", 2.0);
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/outline/depth_threshold", PROPERTY_HINT_RANGE, "0,0.01,0.00001,or_greater"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/outline/planar_depth_tolerance", PROPERTY_HINT_RANGE, "0.5,8,0.25,or_greater"));
		// Voxel Forward consumes this binary mask during directional lighting,
		// replacing per-volume draws in conventional shadow cascades.
		// Experimental until the compute budget and renderer-wide receiver path
		// have been validated. Disabled means stock VoxelMaterial lighting and
		// conventional shadow casting remain untouched.
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/enabled", false);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/debug_view", false);
		// The canonical face buffer is full resolution. Full-resolution shadow
		// receivers avoid assigning one binary result to adjacent stair faces.
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/resolution_scale", 1.0);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/max_distance", 112.0);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/max_steps", 256);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/atlas_resolution", 512);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/atlas_near_extent_ratio", 0.25);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/atlas_recenter_ratio", 0.25);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/incremental_atlas_updates", true);
		// Reliable world-edit rectangles are independent of camera atlas scrolling.
		// This remains useful when scrolling is disabled because a one-brick edit
		// should not require rebuilding every shadow texel.
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/dirty_region_updates_enabled", true);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/temporal_rebuild_enabled", true);
		// A 512x512 two-cascade atlas contains 524,288 texels. A 1,024-texel
		// budget took 512 frames to publish its first complete atlas, which made
		// otherwise bounded initialization appear broken. 4,096 keeps the work
		// explicitly bounded while reducing default convergence to 128 frames.
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/rebuild_texel_budget", 4096);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/force_full_rebuild", false);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/atlas_bias_voxels", 0.125);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/atlas_slope_bias", 1.0);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/atlas_bias_clamp_voxels", 0.75);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/max_local_lights", 4);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/soft_shadow_mode", 0);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/soft_shadow_samples", 8);
		// Local-light softness is intentionally independent. Each local sample is
		// an occupancy ray, while directional samples are inexpensive atlas taps.
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/local_soft_shadow_samples", 1);
		const String legacy_soft_radius_setting = "rendering/voxel_forward/shadow_mask/soft_shadow_radius_scale";
		const String voxel_soft_radius_setting = "rendering/voxel_forward/shadow_mask/soft_shadow_radius_voxels";
		const bool migrate_legacy_soft_radius = ProjectSettings::get_singleton()->has_setting(legacy_soft_radius_setting) &&
				!ProjectSettings::get_singleton()->has_setting(voxel_soft_radius_setting);
		const Variant legacy_soft_radius = migrate_legacy_soft_radius ? ProjectSettings::get_singleton()->get_setting(legacy_soft_radius_setting) : Variant();
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/soft_shadow_radius_voxels", 2.0);
		if (migrate_legacy_soft_radius) {
			ProjectSettings::get_singleton()->set_setting(voxel_soft_radius_setting, legacy_soft_radius);
			ProjectSettings::get_singleton()->set_setting(legacy_soft_radius_setting, Variant());
		}
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/shadow_mask/atlas_resolution", PROPERTY_HINT_RANGE, "128,2048,128"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/shadow_mask/atlas_near_extent_ratio", PROPERTY_HINT_RANGE, "0.05,0.75,0.05"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/shadow_mask/atlas_recenter_ratio", PROPERTY_HINT_RANGE, "0.05,0.75,0.05"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/shadow_mask/rebuild_texel_budget", PROPERTY_HINT_RANGE, "64,4194304,64,or_greater"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/shadow_mask/atlas_bias_voxels", PROPERTY_HINT_RANGE, "0,4,0.01"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/shadow_mask/atlas_slope_bias", PROPERTY_HINT_RANGE, "0,8,0.05"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/shadow_mask/atlas_bias_clamp_voxels", PROPERTY_HINT_RANGE, "0,8,0.05"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/shadow_mask/soft_shadow_mode", PROPERTY_HINT_ENUM, "Hard,Voxel Soft"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/shadow_mask/soft_shadow_samples", PROPERTY_HINT_ENUM, "1 Sample:1,4 Samples:4,8 Samples:8"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/shadow_mask/local_soft_shadow_samples", PROPERTY_HINT_RANGE, "1,8,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/shadow_mask/soft_shadow_radius_voxels", PROPERTY_HINT_RANGE, "0,32,0.25,or_greater"));
		GLOBAL_DEF("rendering/voxel_forward/world_occupancy/incremental_bricks_per_frame", 512);
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/world_occupancy/incremental_bricks_per_frame", PROPERTY_HINT_RANGE, "64,8192,64"));
		// World-snapped near, mid, and distant irradiance cascades reuse the sparse
		// occupancy table and light-space shadow atlas. Camera motion only updates
		// a cascade when it crosses that cascade's independently snapped boundary.
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/enabled", true);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/backend", 2);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/intensity", 1.0);
		// These belonged to the retired voxel-propagation backend and were
		// historically exposed beside settings shared by every backend. Neither
		// ReSTIR GI nor DDGI reads them. Remove stale project overrides as well as
		// their defaults so Project Settings only presents active backend controls.
		const char *retired_voxel_gi_settings[] = {
			"rendering/voxel_forward/indirect_light/resolution",
			"rendering/voxel_forward/indirect_light/near_cell_size",
			"rendering/voxel_forward/indirect_light/far_cell_size",
			"rendering/voxel_forward/indirect_light/distant_cell_size",
			"rendering/voxel_forward/indirect_light/recenter_cells",
			"rendering/voxel_forward/indirect_light/recenter_hysteresis",
			"rendering/voxel_forward/indirect_light/transition_cells",
			"rendering/voxel_forward/indirect_light/propagation_steps",
			"rendering/voxel_forward/indirect_light/propagation_decay",
			"rendering/voxel_forward/indirect_light/invalidation_epsilon",
			"rendering/voxel_forward/indirect_light/shadow_bias_voxels",
			"rendering/voxel_forward/indirect_light/dirty_updates_enabled",
			"rendering/voxel_forward/indirect_light/low_latency_dirty_updates_enabled",
			"rendering/voxel_forward/indirect_light/dirty_cell_pass_budget_per_frame",
			"rendering/voxel_forward/indirect_light/temporal_updates_enabled",
			"rendering/voxel_forward/indirect_light/dispatch_budget_per_frame",
			"rendering/voxel_forward/indirect_light/temporal_blend_enabled",
			"rendering/voxel_forward/indirect_light/blend_dispatch_budget_per_frame",
			"rendering/voxel_forward/indirect_light/temporal_blend_frames",
		};
		for (const char *setting : retired_voxel_gi_settings) {
			if (ProjectSettings::get_singleton()->has_setting(setting)) {
				ProjectSettings::get_singleton()->set_setting(setting, Variant());
			}
		}
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/voxel_gi/debug_mode", 0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/voxel_gi/corner_occlusion_strength", 1.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/voxel_gi/corner_occlusion_tint", Color(0.0, 0.0, 0.0));
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/resolution_scale", 0.25);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/bounce_count", 1);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/spatial_neighbors", 4);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/spatial_radius", 16);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/max_temporal_samples", 30);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/max_spatial_samples", 500);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/denoise_history_frames", 30);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/max_distance", 96.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/max_trace_steps", 512);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/validation_interval", 6);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/radiance_tolerance", 0.25);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/radiance_clamp", 64.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/bias_mode", 0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/normal_tolerance_degrees", 25.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/depth_tolerance_voxels", 1.5);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/camera_cut_distance", 8.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/camera_cut_angle_degrees", 45.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/restir/debug_mode", 0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/spatial_cascade_resolution", 16);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/lod0_spacing_voxels", 8);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/lod1_spacing_voxels", 16);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/lod2_spacing_voxels", 32);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/lod3_spacing_voxels", 64);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/rays_per_probe", 16);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/convergence_rays_per_probe", 64);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/convergence_updates", 5);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/dirty_target_residual", 0.05);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/probes_per_frame", 0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/urgent_probes_per_frame", 256);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/camera_probes_per_frame", 128);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/lighting_probes_per_frame", 256);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/publication_fade_seconds", 0.35);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/local_lights_enabled", true);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/max_local_lights", 8);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/irradiance_hysteresis", 0.98);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/visibility_hysteresis", 0.985);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/bounce_feedback", 0.35);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/self_shadow_bias", 0.3);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/view_bias", 0.8);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/lod_transition", 0.15);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/max_trace_steps", 512);
		// Scene lighting owns these artistic terms. Their neutral defaults keep
		// DDGI physically sampled until a WorldEnvironment explicitly opts in.
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/shadow_fill_strength", 0.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/shadow_fill_tint", Color(1.0, 1.0, 1.0));
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/shadow_fill_reach", 0.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/color_saturation", 1.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/toon_enabled", false);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/toon_band_count", 0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/toon_band_softness", 0.05);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/toon_band_range", 1.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/toon_specular_enabled", false);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/toon_specular_threshold", 0.55);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/toon_specular_softness", 0.04);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/toon_specular_strength", 1.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/debug_mode", 0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/debug_lod", -1);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/debug_marker_size", 4.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/ddgi/debug_depth_test", true);
		GLOBAL_DEF("rendering/voxel_forward/ambient_light/color", Color(0.22, 0.22, 0.22));
		GLOBAL_DEF("rendering/voxel_forward/ambient_light/energy", 1.0);
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/backend", PROPERTY_HINT_ENUM, "Off:0,ReSTIR GI:1,DDGI:2"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/intensity", PROPERTY_HINT_RANGE, "0,8,0.05,or_greater"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/voxel_gi/debug_mode", PROPERTY_HINT_ENUM, "Disabled:0,Cascade Ownership:1,Dirty Regions:2,Cell Occupancy:3,Shared-Face Transmittance:4,+X Radiance:5,-X Radiance:6,+Y Radiance:7,-Y Radiance:8,+Z Radiance:9,-Z Radiance:10,Raw Directional Irradiance (HDR):11,Corner Occlusion:12,Active vs Staging:13,Gather Visibility:14,Shaded Indirect Contribution:15,Ambient Fallback Weight:16"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/voxel_gi/corner_occlusion_strength", PROPERTY_HINT_RANGE, "0,1,0.01"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::COLOR, "rendering/voxel_forward/indirect_light/voxel_gi/corner_occlusion_tint", PROPERTY_HINT_COLOR_NO_ALPHA));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/restir/resolution_scale", PROPERTY_HINT_RANGE, "0.25,1,0.05"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/restir/bounce_count", PROPERTY_HINT_RANGE, "1,1,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/restir/spatial_neighbors", PROPERTY_HINT_RANGE, "0,32,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/restir/spatial_radius", PROPERTY_HINT_RANGE, "1,128,1,suffix:px"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/restir/max_temporal_samples", PROPERTY_HINT_RANGE, "1,128,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/restir/max_spatial_samples", PROPERTY_HINT_RANGE, "1,4096,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/restir/denoise_history_frames", PROPERTY_HINT_RANGE, "1,64,1,suffix:frames"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/restir/max_distance", PROPERTY_HINT_RANGE, "1,512,1,or_greater,suffix:m"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/restir/max_trace_steps", PROPERTY_HINT_RANGE, "16,4096,16"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/restir/validation_interval", PROPERTY_HINT_RANGE, "1,120,1,suffix:frames"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/restir/radiance_tolerance", PROPERTY_HINT_RANGE, "0,10,0.01"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/restir/radiance_clamp", PROPERTY_HINT_RANGE, "1,1024,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/restir/bias_mode", PROPERTY_HINT_ENUM, "Low Bias:0,Fast Biased:1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/restir/normal_tolerance_degrees", PROPERTY_HINT_RANGE, "0,89,1,suffix:degrees"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/restir/depth_tolerance_voxels", PROPERTY_HINT_RANGE, "0.01,8,0.01,suffix:voxels"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/restir/camera_cut_distance", PROPERTY_HINT_RANGE, "0.1,512,0.1,suffix:m"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/restir/camera_cut_angle_degrees", PROPERTY_HINT_RANGE, "1,180,1,suffix:degrees"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/restir/debug_mode", PROPERTY_HINT_ENUM, "Disabled:0,Selected Radiance:1,Reservoir Samples:2,Estimator Weight:3,Temporal Age:4,Spatial Acceptance:5"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/spatial_cascade_resolution", PROPERTY_HINT_RANGE, "8,32,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/lod0_spacing_voxels", PROPERTY_HINT_RANGE, "4,256,1,suffix:voxels"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/lod1_spacing_voxels", PROPERTY_HINT_RANGE, "4,256,1,suffix:voxels"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/lod2_spacing_voxels", PROPERTY_HINT_RANGE, "4,256,1,suffix:voxels"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/lod3_spacing_voxels", PROPERTY_HINT_RANGE, "4,256,1,suffix:voxels"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/rays_per_probe", PROPERTY_HINT_RANGE, "8,64,8"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/convergence_rays_per_probe", PROPERTY_HINT_RANGE, "8,64,8"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/convergence_updates", PROPERTY_HINT_RANGE, "5,32,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/dirty_target_residual", PROPERTY_HINT_RANGE, "0.001,0.5,0.001"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/shadow_fill_strength", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::COLOR, "rendering/voxel_forward/indirect_light/ddgi/shadow_fill_tint", PROPERTY_HINT_COLOR_NO_ALPHA));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/shadow_fill_reach", PROPERTY_HINT_RANGE, "0,1,0.01"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/color_saturation", PROPERTY_HINT_RANGE, "0,2,0.01"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::BOOL, "rendering/voxel_forward/indirect_light/ddgi/toon_enabled"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/toon_band_count", PROPERTY_HINT_RANGE, "0,16,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/toon_band_softness", PROPERTY_HINT_RANGE, "0,1,0.01"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/toon_band_range", PROPERTY_HINT_RANGE, "0.01,8,0.01,or_greater"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::BOOL, "rendering/voxel_forward/indirect_light/ddgi/toon_specular_enabled"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/toon_specular_threshold", PROPERTY_HINT_RANGE, "0,1,0.01"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/toon_specular_softness", PROPERTY_HINT_RANGE, "0,1,0.01"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/toon_specular_strength", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/probes_per_frame", PROPERTY_HINT_RANGE, "0,512,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/urgent_probes_per_frame", PROPERTY_HINT_RANGE, "1,512,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/camera_probes_per_frame", PROPERTY_HINT_RANGE, "1,512,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/lighting_probes_per_frame", PROPERTY_HINT_RANGE, "1,512,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/publication_fade_seconds", PROPERTY_HINT_RANGE, "0.02,2,0.01,suffix:s"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/max_local_lights", PROPERTY_HINT_RANGE, "0,32,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/irradiance_hysteresis", PROPERTY_HINT_RANGE, "0,0.99,0.01"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/visibility_hysteresis", PROPERTY_HINT_RANGE, "0,0.999,0.001"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/bounce_feedback", PROPERTY_HINT_RANGE, "0,0.95,0.05"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/self_shadow_bias", PROPERTY_HINT_RANGE, "0,2,0.05"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/view_bias", PROPERTY_HINT_RANGE, "0,1,0.05"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/max_trace_steps", PROPERTY_HINT_RANGE, "32,4096,32"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/debug_mode", PROPERTY_HINT_ENUM, "Disabled:0,Baked Probe Positions:1,Logical vs Physical:2,Resident Probe State:3,Active Worklist:4,Dirty Probes:5,Probe LOD:6,Irradiance:7,Mean Depth:8,Depth Variance:9,LOD Transition:10,DDGI Contribution:11,Ambient Fallback:12,Final Visibility:13,LOD Blend Weights:14,Direct Light Only:15,Temporal Confidence:16,In-Front Weight:17,Depth Visibility:18"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/ddgi/debug_lod", PROPERTY_HINT_ENUM, "All:-1,LOD 0:0,LOD 1:1,LOD 2:2,LOD 3:3"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/debug_marker_size", PROPERTY_HINT_RANGE, "1,32,1,suffix:px"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/ddgi/lod_transition", PROPERTY_HINT_RANGE, "0.01,0.5,0.01"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::COLOR, "rendering/voxel_forward/ambient_light/color", PROPERTY_HINT_COLOR_NO_ALPHA));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/ambient_light/energy", PROPERTY_HINT_RANGE, "0,8,0.05,or_greater"));
		// Mirror rays are traced through the same sparse world occupancy used by
		// voxel shadows. A live GPU color clipmap supplies hit albedo without
		// duplicating destructible voxel ownership on the CPU.
		GLOBAL_DEF("rendering/voxel_forward/reflections/enabled", true);
		GLOBAL_DEF("rendering/voxel_forward/reflections/resolution_scale", 0.5);
		GLOBAL_DEF("rendering/voxel_forward/reflections/grid_resolution", 64);
		GLOBAL_DEF("rendering/voxel_forward/reflections/near_cell_size", 0.1);
		GLOBAL_DEF("rendering/voxel_forward/reflections/far_cell_size", 0.4);
		GLOBAL_DEF("rendering/voxel_forward/reflections/distant_cell_size", 1.6);
		GLOBAL_DEF("rendering/voxel_forward/reflections/recenter_cells", 8);
		GLOBAL_DEF("rendering/voxel_forward/reflections/max_distance", 96.0);
		GLOBAL_DEF("rendering/voxel_forward/reflections/max_steps", 512);
		GLOBAL_DEF("rendering/voxel_forward/reflections/intensity", 1.0);
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/reflections/resolution_scale", PROPERTY_HINT_RANGE, "0.25,1,0.05"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/reflections/grid_resolution", PROPERTY_HINT_RANGE, "32,128,16"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/reflections/near_cell_size", PROPERTY_HINT_RANGE, "0.05,2,0.05,or_greater,suffix:m"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/reflections/far_cell_size", PROPERTY_HINT_RANGE, "0.1,8,0.1,or_greater,suffix:m"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/reflections/distant_cell_size", PROPERTY_HINT_RANGE, "0.5,32,0.5,or_greater,suffix:m"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/reflections/recenter_cells", PROPERTY_HINT_RANGE, "1,32,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/reflections/max_distance", PROPERTY_HINT_RANGE, "1,512,1,or_greater,suffix:m"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/reflections/max_steps", PROPERTY_HINT_RANGE, "16,4096,16"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/reflections/intensity", PROPERTY_HINT_RANGE, "0,8,0.05,or_greater"));
		// Legacy exact-face proxy generation scales poorly with large resident
		// volume sets. Keep it as an opt-in Forward+ fallback while Voxel Forward
		// owns voxel visibility and shadows directly.
		GLOBAL_DEF("rendering/voxel_volume/shadow_proxy/enabled", false);
		ClassDB::register_class<VoxelShapeData>();
		ClassDB::register_class<VoxelShape3D>();
		ClassDB::register_class<VoxelMaterial>();
		ClassDB::register_class<VoxelVolume3D>();
		ClassDB::register_class<VoxelLightingData>();
		ClassDB::register_class<VoxelLightingBake3D>();
		voxel_lighting_loader.instantiate();
		voxel_lighting_saver.instantiate();
		ResourceLoader::add_resource_format_loader(voxel_lighting_loader);
		ResourceSaver::add_resource_format_saver(voxel_lighting_saver);
		voxel_volume_streaming_manager = memnew(VoxelVolumeStreamingManager);
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorPlugins::add_by_type<VoxelVolume3DEditorPlugin>();
		EditorPlugins::add_by_type<VoxelLightingBakeEditorPlugin>();
	}
#endif
}

void uninitialize_voxel_physics_3d_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		ResourceLoader::remove_resource_format_loader(voxel_lighting_loader);
		ResourceSaver::remove_resource_format_saver(voxel_lighting_saver);
		voxel_lighting_loader.unref();
		voxel_lighting_saver.unref();
		if (voxel_volume_streaming_manager != nullptr) {
			memdelete(voxel_volume_streaming_manager);
			voxel_volume_streaming_manager = nullptr;
		}
		VoxelMaterial::clear_shader_cache();
	}
}
