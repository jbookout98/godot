/**************************************************************************/
/*  voxel_lighting_bake.cpp                                               */
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

#include "core/io/marshalls.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/renderer_rd/voxel_forward/render_voxel_forward.h"

namespace RendererSceneRenderImplementation {

void RenderVoxelForward::lighting_cache_set(RID p_scenario, ObjectID p_owner, Ref<VoxelLightingData> p_data, Callable p_verify) {
	if (!bake_renderer) {
		return;
	}
	auto &worlds = bake_renderer->baked_worlds;
	if (p_data.is_null()) {
		if (worlds.has(p_scenario) && worlds[p_scenario].owner == p_owner) {
			worlds.erase(p_scenario);
		}
		return;
	}
	const String error = p_data->validate();
	if (!error.is_empty()) {
		WARN_PRINT(error + " Using dynamic voxel lighting.");
		return;
	}
	if (worlds.has(p_scenario) && worlds[p_scenario].owner != p_owner) {
		WARN_PRINT("Only one VoxelLightingBake3D cache may be assigned per World3D.");
		return;
	}
	BakedWorld state;
	state.owner = p_owner;
	state.data = p_data;
	state.verify_source = p_verify;
	state.ticket = ++bake_renderer->next_bake_ticket;
	const auto *storage = bake_renderer->volume_registry.get_scenario_storage(p_scenario);
	state.content_revision = storage->get_lighting_content_revision();
	state.shading_revision = storage->get_shading_texture_revision();
	const PackedByteArray bytes = p_data->get_probes();
	for (int offset = 0; offset < bytes.size(); offset += VoxelLightingData::PROBE_BYTES) {
		state.offsets.insert(VoxelLightingData::probe_key(bytes.ptr() + offset), offset);
	}
	worlds.insert(p_scenario, state);
}

void RenderVoxelForward::lighting_cache_confirm(RID p_scenario, ObjectID p_owner, int64_t p_ticket, bool p_valid, Array p_source_bases) {
	if (!bake_renderer) {
		return;
	}
	BakedWorld *cache = bake_renderer->baked_worlds.getptr(p_scenario);
	if (!cache || cache->owner != p_owner || cache->ticket != uint64_t(p_ticket) || cache->invalidated) {
		return;
	}
	if (!p_valid) {
		WARN_PRINT("Level changed before baked lighting was ready; using dynamic lighting.");
		cache->invalidated = true;
		cache->data.unref();
		cache->offsets.clear();
		return;
	}
	const auto *storage = bake_renderer->volume_registry.get_scenario_storage(p_scenario);
	const DdgiWorldState *state = bake_renderer->lighting_scenario == p_scenario ? &bake_renderer->ddgi_state : bake_renderer->ddgi_worlds.getptr(p_scenario);
	if (!state) {
		return;
	}
	// These handles identify live inputs only. They never enter the saved cache.
	HashSet<RID> authored;
	for (int i = 0; i < p_source_bases.size(); i++) {
		authored.insert(RID(p_source_bases[i]));
	}
	for (const auto &entry : storage->get_volumes()) {
		if (!authored.has(entry.key)) {
			cache->refresh_seeds = true;
		} else {
			cache->authored_volume_revisions.insert(entry.key, entry.value.lighting_revision);
		}
	}
	if (state->ddgi_directional_light_base.is_valid() && !authored.has(state->ddgi_directional_light_base)) {
		cache->refresh_seeds = true;
	}
	for (const auto &entry : state->ddgi_local_light_cache) {
		if (!authored.has(RendererRD::LightStorage::get_singleton()->light_instance_get_base_light(entry.key))) {
			cache->refresh_seeds = true;
		} else {
			cache->authored_local_light_versions.insert(entry.key, entry.value.version);
			cache->authored_local_light_transforms.insert(entry.key, entry.value.transform);
		}
	}
	cache->source_verified = true;
	cache->verification_pending = false;
	cache->content_revision = storage->get_lighting_content_revision();
	cache->shading_revision = storage->get_shading_texture_revision();
	cache->lighting_revision = state->ddgi_lighting_revision;
	cache->local_light_hash = state->ddgi_local_light_upload_hash;
}

void RenderVoxelForward::_invalidate_changed_bake() {
	BakedWorld *cache = baked_worlds.getptr(lighting_scenario);
	if (!cache || cache->invalidated || !cache->source_verified) {
		return;
	}
	bool content_changed = cache->content_revision != volume_storage->get_lighting_content_revision() || cache->shading_revision != volume_storage->get_shading_texture_revision();
	if (content_changed && cache->initial_lods != 15) {
		// Moving runtime actors must not prevent the initial warm start forever.
		// Authored edits still invalidate immediately; excluded inputs refresh
		// saved history through the native dirty-probe scheduler.
		content_changed = false;
		for (const auto &entry : cache->authored_volume_revisions) {
			const auto *volume = volume_storage->get_volumes().getptr(entry.key);
			if (!volume || volume->lighting_revision != entry.value) {
				content_changed = true;
				break;
			}
		}
		if (!content_changed) {
			cache->refresh_seeds = true;
			cache->content_revision = volume_storage->get_lighting_content_revision();
			cache->shading_revision = volume_storage->get_shading_texture_revision();
		}
	}
	const bool lighting_changed = ddgi_state.ddgi_light_valid && cache->lighting_revision != UINT64_MAX && cache->lighting_revision != ddgi_state.ddgi_lighting_revision;
	bool local_lights_changed = cache->local_light_hash != UINT64_MAX && cache->local_light_hash != ddgi_state.ddgi_local_light_upload_hash;
	if (local_lights_changed && cache->initial_lods != 15) {
		// A moving actor's lamp is also an excluded runtime input. Preserve the
		// warm start unless one of the verified authored lights actually changed.
		local_lights_changed = false;
		for (const auto &entry : cache->authored_local_light_versions) {
			const auto *light = ddgi_state.ddgi_local_light_cache.getptr(entry.key);
			if (!light || light->version != entry.value || light->transform != cache->authored_local_light_transforms[entry.key]) {
				local_lights_changed = true;
				break;
			}
		}
		if (!local_lights_changed) {
			cache->refresh_seeds = true;
		}
	}
	if (content_changed || lighting_changed || local_lights_changed) {
		cache->invalidated = true;
		cache->offsets.clear();
		cache->data.unref();
		// Refresh resident baked history throughout the windows on the first edit,
		// including distant indirect effects. Subsequent updates remain native.
		if (content_changed || local_lights_changed) {
			ddgi_state.ddgi_lighting_revision++;
			for (DdgiCascade &cascade : ddgi_state.ddgi_cascades) {
				cascade.activation_dirty = true;
			}
		}
		return;
	}
	// Lighting inputs are established by the first native DDGI frame.
	if (ddgi_state.ddgi_light_valid) {
		cache->lighting_revision = ddgi_state.ddgi_lighting_revision;
	}
	cache->local_light_hash = ddgi_state.ddgi_local_light_upload_hash;
}

void RenderVoxelForward::_seed_baked_records(uint32_t p_lod, const Vector<uint32_t> &p_slots, const Vector<DdgiGpuProbeRecord> &p_records) {
	_invalidate_changed_bake();
	BakedWorld *cache = baked_worlds.getptr(lighting_scenario);
	if (!cache || cache->invalidated) {
		return;
	}
	const auto &world = volume_storage->get_world_occupancy();
	if (world.ddgi_pending_page_count || world.voxel_size <= 0) {
		return;
	}
	if (!Math::is_equal_approx(world.voxel_size, cache->data->get_voxel_size()) || !world.origin.is_equal_approx(cache->data->get_world_origin())) {
		WARN_PRINT("Voxel lighting bake grid does not match the loaded world; using dynamic lighting.");
		cache->invalidated = true;
		return;
	}
	const PackedInt32Array spacings = cache->data->get_spacings();
	for (uint32_t i = 0; i < 4; i++) {
		if (uint32_t(spacings[i]) != world.ddgi_cell_spacing_voxels[i]) {
			cache->invalidated = true;
			return;
		}
	}
	if (!cache->source_verified) {
		if (!cache->verification_pending && ddgi_state.ddgi_light_valid) {
			cache->verification_pending = true;
			cache->verify_source.call_deferred(int64_t(cache->ticket), cache->data->get_signature(), lighting_scenario);
		}
		return;
	}
	const PackedByteArray source = cache->data->get_probes();
	PackedByteArray upload;
	for (int i = 0; i < p_slots.size(); i++) {
		const auto &record = p_records[i];
		if (record.physical_position_valid[3] < 0.5f) {
			continue;
		}
		const Vector4i key(record.logical_cell_lod[0], record.logical_cell_lod[1], record.logical_cell_lod[2], p_lod);
		const int *offset = cache->offsets.getptr(key);
		if (!offset) {
			continue;
		}
		const uint8_t *probe = source.ptr() + *offset;
		const Vector3 saved(decode_float(probe + 16), decode_float(probe + 20), decode_float(probe + 24));
		const Vector3 current(record.physical_position_valid[0], record.physical_position_valid[1], record.physical_position_valid[2]);
		if (!saved.is_equal_approx(current) || decode_uint32(probe + 44) != record.state_revision_frame_flags[3]) {
			continue;
		}
		const int begin = upload.size();
		upload.resize(begin + 4 + VoxelLightingData::PROBE_BYTES);
		encode_uint32(p_slots[i], upload.ptrw() + begin);
		memcpy(upload.ptrw() + begin + 4, probe, VoxelLightingData::PROBE_BYTES);
	}
	cache->initial_lods |= 1u << p_lod;
	if (upload.is_empty()) {
		return;
	}
	cache->content_revision = volume_storage->get_lighting_content_revision();
	cache->shading_revision = volume_storage->get_shading_texture_revision();
	if (ddgi_state.ddgi_light_valid) {
		cache->lighting_revision = ddgi_state.ddgi_lighting_revision;
	}
	if (!ddgi_seed_version.is_valid()) {
		Vector<String> modes;
		modes.push_back("");
		ddgi_seed_shader.initialize(modes);
		ddgi_seed_version = ddgi_seed_shader.version_create();
		ddgi_seed_pipeline = RD::get_singleton()->compute_pipeline_create(ddgi_seed_shader.version_get_shader(ddgi_seed_version, 0));
	}
	RD *rd = RD::get_singleton();
	const RID buffer = rd->storage_buffer_create(upload.size(), upload);
	const auto &cascade = ddgi_state.ddgi_cascades[p_lod];
	RD::Uniform u0(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ buffer }));
	RD::Uniform u1(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ cascade.probe_records }));
	RD::Uniform u2(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ cascade.irradiance_rd }));
	RD::Uniform u3(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ cascade.visibility_rd }));
	RD::Uniform u4(RD::UNIFORM_TYPE_IMAGE, 4, Vector<RID>({ cascade.metadata_rd }));
	const RID set = UniformSetCacheRD::get_singleton()->get_cache(ddgi_seed_shader.version_get_shader(ddgi_seed_version, 0), 0, u0, u1, u2, u3, u4);
	const uint32_t count = upload.size() / (4 + VoxelLightingData::PROBE_BYTES);
	uint32_t params[4] = { ddgi_state.ddgi_probe_resolution, count, uint32_t(world.revision), cache->refresh_seeds ? 1u : 0u };
	auto list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, ddgi_seed_pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	rd->compute_list_set_push_constant(list, params, sizeof(params));
	rd->compute_list_dispatch(list, count, 1, 1);
	rd->compute_list_end();
	rd->free_rid(buffer);
	cache->restored_probes += count;
	ddgi_state.ddgi_cascades[p_lod].activation_dirty = true;
	ddgi_state.ddgi_cascades[p_lod].initialized = true;
	ddgi_state.ddgi_cascades[p_lod].lighting_revision = ddgi_state.ddgi_lighting_revision;
	ddgi_state.ddgi_cascades[p_lod].occupancy_revision = world.revision;
	print_verbose(vformat("Voxel lighting cache: restored %d probes in LOD %d.", count, p_lod));
}

