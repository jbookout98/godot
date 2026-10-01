#pragma once

#include "scene/3d/visual_instance_3d.h"
#include "scene/resources/image_texture.h"
#include "voxel_material.h"
#include "voxel_shape_data.h"

class VoxelVolume3D : public GeometryInstance3D {
	GDCLASS(VoxelVolume3D, GeometryInstance3D);

public:
	enum StreamingMode {
		STREAMING_AUTOMATIC,
		STREAMING_ALWAYS_RESIDENT,
		STREAMING_MANUAL,
	};

	enum NeighborFace {
		NEIGHBOR_NEGATIVE_X,
		NEIGHBOR_POSITIVE_X,
		NEIGHBOR_NEGATIVE_Y,
		NEIGHBOR_POSITIVE_Y,
		NEIGHBOR_NEGATIVE_Z,
		NEIGHBOR_POSITIVE_Z,
		NEIGHBOR_FACE_COUNT,
	};
	static constexpr int NEIGHBOR_DIAGONAL_COUNT = 20;
	static constexpr int NEIGHBOR_TEXTURE_LAYER_COUNT = NEIGHBOR_FACE_COUNT + NEIGHBOR_DIAGONAL_COUNT;

private:

	bool voxel_shadows_enabled = true;
	RID procedural_surface;
	// Stable low-resolution geometry used only by shadow passes. The visible
	// surface remains the committed DDA shader and never renders this mesh.
	RID shadow_proxy_mesh;
	RID shadow_proxy_instance;
	bool shadow_proxy_has_surface = false;
	Ref<VoxelShapeData> voxel_data;
	// Shared inspector-facing authored configuration. Several volumes may retain
	// the same resource and receive its changed notification.
	Ref<VoxelMaterial> voxel_material;
	// Private generated material holding Shader and per-volume GPU textures.
	Ref<VoxelMaterial> runtime_material;
	String runtime_material_batch_key;
	bool runtime_material_cached = false;
	// Mixed bricks are tightly packed into this atlas. Empty and uniform bricks
	// never allocate voxel payload on the GPU.
	Ref<ImageTexture3D> mixed_brick_atlas;
	Ref<ImageTexture3D> brick_directory_texture;
	// Six 2D face slices followed by twelve packed edge strips and eight corner
	// bits. They contain only neighbor occupancy and are rebuilt on adjacency edits.
	Ref<ImageTexture3D> neighbor_face_texture;
	Ref<ImageTexture3D> fallback_neighbor_face_texture;
	ObjectID neighbor_ids[NEIGHBOR_FACE_COUNT];
	uint64_t neighbor_revisions[NEIGHBOR_FACE_COUNT] = {
		UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX
	};
	ObjectID neighbor_diagonal_ids[NEIGHBOR_DIAGONAL_COUNT];
	uint64_t neighbor_diagonal_revisions[NEIGHBOR_DIAGONAL_COUNT];
	uint64_t neighbor_self_revision = UINT64_MAX;
	Vector3i neighbor_self_dimensions;
	int neighbor_mask = 0;
	int neighbor_diagonal_mask = 0;
	Vector3i atlas_brick_dimensions = Vector3i(1, 1, 1);
	uint64_t rendered_revision = UINT64_MAX;
	Vector3i voxel_forward_dirty_position;
	Vector3i voxel_forward_dirty_size;
	bool voxel_forward_dirty_valid = false;
	// VoxelShapeData emits voxels_changed followed by Resource::changed for the
	// same edit batch. The dedicated callback already performs the complete
	// render refresh, so the following generic notification must be ignored.
	bool voxel_change_notification_pending = false;
	StreamingMode streaming_mode = STREAMING_AUTOMATIC;
	real_t streaming_distance = 0.0;
	// Automatic volumes start without GPU resources. The streaming manager
	// selects the initial resident set after the complete scene and camera have
	// entered the tree, avoiding load-time uploads that are immediately freed.
	bool streaming_resident = false;
	bool render_resources_built = false;
	Ref<Texture2D> fallback_palette;
	Ref<Texture2D> fallback_material;
	AABB local_aabb;

	void _voxel_data_changed();
	void _voxel_data_voxels_changed(const Vector3i &p_position, const Vector3i &p_size, int64_t p_revision);
	void _voxel_material_changed();
	void _rebuild_volume_textures();
	void _rebuild_runtime_material();
	void _release_runtime_material();
	String _make_runtime_material_batch_key() const;
	void _rebuild_procedural_surface();
	void _ensure_shadow_proxy();
	void _rebuild_shadow_proxy();
	void _sync_shadow_proxy_instance();
	void _sync_voxel_forward_volume(bool p_remove = false);
	void _update_material_bindings(bool p_sync_volume = true);
	Ref<ImageTexture3D> _create_texture_3d(
			const PackedByteArray &p_bytes,
			const Vector3i &p_dimensions,
			Image::Format p_format,
			int p_bytes_per_pixel) const;
	void _ensure_fallback_textures();
	bool _uses_voxel_forward_shadow_mask() const;

protected:
	static void _bind_methods();
	void _notification(int p_what);
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_voxel_shadows_enabled(bool p_enabled);
	bool is_voxel_shadows_enabled() const;
	void set_voxel_data(const Ref<VoxelShapeData> &p_data);
	Ref<VoxelShapeData> get_voxel_data() const;
	void set_voxel_material(const Ref<VoxelMaterial> &p_material);
	Ref<VoxelMaterial> get_voxel_material() const;

	int get_voxel(const Vector3i &p_position) const;
	bool set_voxel(const Vector3i &p_position, int p_palette_index);
	int apply_voxel_edits(const Array &p_positions, const PackedByteArray &p_palette_indices);
	int apply_voxel_edits_by_index(const PackedInt32Array &p_indices, const PackedByteArray &p_palette_indices);
	int fill_voxel_region(const Vector3i &p_position, const Vector3i &p_size, int p_palette_index);
	Vector3i local_to_voxel(const Vector3 &p_local_position) const;
	Vector3i world_to_voxel(const Vector3 &p_world_position) const;
	Vector3 voxel_to_local(const Vector3i &p_voxel, bool p_center = true) const;
	Vector3 voxel_to_world(const Vector3i &p_voxel, bool p_center = true) const;
	void make_voxel_data_unique();
	void set_streaming_mode(StreamingMode p_mode);
	StreamingMode get_streaming_mode() const;
	void set_streaming_distance(real_t p_distance);
	real_t get_streaming_distance() const;
	void set_streaming_resident(bool p_resident);
	bool is_streaming_resident() const;
	// Internal renderer service used by VoxelVolumeStreamingManager.
	static int get_neighbor_diagonal_index(const Vector3i &p_offset);
	static Vector3i get_neighbor_diagonal_offset(int p_index);
	void update_neighbor_faces(VoxelVolume3D *const p_neighbors[NEIGHBOR_FACE_COUNT], VoxelVolume3D *const p_diagonal_neighbors[NEIGHBOR_DIAGONAL_COUNT]);
	void append_cached_neighbor_ids(Vector<ObjectID> &r_ids) const;
	void refresh_cached_neighbor_faces();

	AABB get_aabb() const override;

	VoxelVolume3D();
	~VoxelVolume3D();
};

VARIANT_ENUM_CAST(VoxelVolume3D::StreamingMode);
