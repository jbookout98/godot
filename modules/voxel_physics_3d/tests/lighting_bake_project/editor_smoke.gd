@tool
extends EditorPlugin

const CACHE := "res://editor_smoke.vlight"
var ticks := 0
var failed := false
var cancel_seen := false

func _enter_tree() -> void:
	set_process(true)

func _process(_delta: float) -> void:
	ticks += 1
	if ticks < 120: return
	set_process(false)
	run()

func frames(count: int) -> void:
	for frame in count: await get_tree().process_frame

func check(value: bool, message: String) -> void:
	if not value:
		failed = true
		push_error("LIGHTING_NATIVE_EDITOR_FAILED " + message)

func run() -> void:
	EditorInterface.open_scene_from_path("res://export_fixture.tscn")
	await frames(120)
	var scene := EditorInterface.get_edited_scene_root()
	var baker := scene.get_node("LightingBake") as VoxelLightingBake3D
	EditorInterface.set_main_screen_editor("3D")
	EditorInterface.edit_node(baker)
	await frames(20)
	var button: Button
	for candidate in EditorInterface.get_base_control().find_children("*", "Button", true, false):
		if candidate.text == "Bake Voxel Lighting": button = candidate
	check(button != null and button.is_visible_in_tree(), "bake toolbar missing")
	if failed: get_tree().quit(1); return
	var verify_only := "--editor-verify" in OS.get_cmdline_user_args()
	if not verify_only:
		baker.lighting_data = null
		button.pressed.emit()
		await frames(3)
		var dialog: EditorFileDialog
		for candidate in get_tree().root.find_children("*", "EditorFileDialog", true, false):
			if candidate.title == "Select voxel lighting bake file:": dialog = candidate
		check(dialog != null and dialog.visible, "first-bake file dialog missing")
		if failed: get_tree().quit(1); return
		dialog.file_selected.emit(CACHE)
		await frames(10)
	check(baker.lighting_data != null, "Lighting Data is unassigned")
	if failed: get_tree().quit(1); return
	check(baker.lighting_data.resource_path == CACHE, "resource path was not retained")
	check(FileAccess.file_exists(CACHE), "baked file is missing")
	check(baker.lighting_data.validate().is_empty(), "baked data is invalid")
	var picker_matches := false
	for picker in EditorInterface.get_inspector().find_children("*", "EditorResourcePicker", true, false):
		if picker.get_edited_resource() == baker.lighting_data: picker_matches = true
	check(picker_matches, "Inspector resource picker still shows no Lighting Data")
	if not verify_only:
		var previous := baker.lighting_data
		button.pressed.emit()
		await frames(10)
		check(baker.lighting_data != previous and baker.lighting_data.resource_path == CACHE, "rebake did not replace data at its existing path")
		check(EditorInterface.save_scene() == OK, "scene save failed")
		await frames(20)
		var contents := FileAccess.get_file_as_string("res://export_fixture.tscn")
		check(contents.contains(CACHE) and contents.contains("lighting_data = ExtResource"), "scene did not persist the assigned external resource")
		previous = baker.lighting_data
		var bytes := FileAccess.get_file_as_bytes(CACHE)
		get_tree().create_timer(0.35).timeout.connect(cancel_progress)
		button.pressed.emit()
		await frames(10)
		check(cancel_seen, "native progress cancellation was not exercised")
		check(baker.lighting_data == previous and FileAccess.get_file_as_bytes(CACHE) == bytes, "cancellation changed saved or assigned data")
	print("LIGHTING_NATIVE_EDITOR_", "FAILED" if failed else "OK", " verify_only=", verify_only, " path=", baker.lighting_data.resource_path, " probes=", baker.lighting_data.get_probe_count())
	get_tree().quit(1 if failed else 0)

func cancel_progress() -> void:
	for button in get_tree().root.find_children("*", "Button", true, false):
		if button.text == "Cancel" and button.is_visible_in_tree():
			cancel_seen = true
			button.pressed.emit()
			return
