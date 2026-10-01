extends Node3D

const DATA_PATH := "res://fixture_lighting.vlight"
var failed := false
var baker: VoxelLightingBake3D
var floor_volume: VoxelVolume3D
var sun: DirectionalLight3D
var camera: Camera3D
var status: Dictionary = {}
var finished := false
var bake_error := ""
var mode := "bake"
var started := Time.get_ticks_usec()
var frame_count := 360
var frame_times: Array[float] = []
var last_frame_usec := 0
var measure_frames := false
var moving_actor: Node3D
var motion_time := 0.0

func _process(delta: float) -> void:
 if moving_actor:
  motion_time += delta
  moving_actor.rotation.y = motion_time

func _ready() -> void:
 for argument in OS.get_cmdline_user_args():
  if argument.begins_with("--mode="): mode = argument.trim_prefix("--mode=")
  if argument.begins_with("--frames="): frame_count = int(argument.trim_prefix("--frames="))
 call_deferred("run")

func check(condition: bool, message: String) -> void:
 if not condition:
  failed = true
  push_error("LIGHTING_BAKE_TEST_FAILED: " + message)

func volume(name_value: String, dimensions: Vector3i, position_value: Vector3, parent_value: Node = null) -> VoxelVolume3D:
 var data := VoxelShapeData.new()
 data.voxel_size = 0.25
 data.dimensions = dimensions
 data.fill_voxel_region(Vector3i.ZERO, dimensions, 1)
 var result := VoxelVolume3D.new()
 result.name = name_value
 result.voxel_data = data
 result.voxel_material = preload("res://material.tres")
 result.position = position_value
 result.streaming_mode = 2
 result.streaming_resident = true
 (parent_value if parent_value else self).add_child(result)
 return result

func fixture() -> void:
 floor_volume = volume("Floor", Vector3i(16, 1, 16), Vector3(0, -0.25, 0))
 volume("BackWall", Vector3i(16, 12, 1), Vector3(0, 0, 0))
 volume("SideWall", Vector3i(1, 12, 16), Vector3.ZERO)
 if mode in ["runtime_sun", "unbaked_runtime_sun"]:
  var runtime_sun := DirectionalLight3D.new()
  runtime_sun.name = "RuntimeSun"
  runtime_sun.add_to_group("voxel_bake_excluded")
  runtime_sun.rotation_degrees = Vector3(-35, 10, 0)
  runtime_sun.light_color = Color(0.3, 1.0, 0.4)
  runtime_sun.light_energy = 1.0
  runtime_sun.shadow_enabled = true
  add_child(runtime_sun)
 sun = DirectionalLight3D.new()
 sun.name = "Sun"
 sun.rotation_degrees = Vector3(-55, -30, 0)
 sun.light_energy = 1.5
 sun.shadow_enabled = true
 add_child(sun)
 camera = Camera3D.new()
 camera.name = "Camera"
 add_child(camera)
 camera.position = Vector3(3.5, 2, 5)
 camera.look_at(Vector3(1.5, 1, 1))
 camera.current = true
 if mode in ["runtime_inputs", "unbaked_runtime_inputs", "moving_startup"]:
  var actor := CharacterBody3D.new()
  actor.name = "RuntimeActor"
  add_child(actor)
  if mode == "moving_startup": moving_actor = actor
  volume("RuntimeVoxel", Vector3i(4, 6, 4), Vector3(2, 0, 2), actor)
  var lamp := OmniLight3D.new()
  lamp.position = Vector3(1.5, 1.5, 1.5)
  lamp.omni_range = 8.0
  lamp.light_color = Color(0.2, 1.0, 0.3)
  lamp.light_energy = 4.0
  actor.add_child(lamp)
 baker = VoxelLightingBake3D.new()
 baker.name = "LightingBake"
 baker.bake_bounds = AABB(Vector3(-0.5, -0.5, -0.5), Vector3(5, 4, 5))
 baker.settle_frames = 120
 baker.max_frames_per_view = 3600
 baker.bake_finished.connect(func(error: String): bake_error = error; finished = true)
 baker.bake_progress.connect(func(progress: float): print("BAKE_PROGRESS ", progress))
 baker.cache_status.connect(func(value: Dictionary): status = value)
 add_child(baker)

