#include "voxel_shape_data.h"

#include "core/error/error_macros.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

namespace {

static const Vector3i SURFACE_DIRECTIONS[6] = {
	Vector3i(1, 0, 0), Vector3i(-1, 0, 0),
	Vector3i(0, 1, 0), Vector3i(0, -1, 0),
	Vector3i(0, 0, 1), Vector3i(0, 0, -1)
};

} // namespace

void VoxelShapeData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_dimensions", "dimensions"), &VoxelShapeData::set_dimensions);
	ClassDB::bind_method(D_METHOD("get_dimensions"), &VoxelShapeData::get_dimensions);
	ClassDB::bind_method(D_METHOD("set_voxel_size", "voxel_size"), &VoxelShapeData::set_voxel_size);
	ClassDB::bind_method(D_METHOD("get_voxel_size"), &VoxelShapeData::get_voxel_size);

	ClassDB::bind_method(D_METHOD("get_voxel", "position"), &VoxelShapeData::get_voxel);
	ClassDB::bind_method(D_METHOD("set_voxel", "position", "palette_index"), &VoxelShapeData::set_voxel);
	ClassDB::bind_method(D_METHOD("apply_voxel_edits", "positions", "palette_indices"), &VoxelShapeData::apply_voxel_edits);
	ClassDB::bind_method(D_METHOD("apply_voxel_edits_by_index", "indices", "palette_indices"), &VoxelShapeData::apply_voxel_edits_by_index);
	ClassDB::bind_method(D_METHOD("fill_voxel_region", "position", "size", "palette_index"), &VoxelShapeData::fill_voxel_region);
	ClassDB::bind_method(D_METHOD("begin_edit"), &VoxelShapeData::begin_edit);
	ClassDB::bind_method(D_METHOD("end_edit"), &VoxelShapeData::end_edit);
	ClassDB::bind_method(D_METHOD("get_revision"), &VoxelShapeData::get_revision);
	ClassDB::bind_method(D_METHOD("get_last_dirty_position"), &VoxelShapeData::get_last_dirty_position);
	ClassDB::bind_method(D_METHOD("get_last_dirty_size"), &VoxelShapeData::get_last_dirty_size);

	ClassDB::bind_method(D_METHOD("set_solid_voxels", "voxels"), &VoxelShapeData::set_solid_voxels);
	ClassDB::bind_method(D_METHOD("get_solid_voxels"), &VoxelShapeData::get_solid_voxels);
	ClassDB::bind_method(D_METHOD("set_voxel_data", "voxels"), &VoxelShapeData::set_voxel_data);
	ClassDB::bind_method(D_METHOD("get_voxel_data"), &VoxelShapeData::get_voxel_data);
	ClassDB::bind_method(D_METHOD("set_sparse_brick_data", "data"), &VoxelShapeData::set_sparse_brick_data);
	ClassDB::bind_method(D_METHOD("get_sparse_brick_data"), &VoxelShapeData::get_sparse_brick_data);

	ClassDB::bind_method(D_METHOD("set_palette_texture", "texture"), &VoxelShapeData::set_palette_texture);
	ClassDB::bind_method(D_METHOD("get_palette_texture"), &VoxelShapeData::get_palette_texture);
	ClassDB::bind_method(D_METHOD("set_material_texture", "texture"), &VoxelShapeData::set_material_texture);
	ClassDB::bind_method(D_METHOD("get_material_texture"), &VoxelShapeData::get_material_texture);
	ClassDB::bind_method(D_METHOD("set_metallic_texture", "texture"), &VoxelShapeData::set_metallic_texture);
	ClassDB::bind_method(D_METHOD("get_metallic_texture"), &VoxelShapeData::get_metallic_texture);
	ClassDB::bind_method(D_METHOD("set_transparency_texture", "texture"), &VoxelShapeData::set_transparency_texture);
	ClassDB::bind_method(D_METHOD("get_transparency_texture"), &VoxelShapeData::get_transparency_texture);
	ClassDB::bind_method(D_METHOD("set_specularity_texture", "texture"), &VoxelShapeData::set_specularity_texture);
	ClassDB::bind_method(D_METHOD("get_specularity_texture"), &VoxelShapeData::get_specularity_texture);
	ClassDB::bind_method(D_METHOD("set_emission_texture", "texture"), &VoxelShapeData::set_emission_texture);
	ClassDB::bind_method(D_METHOD("get_emission_texture"), &VoxelShapeData::get_emission_texture);

	ClassDB::bind_method(D_METHOD("set_face_voxels", "voxels"), &VoxelShapeData::set_face_voxels);
	ClassDB::bind_method(D_METHOD("get_face_voxels"), &VoxelShapeData::get_face_voxels);
	ClassDB::bind_method(D_METHOD("set_edge_voxels", "voxels"), &VoxelShapeData::set_edge_voxels);
	ClassDB::bind_method(D_METHOD("get_edge_voxels"), &VoxelShapeData::get_edge_voxels);
	ClassDB::bind_method(D_METHOD("set_corner_voxels", "voxels"), &VoxelShapeData::set_corner_voxels);
	ClassDB::bind_method(D_METHOD("get_corner_voxels"), &VoxelShapeData::get_corner_voxels);
	ClassDB::bind_method(D_METHOD("set_face_masks", "masks"), &VoxelShapeData::set_face_masks);
	ClassDB::bind_method(D_METHOD("get_face_masks"), &VoxelShapeData::get_face_masks);
	ClassDB::bind_method(D_METHOD("set_edge_masks", "masks"), &VoxelShapeData::set_edge_masks);
	ClassDB::bind_method(D_METHOD("get_edge_masks"), &VoxelShapeData::get_edge_masks);
	ClassDB::bind_method(D_METHOD("set_corner_masks", "masks"), &VoxelShapeData::set_corner_masks);
	ClassDB::bind_method(D_METHOD("get_corner_masks"), &VoxelShapeData::get_corner_masks);
	ClassDB::bind_method(D_METHOD("get_face_mask", "position"), &VoxelShapeData::get_face_mask);
	ClassDB::bind_method(D_METHOD("get_edge_mask", "position"), &VoxelShapeData::get_edge_mask);
	ClassDB::bind_method(D_METHOD("get_corner_mask", "position"), &VoxelShapeData::get_corner_mask);

	ClassDB::bind_method(D_METHOD("set_face_shape", "shape"), &VoxelShapeData::set_face_shape);
	ClassDB::bind_method(D_METHOD("get_face_shape"), &VoxelShapeData::get_face_shape);
	ClassDB::bind_method(D_METHOD("set_edge_shape", "shape"), &VoxelShapeData::set_edge_shape);
	ClassDB::bind_method(D_METHOD("get_edge_shape"), &VoxelShapeData::get_edge_shape);
	ClassDB::bind_method(D_METHOD("set_corner_shape", "shape"), &VoxelShapeData::set_corner_shape);
	ClassDB::bind_method(D_METHOD("get_corner_shape"), &VoxelShapeData::get_corner_shape);
	ClassDB::bind_method(D_METHOD("set_face_shape_scale", "scale"), &VoxelShapeData::set_face_shape_scale);
	ClassDB::bind_method(D_METHOD("get_face_shape_scale"), &VoxelShapeData::get_face_shape_scale);
	ClassDB::bind_method(D_METHOD("set_edge_shape_scale", "scale"), &VoxelShapeData::set_edge_shape_scale);
	ClassDB::bind_method(D_METHOD("get_edge_shape_scale"), &VoxelShapeData::get_edge_shape_scale);
	ClassDB::bind_method(D_METHOD("set_corner_shape_scale", "scale"), &VoxelShapeData::set_corner_shape_scale);
	ClassDB::bind_method(D_METHOD("get_corner_shape_scale"), &VoxelShapeData::get_corner_shape_scale);

	ClassDB::bind_method(D_METHOD("is_inside", "position"), &VoxelShapeData::is_inside);
	ClassDB::bind_method(D_METHOD("get_voxel_index", "position"), &VoxelShapeData::get_voxel_index);
	ClassDB::bind_method(D_METHOD("is_solid", "position"), &VoxelShapeData::is_solid);
	ClassDB::bind_method(D_METHOD("is_face", "position"), &VoxelShapeData::is_face);
	ClassDB::bind_method(D_METHOD("is_edge", "position"), &VoxelShapeData::is_edge);
	ClassDB::bind_method(D_METHOD("is_corner", "position"), &VoxelShapeData::is_corner);

	ADD_SIGNAL(MethodInfo("voxels_changed",
			PropertyInfo(Variant::VECTOR3I, "position"),
			PropertyInfo(Variant::VECTOR3I, "size"),
			PropertyInfo(Variant::INT, "revision")));

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "dimensions"), "set_dimensions", "get_dimensions");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "voxel_size", PROPERTY_HINT_RANGE, "0.001,10.0,0.001,or_greater,suffix:m"), "set_voxel_size", "get_voxel_size");

	ADD_GROUP("Sparse Occupancy", "");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "sparse_brick_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_sparse_brick_data", "get_sparse_brick_data");
	// These properties remain recognized for old .tres/.res files but are no
	// longer written. Their getters intentionally materialize dense data.
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "solid_voxels", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_solid_voxels", "get_solid_voxels");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "face_voxels", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_face_voxels", "get_face_voxels");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "edge_voxels", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_edge_voxels", "get_edge_voxels");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "corner_voxels", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_corner_voxels", "get_corner_voxels");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "face_masks", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_face_masks", "get_face_masks");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "edge_masks", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_edge_masks", "get_edge_masks");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "corner_masks", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_corner_masks", "get_corner_masks");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "voxel_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_voxel_data", "get_voxel_data");

	ADD_GROUP("Legacy Material Fallback", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "palette_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_palette_texture", "get_palette_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_material_texture", "get_material_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "metallic_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_metallic_texture", "get_metallic_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "transparency_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_transparency_texture", "get_transparency_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "specularity_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_specularity_texture", "get_specularity_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "emission_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_emission_texture", "get_emission_texture");

	ADD_GROUP("Collision Feature Overrides", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "face_shape", PROPERTY_HINT_RESOURCE_TYPE, "Shape3D"), "set_face_shape", "get_face_shape");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "edge_shape", PROPERTY_HINT_RESOURCE_TYPE, "Shape3D"), "set_edge_shape", "get_edge_shape");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "corner_shape", PROPERTY_HINT_RESOURCE_TYPE, "Shape3D"), "set_corner_shape", "get_corner_shape");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "face_shape_scale"), "set_face_shape_scale", "get_face_shape_scale");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "edge_shape_scale"), "set_edge_shape_scale", "get_edge_shape_scale");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "corner_shape_scale"), "set_corner_shape_scale", "get_corner_shape_scale");
}

