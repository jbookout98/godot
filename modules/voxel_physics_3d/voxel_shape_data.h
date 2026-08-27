#pragma once

#include "core/io/resource.h"
#include "core/math/transform_3d.h"
#include "scene/resources/3d/shape_3d.h"
#include "scene/resources/texture.h"
#include "voxel_brick_storage.h"

class VoxelShapeData : public Resource {
	GDCLASS(VoxelShapeData, Resource);

private:
	Vector3i dimensions = Vector3i(1, 1, 1);
	real_t voxel_size = 0.1;
	VoxelBrickStorage brick_storage;

	Ref<Shape3D> face_shape;
	Ref<Shape3D> edge_shape;
	Ref<Shape3D> corner_shape;
	Vector3 face_shape_scale = Vector3(1, 1, 1);
	Vector3 edge_shape_scale = Vector3(1, 1, 1);
	Vector3 corner_shape_scale = Vector3(1, 1, 1);

	// Palette bindings are per resource, so every volume owns its own palette.
	Ref<Texture2D> palette_texture;
	Ref<Texture2D> material_texture;
	Ref<Texture2D> metallic_texture;
	Ref<Texture2D> transparency_texture;
	Ref<Texture2D> specularity_texture;
	Ref<Texture2D> emission_texture;

	int edit_depth = 0;
	bool edit_pending = false;
	Vector3i dirty_min;
	Vector3i dirty_max;
	uint64_t revision = 0;

	int _get_expected_voxel_count() const;
	bool _validate_binary_array(const PackedByteArray &p_voxels) const;
	uint8_t _get_surface_mask(const Vector3i &p_position) const;
	int _get_surface_class(const Vector3i &p_position, uint8_t *r_mask = nullptr) const;
	void _build_topology_array(int p_surface_class, bool p_masks, PackedByteArray &r_result) const;
	void _mark_voxel_dirty(const Vector3i &p_position);
	void _mark_region_dirty(const Vector3i &p_position, const Vector3i &p_size);
	void _flush_edits();
	void _face_shape_changed();
	void _edge_shape_changed();
	void _corner_shape_changed();

protected:
	static void _bind_methods();

public:
	int get_surface_class(const Vector3i &p_position, uint8_t *r_mask = nullptr) const { return _get_surface_class(p_position, r_mask); }
	VoxelShapeData();

	bool is_inside(const Vector3i &p_position) const;
	int get_voxel_index(const Vector3i &p_position) const;
	int get_voxel(const Vector3i &p_position) const;
	bool set_voxel(const Vector3i &p_position, int p_palette_index);
	PackedInt32Array collect_editable_sphere_indices(const Vector3 &p_center, real_t p_radius, int p_action, int p_palette_index) const;
	PackedInt32Array collect_editable_cuboid_indices(const Transform3D &p_volume_transform, real_t p_voxel_size, const Vector3 &p_center, const Basis &p_shape_basis, const Vector3 &p_half_extents, const Vector3i &p_minimum, const Vector3i &p_maximum, int p_action, int p_palette_index) const;
	Array collect_connected_surface_positions(const Vector3i &p_start, const Vector3i &p_surface_normal, int p_radius, bool p_add_layer) const;
	int apply_voxel_edits(const Array &p_positions, const PackedByteArray &p_palette_indices);
	int apply_voxel_edits_by_index(const PackedInt32Array &p_indices, const PackedByteArray &p_palette_indices);
	int fill_voxel_region(const Vector3i &p_position, const Vector3i &p_size, int p_palette_index);
	void begin_edit();
	void end_edit();
	uint64_t get_revision() const;
	Vector3i get_last_dirty_position() const;
	Vector3i get_last_dirty_size() const;

	bool is_solid(const Vector3i &p_position) const;
	bool is_face(const Vector3i &p_position) const;
	bool is_edge(const Vector3i &p_position) const;
	bool is_corner(const Vector3i &p_position) const;

	void set_dimensions(const Vector3i &p_dimensions);
	Vector3i get_dimensions() const;
	void set_voxel_size(real_t p_size);
	real_t get_voxel_size() const;

	// Dense compatibility API. Dense arrays are materialized only when requested.
	void set_solid_voxels(const PackedByteArray &p_voxels);
	PackedByteArray get_solid_voxels() const;
	void set_voxel_data(const PackedByteArray &p_voxels);
	PackedByteArray get_voxel_data() const;
	void set_sparse_brick_data(const PackedByteArray &p_data);
	PackedByteArray get_sparse_brick_data() const;

	void set_palette_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_palette_texture() const;
	void set_material_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_material_texture() const;
	void set_metallic_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_metallic_texture() const;
	void set_transparency_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_transparency_texture() const;
	void set_specularity_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_specularity_texture() const;
	void set_emission_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_emission_texture() const;

	// Legacy topology properties remain load-compatible; topology is now derived.
	void set_face_voxels(const PackedByteArray &p_voxels);
	PackedByteArray get_face_voxels() const;
	void set_edge_voxels(const PackedByteArray &p_voxels);
	PackedByteArray get_edge_voxels() const;
	void set_corner_voxels(const PackedByteArray &p_voxels);
	PackedByteArray get_corner_voxels() const;
	void set_face_masks(const PackedByteArray &p_masks);
	PackedByteArray get_face_masks() const;
	void set_edge_masks(const PackedByteArray &p_masks);
	PackedByteArray get_edge_masks() const;
	void set_corner_masks(const PackedByteArray &p_masks);
	PackedByteArray get_corner_masks() const;
	uint8_t get_face_mask(const Vector3i &p_position) const;
	uint8_t get_edge_mask(const Vector3i &p_position) const;
	uint8_t get_corner_mask(const Vector3i &p_position) const;

	void set_face_shape(const Ref<Shape3D> &p_shape);
	Ref<Shape3D> get_face_shape() const;
	void set_edge_shape(const Ref<Shape3D> &p_shape);
	Ref<Shape3D> get_edge_shape() const;
	void set_corner_shape(const Ref<Shape3D> &p_shape);
	Ref<Shape3D> get_corner_shape() const;
	void set_face_shape_scale(const Vector3 &p_scale);
	Vector3 get_face_shape_scale() const;
	void set_edge_shape_scale(const Vector3 &p_scale);
	Vector3 get_edge_shape_scale() const;
	void set_corner_shape_scale(const Vector3 &p_scale);
	Vector3 get_corner_shape_scale() const;

	const VoxelBrickStorage &get_brick_storage() const { return brick_storage; }
	VoxelBrickStorage &get_brick_storage_write() { return brick_storage; }
};
