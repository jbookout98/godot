#pragma once
#include "scene/3d/node_3d.h"
#include "servers/rendering/renderer_rd/voxel_forward/voxel_lighting_data.h"
class SubViewport;
class Camera3D;

class VoxelLightingBake3D : public Node3D {
	GDCLASS(VoxelLightingBake3D, Node3D);
	Ref<VoxelLightingData> lighting_data;
	NodePath source_root = NodePath("..");
	AABB bake_bounds = AABB(Vector3(-12, -2, -12), Vector3(24, 8, 24));
	int settle_frames = 120;
	int max_frames_per_view = 3600;
	SubViewport *bake_viewport = nullptr;
	Camera3D *bake_camera = nullptr;
	Vector<Vector3> viewpoints;
	int view_index = 0;
	int view_frames = 0;
	uint64_t last_draw_frame = 0;
	bool capture_pending = false;
	uint64_t bake_generation = 0;
	String bake_signature;
	String bake_save_path;
	ProcessMode prior_process_mode = PROCESS_MODE_INHERIT;
	Ref<VoxelLightingData> result_data;
	HashMap<Vector4i, int> captured_keys;
	RID registered_scenario;
	void _apply_cache();
	void _verify_cache(int64_t p_ticket, String p_signature, RID p_scenario);
	void _cache_status_completed(Dictionary p_status) { emit_signal(SNAME("cache_status"), p_status); }
	void _capture_completed(Dictionary p_result, int64_t p_generation);
	void _finish(const String &p_error);
	void _collect(Node *p_node, Vector<Node *> &r_nodes) const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_lighting_data(const Ref<VoxelLightingData> &p_data);
	Ref<VoxelLightingData> get_lighting_data() const { return lighting_data; }
	void set_source_root(const NodePath &p_path) { source_root = p_path; }
	NodePath get_source_root() const { return source_root; }
	void set_bake_bounds(const AABB &p_bounds) { bake_bounds = p_bounds; }
	AABB get_bake_bounds() const { return bake_bounds; }
	void set_settle_frames(int p_frames) { settle_frames = CLAMP(p_frames, 16, 3600); }
	int get_settle_frames() const { return settle_frames; }
	void set_max_frames_per_view(int p_frames) { max_frames_per_view = CLAMP(p_frames, 120, 36000); }
	int get_max_frames_per_view() const { return max_frames_per_view; }
	String compute_signature() const;
	Error bake(const String &p_save_path = String());
	void cancel_bake();
	bool is_baking() const { return bake_viewport != nullptr; }
	float get_bake_progress() const;
	String get_bake_description() const;
	void request_cache_status(bool p_probe_statistics = false);
};