VoxelShapeData::VoxelShapeData() {
	brick_storage.reset(dimensions);
}

int VoxelShapeData::_get_expected_voxel_count() const {
	return dimensions.x * dimensions.y * dimensions.z;
}

bool VoxelShapeData::_validate_binary_array(const PackedByteArray &p_voxels) const {
	if (p_voxels.size() != _get_expected_voxel_count()) {
		return false;
	}
	for (uint8_t value : p_voxels) {
		if (value > 1) {
			return false;
		}
	}
	return true;
}

uint8_t VoxelShapeData::_get_surface_mask(const Vector3i &p_position) const {
	if (!is_solid(p_position)) {
		return 0;
	}
	uint8_t mask = 0;
	for (int direction = 0; direction < 6; direction++) {
		if (!is_solid(p_position + SURFACE_DIRECTIONS[direction])) {
			mask |= uint8_t(1 << direction);
		}
	}
	return mask;
}

int VoxelShapeData::_get_surface_class(const Vector3i &p_position, uint8_t *r_mask) const {
	const uint8_t mask = _get_surface_mask(p_position);
	if (r_mask) {
		*r_mask = mask;
	}
	int exposed = 0;
	for (uint8_t bits = mask; bits != 0; bits >>= 1) {
		exposed += bits & 1;
	}
	return exposed == 1 ? 1 : (exposed == 2 ? 2 : (exposed >= 3 ? 3 : 0));
}