void RenderVoxelForward::lighting_cache_status(RID p_scenario, bool p_probe_statistics, Callable p_callback) {
	Dictionary status;
	if (bake_renderer && bake_renderer->baked_worlds.has(p_scenario)) {
		const auto &cache = bake_renderer->baked_worlds[p_scenario];
		status["restored_probes"] = int64_t(cache.restored_probes);
		status["invalidated"] = cache.invalidated;
		status["refreshing_runtime_inputs"] = cache.refresh_seeds;
		status["ready"] = cache.source_verified && cache.initial_lods == 15 && !cache.invalidated;
	}

	if (p_probe_statistics && bake_renderer) {
		const DdgiWorldState *state = bake_renderer->lighting_scenario == p_scenario ? &bake_renderer->ddgi_state : bake_renderer->ddgi_worlds.getptr(p_scenario);
		bool ready = state && state->ddgi_probe_resolution > 0;
		int mature = 0, pending = 0, sleeping = 0;
		if (state) {
			for (uint32_t lod = 0; lod < 4; lod++) {
				const auto &cascade = state->ddgi_cascades[lod];
				ready = ready && cascade.initialized;
				if (!cascade.probe_records.is_valid()) {
					continue;
				}
				// Explicit diagnostics only; never requested by ordinary cache loading.
				const PackedByteArray records = RD::get_singleton()->buffer_get_data(cascade.probe_records);
				for (int offset = 0; offset + 48 <= records.size(); offset += 48) {
					const uint8_t *record = records.ptr() + offset;
					if (decode_float(record + 28) < 0.5f) {
						continue;
					}
					const uint32_t lifecycle = decode_uint32(record + 32);
					if (lifecycle == 1) {
						sleeping++;
					} else if (lifecycle == 4 || lifecycle == 5) {
						mature++;
					} else if (lifecycle != 0) {
						pending++;
					}
				}
			}
		}
		status["probe_windows_initialized"] = ready;
		status["mature_probes"] = mature;
		status["pending_probes"] = pending;
		status["sleeping_probes"] = sleeping;
	}
	p_callback.call_deferred(status);
}

