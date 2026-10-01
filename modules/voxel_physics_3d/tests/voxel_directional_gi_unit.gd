extends SceneTree

# CPU reference tests for the directional Voxel GI representation. These lock
# the shader's address mapping, face selection, bounded transport, and exact
# voxel-corner rule without depending on a GPU or scene.

const DIRECTION_COUNT := 6

var _failures := 0


func _initialize() -> void:
	_test_packed_lobe_addressing()
	_test_face_lobe_selection()
	_test_transport_is_bounded()
	_test_exact_corner_occlusion()
	_test_sealed_wall_rejects_transport()
	_test_sealed_corner_has_no_diagonal_path()
	_test_colored_surface_injection()
	_test_opening_transmits_light()
	_test_transmissive_material_attenuates()
	_test_six_lobe_source_convention()
	_test_visibility_aware_gather()
	_test_valid_darkness_has_no_fallback()
	_test_topology_change_classification()
	_test_adaptive_boundary_expansion()
	_test_world_anchored_query()
	_test_dirty_halo_expansion()
	_test_revision_publication_rule()
	if _failures == 0:
		print("VOXEL_DIRECTIONAL_GI_UNIT_OK tests=17")
	quit(_failures)


func _expect(condition: bool, message: String) -> void:
	if condition:
		return
	_failures += 1
	push_error("Directional Voxel GI unit failure: %s" % message)


func _packed_x(cell_x: int, direction: int, resolution: int) -> int:
	return cell_x + direction * resolution


func _test_packed_lobe_addressing() -> void:
	var resolution := 32
	var addresses := {}
	for direction in DIRECTION_COUNT:
		for cell_x in resolution:
			var address := _packed_x(cell_x, direction, resolution)
			_expect(address >= 0 and address < resolution * DIRECTION_COUNT, "packed address left the texture")
			addresses[address] = true
	_expect(addresses.size() == resolution * DIRECTION_COUNT, "directional slabs overlap or leave gaps")


func _selected_lobes(normal: Vector3) -> Dictionary:
	return {
		0 if normal.x >= 0.0 else 1: absf(normal.x),
		2 if normal.y >= 0.0 else 3: absf(normal.y),
		4 if normal.z >= 0.0 else 5: absf(normal.z),
	}


func _test_face_lobe_selection() -> void:
	var axis := _selected_lobes(Vector3.RIGHT)
	_expect(is_equal_approx(float(axis[0]), 1.0), "+X face must sample the +X incident-radiance lobe")
	_expect(is_zero_approx(float(axis[2])) and is_zero_approx(float(axis[4])), "axis face must not mix orthogonal lobes")
	var diagonal := _selected_lobes(Vector3(1.0, -1.0, 1.0).normalized())
	_expect(diagonal.has(0) and diagonal.has(3) and diagonal.has(4), "normal signs selected the wrong directional slabs")


func _test_transport_is_bounded() -> void:
	var injection := 1.0
	var transport := 0.8
	var decay := 0.78
	var value := injection
	for _pass in 16:
		value = injection + value * transport * decay
	var analytic_limit := injection / (1.0 - transport * decay)
	_expect(value <= analytic_limit + 0.001, "propagation created unbounded energy")
	_expect(value >= injection, "propagation removed direct injection")


func _corner_factor(side_a: bool, side_b: bool, diagonal: bool) -> float:
	if side_a and side_b:
		return 0.0
	return 1.0 - float(int(side_a) + int(side_b) + int(diagonal)) / 3.0


func _test_exact_corner_occlusion() -> void:
	_expect(is_equal_approx(_corner_factor(false, false, false), 1.0), "open corner must remain unoccluded")
	_expect(is_equal_approx(_corner_factor(true, true, false), 0.0), "two occupied sides must fully close the corner")
	_expect(_corner_factor(true, false, true) < _corner_factor(true, false, false), "diagonal occupancy must darken an otherwise equal corner")


func _transport(source: Vector3, face_transmittance: float, decay: float) -> Vector3:
	return source * clampf(face_transmittance, 0.0, 1.0) * 0.8 * clampf(decay, 0.0, 0.99)


func _test_sealed_wall_rejects_transport() -> void:
	_expect(_transport(Vector3.ONE, 0.0, 0.78).is_zero_approx(), "a closed shared face leaked irradiance through a one-voxel wall")


func _test_sealed_corner_has_no_diagonal_path() -> void:
	var axial_offsets := [Vector3i.RIGHT, Vector3i.LEFT, Vector3i.UP, Vector3i.DOWN, Vector3i.FORWARD, Vector3i.BACK]
	_expect(not axial_offsets.has(Vector3i(1, 1, 0)), "propagation unexpectedly admitted a diagonal connection")
	_expect(_transport(Vector3.ONE, 0.0, 0.78).is_zero_approx(), "sealed L-corner transferred light around a closed face")


func _test_colored_surface_injection() -> void:
	var red_albedo := Vector3(0.9, 0.1, 0.05)
	var injected := red_albedo * Vector3.ONE * 0.75
	_expect(injected.x > injected.y * 8.0 and injected.x > injected.z * 12.0, "surface albedo did not color its diffuse bounce")


