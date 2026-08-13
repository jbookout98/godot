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
		PhysicsServer3DManager::get_singleton()->register_server("VoxelPhysics3D", callable_mp_static(_create_voxel_physics_3d_callback));
	}

	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GLOBAL_DEF("rendering/voxel_volume/max_resident_volumes", 256);
		GLOBAL_DEF("rendering/voxel_volume/max_loads_per_frame", 8);
		GLOBAL_DEF("rendering/voxel_volume/streaming_distance", 512.0);
		// The native RD visibility pass is deliberately opt-in until its output
		// matches the established DDA path for multi-volume boundaries.
		GLOBAL_DEF("rendering/voxel_forward/experimental_custom_visibility", false);
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
		// Legacy exact-face proxy generation scales poorly with large resident
		// volume sets. Keep it as an opt-in Forward+ fallback while Voxel Forward
		// owns voxel visibility and shadows directly.
		GLOBAL_DEF("rendering/voxel_volume/shadow_proxy/enabled", false);
		GLOBAL_DEF("rendering/voxel_volume/occupancy_shadows/enabled", false);
		GLOBAL_DEF("rendering/voxel_volume/occupancy_shadows/resolution", 128);
		GLOBAL_DEF("rendering/voxel_volume/occupancy_shadows/voxel_size", 0.1);
		GLOBAL_DEF("rendering/voxel_volume/occupancy_shadows/coarse_scale", 4);
		GLOBAL_DEF("rendering/voxel_volume/occupancy_shadows/fine_distance", 2.0);
		GLOBAL_DEF("rendering/voxel_volume/occupancy_shadows/max_distance", 24.0);
		GLOBAL_DEF("rendering/voxel_volume/occupancy_shadows/bias", 0.003);
		GLOBAL_DEF("rendering/voxel_volume/occupancy_shadows/max_steps", 128);
		GLOBAL_DEF("rendering/voxel_volume/occupancy_shadows/update_frames", 4);
		ClassDB::register_class<VoxelShapeData>();
		ClassDB::register_class<VoxelShape3D>();
		ClassDB::register_class<VoxelMaterial>();
		ClassDB::register_class<VoxelVolume3D>();
		voxel_volume_streaming_manager = memnew(VoxelVolumeStreamingManager);
	}
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
