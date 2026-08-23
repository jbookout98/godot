#pragma once

#include "editor/plugins/editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_gizmos.h"

class VoxelVolume3DGizmoPlugin : public EditorNode3DGizmoPlugin {
	GDCLASS(VoxelVolume3DGizmoPlugin, EditorNode3DGizmoPlugin);

public:
	bool has_gizmo(Node3D *p_spatial) override;
	String get_gizmo_name() const override;
	int get_priority() const override;
	void redraw(EditorNode3DGizmo *p_gizmo) override;

	VoxelVolume3DGizmoPlugin();
};

class VoxelVolume3DEditorPlugin : public EditorPlugin {
	GDCLASS(VoxelVolume3DEditorPlugin, EditorPlugin);

	Ref<VoxelVolume3DGizmoPlugin> gizmo_plugin;

public:
	String get_plugin_name() const override { return "VoxelVolume3D"; }
	bool handles(Object *p_object) const override;

	VoxelVolume3DEditorPlugin();
};
