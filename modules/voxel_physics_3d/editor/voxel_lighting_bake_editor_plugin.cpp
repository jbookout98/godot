#include "voxel_lighting_bake_editor_plugin.h"

#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/inspector/editor_inspector.h"
#include "scene/gui/button.h"
#include "scene/main/scene_tree.h"

bool VoxelLightingBakeEditorPlugin::handles(Object *p_object) const {
	return Object::cast_to<VoxelLightingBake3D>(p_object) != nullptr;
}
void VoxelLightingBakeEditorPlugin::edit(Object *p_object) {
	edited = p_object ? p_object->get_instance_id() : ObjectID();
}
void VoxelLightingBakeEditorPlugin::make_visible(bool p_visible) {
	bake_button->set_visible(p_visible);
}
void VoxelLightingBakeEditorPlugin::_choose_path() {
	if (baking) {
		return;
	}
	auto *node = ObjectDB::get_instance<VoxelLightingBake3D>(edited);
	if (!node) {
		return;
	}
	path_target = edited;
	const Ref<VoxelLightingData> lighting = node->get_lighting_data();
	if (lighting.is_valid() && lighting->get_path().is_resource_file()) {
		const String path = lighting->get_path();
		if (FileAccess::exists(path + ".import")) {
			EditorNode::get_singleton()->show_warning("Baked lighting belongs to an imported resource. Assign a local lighting resource before rebaking.");
			return;
		}
		if (path.get_extension() == "vlight") {
			_start(path);
			return;
		}
	}
	Node *scene = get_tree()->get_edited_scene_root();
	String path = scene ? scene->get_scene_file_path() : String();
	path = path.is_empty() ? "res://" + String(node->get_name()) + ".vlight" : path.get_basename() + "." + String(node->get_name()) + ".vlight";
	file_dialog->set_current_path(path);
	file_dialog->popup_file_dialog();
}

static String bake_start_error(Error p_error) {
	switch (p_error) {
		case ERR_OUT_OF_MEMORY:
			return "Bake Bounds require more than 4,096 camera views. Reduce the region's size or probe density. Bounds size is width, height, and depth, not the maximum corner.";
		case ERR_INVALID_PARAMETER:
			return "Bake Bounds must have finite coordinates and positive width, height, and depth.";
		case ERR_INVALID_DATA:
			return "The authored source could not be fingerprinted. Check its voxel resources, material textures, and environment resources.";
		case ERR_BUSY:
			return "This node already has a bake in progress.";
		default:
			return "Enable voxel indirect lighting with the DDGI backend, and set Source Root to a scene subtree containing visible voxel geometry.";
	}
}

void VoxelLightingBakeEditorPlugin::_start(const String &p_path) {
	if (baking) {
		return;
	}
	const ObjectID target = path_target;
	auto *node = ObjectDB::get_instance<VoxelLightingBake3D>(target);
	if (!node) {
		return;
	}
	file_dialog->hide();
	completion_error = String();
	node->connect(SNAME("bake_finished"), callable_mp(this, &VoxelLightingBakeEditorPlugin::_bake_completed));
	const Error error = node->bake(p_path);
	if (error != OK) {
		node->disconnect(SNAME("bake_finished"), callable_mp(this, &VoxelLightingBakeEditorPlugin::_bake_completed));
		EditorNode::get_singleton()->show_warning("Cannot start voxel lighting bake: " + bake_start_error(error));
		return;
	}
	baking = true;
	bake_button->set_disabled(true);
	// Follow VoxelGI/LightmapGI's modal editor workflow. The renderer job is
	// asynchronous; the native progress dialog pumps frames and owns cancellation.
	{
		EditorProgress progress("bake_voxel_lighting", TTR("Bake Voxel Lighting"), 1000, true);
		while ((node = ObjectDB::get_instance<VoxelLightingBake3D>(target)) && node->is_baking()) {
			const bool cancel = progress.step(node->get_bake_description(), int(node->get_bake_progress() * 1000), true);
			node = ObjectDB::get_instance<VoxelLightingBake3D>(target);
			if (cancel && node) {
				node->cancel_bake();
			}
			OS::get_singleton()->delay_usec(1000);
		}
	}
	baking = false;
	bake_button->set_disabled(false);
	node = ObjectDB::get_instance<VoxelLightingBake3D>(target);
	if (!node) {
		return;
	}
	node->disconnect(SNAME("bake_finished"), callable_mp(this, &VoxelLightingBakeEditorPlugin::_bake_completed));
	if (!completion_error.is_empty()) {
		if (completion_error.begins_with("Bake cancelled")) {
			print_line(completion_error);
		} else {
			EditorNode::get_singleton()->show_warning(completion_error);
		}
		return;
	}
	EditorInterface::get_singleton()->mark_scene_as_unsaved();
	EditorInspector *inspector = EditorInterface::get_singleton()->get_inspector();
	if (inspector->get_edited_object() == node) {
		inspector->update_property("lighting_data");
	}
	print_line(vformat("Voxel lighting saved to %s and assigned to %s. Save the scene to retain the assignment.", p_path, node->get_name()));
}

VoxelLightingBakeEditorPlugin::VoxelLightingBakeEditorPlugin() {
	bake_button = memnew(Button);
	bake_button->set_theme_type_variation(SceneStringName(FlatButton));
	bake_button->set_button_icon(EditorNode::get_singleton()->get_editor_theme()->get_icon(SNAME("Bake"), EditorStringName(EditorIcons)));
	bake_button->set_text(TTR("Bake Voxel Lighting"));
	bake_button->hide();
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, bake_button);
	bake_button->connect(SNAME("pressed"), callable_mp(this, &VoxelLightingBakeEditorPlugin::_choose_path));
	file_dialog = memnew(EditorFileDialog);
	file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	file_dialog->add_filter("*.vlight", TTR("Voxel Lighting Bake"));
	file_dialog->set_title(TTR("Select voxel lighting bake file:"));
	bake_button->add_child(file_dialog);
	file_dialog->connect(SNAME("file_selected"), callable_mp(this, &VoxelLightingBakeEditorPlugin::_start));
}
