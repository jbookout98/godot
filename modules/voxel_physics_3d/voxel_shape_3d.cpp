#include "voxel_shape_3d.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server_enums.h"

void VoxelShape3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_voxel_data", "voxel_data"), &VoxelShape3D::set_voxel_data);
	ClassDB::bind_method(D_METHOD("get_voxel_data"), &VoxelShape3D::get_voxel_data);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "voxel_data", PROPERTY_HINT_RESOURCE_TYPE, "VoxelShapeData"), "set_voxel_data", "get_voxel_data");
}

void VoxelShape3D::_update_shape() {
	PhysicsServer3D::get_singleton()->shape_set_data(get_shape(), voxel_data);
	Shape3D::_update_shape();
}

void VoxelShape3D::_voxel_data_changed() {
	_update_shape();
	emit_changed();
}

void VoxelShape3D::set_voxel_data(const Ref<VoxelShapeData> &p_data) {
	if (voxel_data == p_data) {
		return;
	}
	if (voxel_data.is_valid()) {
		voxel_data->disconnect_changed(callable_mp(this, &VoxelShape3D::_voxel_data_changed));
	}
	voxel_data = p_data;
	if (voxel_data.is_valid()) {
		voxel_data->connect_changed(callable_mp(this, &VoxelShape3D::_voxel_data_changed));
	}
	_update_shape();
	emit_changed();
}

Ref<VoxelShapeData> VoxelShape3D::get_voxel_data() const {
	return voxel_data;
}

Vector<Vector3> VoxelShape3D::get_debug_mesh_lines() const {
	Vector<Vector3> lines;
	if (voxel_data.is_null()) {
		return lines;
	}
	AABB bounds(Vector3(), Vector3(voxel_data->get_dimensions()) * voxel_data->get_voxel_size());
	for (int i = 0; i < 12; i++) {
		Vector3 a;
		Vector3 b;
		bounds.get_edge(i, a, b);
		lines.push_back(a);
		lines.push_back(b);
	}
	return lines;
}

Ref<ArrayMesh> VoxelShape3D::get_debug_arraymesh_faces(const Color &p_modulate) const {
	if (voxel_data.is_null()) {
		return Ref<ArrayMesh>();
	}
	const Vector3 size = Vector3(voxel_data->get_dimensions()) * voxel_data->get_voxel_size();
	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	BoxMesh::create_mesh_array(arrays, size);
	PackedVector3Array vertices = arrays[RSE::ARRAY_VERTEX];
	for (int i = 0; i < vertices.size(); i++) {
		vertices.set(i, vertices[i] + size * 0.5);
	}
	arrays[RSE::ARRAY_VERTEX] = vertices;
	PackedColorArray colors;
	colors.resize(vertices.size());
	colors.fill(p_modulate);
	arrays[RSE::ARRAY_COLOR] = colors;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

real_t VoxelShape3D::get_enclosing_radius() const {
	if (voxel_data.is_null()) {
		return 0.0;
	}
	return (Vector3(voxel_data->get_dimensions()) * voxel_data->get_voxel_size()).length();
}

VoxelShape3D::VoxelShape3D() :
		Shape3D(PhysicsServer3D::get_singleton()->shape_create(PhysicsServer3D::SHAPE_CUSTOM)) {
}