void RenderVoxelForward::lighting_cache_capture(RID p_scenario, AABB p_bounds, Callable p_callback) {
	Dictionary result;
	if (!bake_renderer) {
		result["error"] = "Voxel Forward renderer is required.";
		p_callback.call_deferred(result);
		return;
	}
	auto *renderer = bake_renderer;
	DdgiWorldState *state = renderer->lighting_scenario == p_scenario ? &renderer->ddgi_state : renderer->ddgi_worlds.getptr(p_scenario);
	const auto &world = renderer->volume_registry.get_scenario_storage(p_scenario)->get_world_occupancy();
	if (!state || !state->ddgi_probe_resolution || state->shadow_atlas_temporal_rebuild_in_progress || world.ddgi_pending_page_count || !world.occupied_brick_count) {
		result["pending"] = true;
		p_callback.call_deferred(result);
		return;
	}
	for (uint32_t lod = 0; lod < 4; lod++) {
		if (!state->ddgi_cascades[lod].initialized) {
			result["pending"] = true;
			p_callback.call_deferred(result);
			return;
		}
	}
	PackedByteArray payload;
	uint32_t pending = 0;
	RD *rd = RD::get_singleton();
	const uint32_t resolution = state->ddgi_probe_resolution;
	for (uint32_t lod = 0; lod < 4; lod++) {
		const auto &cascade = state->ddgi_cascades[lod];
		const PackedByteArray records = rd->buffer_get_data(cascade.probe_records);
		if (records.size() != int(resolution * resolution * resolution * VoxelLightingData::RECORD_BYTES)) {
			result["error"] = "Could not read DDGI probe records.";
			p_callback.call_deferred(result);
			return;
		}
		Vector<uint32_t> selected;
		for (uint32_t slot = 0; slot < resolution * resolution * resolution; slot++) {
			const uint8_t *record = records.ptr() + slot * 48;
			if (decode_float(record + 28) < 0.5f) {
				continue;
			}
			const Vector4i key = VoxelLightingData::probe_key(record);
			const Vector3 position = world.origin + (Vector3(key.x, key.y, key.z) + Vector3(0.5, 0.5, 0.5)) * (world.voxel_size * world.ddgi_cell_spacing_voxels[lod]);
			if (!p_bounds.grow(world.voxel_size * world.ddgi_cell_spacing_voxels[lod]).has_point(position)) {
				continue;
			}
			const uint32_t lifecycle = decode_uint32(record + 32);
			if (lifecycle == 0) {
				continue;
			}
			if (lifecycle != 1 && lifecycle != 4 && lifecycle != 5) {
				pending++;
				continue;
			}
			selected.push_back(slot);
		}
		if (selected.is_empty()) {
			continue;
		}
		// Explicit bake/capture only. No recurring runtime GPU readback.
		const PackedByteArray irradiance = rd->texture_get_data(cascade.irradiance_rd, 0);
		const PackedByteArray visibility = rd->texture_get_data(cascade.visibility_rd, 0);
		if (irradiance.size() != int(state->ddgi_irradiance_atlas_width * state->ddgi_irradiance_atlas_height * 8) ||
				visibility.size() != int(state->ddgi_visibility_atlas_width * state->ddgi_visibility_atlas_height * 8)) {
			result["error"] = "Could not read DDGI lighting textures.";
			p_callback.call_deferred(result);
			return;
		}
		for (uint32_t slot : selected) {
			if (payload.size() > VoxelLightingData::MAX_BYTES - VoxelLightingData::PROBE_BYTES) {
				result["error"] = "Bake exceeds 256 MiB; reduce bounds or probe density.";
				p_callback.call_deferred(result);
				return;
			}
			const int begin = payload.size();
			payload.resize(begin + VoxelLightingData::PROBE_BYTES);
			uint8_t *destination = payload.ptrw() + begin;
			memcpy(destination, records.ptr() + slot * 48, 48);
			destination += 48;
			for (uint32_t y = 0; y < 10; y++) {
				uint32_t offset = ((slot / (resolution * resolution) * 10 + y) * state->ddgi_irradiance_atlas_width + slot % (resolution * resolution) * 10) * 8;
				memcpy(destination + y * 10 * 8, irradiance.ptr() + offset, 10 * 8);
			}
			destination += VoxelLightingData::IRRADIANCE_BYTES;
			for (uint32_t y = 0; y < 18; y++) {
				uint32_t offset = ((slot / (resolution * resolution) * 18 + y) * state->ddgi_visibility_atlas_width + slot % (resolution * resolution) * 18) * 8;
				memcpy(destination + y * 18 * 8, visibility.ptr() + offset, 18 * 8);
			}
		}
	}
	result["pending"] = pending > 0;
	result["pending_probes"] = pending;
	if (!pending) {
		Ref<VoxelLightingData> data;
		data.instantiate();
		data->set_format_version(VoxelLightingData::FORMAT_VERSION);
		data->set_bounds(p_bounds);
		data->set_world_origin(world.origin);
		data->set_voxel_size(world.voxel_size);
		PackedInt32Array spacings;
		for (int i = 0; i < 4; i++) {
			spacings.push_back(world.ddgi_cell_spacing_voxels[i]);
		}
		data->set_spacings(spacings);
		data->set_probes(payload);
		result["data"] = data;
	}
	p_callback.call_deferred(result);
}

