/**************************************************************************/
/*  voxel_lighting_data.h                                                 */
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

#pragma once

#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/math/vector4i.h"
#include "core/templates/hash_map.h"

// Canonical little-endian probe records, independent of GPU slot and RID identity.
class VoxelLightingData : public Resource {
	GDCLASS(VoxelLightingData, Resource);
	int format_version = 0;
	String signature;
	String payload_hash;
	AABB bounds;
	Vector3 world_origin;
	float voxel_size = 0.0f;
	PackedInt32Array spacings;
	PackedByteArray probes;

protected:
	static void _bind_methods();

public:
	static constexpr int FORMAT_VERSION = 1;
	static constexpr int RECORD_BYTES = 48;
	static constexpr int IRRADIANCE_BYTES = 10 * 10 * 8;
	static constexpr int VISIBILITY_BYTES = 18 * 18 * 8;
	static constexpr int PROBE_BYTES = RECORD_BYTES + IRRADIANCE_BYTES + VISIBILITY_BYTES;
	static constexpr int MAX_BYTES = 256 * 1024 * 1024;
	void set_format_version(int p_value) { format_version = p_value; }
	int get_format_version() const { return format_version; }
	void set_signature(const String &p_value) { signature = p_value; }
	String get_signature() const { return signature; }
	void set_bounds(const AABB &p_value) { bounds = p_value; }
	AABB get_bounds() const { return bounds; }
	void set_world_origin(const Vector3 &p_value) { world_origin = p_value; }
	Vector3 get_world_origin() const { return world_origin; }
	void set_voxel_size(float p_value) { voxel_size = p_value; }
	float get_voxel_size() const { return voxel_size; }
	void set_spacings(const PackedInt32Array &p_value) { spacings = p_value; }
	PackedInt32Array get_spacings() const { return spacings; }
	void set_probes(const PackedByteArray &p_value);
	PackedByteArray get_probes() const { return probes; }
	int get_probe_count() const { return probes.size() / PROBE_BYTES; }
	void seal();
	void set_payload_hash(const String &p_hash) { payload_hash = p_hash; }
	String get_payload_hash() const { return payload_hash; }
	String validate_layout() const;
	String validate() const;
	static Vector4i probe_key(const uint8_t *p_record);
};

// Fixed-size header and a bounded payload; generic .res byte arrays cannot be
// checked by a resource setter until after the generic loader allocates them.
class ResourceFormatLoaderVoxelLighting : public ResourceFormatLoader {
	GDSOFTCLASS(ResourceFormatLoaderVoxelLighting, ResourceFormatLoader);

public:
	Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	void get_recognized_extensions(List<String> *p_extensions) const override { p_extensions->push_back("vlight"); }
	bool handles_type(const String &p_type) const override { return p_type == "VoxelLightingData"; }
	String get_resource_type(const String &p_path) const override { return p_path.get_extension().to_lower() == "vlight" ? "VoxelLightingData" : ""; }
};
class ResourceFormatSaverVoxelLighting : public ResourceFormatSaver {
	GDSOFTCLASS(ResourceFormatSaverVoxelLighting, ResourceFormatSaver);

public:
	Error save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags = 0) override;
	bool recognize(const Ref<Resource> &p_resource) const override { return Object::cast_to<VoxelLightingData>(p_resource.ptr()) != nullptr; }
	void get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const override {
		if (recognize(p_resource)) {
			p_extensions->push_back("vlight");
		}
	}
};
