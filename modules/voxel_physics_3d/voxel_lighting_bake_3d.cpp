/**************************************************************************/
/*  voxel_lighting_bake_3d.cpp                                            */
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

#include "voxel_lighting_bake_3d.h"

#include "voxel_volume_3d.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/crypto/hashing_context.h"
#include "core/io/marshalls.h"
#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/physics/physics_body_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/3d/world_environment.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/texture.h"
#include "servers/rendering/renderer_rd/voxel_forward/render_voxel_forward.h"
#include "servers/rendering/rendering_server.h"
using RendererSceneRenderImplementation::RenderVoxelForward;

// Only serializable content participates. Resource paths and instance IDs do not.
static Variant lighting_value(const Variant &p_value, int p_depth = 0, bool *p_valid = nullptr) {
	if (p_depth > 32 || p_value.get_type() == Variant::RID || p_value.get_type() == Variant::CALLABLE || p_value.get_type() == Variant::SIGNAL) {
		if (p_valid) {
			*p_valid = false;
		}
		return Variant();
	}
	if (p_value.get_type() == Variant::OBJECT) {
		Object *object = p_value;
		if (!object) {
			return Variant();
		}
		if (Texture2D *texture = Object::cast_to<Texture2D>(object)) {
			Ref<Image> image = texture->get_image();
			if (image.is_null()) {
				if (p_valid) {
					*p_valid = false;
				}
				return Variant();
			}
			Array image_data;
			image_data.push_back(image->get_width());
			image_data.push_back(image->get_height());
			image_data.push_back(image->get_format());
			image_data.push_back(image->get_data());
			return image_data;
		}
		Dictionary result;
		result["class"] = object->get_class();
		List<PropertyInfo> properties;
		object->get_property_list(&properties);
		for (const PropertyInfo &info : properties) {
			if (!(info.usage & PROPERTY_USAGE_STORAGE) || info.name == "script" || info.name.begins_with("resource_") || info.name.begins_with("metadata/")) {
				continue;
			}
			result[info.name] = lighting_value(object->get(info.name), p_depth + 1, p_valid);
		}
		return result;
	}
	if (p_value.get_type() == Variant::ARRAY) {
		Array result, values = p_value;
		for (int i = 0; i < values.size(); i++) {
			result.push_back(lighting_value(values[i], p_depth + 1, p_valid));
		}
		return result;
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary result, values = p_value;
		Array keys = values.keys();
		keys.sort();
		for (int i = 0; i < keys.size(); i++) {
			result[keys[i]] = lighting_value(values[keys[i]], p_depth + 1, p_valid);
		}
		return result;
	}
	return p_value;
}

void VoxelLightingBake3D::_collect(Node *p_node, Vector<Node *> &r_nodes) const {
	if (p_node == this || p_node->is_in_group("voxel_bake_excluded")) {
		return;
	}
	if (Object::cast_to<Viewport>(p_node) && p_node != get_viewport()) {
		return;
	}
	if (Object::cast_to<PhysicsBody3D>(p_node) && !Object::cast_to<StaticBody3D>(p_node)) {
		return;
	}
	if (Node3D *spatial = Object::cast_to<Node3D>(p_node)) {
		if (!spatial->is_visible_in_tree()) {
			return;
		}
	}
	if (Object::cast_to<VoxelVolume3D>(p_node) || Object::cast_to<Light3D>(p_node) || Object::cast_to<WorldEnvironment>(p_node)) {
		r_nodes.push_back(p_node);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_collect(p_node->get_child(i), r_nodes);
	}
}

