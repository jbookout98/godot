extends SceneTree

var failures := 0

func check(condition: bool, message: String) -> void:
 if not condition:
  failures += 1
  push_error("LIGHTING_FILE_TEST_FAILED " + message)

func _initialize() -> void:
 var path := "res://fixture_lighting.vlight"
 var good := FileAccess.get_file_as_bytes(path)
 check(good.size() > 200, "bake a fixture first")
 if good.size() <= 200:
  quit(1)
  return
 var cases: Array[PackedByteArray] = []
 cases.append(good.slice(0, 199))
 var oversized := good.slice(0, 200)
 oversized.encode_u32(8, 0xffffffff)
 cases.append(oversized)
 var version := good.duplicate()
 version.encode_u32(4, 10000)
 cases.append(version)
 var nonfinite := good.duplicate()
 nonfinite.encode_float(16, NAN)
 cases.append(nonfinite)
 var checksum := good.duplicate()
 checksum[200] ^= 1
 cases.append(checksum)
 var extra := good.duplicate()
 extra.append(0)
 cases.append(extra)
 for index in cases.size():
  var output := "user://lighting_invalid_" + str(index) + ".vlight"
  var file := FileAccess.open(output, FileAccess.WRITE)
  file.store_buffer(cases[index])
  file.close()
  var before := OS.get_static_memory_usage()
  var resource := ResourceLoader.load(output, "VoxelLightingData", ResourceLoader.CACHE_MODE_IGNORE) as VoxelLightingData
  check(resource != null and not resource.validate().is_empty(), "malformed file accepted: " + str(index))
  if index == 1: check(OS.get_static_memory_usage() - before < 1024 * 1024, "oversized header allocated a large payload")
 var valid := load(path) as VoxelLightingData
 check(valid != null and valid.validate().is_empty(), "valid file rejected")
 if valid and valid.probes.size() >= 6880:
  var duplicate := valid.duplicate() as VoxelLightingData
  var bytes := duplicate.probes
  for byte in 3440: bytes[3440 + byte] = bytes[byte]
  duplicate.probes = bytes
  var hash := HashingContext.new()
  hash.start(HashingContext.HASH_SHA256)
  hash.update(bytes)
  duplicate.payload_hash = hash.finish().hex_encode()
  check(duplicate.validate().contains("duplicate"), "duplicate world-space probe identity accepted")
 var missing_scene := "[gd_scene load_steps=2 format=3]\n[ext_resource type=\"VoxelLightingData\" path=\"user://lighting_intentionally_missing.vlight\" id=\"1\"]\n[node name=\"MissingCache\" type=\"VoxelLightingBake3D\"]\nlighting_data = ExtResource(\"1\")\n"
 var file := FileAccess.open("user://lighting_missing_scene.tscn", FileAccess.WRITE)
 file.store_string(missing_scene)
 file.close()
 var scene := load("user://lighting_missing_scene.tscn") as PackedScene
 check(scene != null, "missing optional cache prevented scene loading")
 if scene:
  var node := scene.instantiate() as VoxelLightingBake3D
  check(node != null and (node.lighting_data == null or not node.lighting_data.validate().is_empty()), "missing cache produced valid lighting")
  node.free()
 print("LIGHTING_FILE_TEST_", "OK" if failures == 0 else "FAILED", " cases=8")
 quit(0 if failures == 0 else 1)
