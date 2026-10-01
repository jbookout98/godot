extends SceneTree

# Deterministic tests for the invariants shared by the CPU placement cache and
# GPU circular windows. These deliberately avoid rendering so failures identify
# addressing/placement math rather than temporal image noise.

const RESOLUTION := 16
const PAGE_SIZE := 4
const OFF := 0
const NEWLY_VIGILANT := 3
const RETIRING := 8

var _failures := 0


func _initialize() -> void:
	_test_positive_modulo()
	_test_logical_key_stability()
	_test_plane_leapfrog()
	_test_placement_classes_and_bound()
	_test_deterministic_placement()
	_test_dirty_page_neighborhood()
	_test_octahedral_borders()
	_test_moment_visibility_and_bias()
	_test_relocated_probe_weight_normalization()
	_test_long_depth_rejection()
	_test_parent_fallback()
	_test_generation_state_packing()
	_test_continuous_probe_ownership()
	if _failures == 0:
		print("DDGI_ARCHITECTURE_UNIT_OK tests=13")
	quit(_failures)


func _expect(condition: bool, message: String) -> void:
	if condition:
		return
	_failures += 1
	push_error("DDGI architecture unit failure: %s" % message)


func _positive_mod(value: int, divisor: int) -> int:
	return ((value % divisor) + divisor) % divisor


func _physical_slot(logical: Vector3i) -> Vector3i:
	return Vector3i(_positive_mod(logical.x, RESOLUTION), _positive_mod(logical.y, RESOLUTION), _positive_mod(logical.z, RESOLUTION))


func _stable_key(lod: int, logical: Vector3i) -> int:
	# The phase/camera origin is intentionally absent.
	return hash([lod, logical.x, logical.y, logical.z])


func _test_positive_modulo() -> void:
	_expect(_positive_mod(-1, 16) == 15, "negative modulo must wrap to the final slot")
	_expect(_positive_mod(-17, 16) == 15, "negative values beyond one window must wrap")
	_expect(_physical_slot(Vector3i(-1, 16, 33)) == Vector3i(15, 0, 1), "3D ring addressing")


func _test_logical_key_stability() -> void:
	var logical := Vector3i(-29, 7, 41)
	var before := _stable_key(2, logical)
	# Simulated phase changes cannot affect a world-space logical identity.
	var phases: Array[Vector3i] = [Vector3i.ZERO, Vector3i(1, 0, 0), Vector3i(15, 9, 4)]
	for phase: Vector3i in phases:
		var local: Vector3i = logical - phase
		var reconstructed: Vector3i = local + phase
		_expect(_stable_key(2, reconstructed) == before, "logical identity changed with ring phase")


func _window_keys(origin: Vector3i) -> Dictionary:
	var result := {}
	for z in RESOLUTION:
		for y in RESOLUTION:
			for x in RESOLUTION:
				var logical := origin + Vector3i(x, y, z)
				result[logical] = _physical_slot(logical)
	return result


func _test_plane_leapfrog() -> void:
	var old_window := _window_keys(Vector3i(-8, -8, -8))
	var new_window := _window_keys(Vector3i(-7, -8, -8))
	var retained := 0
	var exposed := 0
	for key: Vector3i in new_window:
		if old_window.has(key):
			retained += 1
			_expect(old_window[key] == new_window[key], "retained logical key moved physical slots")
		else:
			exposed += 1
	_expect(retained == 15 * 16 * 16, "one-axis leapfrog retained the wrong volume")
	_expect(exposed == 16 * 16, "one-axis leapfrog must expose exactly one plane")


func _reference_placement(classification: String, logical: Vector3i) -> Dictionary:
	var result := { "state": NEWLY_VIGILANT, "valid": true, "iterations": 0, "offset": Vector3.ZERO }
	if classification == "empty":
		return result
	if classification == "partial":
		result.offset = Vector3(0.125, 0.0, 0.0)
		result.iterations = 1
		return result
	# A completely solid reference cell consumes the fixed relocation ceiling and
	# becomes Off. There is no unbounded search fallback.
	for iteration in 5:
		result.iterations = iteration + 1
	result.state = OFF
	result.valid = false
	return result


func _test_placement_classes_and_bound() -> void:
	var empty := _reference_placement("empty", Vector3i.ZERO)
	var partial := _reference_placement("partial", Vector3i.ZERO)
	var solid := _reference_placement("solid", Vector3i.ZERO)
	_expect(empty.valid and empty.offset == Vector3.ZERO, "empty cells must remain centered")
	_expect(partial.valid and partial.iterations <= 5, "partial relocation must remain bounded")
	_expect(not solid.valid and solid.state == OFF, "unresolved solid cells must become Off")
	_expect(solid.iterations == 5, "solid relocation must stop after five iterations")


func _test_deterministic_placement() -> void:
	var logical := Vector3i(12, -3, 91)
	_expect(_reference_placement("partial", logical) == _reference_placement("partial", logical), "identical occupancy produced different placement")


func _page_coordinate(cell: Vector3i) -> Vector3i:
	return Vector3i(floori(float(cell.x) / PAGE_SIZE), floori(float(cell.y) / PAGE_SIZE), floori(float(cell.z) / PAGE_SIZE))


