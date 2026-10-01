#pragma once
#include "editor/plugins/editor_plugin.h"

#include "modules/voxel_physics_3d/voxel_lighting_bake_3d.h"

class Button;
class EditorFileDialog;

class VoxelLightingBakeEditorPlugin : public EditorPlugin {
	GDCLASS(VoxelLightingBakeEditorPlugin, EditorPlugin);
	Button *bake_button = nullptr;
	EditorFileDialog *file_dialog = nullptr;
	ObjectID edited;
	ObjectID path_target;
	bool baking = false;
	String completion_error;
	void _choose_path();
	void _start(const String &p_path);
	void _bake_completed(const String &p_error) { completion_error = p_error; }

public:
	bool handles(Object *p_object) const override;
	void edit(Object *p_object) override;
	void make_visible(bool p_visible) override;
	String get_plugin_name() const override { return "VoxelLightingBake"; }
	VoxelLightingBakeEditorPlugin();
};