String VoxelLightingBake3D::compute_signature() const {
	Node *source = get_node_or_null(source_root);
	if (!source || !is_inside_tree()) {
		return String();
	}
	Vector<Node *> nodes;
	_collect(source, nodes);
	Array content;
	content.push_back(VoxelLightingData::FORMAT_VERSION);
	content.push_back(get_global_transform().xform(bake_bounds));
	for (Node *node : nodes) {
		Array entry;
		entry.push_back(String(source->get_path_to(node)));
		entry.push_back(node->get_class());
		if (Node3D *spatial = Object::cast_to<Node3D>(node)) {
			entry.push_back(spatial->get_global_transform());
		}
		if (VoxelVolume3D *volume = Object::cast_to<VoxelVolume3D>(node)) {
			entry.push_back(volume->get_voxel_data());
			entry.push_back(volume->get_voxel_material());
		} else if (WorldEnvironment *environment = Object::cast_to<WorldEnvironment>(node)) {
			entry.push_back(environment->get_environment());
		} else if (Light3D *light = Object::cast_to<Light3D>(node)) {
			Dictionary properties;
			List<PropertyInfo> infos;
			light->get_property_list(&infos);
			for (const PropertyInfo &info : infos) {
				if ((info.usage & PROPERTY_USAGE_STORAGE) && (info.name.begins_with("light_") || info.name.begins_with("shadow_") || info.name.begins_with("omni_") || info.name.begins_with("spot_") || info.name.begins_with("directional_"))) {
					properties[info.name] = light->get(info.name);
				}
			}
			entry.push_back(properties);
		}
		content.push_back(entry);
	}
	Dictionary settings;
	List<PropertyInfo> infos;
	ProjectSettings::get_singleton()->get_property_list(&infos);
	for (const PropertyInfo &info : infos) {
		if (info.name.begins_with("rendering/voxel_forward/")) {
			settings[info.name] = ProjectSettings::get_singleton()->get(info.name);
		}
	}
	content.push_back(settings);
	bool fingerprint_valid = true;
	const Variant canonical = lighting_value(content, 0, &fingerprint_valid);
	if (!fingerprint_valid) {
		return String();
	}
	int length = 0;
	if (encode_variant(canonical, nullptr, length) != OK || length > VoxelLightingData::MAX_BYTES) {
		return String();
	}
	PackedByteArray bytes;
	bytes.resize(length);
	if (encode_variant(canonical, bytes.ptrw(), length) != OK) {
		return String();
	}
	Ref<HashingContext> hash;
	hash.instantiate();
	hash->start(HashingContext::HASH_SHA256);
	hash->update(bytes);
	const PackedByteArray digest = hash->finish();
	return String::hex_encode_buffer(digest.ptr(), digest.size());
}

void VoxelLightingBake3D::set_lighting_data(const Ref<VoxelLightingData> &p_data) {
	lighting_data = p_data;
	notify_property_list_changed();
	update_configuration_warnings();
	if (is_ready()) {
		call_deferred(SNAME("_apply_cache"));
	}
}

void VoxelLightingBake3D::_apply_cache() {
	if (!is_inside_tree() || !get_world_3d().is_valid()) {
		return;
	}
	registered_scenario = get_world_3d()->get_scenario();
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&RenderVoxelForward::lighting_cache_set).bind(registered_scenario, get_instance_id(), Ref<VoxelLightingData>(), Callable()));
	if (lighting_data.is_null()) {
		return;
	}
	const String error = lighting_data->validate();
	if (!error.is_empty() || lighting_data->get_signature() != compute_signature()) {
		WARN_PRINT(error.is_empty() ? "Stale voxel lighting bake; using dynamic lighting. Re-bake this level." : error);
		return;
	}
	Ref<VoxelLightingData> immutable = lighting_data->duplicate();
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&RenderVoxelForward::lighting_cache_set).bind(registered_scenario, get_instance_id(), immutable, callable_mp(this, &VoxelLightingBake3D::_verify_cache)));
}

void VoxelLightingBake3D::_verify_cache(int64_t p_ticket, String p_signature, RID p_scenario) {
	if (!is_inside_tree() || registered_scenario != p_scenario) {
		return;
	}
	const bool valid = lighting_data.is_valid() && lighting_data->get_signature() == p_signature && compute_signature() == p_signature;
	Array source_bases;
	Vector<Node *> nodes;
	if (valid) {
		_collect(get_node_or_null(source_root), nodes);
	}
	for (Node *node : nodes) {
		if (auto *volume = Object::cast_to<VoxelVolume3D>(node)) {
			source_bases.push_back(volume->get_base());
		} else if (auto *light = Object::cast_to<Light3D>(node)) {
			source_bases.push_back(light->get_base());
		}
	}
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&RenderVoxelForward::lighting_cache_confirm).bind(p_scenario, get_instance_id(), p_ticket, valid, source_bases));
}