void VoxelShapeData::_build_topology_array(int p_surface_class, bool p_masks, PackedByteArray &r_result) const {
	r_result.resize(_get_expected_voxel_count());
	r_result.fill(0);
	for (int z = 0; z < dimensions.z; z++) {
		for (int y = 0; y < dimensions.y; y++) {
			for (int x = 0; x < dimensions.x; x++) {
				const Vector3i position(x, y, z);
				uint8_t mask = 0;
				if (_get_surface_class(position, &mask) == p_surface_class) {
					r_result.set(get_voxel_index(position), p_masks ? mask : 1);
				}
			}
		}
	}
}

void VoxelShapeData::_mark_voxel_dirty(const Vector3i &p_position) {
	if (!edit_pending) {
		dirty_min = p_position;
		dirty_max = p_position;
		edit_pending = true;
	} else {
		dirty_min.x = MIN(dirty_min.x, p_position.x);
		dirty_min.y = MIN(dirty_min.y, p_position.y);
		dirty_min.z = MIN(dirty_min.z, p_position.z);
		dirty_max.x = MAX(dirty_max.x, p_position.x);
		dirty_max.y = MAX(dirty_max.y, p_position.y);
		dirty_max.z = MAX(dirty_max.z, p_position.z);
	}
}

void VoxelShapeData::_mark_region_dirty(const Vector3i &p_position, const Vector3i &p_size) {
	if (p_size.x <= 0 || p_size.y <= 0 || p_size.z <= 0) {
		return;
	}
	_mark_voxel_dirty(p_position);
	_mark_voxel_dirty(p_position + p_size - Vector3i(1, 1, 1));
}

