#pragma once

#include "scene/resources/material.h"

class VoxelMaterial : public ShaderMaterial {
	GDCLASS(VoxelMaterial, ShaderMaterial);

public:
	enum ShadingMode {
		SHADING_MODE_UNLIT,
		SHADING_MODE_PBR,
	};

private:
	ShadingMode shading_mode = SHADING_MODE_PBR;
	real_t emission_energy = 1.0;
	bool transparency_enabled = false;
	void _rebuild_shader();

protected:
	static void _bind_methods();

public:
	void set_shading_mode(ShadingMode p_mode);
	ShadingMode get_shading_mode() const;
	void set_emission_energy(real_t p_energy);
	real_t get_emission_energy() const;
	void set_transparency_enabled(bool p_enabled);
	bool is_transparency_enabled() const;
	// VoxelMaterial is serialized as lightweight configuration. VoxelVolume3D
	// calls this only on its private runtime copy so generated Shader and 3D
	// texture resources never become scene subresources.
	void ensure_shader();
	static void clear_shader_cache();

	VoxelMaterial();
};

VARIANT_ENUM_CAST(VoxelMaterial::ShadingMode);