Error VoxelLightingBake3D::bake(const String &p_save_path) {
	if (is_baking()) {
		return ERR_BUSY;
	}
	if (!is_inside_tree() || !get_node_or_null(source_root)) {
		return ERR_UNCONFIGURED;
	}
	if (!bool(GLOBAL_GET("rendering/voxel_forward/indirect_light/enabled")) || int(GLOBAL_GET("rendering/voxel_forward/indirect_light/backend")) != 2) {
		return ERR_UNCONFIGURED;
	}
	const AABB bounds = get_global_transform().xform(bake_bounds);
	if (!bounds.position.is_finite() || !bounds.size.is_finite() || bounds.size.x <= 0 || bounds.size.y <= 0 || bounds.size.z <= 0) {
		return ERR_INVALID_PARAMETER;
	}
	bake_signature = compute_signature();
	if (bake_signature.is_empty()) {
		return ERR_INVALID_DATA;
	}
	Vector<Node *> nodes;
	_collect(get_node_or_null(source_root), nodes);
	float cell_size = 0.0f;
	for (Node *node : nodes) {
		VoxelVolume3D *volume = Object::cast_to<VoxelVolume3D>(node);
		if (!volume || volume->get_voxel_data().is_null()) {
			continue;
		}
		const Vector3 scale = volume->get_global_transform().basis.get_scale().abs();
		const float size = volume->get_voxel_data()->get_voxel_size() * MIN(scale.x, MIN(scale.y, scale.z));
		if (size > 0 && (cell_size == 0 || size < cell_size)) {
			cell_size = size;
		}
	}
	if (cell_size <= 0) {
		return ERR_UNCONFIGURED;
	}
	const int resolution = CLAMP(int(GLOBAL_GET("rendering/voxel_forward/indirect_light/ddgi/spatial_cascade_resolution")), 8, 32);
	int spacing = 4096;
	for (int lod = 0; lod < 4; lod++) {
		spacing = MIN(spacing, MAX(1, int(GLOBAL_GET(vformat("rendering/voxel_forward/indirect_light/ddgi/lod%d_spacing_voxels", lod)))));
	}
	// Overlap one probe cage on both sides of each camera-centered window.
	const float stride = cell_size * spacing * MAX(1, resolution - 4);
	const Vector3 counts = (bounds.size / stride).ceil();
	if (!counts.is_finite() || counts.x > 4096 || counts.y > 4096 || counts.z > 4096) {
		return ERR_OUT_OF_MEMORY;
	}
	const Vector3i steps = Vector3i(counts).max(Vector3i(1, 1, 1));
	if (int64_t(steps.x) * steps.y * steps.z > 4096) {
		return ERR_OUT_OF_MEMORY;
	}
	viewpoints.clear();
	for (int z = 0; z < steps.z; z++) {
		for (int y = 0; y < steps.y; y++) {
			for (int x = 0; x < steps.x; x++) {
				viewpoints.push_back(bounds.position + Vector3((x + 0.5) * bounds.size.x / steps.x, (y + 0.5) * bounds.size.y / steps.y, (z + 0.5) * bounds.size.z / steps.z));
			}
		}
	}
	bake_generation++;
	bake_save_path = p_save_path;
	prior_process_mode = get_process_mode();
	// Godot's modal bake progress disables processing in editor host windows.
	// This asynchronous renderer job must continue beneath that modal dialog.
	set_process_mode(PROCESS_MODE_ALWAYS);
	view_index = 0;
	view_frames = 0;
	last_draw_frame = Engine::get_singleton()->get_frames_drawn();
	capture_pending = false;
	captured_keys.clear();
	result_data.unref();
	bake_viewport = memnew(SubViewport);
	bake_viewport->set_size(Size2i(64, 64));
	Ref<World3D> world;
	world.instantiate();
	bake_viewport->set_world_3d(world);
	bake_viewport->set_update_mode(SubViewport::UPDATE_ONCE);
	add_child(bake_viewport);
	Node3D *root = memnew(Node3D);
	bake_viewport->add_child(root);
	for (Node *node : nodes) {
		if (VoxelVolume3D *volume = Object::cast_to<VoxelVolume3D>(node)) {
			VoxelVolume3D *copy = memnew(VoxelVolume3D);
			copy->set_voxel_data(volume->get_voxel_data());
			copy->set_voxel_material(volume->get_voxel_material());
			copy->set_streaming_mode(VoxelVolume3D::STREAMING_MANUAL);
			copy->set_streaming_resident(true);
			copy->set_transform(volume->get_global_transform());
			root->add_child(copy);
		} else {
			Node *copy = node->duplicate(0);
			for (int i = copy->get_child_count() - 1; i >= 0; i--) {
				memdelete(copy->get_child(i));
			}
			copy->set_script(Variant());
			if (Node3D *spatial = Object::cast_to<Node3D>(copy)) {
				spatial->set_transform(Object::cast_to<Node3D>(node)->get_global_transform());
			}
			root->add_child(copy);
		}
	}
	bake_camera = memnew(Camera3D);
	root->add_child(bake_camera);
	bake_camera->set_position(viewpoints[0]);
	bake_camera->set_current(true);
	set_process_internal(true);
	emit_signal(SNAME("bake_progress"), 0.0f);
	return OK;
}