void RenderVoxelForward::_free_world_lighting() {
	_free_ddgi();
	_free_dynamic_voxel_lighting_scene();
	_free_voxel_reflections();
	_reset_shadow_atlas_state();
	if (ddgi_state.shadow_mask_texture.is_valid()) {
		RendererRD::TextureStorage::get_singleton()->texture_free(ddgi_state.shadow_mask_texture);
	}
	for (uint32_t i = 0; i < DDGI_FRAME_RESOURCE_COUNT; i++) {
		if (ddgi_state.ddgi_resolve_uniform_buffer[i].is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.ddgi_resolve_uniform_buffer[i]);
		}
		if (ddgi_state.ddgi_temporal_uniform_buffer[i].is_valid()) {
			RD::get_singleton()->free_rid(ddgi_state.ddgi_temporal_uniform_buffer[i]);
		}
	}
	ddgi_state = DdgiWorldState();
}
void RenderVoxelForward::free_voxel_world(RID p_scenario) {
	const bool active = lighting_scenario == p_scenario;
	if (active || ddgi_worlds.has(p_scenario)) {
		if (!active) {
			SWAP(ddgi_state, ddgi_worlds[p_scenario]);
		}
		_free_world_lighting();
		if (!active) {
			SWAP(ddgi_state, ddgi_worlds[p_scenario]);
		}
	}
	ddgi_worlds.erase(p_scenario);
	baked_worlds.erase(p_scenario);
	if (active) {
		lighting_scenario = RID();
		volume_storage = &volume_registry;
		_free_indirect_light();
		_free_restir_gi();
	}
	volume_registry.free_scenario_storage(p_scenario);
}

} // namespace RendererSceneRenderImplementation
