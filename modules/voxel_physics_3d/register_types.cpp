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

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "godot_physics_server_3d.h"
#include "voxel_shape_3d.h"
#include "voxel_shape_data.h"
#include "voxel_material.h"
#include "voxel_volume_3d.h"
#include "voxel_volume_streaming_manager.h"
#include "core/config/project_settings.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/physics_3d/physics_server_3d_wrap_mt.h"

#ifdef TOOLS_ENABLED
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

void initialize_voxel_physics_3d_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		// This is consumed while the Voxel Forward renderer is constructed, before
		// scene-level voxel material settings are registered.
		GLOBAL_DEF("rendering/voxel_forward/architectural_hit_buffer/exact_position_enabled", false);
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
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/resolution_scale", 0.5);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/max_distance", 128.0);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/max_steps", 512);
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
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/max_local_lights", 4);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/soft_shadow_mode", 0);
		GLOBAL_DEF("rendering/voxel_forward/shadow_mask/soft_shadow_samples", 8);
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
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/shadow_mask/soft_shadow_mode", PROPERTY_HINT_ENUM, "Hard,Voxel Soft"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/shadow_mask/soft_shadow_samples", PROPERTY_HINT_ENUM, "1 Sample:1,4 Samples:4,8 Samples:8"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/shadow_mask/soft_shadow_radius_voxels", PROPERTY_HINT_RANGE, "0,32,0.25,or_greater"));
		// World-snapped near, mid, and distant irradiance cascades reuse the sparse
		// occupancy table and light-space shadow atlas. Camera motion only updates
		// a cascade when it crosses that cascade's independently snapped boundary.
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/enabled", true);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/resolution", 48);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/near_cell_size", 0.4);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/far_cell_size", 1.6);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/distant_cell_size", 6.4);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/recenter_cells", 4);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/recenter_hysteresis", 0.75);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/transition_cells", 6.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/propagation_steps", 6);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/propagation_decay", 0.78);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/shadow_bias_voxels", 1.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/intensity", 1.0);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/dirty_updates_enabled", true);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/low_latency_dirty_updates_enabled", true);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/dirty_cell_pass_budget_per_frame", 65536);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/temporal_updates_enabled", true);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/dispatch_budget_per_frame", 1);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/temporal_blend_enabled", true);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/blend_dispatch_budget_per_frame", 1);
		GLOBAL_DEF("rendering/voxel_forward/indirect_light/temporal_blend_frames", 6);
		GLOBAL_DEF("rendering/voxel_forward/ambient_light/color", Color(0.22, 0.22, 0.22));
		GLOBAL_DEF("rendering/voxel_forward/ambient_light/energy", 1.0);
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/resolution", PROPERTY_HINT_RANGE, "24,96,8"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/near_cell_size", PROPERTY_HINT_RANGE, "0.05,4,0.05,or_greater,suffix:m"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/far_cell_size", PROPERTY_HINT_RANGE, "0.1,16,0.1,or_greater,suffix:m"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/distant_cell_size", PROPERTY_HINT_RANGE, "0.5,64,0.5,or_greater,suffix:m"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/recenter_cells", PROPERTY_HINT_RANGE, "1,16,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/recenter_hysteresis", PROPERTY_HINT_RANGE, "0.55,0.95,0.05"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/transition_cells", PROPERTY_HINT_RANGE, "2,24,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/propagation_steps", PROPERTY_HINT_RANGE, "2,16,2"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/propagation_decay", PROPERTY_HINT_RANGE, "0,0.99,0.01"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/shadow_bias_voxels", PROPERTY_HINT_RANGE, "0,8,0.05"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::FLOAT, "rendering/voxel_forward/indirect_light/intensity", PROPERTY_HINT_RANGE, "0,8,0.05,or_greater"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/dirty_cell_pass_budget_per_frame", PROPERTY_HINT_RANGE, "4096,16777216,4096,or_greater"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/dispatch_budget_per_frame", PROPERTY_HINT_RANGE, "1,16,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/blend_dispatch_budget_per_frame", PROPERTY_HINT_RANGE, "1,3,1"));
		ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::INT, "rendering/voxel_forward/indirect_light/temporal_blend_frames", PROPERTY_HINT_RANGE, "1,60,1"));
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
		voxel_volume_streaming_manager = memnew(VoxelVolumeStreamingManager);
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorPlugins::add_by_type<VoxelVolume3DEditorPlugin>();
	}
#endif
}

void uninitialize_voxel_physics_3d_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		if (voxel_volume_streaming_manager != nullptr) {
			memdelete(voxel_volume_streaming_manager);
			voxel_volume_streaming_manager = nullptr;
		}
		VoxelMaterial::clear_shader_cache();
	}
}