void VoxelLightingBake3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY || (p_what == NOTIFICATION_ENTER_WORLD && is_ready())) {
		call_deferred(SNAME("_apply_cache"));
	}
	if (p_what == NOTIFICATION_EXIT_WORLD) {
		cancel_bake();
		if (registered_scenario.is_valid()) {
			RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&RenderVoxelForward::lighting_cache_set).bind(registered_scenario, get_instance_id(), Ref<VoxelLightingData>(), Callable()));
			registered_scenario = RID();
		}
	}
	if (p_what != NOTIFICATION_INTERNAL_PROCESS || !is_baking()) {
		return;
	}
	// An editor in low-processor mode does not draw just because an offscreen
	// viewport is UPDATE_ALWAYS. Request each bake frame explicitly, and count
	// rendered frames instead of idle iterations when enforcing convergence.
	bake_viewport->set_update_mode(SubViewport::UPDATE_ONCE);
	const uint64_t drawn = Engine::get_singleton()->get_frames_drawn();
	if (drawn == last_draw_frame) {
		return;
	}
	last_draw_frame = drawn;
	if (++view_frames > max_frames_per_view) {
		_finish("Lighting did not converge before the per-view frame limit. Increase the limit or reduce bake bounds.");
		return;
	}
	if (capture_pending || view_frames < settle_frames || view_frames % 16) {
		return;
	}
	emit_signal(SNAME("bake_progress"), get_bake_progress());
	capture_pending = true;
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&RenderVoxelForward::lighting_cache_capture).bind(bake_viewport->get_world_3d()->get_scenario(), get_global_transform().xform(bake_bounds), callable_mp(this, &VoxelLightingBake3D::_capture_completed).bind(int64_t(bake_generation))));
}

void VoxelLightingBake3D::_capture_completed(Dictionary p_result, int64_t p_generation) {
	if (!is_baking() || uint64_t(p_generation) != bake_generation) {
		return;
	}
	capture_pending = false;
	if (p_result.has("error")) {
		_finish(p_result["error"]);
		return;
	}
	if (bool(p_result.get("pending", true))) {
		return;
	}
	Ref<VoxelLightingData> captured = p_result.get("data", Variant());
	if (captured.is_null()) {
		_finish("Renderer returned no bake data.");
		return;
	}
	const PackedByteArray added = captured->get_probes();
	if (result_data.is_null()) {
		result_data = captured;
		result_data->set_probes(PackedByteArray());
	}
	PackedByteArray accumulated = result_data->get_probes();
	for (int offset = 0; offset < added.size(); offset += VoxelLightingData::PROBE_BYTES) {
		const Vector4i key = VoxelLightingData::probe_key(added.ptr() + offset);
		if (captured_keys.has(key)) {
			continue;
		}
		if (accumulated.size() > VoxelLightingData::MAX_BYTES - VoxelLightingData::PROBE_BYTES) {
			_finish("Bake exceeds the 256 MiB limit.");
			return;
		}
		captured_keys.insert(key, accumulated.size());
		const int begin = accumulated.size();
		accumulated.resize(begin + VoxelLightingData::PROBE_BYTES);
		memcpy(accumulated.ptrw() + begin, added.ptr() + offset, VoxelLightingData::PROBE_BYTES);
	}
	result_data->set_probes(accumulated);
	view_index++;
	view_frames = 0;
	last_draw_frame = Engine::get_singleton()->get_frames_drawn();
	emit_signal(SNAME("bake_progress"), get_bake_progress());
	if (view_index == viewpoints.size()) {
		result_data->set_signature(bake_signature);
		result_data->seal();
		String error = result_data->validate();
		if (error.is_empty() && compute_signature() != bake_signature) {
			error = "Scene changed during baking; result discarded.";
		}
		if (error.is_empty() && !bake_save_path.is_empty()) {
			// Match LightmapGI: give the external resource its canonical path,
			// persist it, then attach it to the scene. A failed save keeps the old data.
			const String previous_path = lighting_data.is_valid() ? lighting_data->get_path() : String();
			result_data->set_path(bake_save_path, true);
			const Error save_error = ResourceSaver::save(result_data);
			if (save_error != OK) {
				result_data->set_path(String());
				if (lighting_data.is_valid() && !previous_path.is_empty()) {
					lighting_data->set_path(previous_path, true);
				}
				error = vformat("Could not save baked voxel lighting to '%s' (error %d). Previous Lighting Data retained.", bake_save_path, int(save_error));
			}
		}
		if (error.is_empty()) {
			set_lighting_data(result_data);
		}
		_finish(error);
		return;
	}
	bake_camera->set_position(viewpoints[view_index]);
}

