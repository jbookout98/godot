#pragma once

#include "scene/resources/material.h"
#include "scene/resources/texture.h"

class VoxelMaterial : public ShaderMaterial {
	GDCLASS(VoxelMaterial, ShaderMaterial);

public:
	enum ShadingMode {
		SHADING_MODE_UNLIT,
		SHADING_MODE_PBR,
	};
	enum LightingPositionMode {
		LIGHTING_POSITION_EXACT_HIT,
		LIGHTING_POSITION_VOXEL_FACE_CENTER,
	};
	enum AmbientOcclusionMode {
		AMBIENT_OCCLUSION_MODE_SMOOTH,
		AMBIENT_OCCLUSION_MODE_VOXELIZED,
		AMBIENT_OCCLUSION_MODE_HARD_CORNERS,
	};
	enum AmbientOcclusionFaceMode {
		AMBIENT_OCCLUSION_FACE_MODE_ALL,
		AMBIENT_OCCLUSION_FACE_MODE_FLOORS,
		AMBIENT_OCCLUSION_FACE_MODE_CEILINGS,
	};

private:
	ShadingMode shading_mode = SHADING_MODE_PBR;
	LightingPositionMode lighting_position_mode = LIGHTING_POSITION_VOXEL_FACE_CENTER;
	Color albedo_modulate = Color(1, 1, 1, 1);
	Ref<Texture2D> palette_texture;
	Ref<Texture2D> material_texture;
	Ref<Texture2D> metallic_texture;
	Ref<Texture2D> transparency_texture;
	Ref<Texture2D> specularity_texture;
	Ref<Texture2D> emission_texture;
	real_t roughness_multiplier = 1.0;
	real_t metallic_multiplier = 1.0;
	real_t specularity_multiplier = 1.0;
	real_t emission_energy = 1.0;
	bool ambient_occlusion_enabled = true;
	Color ambient_occlusion_color = Color(0, 0, 0, 1);
	real_t ambient_occlusion_strength = 1.0;
	real_t ambient_occlusion_hardness = 0.5;
	real_t ambient_occlusion_direct_light_influence = 0.0;
	bool ambient_occlusion_tint_enabled = true;
	real_t ambient_occlusion_tint_strength = 1.0;
	Ref<Texture2D> ambient_occlusion_tint_palette_texture;
	AmbientOcclusionMode ambient_occlusion_mode = AMBIENT_OCCLUSION_MODE_VOXELIZED;
	AmbientOcclusionFaceMode ambient_occlusion_face_mode = AMBIENT_OCCLUSION_FACE_MODE_ALL;
	bool ambient_occlusion_change_in_progress = false;
	bool outline_enabled = false;
	Color outline_color = Color(0, 0, 0, 1);
	real_t outline_width = 1.0;
	bool transparency_enabled = false;
	bool metallic_texture_enabled = false;
	bool specularity_texture_enabled = false;
	bool emission_texture_enabled = false;
	bool batched_resources_enabled = false;
	void _emit_ambient_occlusion_changed();
	void _rebuild_shader();

protected:
	static void _bind_methods();

public:
	void set_shading_mode(ShadingMode p_mode);
	ShadingMode get_shading_mode() const;
	void set_lighting_position_mode(LightingPositionMode p_mode);
	LightingPositionMode get_lighting_position_mode() const;
	void set_emission_energy(real_t p_energy);
	real_t get_emission_energy() const;
	void set_ambient_occlusion_enabled(bool p_enabled);
	bool is_ambient_occlusion_enabled() const;
	void set_ambient_occlusion_color(const Color &p_color);
	Color get_ambient_occlusion_color() const;
	void set_ambient_occlusion_strength(real_t p_strength);
	real_t get_ambient_occlusion_strength() const;
	void set_ambient_occlusion_intensity(real_t p_intensity);
	real_t get_ambient_occlusion_intensity() const;
	void set_ambient_occlusion_hardness(real_t p_hardness);
	real_t get_ambient_occlusion_hardness() const;
	void set_ambient_occlusion_contrast(real_t p_contrast);
	real_t get_ambient_occlusion_contrast() const;
	void set_ambient_occlusion_direct_light_influence(real_t p_influence);
	real_t get_ambient_occlusion_direct_light_influence() const;
	void set_ambient_occlusion_tint_enabled(bool p_enabled);
	bool is_ambient_occlusion_tint_enabled() const;
	void set_ambient_occlusion_tint_strength(real_t p_strength);
	real_t get_ambient_occlusion_tint_strength() const;
	void set_ambient_occlusion_tint_palette_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_ambient_occlusion_tint_palette_texture() const;
	void set_ambient_occlusion_mode(AmbientOcclusionMode p_mode);
	AmbientOcclusionMode get_ambient_occlusion_mode() const;
	void set_ambient_occlusion_face_mode(AmbientOcclusionFaceMode p_mode);
	AmbientOcclusionFaceMode get_ambient_occlusion_face_mode() const;
	bool is_ambient_occlusion_change_in_progress() const;
	void set_albedo_modulate(const Color &p_color);
	Color get_albedo_modulate() const;
	void set_palette_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_palette_texture() const;
	void set_material_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_material_texture() const;
	void set_metallic_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_metallic_texture() const;
	void set_transparency_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_transparency_texture() const;
	void set_specularity_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_specularity_texture() const;
	void set_emission_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_emission_texture() const;
	void set_roughness_multiplier(real_t p_multiplier);
	real_t get_roughness_multiplier() const;
	void set_metallic_multiplier(real_t p_multiplier);
	real_t get_metallic_multiplier() const;
	void set_specularity_multiplier(real_t p_multiplier);
	real_t get_specularity_multiplier() const;
	void set_outline_enabled(bool p_enabled);
	bool is_outline_enabled() const;
	void set_outline_color(const Color &p_color);
	Color get_outline_color() const;
	void set_outline_width(real_t p_width);
	real_t get_outline_width() const;
	void set_transparency_enabled(bool p_enabled);
	bool is_transparency_enabled() const;
	void set_texture_features(bool p_metallic_enabled, bool p_specularity_enabled, bool p_emission_enabled);
	// Internal runtime switch. Batched volume resources are selected from the
	// Forward Clustered scene descriptor table using the current instance row.
	void set_batched_resources_enabled(bool p_enabled);
	bool is_batched_resources_enabled() const;
	virtual bool is_editor_preview_supported() const override { return false; }
	// VoxelMaterial is serialized as lightweight configuration. VoxelVolume3D
	// calls this only on its private runtime copy so generated Shader and 3D
	// texture resources never become scene subresources.
	void ensure_shader();
	static void clear_shader_cache();

	VoxelMaterial();
};

VARIANT_ENUM_CAST(VoxelMaterial::ShadingMode);
VARIANT_ENUM_CAST(VoxelMaterial::LightingPositionMode);
VARIANT_ENUM_CAST(VoxelMaterial::AmbientOcclusionMode);
VARIANT_ENUM_CAST(VoxelMaterial::AmbientOcclusionFaceMode);