void VoxelShapeData::_flush_edits() {
	if (!edit_pending || edit_depth > 0) {
		return;
	}
	edit_pending = false;
	revision++;
	const Vector3i changed_position = dirty_min;
	const Vector3i changed_size = dirty_max - dirty_min + Vector3i(1, 1, 1);
	emit_signal(SNAME("voxels_changed"), changed_position, changed_size, int64_t(revision));
	emit_changed();
}

void VoxelShapeData::begin_edit() {
	edit_depth++;
}

void VoxelShapeData::end_edit() {
	ERR_FAIL_COND_MSG(edit_depth <= 0, "end_edit() called without a matching begin_edit().");
	edit_depth--;
	_flush_edits();
}

int VoxelShapeData::get_voxel(const Vector3i &p_position) const {
	return brick_storage.get_voxel(p_position);
}

bool VoxelShapeData::set_voxel(const Vector3i &p_position, int p_palette_index) {
	ERR_FAIL_COND_V_MSG(!is_inside(p_position), false, "Voxel position is outside VoxelShapeData dimensions.");
	ERR_FAIL_COND_V_MSG(p_palette_index < 0 || p_palette_index > 255, false, "palette_index must be in the range 0 - 255.");
	if (!brick_storage.set_voxel(p_position, uint8_t(p_palette_index))) {
		return false;
	}
	_mark_voxel_dirty(p_position);
	_flush_edits();
	return true;
}

int VoxelShapeData::apply_voxel_edits(const Array &p_positions, const PackedByteArray &p_palette_indices) {
	ERR_FAIL_COND_V_MSG(p_positions.size() != p_palette_indices.size(), 0, "positions and palette_indices must have the same length.");
	begin_edit();
	int changed = 0;
	for (int i = 0; i < p_positions.size(); i++) {
		const Vector3i position = p_positions[i];
		if (!is_inside(position)) {
			continue;
		}
		changed += set_voxel(position, p_palette_indices[i]) ? 1 : 0;
	}
	end_edit();
	return changed;
}

int VoxelShapeData::apply_voxel_edits_by_index(const PackedInt32Array &p_indices, const PackedByteArray &p_palette_indices) {
	ERR_FAIL_COND_V_MSG(p_indices.size() != p_palette_indices.size(), 0, "indices and palette_indices must have the same length.");
	begin_edit();
	int changed = 0;
	const int plane = dimensions.x * dimensions.y;
	for (int i = 0; i < p_indices.size(); i++) {
		const int index = p_indices[i];
		if (index < 0 || index >= _get_expected_voxel_count()) {
			continue;
		}
		const Vector3i position(index % dimensions.x, (index / dimensions.x) % dimensions.y, index / plane);
		changed += set_voxel(position, p_palette_indices[i]) ? 1 : 0;
	}
	end_edit();
	return changed;
}

