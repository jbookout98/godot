#include "voxel_volume_3d_editor_plugin.h"

#include "core/math/triangle_mesh.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "modules/voxel_physics_3d/voxel_volume_3d.h"

VoxelVolume3DGizmoPlugin::VoxelVolume3DGizmoPlugin() {
	create_material("bounds", EDITOR_GET("editors/3d_gizmos/gizmo_colors/aabb"));
}

bool VoxelVolume3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<VoxelVolume3D>(p_spatial) != nullptr;
}

String VoxelVolume3DGizmoPlugin::get_gizmo_name() const {
	return "VoxelVolume3D";
}

int VoxelVolume3DGizmoPlugin::get_priority() const {
	return 0;
}

void VoxelVolume3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	VoxelVolume3D *volume = Object::cast_to<VoxelVolume3D>(p_gizmo->get_node_3d());
	ERR_FAIL_NULL(volume);
	p_gizmo->clear();

	const AABB bounds = volume->get_aabb();
	if (bounds.size.x <= 0.0 || bounds.size.y <= 0.0 || bounds.size.z <= 0.0) {
		return;
	}

	static constexpr int FACE_INDICES[36] = {
		0, 1, 2, 1, 3, 2,
		4, 6, 5, 5, 6, 7,
		0, 4, 1, 1, 4, 5,
		2, 3, 6, 3, 7, 6,
		0, 2, 4, 2, 6, 4,
		1, 5, 3, 3, 5, 7,
	};
	Vector<Vector3> faces;
	faces.resize(36);
	for (int index = 0; index < 36; index++) {
		faces.write[index] = bounds.get_endpoint(FACE_INDICES[index]);
	}
	Ref<TriangleMesh> collision;
	collision.instantiate();
	collision->create(faces);
	p_gizmo->add_collision_triangles(collision);

	if (p_gizmo->is_selected()) {
		Vector<Vector3> lines;
		for (int edge = 0; edge < 12; edge++) {
			Vector3 from;
			Vector3 to;
			bounds.get_edge(edge, from, to);
			lines.push_back(from);
			lines.push_back(to);
		}
		p_gizmo->add_lines(lines, get_material("bounds", p_gizmo));
	}
}

bool VoxelVolume3DEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<VoxelVolume3D>(p_object) != nullptr;
}

VoxelVolume3DEditorPlugin::VoxelVolume3DEditorPlugin() {
	gizmo_plugin.instantiate();
	Node3DEditor::get_singleton()->add_gizmo_plugin(gizmo_plugin);
}