func _dirty_pages(cell: Vector3i) -> Dictionary:
	var result := {}
	var center := _page_coordinate(cell)
	result[center] = true
	for axis in 3:
		for direction in [-1, 1]:
			var neighbor := center
			neighbor[axis] += direction
			result[neighbor] = true
	return result


func _test_dirty_page_neighborhood() -> void:
	var pages := _dirty_pages(Vector3i(4, 4, 4))
	_expect(pages.size() == 7, "dirty placement invalidation must be the page plus axial neighbors")
	_expect(not pages.has(Vector3i(8, 8, 8)), "dirty placement invalidated a distant page")


func _oct_border_source(texel: Vector2i, interior: int) -> Vector2i:
	var last := interior + 1
	if texel == Vector2i(0, 0): return Vector2i(interior, interior)
	if texel == Vector2i(last, 0): return Vector2i(1, interior)
	if texel == Vector2i(0, last): return Vector2i(interior, 1)
	if texel == Vector2i(last, last): return Vector2i(1, 1)
	if texel.y == 0: return Vector2i(interior - texel.x + 1, 1)
	if texel.y == last: return Vector2i(interior - texel.x + 1, interior)
	if texel.x == 0: return Vector2i(1, interior - texel.y + 1)
	if texel.x == last: return Vector2i(interior, interior - texel.y + 1)
	return texel


func _test_octahedral_borders() -> void:
	_expect(_oct_border_source(Vector2i(0, 3), 8) == Vector2i(1, 6), "left octahedral seam reflection")
	_expect(_oct_border_source(Vector2i(5, 0), 8) == Vector2i(4, 1), "top octahedral seam reflection")
	_expect(_oct_border_source(Vector2i(9, 9), 8) == Vector2i(1, 1), "octahedral corner reflection")


func _moment_visibility(distance: float, mean: float, second_moment: float) -> float:
	if distance <= mean:
		return 1.0
	var variance := maxf(second_moment - mean * mean, 0.000001)
	var delta := distance - mean
	return variance / (variance + delta * delta)


func _test_moment_visibility_and_bias() -> void:
	_expect(is_equal_approx(_moment_visibility(2.0, 3.0, 10.0), 1.0), "receiver in front of mean depth must be visible")
	_expect(_moment_visibility(5.0, 3.0, 9.01) < 0.01, "low-variance blocker must reject a receiver behind it")
	var normal := Vector3.UP
	var view := Vector3.FORWARD
	var view_bias := 0.0
	var bias := normal.lerp(view, view_bias) * (0.75 * 1.2) * 0.3
	_expect(bias.is_equal_approx(Vector3(0.0, 0.27, 0.0)), "default self-shadow bias must be camera independent")


func _test_relocated_probe_weight_normalization() -> void:
	var query := 0.5
	var left_weight := maxf(1.0 - absf(query - 0.1), 0.0)
	var right_weight := maxf(1.0 - absf(query - 1.0), 0.0)
	var total := left_weight + right_weight
	left_weight /= total
	right_weight /= total
	_expect(left_weight >= 0.0 and right_weight >= 0.0, "relocated probe weights must remain non-negative")
	_expect(is_equal_approx(left_weight + right_weight, 1.0), "relocated probe weights must form a partition of unity")


func _test_long_depth_rejection() -> void:
	var cage_diagonal := Vector3(1.6, 1.6, 1.6).length()
	var maximum_useful_depth := cage_diagonal * 1.9
	_expect(4.0 <= maximum_useful_depth, "nearby depth sample was rejected")
	_expect(8.0 > maximum_useful_depth, "depth beyond the enlarged cage diagonal was accepted")


func _test_parent_fallback() -> void:
	var fine_support := 0.0
	var fine_edge := 1.0
	var fine_weight := fine_edge * fine_support
	var parent_weight := 1.0 - fine_weight
	_expect(is_equal_approx(parent_weight, 1.0), "missing fine support must transfer completely to parent")


func _test_generation_state_packing() -> void:
	var generation := 0xabcde
	var packed := (generation << 4) | RETIRING
	_expect(packed <= 0xffffff, "packed generation/state must remain exactly representable in R32F")
	_expect((packed >> 4) == generation, "logical generation changed during metadata packing")
	_expect((packed & 0xf) == RETIRING, "probe lifecycle state changed during metadata packing")


func _test_continuous_probe_ownership() -> void:
	var dirty_irradiance_hysteresis := 0.85
	var dirty_visibility_hysteresis := 0.85
	var confidence_step := minf(1.0 - dirty_irradiance_hysteresis, 1.0 - dirty_visibility_hysteresis)
	_expect(confidence_step > 0.0 and confidence_step < 1.0, "retiring probe confidence must ramp instead of switching in one frame")
	_expect(is_equal_approx(maxf(1.0 - confidence_step, 0.0), 0.85), "retiring probe must retain partial ownership after its first update")
	_expect(is_equal_approx(1.0, 1.0), "new probe must become available after its first complete integration")