void VoxelLightingBake3D::_finish(const String &p_error) {
	if (bake_viewport) {
		bake_viewport->queue_free();
	}
	bake_viewport = nullptr;
	bake_camera = nullptr;
	capture_pending = false;
	set_process_internal(false);
	set_process_mode(prior_process_mode);
	result_data.unref();
	captured_keys.clear();
	viewpoints.clear();
	emit_signal(SNAME("bake_finished"), p_error);
}
void VoxelLightingBake3D::cancel_bake() {
	if (is_baking()) {
		bake_generation++;
		_finish("Bake cancelled; previous lighting data retained.");
	}
}
float VoxelLightingBake3D::get_bake_progress() const {
	return viewpoints.is_empty() ? 0.0f : (float(view_index) + MIN(0.9f, float(view_frames) / max_frames_per_view)) / viewpoints.size();
}
String VoxelLightingBake3D::get_bake_description() const {
	return vformat("Baking view %d of %d (%d rendered frames)", MIN(view_index + 1, viewpoints.size()), viewpoints.size(), view_frames);
}
void VoxelLightingBake3D::request_cache_status(bool p_probe_statistics) {
	if (is_inside_tree()) {
		RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&RenderVoxelForward::lighting_cache_status).bind(get_world_3d()->get_scenario(), p_probe_statistics, callable_mp(this, &VoxelLightingBake3D::_cache_status_completed)));
	}
}
void VoxelLightingBake3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_apply_cache"), &VoxelLightingBake3D::_apply_cache);
	ClassDB::bind_method(D_METHOD("compute_signature"), &VoxelLightingBake3D::compute_signature);
	ClassDB::bind_method(D_METHOD("bake", "save_path"), &VoxelLightingBake3D::bake, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_bake_description"), &VoxelLightingBake3D::get_bake_description);
	ClassDB::bind_method(D_METHOD("cancel_bake"), &VoxelLightingBake3D::cancel_bake);
	ClassDB::bind_method(D_METHOD("is_baking"), &VoxelLightingBake3D::is_baking);
	ClassDB::bind_method(D_METHOD("get_bake_progress"), &VoxelLightingBake3D::get_bake_progress);
	ClassDB::bind_method(D_METHOD("request_cache_status", "probe_statistics"), &VoxelLightingBake3D::request_cache_status, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("set_lighting_data", "value"), &VoxelLightingBake3D::set_lighting_data);
	ClassDB::bind_method(D_METHOD("get_lighting_data"), &VoxelLightingBake3D::get_lighting_data);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "lighting_data", PROPERTY_HINT_RESOURCE_TYPE, "VoxelLightingData"), "set_lighting_data", "get_lighting_data");
	ClassDB::bind_method(D_METHOD("set_source_root", "value"), &VoxelLightingBake3D::set_source_root);
	ClassDB::bind_method(D_METHOD("get_source_root"), &VoxelLightingBake3D::get_source_root);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "source_root", PROPERTY_HINT_NONE, ""), "set_source_root", "get_source_root");
	ClassDB::bind_method(D_METHOD("set_bake_bounds", "value"), &VoxelLightingBake3D::set_bake_bounds);
	ClassDB::bind_method(D_METHOD("get_bake_bounds"), &VoxelLightingBake3D::get_bake_bounds);
	ADD_PROPERTY(PropertyInfo(Variant::AABB, "bake_bounds", PROPERTY_HINT_NONE, ""), "set_bake_bounds", "get_bake_bounds");
	ClassDB::bind_method(D_METHOD("set_settle_frames", "value"), &VoxelLightingBake3D::set_settle_frames);
	ClassDB::bind_method(D_METHOD("get_settle_frames"), &VoxelLightingBake3D::get_settle_frames);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "settle_frames", PROPERTY_HINT_RANGE, "16,3600,1"), "set_settle_frames", "get_settle_frames");
	ClassDB::bind_method(D_METHOD("set_max_frames_per_view", "value"), &VoxelLightingBake3D::set_max_frames_per_view);
	ClassDB::bind_method(D_METHOD("get_max_frames_per_view"), &VoxelLightingBake3D::get_max_frames_per_view);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_frames_per_view", PROPERTY_HINT_RANGE, "120,36000,1"), "set_max_frames_per_view", "get_max_frames_per_view");
	ADD_SIGNAL(MethodInfo("bake_progress", PropertyInfo(Variant::FLOAT, "progress")));
	ADD_SIGNAL(MethodInfo("bake_finished", PropertyInfo(Variant::STRING, "error")));
	ADD_SIGNAL(MethodInfo("cache_status", PropertyInfo(Variant::DICTIONARY, "status")));
}
