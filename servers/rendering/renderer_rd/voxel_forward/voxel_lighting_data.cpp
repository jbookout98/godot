#include "voxel_lighting_data.h"

#include "core/crypto/hashing_context.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/hash_set.h"

void VoxelLightingData::set_probes(const PackedByteArray &p_value) {
	if (p_value.size() > MAX_BYTES) {
		probes.clear();
		WARN_PRINT("Voxel lighting cache exceeds the 256 MiB limit; using dynamic lighting.");
		return;
	}
	probes = p_value;
}

Vector4i VoxelLightingData::probe_key(const uint8_t *p) {
	return Vector4i(int32_t(decode_uint32(p)), int32_t(decode_uint32(p + 4)), int32_t(decode_uint32(p + 8)), int32_t(decode_uint32(p + 12)));
}

static String probe_digest(const PackedByteArray &p_bytes) {
	Ref<HashingContext> hash;
	hash.instantiate();
	hash->start(HashingContext::HASH_SHA256);
	hash->update(p_bytes);
	const PackedByteArray digest = hash->finish();
	return String::hex_encode_buffer(digest.ptr(), digest.size());
}
void VoxelLightingData::seal() {
	payload_hash = probe_digest(probes);
}

String VoxelLightingData::validate_layout() const {
	if (format_version != FORMAT_VERSION) {
		return "Unsupported voxel lighting cache version.";
	}
	if (signature.length() != 64) {
		return "Missing lighting source fingerprint.";
	}
	if (!bounds.position.is_finite() || !bounds.size.is_finite() || bounds.size.x <= 0 || bounds.size.y <= 0 || bounds.size.z <= 0) {
		return "Invalid bake bounds.";
	}
	if (!world_origin.is_finite() || !Math::is_finite(voxel_size) || voxel_size <= 0 || spacings.size() != 4) {
		return "Invalid probe layout.";
	}
	for (int i = 0; i < 4; i++) {
		if (spacings[i] < 1 || spacings[i] > 4096) {
			return "Invalid probe spacing.";
		}
	}
	return String();
}

String VoxelLightingData::validate() const {
	const String layout_error = validate_layout();
	if (!layout_error.is_empty()) {
		return layout_error;
	}
	if (probes.is_empty() || probes.size() > MAX_BYTES || probes.size() % PROBE_BYTES) {
		return "Incomplete or oversized probe payload.";
	}
	if (payload_hash.length() != 64 || payload_hash != probe_digest(probes)) {
		return "Lighting payload checksum mismatch.";
	}
	HashSet<Vector4i> identities;
	for (int offset = 0; offset < probes.size(); offset += PROBE_BYTES) {
		const uint8_t *p = probes.ptr() + offset;
		const Vector4i key = probe_key(p);
		if (key.w < 0 || key.w >= 4 || identities.has(key)) {
			return "Invalid or duplicate probe identity.";
		}
		identities.insert(key);
		const Vector3 position(decode_float(p + 16), decode_float(p + 20), decode_float(p + 24));
		if (!position.is_finite() || !Math::is_equal_approx(decode_float(p + 28), 1.0f)) {
			return "Invalid probe placement.";
		}
		const Vector3 expected = world_origin + (Vector3(key.x, key.y, key.z) + Vector3(0.5, 0.5, 0.5)) * (voxel_size * spacings[key.w]);
		// Native placement may relocate by 45% of one cell from the center.
		if (!bounds.grow(voxel_size * spacings[key.w]).has_point(expected) || position.distance_to(expected) > voxel_size * spacings[key.w] * 2.0f) {
			return "Probe outside bake layout.";
		}
		const uint32_t state = decode_uint32(p + 32);
		if (state != 4 && state != 5 && state != 1) {
			return "Bake contains unconverged probes.";
		}
		for (int byte = RECORD_BYTES; byte < PROBE_BYTES; byte += 2) {
			if ((decode_uint16(p + byte) & 0x7c00u) == 0x7c00u) {
				return "Nonfinite lighting sample.";
			}
		}
	}
	return String();
}

