
#pragma once

#include "godot_shape_3d.h"
#include "voxel_contact_3d.h"
#include "voxel_shape_data.h"

struct VoxelFeatureProxy3D {
	GodotShape3D *shape = nullptr;
	Transform3D local_transform;
	bool is_valid() const { return shape != nullptr; }
};

class GodotVoxelShape3D : public GodotShape3D {
	struct PhysicsBrickCache {
		bool initialized = false;
		bool uniform_solid = false;
		uint64_t solid_bits[8] = {};
		uint64_t surface_bits[8] = {};
		uint64_t face_bits[8] = {};
		uint64_t edge_bits[8] = {};
		uint64_t corner_bits[8] = {};
		Vector<uint16_t> solid_local_indices;
		Vector<uint16_t> surface_local_indices;
		Vector<uint16_t> face_local_indices;
		Vector<uint16_t> edge_local_indices;
		Vector<uint16_t> corner_local_indices;
		int occupied_count = 0;
		Vector3 center_sum_grid;
		Vector3 center_squared_sum_grid;
	};

	Ref<VoxelShapeData> voxel_data;
	AABB local_aabb;
	VoxelFeatureProxy3D face_feature_table[64];
	VoxelFeatureProxy3D edge_feature_table[64];
	VoxelFeatureProxy3D corner_feature_table[64];
	GodotBoxShape3D default_cube_feature;
	GodotSphereShape3D default_corner_feature;
	mutable Vector<PhysicsBrickCache> brick_caches;
	int solid_count = 0;
	uint64_t cached_revision = UINT64_MAX;
	Vector3i cached_dimensions;
	mutable Vector3 center_of_mass;
	mutable Vector3 inertia_per_unit_mass;
	real_t occupied_volume = 0.0;
	mutable bool mass_properties_valid = false;

	void _build_feature_table(GodotShape3D *p_shape, const Vector3 &p_scale, VoxelFeatureProxy3D (&r_table)[64]);
	void _invalidate_brick_caches();
	void _ensure_brick_cache(int p_brick_index) const;
	void _ensure_all_brick_caches() const;
	void _rebuild_mass_properties() const;
	bool _brick_local_matches_feature(int p_brick_index, int p_local_index, int p_feature) const;
	bool _find_next_index(int p_feature, bool p_bounded, const Vector3i &p_from, const Vector3i &p_to, int &r_brick_index, int &r_cursor, int &r_index) const;

public:
	class IndexIterator {
		const GodotVoxelShape3D *shape = nullptr;
		int feature = 0;
		int brick_index = 0;
		int cursor = 0;
		int current = -1;
		bool bounded = false;
		Vector3i range_from;
		Vector3i range_to;

		void _advance();

	public:
		IndexIterator() = default;
		IndexIterator(const GodotVoxelShape3D *p_shape, int p_feature, bool p_bounded, const Vector3i &p_from, const Vector3i &p_to, bool p_end);
		int operator*() const { return current; }
		bool operator!=(const IndexIterator &p_other) const { return current != p_other.current || shape != p_other.shape; }
		IndexIterator &operator++() { _advance(); return *this; }
	};

	class IndexRange {
		const GodotVoxelShape3D *shape = nullptr;
		int feature = 0;
		bool bounded = false;
		Vector3i range_from;
		Vector3i range_to;

	public:
		IndexRange() = default;
		IndexRange(const GodotVoxelShape3D *p_shape, int p_feature) : shape(p_shape), feature(p_feature) {}
		IndexRange(const GodotVoxelShape3D *p_shape, int p_feature, const Vector3i &p_from, const Vector3i &p_to) : shape(p_shape), feature(p_feature), bounded(true), range_from(p_from), range_to(p_to) {}
		IndexIterator begin() const { return IndexIterator(shape, feature, bounded, range_from, range_to, false); }
		IndexIterator end() const { return IndexIterator(shape, feature, bounded, range_from, range_to, true); }
		bool is_empty() const;
	};

	PhysicsServer3D::ShapeType get_type() const override { return PhysicsServer3D::SHAPE_CUSTOM; }
	real_t get_volume() const override { return occupied_volume; }
	Vector3 get_center_of_mass() const override;
	void project_range(const Vector3 &p_normal, const Transform3D &p_transform, real_t &r_min, real_t &r_max) const override;
	void get_supports(const Vector3 &p_normal, int p_max, Vector3 *r_supports, int &r_amount, FeatureType &r_type) const override;
	Vector3 get_closest_point_to(const Vector3 &p_point) const override;
	bool intersect_segment(const Vector3 &p_begin, const Vector3 &p_end, Vector3 &r_point, Vector3 &r_normal, int &r_face_index, bool p_hit_back_faces) const override;
	bool intersect_point(const Vector3 &p_point) const override;
	Vector3 get_moment_of_inertia(real_t p_mass) const override;

	void set_data(const Variant &p_data) override;
	Variant get_data() const override;
	Ref<VoxelShapeData> get_voxel_data() const { return voxel_data; }
	void set_feature_shapes(GodotShape3D *p_face_shape, GodotShape3D *p_edge_shape, GodotShape3D *p_corner_shape);
	const VoxelFeatureProxy3D &get_feature_proxy(VoxelFeatureType p_feature, uint8_t p_mask) const;
	IndexRange get_feature_indices(VoxelFeatureType p_feature) const;
	IndexRange get_feature_indices(VoxelFeatureType p_feature, const Vector3i &p_from, const Vector3i &p_to) const;
	IndexRange get_solid_indices() const { return IndexRange(this, 0); }
	IndexRange get_solid_indices(const Vector3i &p_from, const Vector3i &p_to) const { return IndexRange(this, 0, p_from, p_to); }
	IndexRange get_surface_indices() const { return IndexRange(this, 4); }
	IndexRange get_surface_indices(const Vector3i &p_from, const Vector3i &p_to) const { return IndexRange(this, 4, p_from, p_to); }
	bool has_solids() const { return solid_count > 0; }
	bool is_edge_feature(const Vector3i &p_voxel) const;

	GodotVoxelShape3D();
};
