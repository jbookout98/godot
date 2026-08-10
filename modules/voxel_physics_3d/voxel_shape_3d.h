#pragma once

#include "scene/resources/3d/shape_3d.h"
#include "voxel_shape_data.h"

class VoxelShape3D : public Shape3D {
	GDCLASS(VoxelShape3D, Shape3D);

	Ref<VoxelShapeData> voxel_data;
	void _voxel_data_changed();

protected:
	static void _bind_methods();
	void _update_shape() override;

public:
	void set_voxel_data(const Ref<VoxelShapeData> &p_data);
	Ref<VoxelShapeData> get_voxel_data() const;

	Vector<Vector3> get_debug_mesh_lines() const override;
	Ref<ArrayMesh> get_debug_arraymesh_faces(const Color &p_modulate) const override;
	real_t get_enclosing_radius() const override;

	VoxelShape3D();
};