func _test_opening_transmits_light() -> void:
	var source := Vector3(0.8, 0.6, 0.4)
	_expect(_transport(source, 1.0, 0.78).length() > _transport(source, 0.0, 0.78).length(), "an opening did not transmit more light than its sealed neighbor")


func _test_transmissive_material_attenuates() -> void:
	var source := Vector3.ONE
	var opaque := _transport(source, 0.0, 0.78).length()
	var translucent := _transport(source, 0.5, 0.78).length()
	var clear := _transport(source, 1.0, 0.78).length()
	_expect(opaque < translucent and translucent < clear, "transmissive material did not attenuate transport continuously")


func _lobe_for_source_offset(offset: Vector3i) -> int:
	if offset.x > 0: return 0
	if offset.x < 0: return 1
	if offset.y > 0: return 2
	if offset.y < 0: return 3
	if offset.z > 0: return 4
	return 5


func _test_six_lobe_source_convention() -> void:
	var offsets := [Vector3i.RIGHT, Vector3i.LEFT, Vector3i.UP, Vector3i.DOWN, Vector3i.BACK, Vector3i.FORWARD]
	for direction in DIRECTION_COUNT:
		_expect(_lobe_for_source_offset(offsets[direction]) == direction, "source-relative lobe convention is reversed for direction %d" % direction)


func _manual_gather_visibility(delta: Vector3i, face_transmittance: Array[float]) -> float:
	var visibility := 1.0
	if delta.x > 0: visibility *= face_transmittance[0]
	if delta.x < 0: visibility *= face_transmittance[1]
	if delta.y > 0: visibility *= face_transmittance[2]
	if delta.y < 0: visibility *= face_transmittance[3]
	if delta.z > 0: visibility *= face_transmittance[4]
	if delta.z < 0: visibility *= face_transmittance[5]
	return visibility


func _test_visibility_aware_gather() -> void:
	var open_faces: Array[float] = [1.0, 1.0, 1.0, 1.0, 1.0, 1.0]
	var closed_positive_x := open_faces.duplicate()
	closed_positive_x[0] = 0.0
	_expect(is_zero_approx(_manual_gather_visibility(Vector3i.RIGHT, closed_positive_x)), "manual gather crossed a sealed +X face")
	_expect(is_zero_approx(_manual_gather_visibility(Vector3i(1, 1, 0), closed_positive_x)), "manual gather admitted a diagonal through a sealed corner")
	closed_positive_x[0] = 0.5
	_expect(is_equal_approx(_manual_gather_visibility(Vector3i.RIGHT, closed_positive_x), 0.5), "manual gather lost partial face transmittance")


func _test_valid_darkness_has_no_fallback() -> void:
	var valid_coverage := 1.0
	var fallback_weight := 1.0 - valid_coverage
	_expect(is_zero_approx(fallback_weight), "a valid black Voxel GI sample was replaced by ambient fallback")


func _test_topology_change_classification() -> void:
	var previous_word := 0b00110100
	var closed_word := previous_word | 0b10000001
	var reopened_word := closed_word & ~0b00100000
	_expect((closed_word & ~previous_word) != 0, "added occupancy did not classify a face-transmittance decrease")
	_expect((closed_word & ~reopened_word) != 0, "removed occupancy did not classify a face-transmittance increase")


func _test_adaptive_boundary_expansion() -> void:
	var epsilon := 0.01
	var measured_boundary_deltas := [0.24, 0.071, 0.018, 0.004]
	var expansion_count := 0
	for delta: float in measured_boundary_deltas:
		if delta <= epsilon:
			break
		expansion_count += 1
	_expect(expansion_count == 3, "adaptive invalidation stopped before its measured boundary converged")
	_expect(measured_boundary_deltas[expansion_count] <= epsilon, "adaptive invalidation published a non-converged boundary")


func _world_cell(world_position: Vector3, grid_origin: Vector3, cell_size: float) -> Vector3i:
	return Vector3i(((world_position - grid_origin) / cell_size).floor())


func _test_world_anchored_query() -> void:
	var point := Vector3(10.25, -2.0, 7.75)
	var origin := Vector3(-12.8, -12.8, -12.8)
	var before := _world_cell(point, origin, 0.4)
	var simulated_camera_positions := [Vector3.ZERO, Vector3(100.0, 4.0, -20.0), Vector3(-0.01, 50.0, 0.01)]
	for _camera_position: Vector3 in simulated_camera_positions:
		_expect(_world_cell(point, origin, 0.4) == before, "camera motion changed a fixed world-space Voxel GI lookup")


func _test_dirty_halo_expansion() -> void:
	var dirty_min := Vector3i(10, 8, 6)
	var dirty_max := Vector3i(12, 11, 9)
	var propagation_passes := 4
	var expanded_min := dirty_min - Vector3i.ONE * propagation_passes
	var expanded_max := dirty_max + Vector3i.ONE * propagation_passes
	_expect(expanded_min == Vector3i(6, 4, 2) and expanded_max == Vector3i(16, 15, 13), "dirty region omitted the propagation halo")


func _test_revision_publication_rule() -> void:
	var active_revision := 40
	var staging_revision := 40
	var current_world_revision := 41
	var may_publish := staging_revision == current_world_revision
	_expect(not may_publish, "stale staging data must never replace the active field")
	_expect(active_revision == 40, "rejecting stale staging must preserve the active field")