int VoxelShapeData::fill_voxel_region(const Vector3i &p_position, const Vector3i &p_size, int p_palette_index) {
	ERR_FAIL_COND_V_MSG(p_size.x < 0 || p_size.y < 0 || p_size.z < 0, 0, "Region size cannot be negative.");
	ERR_FAIL_COND_V_MSG(p_palette_index < 0 || p_palette_index > 255, 0, "palette_index must be in the range 0..255.");
	const Vector3i from(MAX(0, p_position.x), MAX(0, p_position.y), MAX(0, p_position.z));
	const Vector3i requested_end = p_position + p_size;
	const Vector3i to(MIN(dimensions.x, requested_end.x), MIN(dimensions.y, requested_end.y), MIN(dimensions.z, requested_end.z));
	if (from.x >= to.x || from.y >= to.y || from.z >= to.z) {
		return 0;
	}
	const int changed = brick_storage.fill_region(from, to - from, uint8_t(p_palette_index));
	if (changed > 0) {
		_mark_region_dirty(from, to - from);
		_flush_edits();
	}
	return changed;
}

uint64_t VoxelShapeData::get_revision() const { return revision; }
Vector3i VoxelShapeData::get_last_dirty_position() const { return dirty_min; }
Vector3i VoxelShapeData::get_last_dirty_size() const { return dirty_max - dirty_min + Vector3i(1, 1, 1); }

void VoxelShapeData::set_dimensions(const Vector3i &p_dimensions) {
	ERR_FAIL_COND_MSG(p_dimensions.x <= 0 || p_dimensions.y <= 0 || p_dimensions.z <= 0, "VoxelShapeData dimensions must be positive.");
	if (dimensions == p_dimensions) {
		return;
	}
	dimensions = p_dimensions;
	brick_storage.reset(dimensions);
	_mark_region_dirty(Vector3i(), dimensions);
	_flush_edits();
}

Vector3i VoxelShapeData::get_dimensions() const { return dimensions; }

void VoxelShapeData::set_voxel_size(real_t p_voxel_size) {
	ERR_FAIL_COND_MSG(p_voxel_size <= 0.0, "VoxelShapeData voxel_size must be greater than zero.");
	if (Math::is_equal_approx(voxel_size, p_voxel_size)) {
		return;
	}
	voxel_size = p_voxel_size;
	emit_changed();
}

real_t VoxelShapeData::get_voxel_size() const { return voxel_size; }

void VoxelShapeData::set_solid_voxels(const PackedByteArray &p_voxels) {
	ERR_FAIL_COND_MSG(!_validate_binary_array(p_voxels), "solid_voxels must match dimensions and contain only 0 or 1.");
	PackedByteArray dense = brick_storage.export_dense();
	for (int i = 0; i < dense.size(); i++) {
		dense.set(i, p_voxels[i] == 0 ? 0 : (dense[i] == 0 ? 1 : dense[i]));
	}
	set_voxel_data(dense);
}

PackedByteArray VoxelShapeData::get_solid_voxels() const {
	PackedByteArray dense = brick_storage.export_dense();
	for (int i = 0; i < dense.size(); i++) {
		dense.set(i, dense[i] == 0 ? 0 : 1);
	}
	return dense;
}

void VoxelShapeData::set_voxel_data(const PackedByteArray &p_voxels) {
	ERR_FAIL_COND_MSG(p_voxels.size() != _get_expected_voxel_count(), "voxel_data must contain exactly one palette byte per voxel.");
	brick_storage.import_dense(p_voxels, dimensions);
	_mark_region_dirty(Vector3i(), dimensions);
	_flush_edits();
}

PackedByteArray VoxelShapeData::get_voxel_data() const { return brick_storage.export_dense(); }

void VoxelShapeData::set_sparse_brick_data(const PackedByteArray &p_data) {
	if (p_data.is_empty()) {
		brick_storage.reset(dimensions);
	} else {
		ERR_FAIL_COND_MSG(!brick_storage.deserialize_sparse(p_data, dimensions), "Invalid sparse VoxelShapeData brick stream.");
	}
	_mark_region_dirty(Vector3i(), dimensions);
	_flush_edits();
}

