extends Node3D

func _ready() -> void:
 var baker := get_node("LightingBake") as VoxelLightingBake3D
 var status: Dictionary = {}
 baker.cache_status.connect(func(value: Dictionary): status.merge(value, true))
 for frame in 360: await get_tree().process_frame
 baker.request_cache_status()
 for frame in 4: await get_tree().process_frame
 var valid := int(status.get("restored_probes", 0)) > 0
 print("EXPORTED_LIGHTING_", "OK" if valid else "FAILED", " ", JSON.stringify(status))
 get_tree().quit(0 if valid else 1)
