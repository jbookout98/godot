def can_build(env, platform):
    if env["disable_3d"]:
        return False

    # This module is currently a fork of GodotPhysics3D and intentionally
    # retains its internal Godot* class names. Building both modules would
    # define the same C++ symbols twice.
    if env.get("module_godot_physics_3d_enabled", False):
        print(
            "VoxelPhysics3D disabled: build with "
            "module_godot_physics_3d_enabled=no."
        )
        return False

    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "VoxelMaterial",
        "VoxelShape3D",
        "VoxelShapeData",
        "VoxelVolume3D",
    ]


def get_doc_path():
    return "doc_classes"