PackedByteArray VoxelShapeData::get_sparse_brick_data() const { return brick_storage.serialize_sparse(); }

void VoxelShapeData::set_palette_texture(const Ref<Texture2D> &p_texture) {
	if (palette_texture == p_texture) return;
	palette_texture = p_texture;
	emit_changed();
}
Ref<Texture2D> VoxelShapeData::get_palette_texture() const { return palette_texture; }

void VoxelShapeData::set_material_texture(const Ref<Texture2D> &p_texture) {
	if (material_texture == p_texture) return;
	material_texture = p_texture;
	emit_changed();
}
Ref<Texture2D> VoxelShapeData::get_material_texture() const { return material_texture; }

#define VOXEL_TEXTURE_ACCESSORS(name) \
	void VoxelShapeData::set_##name##_texture(const Ref<Texture2D> &p_texture) { \
		if (name##_texture == p_texture) return; \
		name##_texture = p_texture; \
		emit_changed(); \
	} \
	Ref<Texture2D> VoxelShapeData::get_##name##_texture() const { return name##_texture; }

VOXEL_TEXTURE_ACCESSORS(metallic)
VOXEL_TEXTURE_ACCESSORS(transparency)
VOXEL_TEXTURE_ACCESSORS(specularity)
VOXEL_TEXTURE_ACCESSORS(emission)

#undef VOXEL_TEXTURE_ACCESSORS

void VoxelShapeData::set_face_voxels(const PackedByteArray &p_voxels) { ERR_FAIL_COND_MSG(!_validate_binary_array(p_voxels), "face_voxels must match dimensions and contain only 0 or 1."); }
PackedByteArray VoxelShapeData::get_face_voxels() const { PackedByteArray result; _build_topology_array(1, false, result); return result; }
void VoxelShapeData::set_edge_voxels(const PackedByteArray &p_voxels) { ERR_FAIL_COND_MSG(!_validate_binary_array(p_voxels), "edge_voxels must match dimensions and contain only 0 or 1."); }
PackedByteArray VoxelShapeData::get_edge_voxels() const { PackedByteArray result; _build_topology_array(2, false, result); return result; }
void VoxelShapeData::set_corner_voxels(const PackedByteArray &p_voxels) { ERR_FAIL_COND_MSG(!_validate_binary_array(p_voxels), "corner_voxels must match dimensions and contain only 0 or 1."); }
PackedByteArray VoxelShapeData::get_corner_voxels() const { PackedByteArray result; _build_topology_array(3, false, result); return result; }

void VoxelShapeData::set_face_masks(const PackedByteArray &p_masks) { ERR_FAIL_COND_MSG(p_masks.size() != _get_expected_voxel_count(), "face_masks must match dimensions."); }
PackedByteArray VoxelShapeData::get_face_masks() const { PackedByteArray result; _build_topology_array(1, true, result); return result; }
void VoxelShapeData::set_edge_masks(const PackedByteArray &p_masks) { ERR_FAIL_COND_MSG(p_masks.size() != _get_expected_voxel_count(), "edge_masks must match dimensions."); }
PackedByteArray VoxelShapeData::get_edge_masks() const { PackedByteArray result; _build_topology_array(2, true, result); return result; }
void VoxelShapeData::set_corner_masks(const PackedByteArray &p_masks) { ERR_FAIL_COND_MSG(p_masks.size() != _get_expected_voxel_count(), "corner_masks must match dimensions."); }
PackedByteArray VoxelShapeData::get_corner_masks() const { PackedByteArray result; _build_topology_array(3, true, result); return result; }
uint8_t VoxelShapeData::get_face_mask(const Vector3i &p_position) const { uint8_t mask = 0; return _get_surface_class(p_position, &mask) == 1 ? mask : 0; }
uint8_t VoxelShapeData::get_edge_mask(const Vector3i &p_position) const { uint8_t mask = 0; return _get_surface_class(p_position, &mask) == 2 ? mask : 0; }
uint8_t VoxelShapeData::get_corner_mask(const Vector3i &p_position) const { uint8_t mask = 0; return _get_surface_class(p_position, &mask) == 3 ? mask : 0; }