func frames(count: int) -> void:
 for frame in count:
  await get_tree().process_frame
  var now := Time.get_ticks_usec()
  if measure_frames and last_frame_usec > 0: frame_times.append((now - last_frame_usec) / 1000.0)
  last_frame_usec = now

func run() -> void:
 fixture()
 if mode == "unit":
  check(not VoxelLightingData.new().validate().is_empty(), "empty cache accepted")
  var signature := baker.compute_signature()
  check(signature.length() == 64, "missing stable fingerprint")
  sun.light_energy += 0.1
  check(baker.compute_signature() != signature, "light edit did not invalidate fingerprint")
  sun.light_energy -= 0.1
  var original_bounds := baker.bake_bounds
  baker.bake_bounds = AABB(Vector3(-512, -512, -512), Vector3(512, 512, 512))
  check(baker.bake() == ERR_OUT_OF_MEMORY, "oversized bake region was not rejected")
  baker.bake_bounds = AABB(Vector3.ZERO, Vector3.ZERO)
  check(baker.bake() == ERR_INVALID_PARAMETER, "zero-sized bake bounds were accepted")
  baker.bake_bounds = original_bounds
  var start_error := baker.bake()
  check(start_error == OK, "bake did not start")
  baker.cancel_bake()
  check(not baker.is_baking() and baker.lighting_data == null, "cancel published partial data")
 elif mode in ["bake", "repeat", "save_failure"]:
  var retained: VoxelLightingData
  if mode == "save_failure":
   retained = load(DATA_PATH)
   baker.lighting_data = retained
   check(baker.bake("user://missing_lighting_test_directory/failure.vlight") == OK, "save-failure bake could not start")
   var failure_deadline := Time.get_ticks_msec() + 240000
   while not finished and Time.get_ticks_msec() < failure_deadline: await get_tree().process_frame
   check(finished and bake_error.contains("Could not save"), "save failure was not reported")
   check(baker.lighting_data == retained and retained.resource_path == DATA_PATH, "failed save replaced the previous resource")
   print("LIGHTING_BAKE_TEST_", "FAILED" if failed else "OK", " mode=", mode)
   get_tree().quit(1 if failed else 0)
   return
  check(baker.bake() == OK, "could not start bake")
  var deadline := Time.get_ticks_msec() + 240000
  while not finished and Time.get_ticks_msec() < deadline: await get_tree().process_frame
  check(finished, "bake timeout")
  check(bake_error.is_empty(), bake_error)
  if mode == "repeat" and baker.lighting_data:
   var first_signature := baker.lighting_data.signature
   var previous := baker.lighting_data
   finished = false
   check(baker.bake() == OK, "repeated bake could not start")
   await frames(16)
   baker.cancel_bake()
   check(baker.lighting_data == previous, "cancellation replaced the previous cache")
   finished = false
   bake_error = ""
   check(baker.bake() == OK, "bake after cancellation could not start")
   deadline = Time.get_ticks_msec() + 240000
   while not finished and Time.get_ticks_msec() < deadline: await get_tree().process_frame
   check(finished and bake_error.is_empty(), "repeated bake failed: " + bake_error)
   check(baker.lighting_data != null and baker.lighting_data.signature == first_signature, "repeated bake changed source identity")
  if baker.lighting_data != null:
   check(baker.lighting_data.validate().is_empty(), "produced invalid data")
   check(ResourceSaver.save(baker.lighting_data, DATA_PATH, ResourceSaver.FLAG_COMPRESS) == OK, "save failed")
   baker.lighting_data.resource_path = DATA_PATH
   save_authored_fixture()
   print("BAKE_RESULT probes=", baker.lighting_data.get_probe_count(), " elapsed_ms=", (Time.get_ticks_usec()-started)/1000.0)
  else: check(false, "no lighting resource published")
 else:
  var data: VoxelLightingData
  if not mode.begins_with("unbaked"): data = load(DATA_PATH) as VoxelLightingData
  check(mode.begins_with("unbaked") or data != null, "missing bake fixture; run bake first")
  if data != null or mode.begins_with("unbaked"):
   if data: check(data.validate().is_empty(), "saved data is invalid")
   if mode == "stale":
    sun.light_energy += 1.0
   elif mode == "corrupt":
    data.probes = PackedByteArray([1, 2, 3])
   elif mode == "version":
    data.format_version = 10000
   if not mode.begins_with("unbaked"): baker.lighting_data = data

   if mode == "early_light":
    await frames(1)
    sun.light_energy = 0.25
   last_frame_usec = Time.get_ticks_usec()
   measure_frames = true
   await frames(frame_count)
   measure_frames = false
   var sorted := frame_times.duplicate()
   sorted.sort()
   print("BENCHMARK ", JSON.stringify({"mode": mode, "frames": frame_count, "elapsed_ms": (Time.get_ticks_usec()-started)/1000.0, "engine_elapsed_ms": Time.get_ticks_msec(), "median_frame_ms": sorted[sorted.size()/2], "p95_frame_ms": sorted[int(sorted.size()*0.95)], "max_frame_ms": sorted[-1], "static_memory": OS.get_static_memory_usage(), "video_memory": RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIDEO_MEM_USED), "cache_payload_bytes": data.probes.size() if data else 0, "cpu": OS.get_processor_name(), "gpu": RenderingServer.get_video_adapter_name()}))
   if mode in ["load", "unbaked", "runtime_inputs", "unbaked_runtime_inputs", "runtime_sun", "unbaked_runtime_sun"]:
    await RenderingServer.frame_post_draw
    print("SNAPSHOT ", JSON.stringify({"mode": mode, "frame": frame_count, "elapsed_ms": (Time.get_ticks_usec()-started)/1000.0, "engine_elapsed_ms": Time.get_ticks_msec()}))
    get_viewport().get_texture().get_image().save_png("user://lighting_" + mode + "_" + str(frame_count) + ".png")
   baker.request_cache_status(true)
   await frames(4)
   print("CACHE_STATUS ", JSON.stringify(status))
   if mode == "moving_startup": check(int(status.get("restored_probes", 0)) > 0, "continuous startup motion prevented cache restore")
   if frame_count >= 120 and mode in ["load", "mutate", "camera", "construct", "move", "light", "emission", "worlds", "views", "switch", "runtime_inputs", "local_light", "detach"]:
    check(int(status.get("restored_probes", 0)) > 0, "fresh process restored no baked probes")
   if mode in ["stale", "corrupt", "version"]:
    check(int(status.get("restored_probes", 0)) == 0, "invalid cache was applied")
   if mode in ["runtime_inputs", "runtime_sun"]: check(bool(status.get("refreshing_runtime_inputs", false)), "initial runtime inputs did not trigger warm-start refresh")
   if mode == "early_light": check(bool(status.get("invalidated", false)), "light edit during startup did not reject stale seeds")
   var change_mode := mode.trim_prefix("unbaked_")
   if change_mode in ["mutate", "construct", "move", "light", "local_light", "emission", "detach"]:
    match change_mode:
     "mutate": floor_volume.voxel_data.fill_voxel_region(Vector3i(4, 0, 4), Vector3i(8, 1, 8), 0)
     "construct": volume("Added", Vector3i(4, 8, 4), Vector3(2, 0, 2))
     "move": floor_volume.position += Vector3(0, 0.5, 0)
     "light": sun.light_energy = 0.2
     "local_light":
      var lamp := OmniLight3D.new()
      lamp.position = Vector3(1.5, 1.5, 1.5)
      lamp.omni_range = 8.0
      lamp.light_color = Color.RED
      lamp.light_energy = 4.0
      add_child(lamp)
     "emission": floor_volume.voxel_material = preload("res://emission_material.tres")
     "detach":
      remove_child(floor_volume)
      floor_volume.voxel_material.emit_changed()
      floor_volume.free()
    await frames(120)
    camera.position += Vector3(3, 0, 0)
    await frames(120)
    baker.request_cache_status()
    await frames(4)
    if not mode.begins_with("unbaked"):
     check(bool(status.get("invalidated", false)), "world edit allowed stale cache reseeding")
   if mode == "switch" and not get_tree().has_meta("lighting_scene_switched"):
    get_tree().set_meta("lighting_scene_switched", true)
    print("LIGHTING_SCENE_SWITCH_BEGIN")
    check(get_tree().reload_current_scene() == OK, "could not reload fixture")
    return
   if mode == "views":
    var view := SubViewport.new()
    view.size = Vector2i(96, 96)
    view.world_3d = get_world_3d()
    view.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    add_child(view)
    var second_camera := Camera3D.new()
    view.add_child(second_camera)
    second_camera.global_position = camera.global_position + Vector3(5, 0, 0)
    second_camera.global_rotation = camera.global_rotation
    second_camera.current = true
    await frames(240)
    view.queue_free()
    await frames(120)
    baker.request_cache_status()
    await frames(4)
    check(not bool(status.get("invalidated", true)), "second viewport invalidated the shared world bake")
   if mode == "worlds":
    var view := SubViewport.new()
    view.size = Vector2i(96, 96)
    view.world_3d = World3D.new()
    view.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    add_child(view)
    var other := Node3D.new()
    view.add_child(other)
    var alternate := volume("OtherWorldVolume", Vector3i(4, 8, 4), Vector3(1, 0, 1), other)
    check(alternate.get_world_3d() != get_world_3d(), "test viewport shares a world")
    var other_camera := Camera3D.new()
    other.add_child(other_camera)
    other_camera.position = camera.position
    other_camera.rotation = camera.rotation
    other_camera.current = true
    var other_sun := DirectionalLight3D.new()
    other.add_child(other_sun)
    other_sun.rotation_degrees = Vector3(-40, 20, 0)
    other_sun.light_color = Color.RED
    await frames(240)
    await RenderingServer.frame_post_draw
    get_viewport().get_texture().get_image().save_png("user://lighting_worlds_concurrent_" + str(frame_count) + ".png")
    view.queue_free()
    await frames(120)
    baker.request_cache_status()
    await frames(4)
    check(not bool(status.get("invalidated", true)), "separate world invalidated the original bake")
   if mode == "camera":
    var initial := int(status.get("restored_probes", 0))
    camera.position += Vector3(12, 0, 0)
    await frames(120)
    camera.position -= Vector3(12, 0, 0)
    await frames(180)
    baker.request_cache_status()
    await frames(4)
    check(int(status.get("restored_probes", 0)) >= initial, "camera scroll lost cache")
   await RenderingServer.frame_post_draw
   if mode not in ["load", "unbaked", "runtime_inputs", "unbaked_runtime_inputs", "runtime_sun", "unbaked_runtime_sun"]: get_viewport().get_texture().get_image().save_png("user://lighting_" + mode + "_" + str(frame_count) + ".png")
 print("LIGHTING_BAKE_TEST_", "FAILED" if failed else "OK", " mode=", mode)
 get_tree().quit(1 if failed else 0)

func save_authored_fixture() -> void:
 var copy := duplicate(0) as Node3D
 copy.set_script(load("res://export_harness.gd"))
 for child in copy.get_children():
  if child is SubViewport:
   child.free()
   continue
  child.owner = copy
 # Save an assigned resource dependency, then reload the scene in export tests.
 var scene := PackedScene.new()
 check(scene.pack(copy) == OK, "could not pack authored bake fixture")
 check(ResourceSaver.save(scene, "res://export_fixture.tscn") == OK, "could not save authored bake fixture")
 copy.free()