void VoxelLightingData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_payload_hash", "value"), &VoxelLightingData::set_payload_hash);
	ClassDB::bind_method(D_METHOD("get_payload_hash"), &VoxelLightingData::get_payload_hash);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "payload_hash", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_payload_hash", "get_payload_hash");
	ClassDB::bind_method(D_METHOD("validate"), &VoxelLightingData::validate);
	ClassDB::bind_method(D_METHOD("get_probe_count"), &VoxelLightingData::get_probe_count);
	ClassDB::bind_method(D_METHOD("set_format_version", "value"), &VoxelLightingData::set_format_version);
	ClassDB::bind_method(D_METHOD("get_format_version"), &VoxelLightingData::get_format_version);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "format_version", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_format_version", "get_format_version");
	ClassDB::bind_method(D_METHOD("set_signature", "value"), &VoxelLightingData::set_signature);
	ClassDB::bind_method(D_METHOD("get_signature"), &VoxelLightingData::get_signature);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "signature", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_signature", "get_signature");
	ClassDB::bind_method(D_METHOD("set_bounds", "value"), &VoxelLightingData::set_bounds);
	ClassDB::bind_method(D_METHOD("get_bounds"), &VoxelLightingData::get_bounds);
	ADD_PROPERTY(PropertyInfo(Variant::AABB, "bounds", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_bounds", "get_bounds");
	ClassDB::bind_method(D_METHOD("set_world_origin", "value"), &VoxelLightingData::set_world_origin);
	ClassDB::bind_method(D_METHOD("get_world_origin"), &VoxelLightingData::get_world_origin);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "world_origin", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_world_origin", "get_world_origin");
	ClassDB::bind_method(D_METHOD("set_voxel_size", "value"), &VoxelLightingData::set_voxel_size);
	ClassDB::bind_method(D_METHOD("get_voxel_size"), &VoxelLightingData::get_voxel_size);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "voxel_size", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_voxel_size", "get_voxel_size");
	ClassDB::bind_method(D_METHOD("set_spacings", "value"), &VoxelLightingData::set_spacings);
	ClassDB::bind_method(D_METHOD("get_spacings"), &VoxelLightingData::get_spacings);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "spacings", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_spacings", "get_spacings");
	ClassDB::bind_method(D_METHOD("set_probes", "value"), &VoxelLightingData::set_probes);
	ClassDB::bind_method(D_METHOD("get_probes"), &VoxelLightingData::get_probes);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "probes", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_probes", "get_probes");
}

static constexpr uint32_t LIGHTING_FILE_MAGIC = 0x544c5856; // VXLT
static constexpr uint64_t LIGHTING_FILE_HEADER_BYTES = 200;

Ref<Resource> ResourceFormatLoaderVoxelLighting::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Ref<VoxelLightingData> result;
	result.instantiate();
	// Return an invalid resource on failure so a broken optional cache cannot
	// prevent the containing PackedScene from loading. The node falls back.
	if (r_error) {
		*r_error = OK;
	}
	auto invalid = [&result, &p_path](const String &p_reason) -> Ref<Resource> {
		result->set_format_version(0);
		result->set_probes(PackedByteArray());
		WARN_PRINT(vformat("Voxel lighting cache %s: %s Using dynamic lighting.", p_path, p_reason));
		return result;
	};
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null() || file->get_length() < LIGHTING_FILE_HEADER_BYTES) {
		return invalid("missing or truncated header.");
	}
	if (file->get_32() != LIGHTING_FILE_MAGIC) {
		return invalid("invalid file signature.");
	}
	result->set_format_version(file->get_32());
	const uint32_t payload_size = file->get_32();
	const uint32_t reserved = file->get_32();
	if (result->get_format_version() != VoxelLightingData::FORMAT_VERSION || reserved != 0) {
		return invalid("unsupported format.");
	}
	if (!payload_size || payload_size > VoxelLightingData::MAX_BYTES || payload_size % VoxelLightingData::PROBE_BYTES || file->get_length() != LIGHTING_FILE_HEADER_BYTES + payload_size) {
		return invalid("invalid or oversized payload length.");
	}
	result->set_voxel_size(file->get_float());
	Vector3 origin, position, size;
	for (int axis = 0; axis < 3; axis++) {
		origin[axis] = file->get_float();
	}
	for (int axis = 0; axis < 3; axis++) {
		position[axis] = file->get_float();
	}
	for (int axis = 0; axis < 3; axis++) {
		size[axis] = file->get_float();
	}
	result->set_world_origin(origin);
	result->set_bounds(AABB(position, size));
	PackedInt32Array spacings;
	for (int i = 0; i < 4; i++) {
		spacings.push_back(file->get_32());
	}
	result->set_spacings(spacings);
	uint8_t text[64];
	if (file->get_buffer(text, 64) != 64) {
		return invalid("truncated source fingerprint.");
	}
	result->set_signature(String::utf8(reinterpret_cast<const char *>(text), 64));
	if (file->get_buffer(text, 64) != 64) {
		return invalid("truncated payload checksum.");
	}
	result->set_payload_hash(String::utf8(reinterpret_cast<const char *>(text), 64));
	const String layout_error = result->validate_layout();
	if (!layout_error.is_empty()) {
		return invalid(layout_error);
	}
	PackedByteArray payload;
	if (payload.resize(payload_size) != OK) {
		return invalid("insufficient memory.");
	}
	if (file->get_buffer(payload.ptrw(), payload_size) != payload_size) {
		return invalid("truncated payload.");
	}
	result->set_probes(payload);
	const String validation_error = result->validate();
	if (!validation_error.is_empty()) {
		return invalid(validation_error);
	}
	if (r_progress) {
		*r_progress = 1.0f;
	}
	return result;
}

Error ResourceFormatSaverVoxelLighting::save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<VoxelLightingData> data = p_resource;
	ERR_FAIL_COND_V(data.is_null() || !data->validate().is_empty(), ERR_INVALID_DATA);
	Error error;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &error);
	if (file.is_null()) {
		return error;
	}
	const PackedByteArray payload = data->get_probes();
	file->store_32(LIGHTING_FILE_MAGIC);
	file->store_32(data->get_format_version());
	file->store_32(payload.size());
	file->store_32(0);
	file->store_float(data->get_voxel_size());
	const Vector3 origin = data->get_world_origin();
	const AABB bounds = data->get_bounds();
	for (int axis = 0; axis < 3; axis++) {
		file->store_float(origin[axis]);
	}
	for (int axis = 0; axis < 3; axis++) {
		file->store_float(bounds.position[axis]);
	}
	for (int axis = 0; axis < 3; axis++) {
		file->store_float(bounds.size[axis]);
	}
	const PackedInt32Array spacings = data->get_spacings();
	for (int i = 0; i < 4; i++) {
		file->store_32(spacings[i]);
	}
	const CharString signature = data->get_signature().utf8();
	const CharString digest = data->get_payload_hash().utf8();
	ERR_FAIL_COND_V(signature.length() != 64 || digest.length() != 64, ERR_INVALID_DATA);
	file->store_buffer(reinterpret_cast<const uint8_t *>(signature.get_data()), 64);
	file->store_buffer(reinterpret_cast<const uint8_t *>(digest.get_data()), 64);
	file->store_buffer(payload.ptr(), payload.size());
	file->flush();
	return file->get_error();
}