void VoxelShapeData::set_face_shape(const Ref<Shape3D> &p_shape) {
	if (face_shape == p_shape) return;
	if (face_shape.is_valid()) face_shape->disconnect_changed(callable_mp(this, &VoxelShapeData::_face_shape_changed));
	face_shape = p_shape;
	if (face_shape.is_valid()) face_shape->connect_changed(callable_mp(this, &VoxelShapeData::_face_shape_changed));
	emit_changed();
}
Ref<Shape3D> VoxelShapeData::get_face_shape() const { return face_shape; }
void VoxelShapeData::set_edge_shape(const Ref<Shape3D> &p_shape) {
	if (edge_shape == p_shape) return;
	if (edge_shape.is_valid()) edge_shape->disconnect_changed(callable_mp(this, &VoxelShapeData::_edge_shape_changed));
	edge_shape = p_shape;
	if (edge_shape.is_valid()) edge_shape->connect_changed(callable_mp(this, &VoxelShapeData::_edge_shape_changed));
	emit_changed();
}
Ref<Shape3D> VoxelShapeData::get_edge_shape() const { return edge_shape; }
void VoxelShapeData::set_corner_shape(const Ref<Shape3D> &p_shape) {
	if (corner_shape == p_shape) return;
	if (corner_shape.is_valid()) corner_shape->disconnect_changed(callable_mp(this, &VoxelShapeData::_corner_shape_changed));
	corner_shape = p_shape;
	if (corner_shape.is_valid()) corner_shape->connect_changed(callable_mp(this, &VoxelShapeData::_corner_shape_changed));
	emit_changed();
}
Ref<Shape3D> VoxelShapeData::get_corner_shape() const { return corner_shape; }
void VoxelShapeData::_face_shape_changed() { emit_changed(); }
void VoxelShapeData::_edge_shape_changed() { emit_changed(); }
void VoxelShapeData::_corner_shape_changed() { emit_changed(); }

bool VoxelShapeData::is_inside(const Vector3i &p_position) const { return brick_storage.is_inside(p_position); }
int VoxelShapeData::get_voxel_index(const Vector3i &p_position) const {
	ERR_FAIL_COND_V_MSG(!is_inside(p_position), -1, "Voxel position is outside VoxelShapeData dimensions.");
	return p_position.x + p_position.y * dimensions.x + p_position.z * dimensions.x * dimensions.y;
}
bool VoxelShapeData::is_solid(const Vector3i &p_position) const { return brick_storage.get_voxel(p_position) != 0; }
bool VoxelShapeData::is_face(const Vector3i &p_position) const { return _get_surface_class(p_position) == 1; }
bool VoxelShapeData::is_edge(const Vector3i &p_position) const { return _get_surface_class(p_position) == 2; }
bool VoxelShapeData::is_corner(const Vector3i &p_position) const { return _get_surface_class(p_position) == 3; }

void VoxelShapeData::set_face_shape_scale(const Vector3 &p_scale) {
	ERR_FAIL_COND_MSG(p_scale.x <= 0.0 || p_scale.y <= 0.0 || p_scale.z <= 0.0, "face_shape_scale components must be positive.");
	if (face_shape_scale == p_scale) return;
	face_shape_scale = p_scale; emit_changed();
}
Vector3 VoxelShapeData::get_face_shape_scale() const { return face_shape_scale; }
void VoxelShapeData::set_edge_shape_scale(const Vector3 &p_scale) {
	ERR_FAIL_COND_MSG(p_scale.x <= 0.0 || p_scale.y <= 0.0 || p_scale.z <= 0.0, "edge_shape_scale components must be positive.");
	if (edge_shape_scale == p_scale) return;
	edge_shape_scale = p_scale; emit_changed();
}
Vector3 VoxelShapeData::get_edge_shape_scale() const { return edge_shape_scale; }
void VoxelShapeData::set_corner_shape_scale(const Vector3 &p_scale) {
	ERR_FAIL_COND_MSG(p_scale.x <= 0.0 || p_scale.y <= 0.0 || p_scale.z <= 0.0, "corner_shape_scale components must be positive.");
	if (corner_shape_scale == p_scale) return;
	corner_shape_scale = p_scale; emit_changed();
}
Vector3 VoxelShapeData::get_corner_shape_scale() const { return corner_shape_scale; }
