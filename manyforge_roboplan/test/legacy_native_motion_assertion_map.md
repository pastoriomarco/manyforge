# Legacy native motion assertion map

This is a source-level migration map, not execution evidence. The legacy source
is `manyforge_roboplan/test/test_native_execution_path.py` at commit
`77535fd020d7d6d76fd923c590c20dec4d5c7bac` (the parent of `8cef3a71`);
the full source file SHA-256 is
`b196a10f844b7ca04400b5c2ee31390981acb8f79fd40d102a916366c3a00014`.
The function was `test_first_complete_addressed_native_motion_path`.
AST enumeration yields exactly **244 `assert` statements and 8 `pytest.raises`
contexts**. Each appears once below, identified by its original source line.
Repeated loop assertions appear once, not once for each loop iteration.

Current references, with line numbers in this checkout:

- **M**: `manyforge_roboplan/test/test_native_execution_path.py`,
  `test_native_manual_preview_control_and_restart_contracts`.
- **D**: same file, `test_manual_motion_malformed_stop_recovers_with_durable_evidence`.
- **H**: same file, `_check_malformed_stop_recovery`, called by D.
- **Q**: `scripts/qualification/ui6h_target_queries.py`; its named live
  qualification path is separate from the CTest process tests.

**Status legend:** E = identical assertion expression in a replacement;
A = adapted/current or separate qualification check; P = partial/weaker;
C = changed value or contract needing reconciliation; S = superseded;
O = open/no equivalent found. Exact text equality is evidence of a matching
assertion, not proof that setup, behavior, lane, or test result are identical.

**Origin-scope caveat:** the old `_runner_submission`/`_gripper_submission`
checks exercised Program-shaped submissions. M and D now use direct/manual
origins for many checks. Consequently E/A on those rows proves only the shared
manual-path assertion. It does not establish equivalent captured-Program
coverage. Q has real Program lookup and post-retirement checks, and other
process tests cover captured Program admission, but neither should be treated
as an automatic replacement for every Program-origin assertion.

**Superseded checks:** Scene revision is no longer a runtime precondition;
operation-level Pause/Resume has been removed. Do not reinstate those former
assertions. The legacy Stop section here checks a soft-on-path stage; no row
was found to assert a hard-braking behavior contradicted by the later Q42/Q2
hard-Stop decision, so no additional row was marked superseded on that basis.

**Open points:** the old pick/place owner-unavailable interfaces are absent;
the restored arm/gripper resource-list check examines dispatched operations,
not the old observation DTO; some first-status, cross-projection and
Program-origin checks are only partial. The gripper authority expectation
changed from `predicted` to `commanded`: it is present by the committed R4 M1
milestone `d3920e13` (and its parent already contains it), but it is not
equivalent coverage of the old behavior.

Current assertions were restored for the gripper target role, command schema
revision, observed-target count and arm role, dispatched arm/gripper resource distinction,
and stable/monotonic settled projections in M; D checks the actual cleared
fault immediately after malformed-Stop recovery. No product code changed.
Neither the two process tests nor Q was run while creating this map; the
coordinator owns the ROS build and test lane.

Counts: E=190, A=42, P=9, C=3, S=4, O=4; total=252.

| Old line | Legacy assertion / expected exception | Status | Current reference / disposition |
|---:|---|:---:|---|
| 1835 | <code>manifest[&#x27;api_version&#x27;] == &#x27;manyforge.i6.native-execution-fixture.v1&#x27;</code> | E | M:2719 — same assertion expression |
| 1836 | <code>manifest[&#x27;source_joint_names&#x27;] == [&#x27;joint1&#x27;, &#x27;joint2&#x27;, &#x27;joint3&#x27;, &#x27;joint4&#x27;, &#x27;joint5&#x27;, &#x27;joint6&#x27;]</code> | E | M:2720 — same assertion expression |
| 1882 | <code>pytest.raises(HeadlessClientError)</code> | O | Removed `start_pick` endpoint; no matching unavailable-owner refusal found. The new pick planner is a different interface. |
| 1884 | <code>unavailable_start.value.code == &#x27;pick_place_execution_owner_unavailable&#x27;</code> | O | Removed `start_pick` endpoint; no matching unavailable-owner refusal found. |
| 1887 | <code>pytest.raises(HeadlessClientError)</code> | O | Removed `pick_place_sequence` endpoint; no matching unavailable-owner refusal found. |
| 1889 | <code>unavailable_status.value.code == &#x27;pick_place_execution_owner_unavailable&#x27;</code> | O | Removed `pick_place_sequence` endpoint; no matching unavailable-owner refusal found. |
| 1894 | <code>first_status[&#x27;program_binding_id&#x27;] == manifest[&#x27;program_binding_id&#x27;]</code> | P | M:3466 — generic status now identifies the direct-manual binding, not the old global Program binding; Q:2885–2890 checks a captured Program binding separately |
| 1895 | <code>first_status[&#x27;retained_bundle_id&#x27;] == manifest[&#x27;retained_bundle_id&#x27;]</code> | A | M:2767 — identity nesting changed |
| 1896 | <code>first_status[&#x27;gripper_target_role_id&#x27;] == &#x27;gripper&#x27;</code> | A | M:2788 — newly restored target-role check |
| 1897 | <code>first_status[&#x27;gripper_command_schema_id&#x27;] == manifest[&#x27;gripper_command_schema_id&#x27;]</code> | A | M:2902 — target command replaces old status field |
| 1901 | <code>first_status[&#x27;gripper_command_schema_revision&#x27;] == manifest[&#x27;gripper_command_schema_revision&#x27;]</code> | A | M:2904 — newly restored schema-revision check |
| 1905 | <code>first_status[&#x27;cell_stop_action&#x27;] == {&#x27;available&#x27;: True, &#x27;expected_stop_generation&#x27;: 0, &#x27;fault&#x27;: None, &#x27;latest_terminal&#x27;: None, &#x27;unavailable_reason&#x27;: None}</code> | A | M:2768 — field nested under control_state |
| 1916 | <code>set(observed) == {&#x27;arm&#x27;, &#x27;gripper&#x27;}</code> | A | M:2792 — together with _status_observation unique-key checks for arm and gripper |
| 1917 | <code>all((position[&#x27;kind&#x27;] == &#x27;angular_rad&#x27; for position in observed[&#x27;arm&#x27;][&#x27;positions&#x27;]))</code> | A | M:2793 — positions_rad in current DTO |
| 1921 | <code>observed[&#x27;gripper&#x27;][&#x27;positions&#x27;] == [{&#x27;kind&#x27;: &#x27;linear_m&#x27;, &#x27;value_m&#x27;: 0.0}]</code> | A | M:2797 — singular-position DTO |
| 1924 | <code>observed[&#x27;arm&#x27;][&#x27;resources&#x27;] != observed[&#x27;gripper&#x27;][&#x27;resources&#x27;]</code> | P | M:3220 — restored inequality of dispatched participant resource lists; old check compared observed resource lists, which the current observation DTO does not expose |
| 1927 | <code>direct[&#x27;program_id&#x27;] == &#x27;manyforge.direct_control.motion&#x27;</code> | A | M:2808 — manual-control nesting |
| 1928 | <code>direct[&#x27;program_binding_id&#x27;] != manifest[&#x27;program_binding_id&#x27;]</code> | A | M:2809 — manual-control nesting |
| 1929 | <code>direct[&#x27;controllable_component_id&#x27;] == observed[&#x27;arm&#x27;][&#x27;controllable_component_id&#x27;]</code> | A | M:2836 — descriptor equals selected component; _status_observation matches the same component to the observed row |
| 1933 | <code>direct[&#x27;command_target_id&#x27;] == observed[&#x27;arm&#x27;][&#x27;command_target_id&#x27;]</code> | A | M:2839 — descriptor equals selected target; _status_observation matches the same target to the observed row |
| 1934 | <code>direct[&#x27;base_frame_role_id&#x27;] == &#x27;base&#x27;</code> | E | M:2810 — same assertion expression |
| 1935 | <code>direct[&#x27;tcp_frame_role_id&#x27;] == &#x27;tcp&#x27;</code> | E | M:2811 — same assertion expression |
| 1936 | <code>direct[&#x27;planning_capability_ids&#x27;] == [&#x27;planning.motion.cartesian_path&#x27;, &#x27;planning.motion.joint_goal&#x27;, &#x27;planning.motion.pose_goal&#x27;]</code> | E | M:2812 — same assertion expression |
| 1941 | <code>[joint[&#x27;source_joint_name&#x27;] for joint in direct[&#x27;joints&#x27;]] == manifest[&#x27;source_joint_names&#x27;]</code> | E | M:2817 — same assertion expression |
| 1944 | <code>direct[&#x27;observed_state&#x27;][&#x27;source_joint_names&#x27;] == manifest[&#x27;source_joint_names&#x27;]</code> | A | M:2820 — observation moved out of command |
| 1951 | <code>sum((value * value for value in current_orientation.values())) == pytest.approx(1.0)</code> | E | M:2822 — same assertion expression |
| 1954 | <code>all((joint[&#x27;min_position_rad&#x27;] &lt;= joint[&#x27;max_position_rad&#x27;] and joint[&#x27;max_velocity_rad_s&#x27;] == pytest.approx(2.14) and (joint[&#x27;max_acceleration_rad_s2&#x27;] == pytest.approx(10.0)) for joint in direct[&#x27;joints&#x27;]))</code> | E | M:2825 — same assertion expression |
| 1961 | <code>motion_descriptor[&#x27;schema_version&#x27;] == &#x27;manyforge.manual_control_motion_descriptor.v4&#x27;</code> | C | M:2832 — expects v5 instead of v4 |
| 1964 | <code>motion_descriptor[&#x27;controllable_component_id&#x27;] == direct[&#x27;controllable_component_id&#x27;]</code> | A | M:2836 — compare selected motion component |
| 1968 | <code>motion_descriptor[&#x27;command_target_id&#x27;] == direct[&#x27;command_target_id&#x27;]</code> | A | M:2839 — compare selected motion target |
| 1969 | <code>[joint[&#x27;source_joint_name&#x27;] for joint in motion_descriptor[&#x27;joints&#x27;]] == manifest[&#x27;source_joint_names&#x27;]</code> | E | M:2840 — same assertion expression |
| 1972 | <code>all((joint[&#x27;max_velocity_rad_s&#x27;] == pytest.approx(2.14) and joint[&#x27;max_acceleration_rad_s2&#x27;] == pytest.approx(10.0) for joint in motion_descriptor[&#x27;joints&#x27;]))</code> | E | M:2843 — same assertion expression |
| 1977 | <code>[limit[&#x27;source_joint_name&#x27;] for limit in motion_descriptor[&#x27;operational_joint_limits&#x27;]] == manifest[&#x27;source_joint_names&#x27;]</code> | E | M:2848 — same assertion expression |
| 1982 | <code>all((limit[&#x27;model_max_velocity_rad_s&#x27;] == pytest.approx(2.14) and limit[&#x27;model_max_acceleration_rad_s2&#x27;] == pytest.approx(10.0) for limit in operational_limits))</code> | E | M:2853 — same assertion expression |
| 1987 | <code>operational_limits[0][&#x27;source_joint_name&#x27;] == &#x27;joint1&#x27;</code> | E | M:2858 — same assertion expression |
| 1988 | <code>operational_limits[0][&#x27;deployment_max_velocity_rad_s&#x27;] == pytest.approx(1.5)</code> | E | M:2859 — same assertion expression |
| 1991 | <code>operational_limits[0][&#x27;deployment_max_acceleration_rad_s2&#x27;] == pytest.approx(2.5)</code> | E | M:2862 — same assertion expression |
| 1994 | <code>operational_limits[0][&#x27;deployment_max_jerk_rad_s3&#x27;] is None</code> | E | M:2865 — same assertion expression |
| 1995 | <code>operational_limits[0][&#x27;effective_max_velocity_rad_s&#x27;] == pytest.approx(1.5)</code> | E | M:2866 — same assertion expression |
| 1998 | <code>operational_limits[0][&#x27;effective_max_acceleration_rad_s2&#x27;] == pytest.approx(2.5)</code> | E | M:2869 — same assertion expression |
| 2001 | <code>operational_limits[0][&#x27;effective_max_jerk_rad_s3&#x27;] is None</code> | E | M:2872 — same assertion expression |
| 2002 | <code>all((limit[&#x27;deployment_max_velocity_rad_s&#x27;] is None and limit[&#x27;deployment_max_acceleration_rad_s2&#x27;] is None and (limit[&#x27;deployment_max_jerk_rad_s3&#x27;] is None) and (limit[&#x27;effective_max_velocity_rad_s&#x27;] == pytest.approx(2.14)) and (limit[&#x27;effective_max_acceleration_rad_s2&#x27;] == pytest.approx(10.0)) and (limit[&#x27;effective_max_jerk_rad_s3&#x27;] is None) for limit in operational_limits[1:]))</code> | E | M:2873 — same assertion expression |
| 2011 | <code>{frame[&#x27;frame_role_id&#x27;] for frame in motion_descriptor[&#x27;frames&#x27;]} &gt;= {&#x27;base&#x27;, &#x27;tcp&#x27;}</code> | E | M:2882 — same assertion expression |
| 2015 | <code>motion_descriptor[&#x27;contracts&#x27;][&#x27;operation_capability_id&#x27;] == direct[&#x27;operation_capability_id&#x27;]</code> | A | M:2886 — compare selected target capability |
| 2018 | <code>motion_descriptor[&#x27;contracts&#x27;][&#x27;supporting_providers&#x27;]</code> | E | M:2889 — same assertion expression |
| 2019 | <code>motion_descriptor[&#x27;contracts&#x27;][&#x27;control_path&#x27;][&#x27;stop_evidence_policy_id&#x27;] == &#x27;lite6_motion_controlled_stop&#x27;</code> | E | M:2890 — same assertion expression |
| 2025 | <code>direct_gripper[&#x27;program_id&#x27;] == &#x27;manyforge.direct_control.gripper&#x27;</code> | A | M:2895 — manual-control nesting |
| 2026 | <code>direct_gripper[&#x27;program_binding_id&#x27;] != manifest[&#x27;program_binding_id&#x27;]</code> | A | M:2898 — manual-control nesting |
| 2027 | <code>direct_gripper[&#x27;command_schema_id&#x27;] == manifest[&#x27;gripper_command_schema_id&#x27;]</code> | E | M:2901 — same assertion expression |
| 2030 | <code>direct_gripper[&#x27;resource&#x27;][&#x27;coordinate_kind&#x27;] == &#x27;linear_m&#x27;</code> | E | M:2907 — same assertion expression |
| 2031 | <code>direct_gripper[&#x27;resource&#x27;][&#x27;source_coordinate_name&#x27;]</code> | E | M:2908 — same assertion expression |
| 2032 | <code>direct_gripper[&#x27;resource&#x27;][&#x27;min_position&#x27;] &lt;= direct_gripper[&#x27;resource&#x27;][&#x27;max_position&#x27;]</code> | E | M:2909 — same assertion expression |
| 2036 | <code>gripper_descriptor[&#x27;schema_version&#x27;] == &#x27;manyforge.manual_control_gripper_descriptor.v2&#x27;</code> | E | M:2913 — same assertion expression |
| 2039 | <code>gripper_descriptor[&#x27;description&#x27;] == &#x27;Native simulated UFACTORY lite pneumatic gripper.&#x27;</code> | C | M:2916 — expects ROS mock description rather than old native simulated description |
| 2042 | <code>gripper_descriptor[&#x27;command_kind&#x27;] == &#x27;gripper_position&#x27;</code> | E | M:2919 — same assertion expression |
| 2043 | <code>gripper_descriptor[&#x27;actuation_kind&#x27;] == &#x27;position_commanded&#x27;</code> | E | M:2920 — same assertion expression |
| 2044 | <code>gripper_descriptor[&#x27;transport_kind&#x27;] == &#x27;resolved_binding&#x27;</code> | E | M:2921 — same assertion expression |
| 2045 | <code>gripper_descriptor[&#x27;state_authority_class&#x27;] == &#x27;predicted&#x27;</code> | C | M:2922 — expects commanded, not predicted; already present by committed R4 M1 d3920e13 (and its parent); different semantics, not equivalent coverage |
| 2046 | <code>gripper_descriptor[&#x27;state_authority_id&#x27;]</code> | E | M:2923 — same assertion expression |
| 2047 | <code>gripper_descriptor[&#x27;completion_policy&#x27;] == &#x27;goal_fenced_target_reached&#x27;</code> | E | M:2924 — same assertion expression |
| 2048 | <code>gripper_descriptor[&#x27;grasp_confirmation_policy&#x27;] == &#x27;binding_feedback_required&#x27;</code> | E | M:2925 — same assertion expression |
| 2051 | <code>gripper_descriptor[&#x27;source_group_name&#x27;] == &#x27;lite_gripper&#x27;</code> | E | M:2928 — same assertion expression |
| 2052 | <code>gripper_descriptor[&#x27;command_duration_s&#x27;] == pytest.approx(0.8)</code> | E | M:2929 — same assertion expression |
| 2053 | <code>gripper_descriptor[&#x27;open_state_ref&#x27;] == &#x27;open&#x27;</code> | E | M:2930 — same assertion expression |
| 2054 | <code>gripper_descriptor[&#x27;closed_state_ref&#x27;] == &#x27;closed&#x27;</code> | E | M:2931 — same assertion expression |
| 2055 | <code>gripper_descriptor[&#x27;touch_links&#x27;] == [&#x27;lite_gripper_base_link&#x27;, &#x27;lite_gripper_left_finger&#x27;, &#x27;lite_gripper_right_finger&#x27;]</code> | E | M:2932 — same assertion expression |
| 2060 | <code>gripper_descriptor[&#x27;mount_frame&#x27;] == &#x27;link_eef&#x27;</code> | E | M:2937 — same assertion expression |
| 2061 | <code>gripper_descriptor[&#x27;attach_frame&#x27;] == &#x27;link_tcp&#x27;</code> | E | M:2938 — same assertion expression |
| 2062 | <code>gripper_descriptor[&#x27;tcp_frame&#x27;] == &#x27;link_tcp&#x27;</code> | E | M:2939 — same assertion expression |
| 2063 | <code>gripper_descriptor[&#x27;auxiliary_frames&#x27;] == [&#x27;lite_gripper_base_link&#x27;]</code> | E | M:2940 — same assertion expression |
| 2064 | <code>gripper_descriptor[&#x27;source_coordinate_name&#x27;] == direct_gripper[&#x27;resource&#x27;][&#x27;source_coordinate_name&#x27;]</code> | E | M:2941 — same assertion expression |
| 2067 | <code>gripper_descriptor[&#x27;coordinate_kind&#x27;] == &#x27;linear_m&#x27;</code> | E | M:2944 — same assertion expression |
| 2068 | <code>gripper_descriptor[&#x27;unit&#x27;] == &#x27;m&#x27;</code> | E | M:2945 — same assertion expression |
| 2069 | <code>gripper_descriptor[&#x27;min_position&#x27;] == direct_gripper[&#x27;resource&#x27;][&#x27;min_position&#x27;]</code> | E | M:2946 — same assertion expression |
| 2073 | <code>gripper_descriptor[&#x27;max_position&#x27;] == direct_gripper[&#x27;resource&#x27;][&#x27;max_position&#x27;]</code> | E | M:2950 — same assertion expression |
| 2077 | <code>[preset[&#x27;name&#x27;] for preset in gripper_descriptor[&#x27;named_presets&#x27;]] == [&#x27;close&#x27;, &#x27;closed&#x27;, &#x27;open&#x27;]</code> | E | M:2954 — same assertion expression |
| 2082 | <code>len(gripper_descriptor[&#x27;mimic_followers&#x27;]) == 1</code> | E | M:2959 — same assertion expression |
| 2084 | <code>follower[&#x27;source_coordinate_name&#x27;] == &#x27;left_finger_joint&#x27;</code> | E | M:2961 — same assertion expression |
| 2085 | <code>follower[&#x27;compiled_coordinate_name&#x27;].startswith(&#x27;mf2_joint_&#x27;)</code> | E | M:2962 — same assertion expression |
| 2086 | <code>follower[&#x27;multiplier&#x27;] == pytest.approx(1.0)</code> | E | M:2963 — same assertion expression |
| 2087 | <code>follower[&#x27;offset&#x27;] == pytest.approx(0.0)</code> | E | M:2964 — same assertion expression |
| 2088 | <code>gripper_descriptor[&#x27;contracts&#x27;][&#x27;command_schema&#x27;] == {&#x27;command_schema_id&#x27;: direct_gripper[&#x27;command_schema_id&#x27;], &#x27;command_schema_revision&#x27;: direct_gripper[&#x27;command_schema_revision&#x27;]}</code> | E | M:2965 — same assertion expression |
| 2092 | <code>gripper_descriptor[&#x27;contracts&#x27;][&#x27;control_path&#x27;][&#x27;stop_evidence_policy_id&#x27;] == &#x27;lite6_controller_goal_terminal_stop&#x27;</code> | E | M:2969 — same assertion expression |
| 2112 | <code>gripper_receipt[&#x27;commit_eligible&#x27;] is True</code> | E | M:2989 — same assertion expression |
| 2113 | <code>gripper_receipt[&#x27;target_position&#x27;] == pytest.approx(gripper_preview_target)</code> | E | M:2990 — same assertion expression |
| 2116 | <code>gripper_receipt[&#x27;coordinate_kind&#x27;] == &#x27;linear_m&#x27;</code> | E | M:2993 — same assertion expression |
| 2117 | <code>gripper_receipt[&#x27;program_binding_id&#x27;] == direct_gripper[&#x27;program_binding_id&#x27;]</code> | A | M:2995 — manual-control nesting |
| 2121 | <code>native.events(first_status[&#x27;owner_epoch&#x27;], 0)[&#x27;events&#x27;] == before_preview_events</code> | A | M:2998 — owner epoch nesting |
| 2130 | <code>after_gripper_observed[&#x27;positions&#x27;] == before_gripper_preview_state</code> | A | M:3005 — singular-position DTO |
| 2143 | <code>receipt[&#x27;commit_eligible&#x27;] is True</code> | E | M:3018 — same assertion expression |
| 2144 | <code>receipt[&#x27;source_joint_names&#x27;] == manifest[&#x27;source_joint_names&#x27;]</code> | E | M:3019 — same assertion expression |
| 2145 | <code>receipt[&#x27;final_positions_rad&#x27;] == pytest.approx(direct_positions)</code> | E | M:3020 — same assertion expression |
| 2146 | <code>receipt[&#x27;program_binding_id&#x27;] == direct[&#x27;program_binding_id&#x27;]</code> | A | M:3021 — manual-control nesting |
| 2147 | <code>&#x27;trajectory&#x27; not in receipt</code> | E | M:3022 — same assertion expression |
| 2148 | <code>&#x27;compiled&#x27; not in receipt</code> | E | M:3023 — same assertion expression |
| 2149 | <code>&#x27;providers&#x27; not in receipt</code> | E | M:3024 — same assertion expression |
| 2162 | <code>repeated_receipt[&#x27;commit_eligible&#x27;] is True</code> | E | M:3037 — same assertion expression |
| 2163 | <code>repeated_receipt[&#x27;final_positions_rad&#x27;] == pytest.approx(direct_positions)</code> | E | M:3038 — same assertion expression |
| 2176 | <code>_translation_error_m(pose_preview[&#x27;final_tcp_pose&#x27;][&#x27;position_m&#x27;], receipt[&#x27;final_tcp_pose&#x27;][&#x27;position_m&#x27;]) &lt;= pose_tolerances[&#x27;translation_tol_m&#x27;]</code> | E | M:3051 — same assertion expression |
| 2183 | <code>_rotation_error_rad(pose_preview[&#x27;final_tcp_pose&#x27;][&#x27;orientation&#x27;], receipt[&#x27;final_tcp_pose&#x27;][&#x27;orientation&#x27;]) &lt;= pose_tolerances[&#x27;rotation_tol_rad&#x27;]</code> | E | M:3058 — same assertion expression |
| 2210 | <code>cartesian_preview[&#x27;commit_eligible&#x27;] is True</code> | E | M:3084 — same assertion expression |
| 2211 | <code>native.events(first_status[&#x27;owner_epoch&#x27;], 0)[&#x27;events&#x27;] == before_preview_events</code> | A | M:3085 — owner epoch nesting |
| 2220 | <code>after_preview_arm[&#x27;positions&#x27;] == before_preview_state</code> | A | M:3092 — current observation DTO |
| 2227 | <code>pytest.raises(HeadlessClientError)</code> | S | Exact Scene-revision refusal removed by the approved current Scene rule. |
| 2229 | <code>stale_scene.value.code == &#x27;scene_changed_during_validation&#x27;</code> | S | Exact Scene-revision refusal removed by the approved current Scene rule. |
| 2234 | <code>direct_terminal[&#x27;state&#x27;] == &#x27;completed&#x27;</code> | E | M:3097 — same assertion expression |
| 2235 | <code>direct_terminal[&#x27;program_binding_id&#x27;] == direct[&#x27;program_binding_id&#x27;]</code> | A | M:3098 — manual-control nesting |
| 2259 | <code>relative_terminal[&#x27;state&#x27;] == &#x27;completed&#x27;</code> | E | M:3127 — same assertion expression |
| 2263 | <code>_translation_error_m(relative_end_pose[&#x27;position_m&#x27;], relative_start_pose[&#x27;position_m&#x27;]) &lt;= relative_payload[&#x27;tolerances&#x27;][&#x27;translation_tol_m&#x27;]</code> | E | M:3131 — same assertion expression |
| 2269 | <code>_rotation_error_rad(relative_end_pose[&#x27;orientation&#x27;], relative_start_pose[&#x27;orientation&#x27;]) &lt;= relative_payload[&#x27;tolerances&#x27;][&#x27;rotation_tol_rad&#x27;]</code> | E | M:3137 — same assertion expression |
| 2284 | <code>direct_gripper_terminal[&#x27;state&#x27;] == &#x27;completed&#x27;</code> | E | M:3216 — same assertion expression |
| 2285 | <code>direct_gripper_terminal[&#x27;program_binding_id&#x27;] == direct_gripper[&#x27;program_binding_id&#x27;]</code> | A | M:3217 — manual-control nesting |
| 2290 | <code>pytest.raises(HeadlessClientError)</code> | P | M:3253 — manual-origin invalid joint; Program-origin behavior is not proved here |
| 2297 | <code>rejected.value.code == &#x27;execution_rejected&#x27;</code> | E | M:3255 — same assertion expression; origin-scope caveat below |
| 2298 | <code>rejected.value.status_code == 400</code> | E | M:3256 — same assertion expression; origin-scope caveat below |
| 2299 | <code>recording.last_submission is not None</code> | A | M:3244 — input is built directly, rather than captured from a nullable recorder; origin-scope caveat below |
| 2303 | <code>raw_rejection.status_code == 400</code> | E | M:3261 — same assertion expression; origin-scope caveat below |
| 2304 | <code>raw_rejection.json()[&#x27;error&#x27;][&#x27;details&#x27;][&#x27;reason&#x27;] == &#x27;joint_goal_source_joint_unresolved&#x27;</code> | E | M:3262 — same assertion expression; origin-scope caveat below |
| 2315 | <code>stale_submission is not None</code> | A | M:3322 — copy of an existing dict is structurally non-null; origin-scope caveat below |
| 2317 | <code>&#x27;robot_id&#x27; not in serialized</code> | E | M:3324 — same assertion expression; origin-scope caveat below |
| 2318 | <code>&#x27;group_id&#x27; not in serialized</code> | E | M:3325 — same assertion expression; origin-scope caveat below |
| 2321 | <code>terminal[&#x27;operation&#x27;][&#x27;state&#x27;] == &#x27;completed&#x27;</code> | E | M:3328 — same assertion expression; origin-scope caveat below |
| 2324 | <code>composer_projection[&#x27;operation&#x27;] == terminal[&#x27;operation&#x27;]</code> | P | M:3335 — settled Composer and Headless reads; added comparison to terminal state excluding bookkeeping with monotonic revision/time; not old exact pre-retirement equality |
| 2325 | <code>composer.execution_diagnostics()[&#x27;status&#x27;][&#x27;owner_epoch&#x27;] == first_status[&#x27;owner_epoch&#x27;]</code> | A | M:3353 — owner epoch nesting; origin-scope caveat below |
| 2344 | <code>acknowledged[&#x27;accepted&#x27;][&#x27;control&#x27;] == &#x27;acknowledge_completed&#x27;</code> | A | M:3372 — accepted control read from journal; origin-scope caveat below |
| 2346 | <code>acknowledged_projection[&#x27;operation&#x27;][&#x27;operation_revision&#x27;] == acknowledged[&#x27;accepted&#x27;][&#x27;operation_revision_after_acceptance&#x27;]</code> | A | M:3373 — journal acknowledgement revision; origin-scope caveat below |
| 2350 | <code>pytest.raises(HeadlessClientError)</code> | P | M:3279 — manual-origin wrong unit; not Program-origin proof |
| 2357 | <code>wrong_unit.value.status_code == 400</code> | E | M:3281 — same assertion expression; origin-scope caveat below |
| 2358 | <code>recording.last_submission is not None</code> | A | M:3267 — input constructed directly; origin-scope caveat below |
| 2362 | <code>raw_wrong_unit.json()[&#x27;error&#x27;][&#x27;details&#x27;][&#x27;reason&#x27;] == &#x27;runtime_command_resource_unit_mismatch&#x27;</code> | E | M:3287 — same assertion expression; origin-scope caveat below |
| 2366 | <code>pytest.raises(HeadlessClientError)</code> | P | M:3299 — manual-origin limit; not Program-origin proof |
| 2373 | <code>beyond_limit.value.status_code == 400</code> | E | M:3301 — same assertion expression; origin-scope caveat below |
| 2374 | <code>recording.last_submission is not None</code> | A | M:3291 — input constructed directly; origin-scope caveat below |
| 2378 | <code>raw_beyond_limit.json()[&#x27;error&#x27;][&#x27;code&#x27;] == &#x27;invalid_request&#x27;</code> | E | M:3307 — same assertion expression; origin-scope caveat below |
| 2379 | <code>raw_beyond_limit.json()[&#x27;error&#x27;][&#x27;details&#x27;][&#x27;reason&#x27;] == &#x27;gripper_position_limit_violation&#x27;</code> | E | M:3308 — same assertion expression; origin-scope caveat below |
| 2391 | <code>gripper_terminal[&#x27;state&#x27;] == &#x27;completed&#x27;</code> | E | M:3388 — same assertion expression; origin-scope caveat below |
| 2392 | <code>gripper_terminal[&#x27;participants&#x27;][0][&#x27;payload_kind&#x27;] == &#x27;capability_command&#x27;</code> | E | M:3389 — same assertion expression; origin-scope caveat below |
| 2395 | <code>composer.operation(gripper_id)[&#x27;operation&#x27;] == gripper_terminal</code> | P | M:3398 — excludes operation revision and update time; added monotonic revision/time assertions; old equality was exact |
| 2402 | <code>gripper_observed[&#x27;positions&#x27;] == [{&#x27;kind&#x27;: &#x27;linear_m&#x27;, &#x27;value_m&#x27;: pytest.approx(0.004)}]</code> | A | M:3403 — singular-position DTO; origin-scope caveat below |
| 2433 | <code>cancelled[&#x27;state&#x27;] == &#x27;stopped&#x27;</code> | E | M:3431 — same assertion expression; origin-scope caveat below |
| 2434 | <code>cancelled[&#x27;active_stop_id&#x27;] == gripper_stop[&#x27;accepted&#x27;][&#x27;stop_id&#x27;]</code> | E | M:3432 — same assertion expression; origin-scope caveat below |
| 2452 | <code>gripper_recovery is None</code> | E | M:3444 — same assertion expression; origin-scope caveat below |
| 2453 | <code>headless.stop_fault()[&#x27;stop_fault&#x27;] is None</code> | E | M:3445 — same assertion expression; origin-scope caveat below |
| 2470 | <code>scale_selector == {&#x27;program_binding_id&#x27;: first_status[&#x27;program_binding_id&#x27;], &#x27;component_role_id&#x27;: &#x27;robot&#x27;, &#x27;target_role_id&#x27;: &#x27;arm&#x27;}</code> | P | M:3466 — generic selector uses the direct-manual binding; the old asserted selector used the authored Program binding |
| 2478 | <code>initial_scale_control[&#x27;supported&#x27;] is True</code> | E | M:3474 — same assertion expression; origin-scope caveat below |
| 2494 | <code>scaled[&#x27;accepted&#x27;][&#x27;control_generation&#x27;] == initial_scale_control[&#x27;control_generation&#x27;] + 1</code> | E | M:3490 — same assertion expression; origin-scope caveat below |
| 2497 | <code>scaled[&#x27;accepted&#x27;][&#x27;active_operation_id&#x27;] == controlled_id</code> | E | M:3493 — same assertion expression; origin-scope caveat below |
| 2505 | <code>scale_status[&#x27;runtime_scale_control&#x27;][&#x27;requested_scale&#x27;] == 0.5</code> | E | M:3501 — same assertion expression; origin-scope caveat below |
| 2506 | <code>scale_status[&#x27;runtime_scale_control&#x27;][&#x27;effective_scale&#x27;] == 0.5</code> | E | M:3502 — same assertion expression; origin-scope caveat below |
| 2507 | <code>scale_status[&#x27;runtime_scale_control&#x27;][&#x27;active_operation_scale_generation&#x27;] == 1</code> | E | M:3503 — same assertion expression; origin-scope caveat below |
| 2527 | <code>paused_state[&#x27;operation_revision&#x27;] == paused[&#x27;accepted&#x27;][&#x27;operation_revision_after_acceptance&#x27;]</code> | S | Operation-level Pause was removed in the current design. |
| 2547 | <code>resumed_state[&#x27;operation_revision&#x27;] == resumed[&#x27;accepted&#x27;][&#x27;operation_revision_after_acceptance&#x27;]</code> | S | Operation-level Pause/Resume was removed in the current design. |
| 2567 | <code>stop_replay[&#x27;accepted&#x27;][&#x27;stop_id&#x27;] == stop_accepted[&#x27;accepted&#x27;][&#x27;stop_id&#x27;]</code> | E | M:3525 — same assertion expression; origin-scope caveat below |
| 2570 | <code>stop_replay[&#x27;accepted&#x27;][&#x27;stop_generation&#x27;] == stop_accepted[&#x27;accepted&#x27;][&#x27;stop_generation&#x27;]</code> | E | M:3528 — same assertion expression; origin-scope caveat below |
| 2574 | <code>stop_replay[&#x27;accepted&#x27;][&#x27;replay_status&#x27;] == &#x27;replayed_exact&#x27;</code> | E | M:3532 — same assertion expression; origin-scope caveat below |
| 2577 | <code>stopped[&#x27;state&#x27;] == &#x27;stopped&#x27;</code> | E | M:3535 — same assertion expression; origin-scope caveat below |
| 2588 | <code>current_query.query_operation(controlled_query)[&#x27;operation&#x27;][&#x27;operation_id&#x27;] == controlled_id</code> | P | Q:2877–2890 checks captured Program/run and binding and records the returned operation ID, but does not compare it to the independently known controlled ID as the old check did. |
| 2592 | <code>stopped[&#x27;active_stop_id&#x27;] == stop_accepted[&#x27;accepted&#x27;][&#x27;stop_id&#x27;]</code> | E | M:3536 — same assertion expression; origin-scope caveat below |
| 2593 | <code>stop_acceptance_elapsed_s &lt; 0.5</code> | E | M:3537 — same assertion expression; origin-scope caveat below |
| 2595 | <code>0.9 &lt;= stop_settlement_elapsed_s &lt; 1.75</code> | E | M:3541 — same assertion expression; origin-scope caveat below |
| 2602 | <code>projected_stop[&#x27;expected_stop_generation&#x27;] == stop_accepted[&#x27;accepted&#x27;][&#x27;stop_generation&#x27;]</code> | E | M:3548 — same assertion expression; origin-scope caveat below |
| 2605 | <code>projected_stop[&#x27;latest_terminal&#x27;][&#x27;certificate&#x27;][&#x27;stop_id&#x27;] == stop_accepted[&#x27;accepted&#x27;][&#x27;stop_id&#x27;]</code> | E | M:3551 — same assertion expression; origin-scope caveat below |
| 2609 | <code>terminal[&#x27;result&#x27;] == &#x27;succeeded_nominal&#x27;</code> | E | M:3555 — same assertion expression; origin-scope caveat below |
| 2610 | <code>terminal[&#x27;escalated&#x27;] is False</code> | E | M:3556 — same assertion expression; origin-scope caveat below |
| 2611 | <code>terminal[&#x27;hard_stop_floor_used&#x27;] is False</code> | E | M:3557 — same assertion expression; origin-scope caveat below |
| 2612 | <code>len(terminal[&#x27;executed_stages&#x27;]) == 1</code> | E | M:3558 — same assertion expression; origin-scope caveat below |
| 2614 | <code>controlled_stage[&#x27;action_class&#x27;] == &#x27;controlled&#x27;</code> | E | M:3560 — same assertion expression; origin-scope caveat below |
| 2615 | <code>controlled_stage[&#x27;executed_mode&#x27;] == &#x27;soft_on_path&#x27;</code> | E | M:3561 — same assertion expression; origin-scope caveat below |
| 2616 | <code>controlled_stage[&#x27;stage_id&#x27;] == &#x27;nominal&#x27;</code> | E | M:3562 — same assertion expression; origin-scope caveat below |
| 2617 | <code>controlled_stage[&#x27;stage_index&#x27;] == 0</code> | E | M:3563 — same assertion expression; origin-scope caveat below |
| 2618 | <code>controlled_stage[&#x27;outcome&#x27;] == &#x27;succeeded&#x27;</code> | E | M:3564 — same assertion expression; origin-scope caveat below |
| 2619 | <code>controlled_stage[&#x27;substitution_reason&#x27;] is None</code> | E | M:3565 — same assertion expression; origin-scope caveat below |
| 2645 | <code>durable_stop is not None</code> | E | M:3591 — same assertion expression; origin-scope caveat below |
| 2648 | <code>durable_stop_payload[&#x27;intent&#x27;][&#x27;value&#x27;][&#x27;request_attribution&#x27;] == {&#x27;assurance&#x27;: &#x27;externally_authenticated&#x27;, &#x27;principal_id&#x27;: &#x27;operator.process-test&#x27;, &#x27;transport&#x27;: &#x27;http&#x27;}</code> | E | M:3594 — same assertion expression; origin-scope caveat below |
| 2656 | <code>len(durable_stop_page[&#x27;events&#x27;]) == 1</code> | E | M:3602 — same assertion expression; origin-scope caveat below |
| 2658 | <code>durable_stop_event[&#x27;sequence&#x27;] == durable_stop_sequence</code> | E | M:3604 — same assertion expression; origin-scope caveat below |
| 2659 | <code>durable_stop_event[&#x27;record_kind&#x27;] == &#x27;stop_acceptance_projected&#x27;</code> | E | M:3605 — same assertion expression; origin-scope caveat below |
| 2660 | <code>durable_stop_event[&#x27;operation_id&#x27;] is None</code> | E | M:3606 — same assertion expression; origin-scope caveat below |
| 2661 | <code>durable_stop_event[&#x27;operation_revision&#x27;] is None</code> | E | M:3607 — same assertion expression; origin-scope caveat below |
| 2662 | <code>durable_stop_event[&#x27;payload&#x27;] == durable_stop_payload</code> | E | M:3608 — same assertion expression; origin-scope caveat below |
| 2663 | <code>durable_terminal is not None</code> | E | M:3609 — same assertion expression; origin-scope caveat below |
| 2667 | <code>len(terminal_page[&#x27;events&#x27;]) == 1</code> | E | M:3613 — same assertion expression; origin-scope caveat below |
| 2669 | <code>terminal_event[&#x27;record_kind&#x27;] == &#x27;stop_terminal_projected&#x27;</code> | E | M:3615 — same assertion expression; origin-scope caveat below |
| 2670 | <code>terminal_event[&#x27;request_id&#x27;] is None</code> | E | M:3616 — same assertion expression; origin-scope caveat below |
| 2671 | <code>terminal_event[&#x27;operation_id&#x27;] is None</code> | E | M:3617 — same assertion expression; origin-scope caveat below |
| 2672 | <code>terminal_event[&#x27;operation_revision&#x27;] is None</code> | E | M:3618 — same assertion expression; origin-scope caveat below |
| 2674 | <code>terminal_history[&#x27;api_version&#x27;] == &#x27;manyforge.stop_terminal_projection.v1&#x27;</code> | E | M:3620 — same assertion expression; origin-scope caveat below |
| 2677 | <code>terminal_history[&#x27;durable_projection_digest&#x27;] == durable_terminal[1]</code> | E | M:3623 — same assertion expression; origin-scope caveat below |
| 2678 | <code>terminal_history[&#x27;details_truncated&#x27;] is False</code> | E | M:3624 — same assertion expression; origin-scope caveat below |
| 2679 | <code>terminal_history[&#x27;request_id&#x27;] == &#x27;stop-controlled-operation&#x27;</code> | E | M:3625 — same assertion expression; origin-scope caveat below |
| 2680 | <code>terminal_history[&#x27;result&#x27;] == &#x27;succeeded_nominal&#x27;</code> | E | M:3626 — same assertion expression; origin-scope caveat below |
| 2681 | <code>terminal_history[&#x27;state&#x27;] == &#x27;succeeded&#x27;</code> | E | M:3627 — same assertion expression; origin-scope caveat below |
| 2682 | <code>900000000 &lt;= terminal_history[&#x27;acceptance_to_terminal_latency_ns&#x27;] &lt; 1750000000</code> | E | M:3628 — same assertion expression; origin-scope caveat below |
| 2687 | <code>terminal_history[&#x27;attempted_stage_count&#x27;] == 1</code> | E | M:3633 — same assertion expression; origin-scope caveat below |
| 2689 | <code>terminal_attempt[&#x27;stage&#x27;][&#x27;command_kind&#x27;] == &#x27;motion_controlled_stop&#x27;</code> | E | M:3635 — same assertion expression; origin-scope caveat below |
| 2690 | <code>terminal_attempt[&#x27;profile&#x27;] is not None</code> | E | M:3636 — same assertion expression; origin-scope caveat below |
| 2691 | <code>terminal_attempt[&#x27;profile&#x27;][&#x27;profile_id&#x27;] == &#x27;operator_soft_on_path&#x27;</code> | E | M:3637 — same assertion expression; origin-scope caveat below |
| 2692 | <code>terminal_attempt[&#x27;profile&#x27;][&#x27;stop_mode&#x27;] == &#x27;soft_on_path&#x27;</code> | E | M:3638 — same assertion expression; origin-scope caveat below |
| 2693 | <code>terminal_attempt[&#x27;actual_stop_mode&#x27;] == &#x27;soft_on_path&#x27;</code> | E | M:3639 — same assertion expression; origin-scope caveat below |
| 2697 | <code>terminal_attempt[&#x27;profile_source&#x27;] == &#x27;participant_profile_mapping&#x27;</code> | E | M:3643 — same assertion expression; origin-scope caveat below |
| 2698 | <code>terminal_attempt[&#x27;outcome&#x27;] == &#x27;succeeded&#x27;</code> | E | M:3644 — same assertion expression; origin-scope caveat below |
| 2699 | <code>any((item[&#x27;evidence&#x27;] is not None and item[&#x27;evidence&#x27;].get(&#x27;requested_stop_mode&#x27;) == &#x27;soft_on_path&#x27; and (item[&#x27;evidence&#x27;].get(&#x27;actual_stop_mode&#x27;) == &#x27;soft_on_path&#x27;) and (item[&#x27;evidence&#x27;].get(&#x27;backend_stop_confirmed&#x27;) is True) for item in terminal_attempt[&#x27;evidence_items&#x27;]))</code> | E | M:3645 — same assertion expression; origin-scope caveat below |
| 2706 | <code>terminal_history[&#x27;participant_settlement_count&#x27;] == 1</code> | E | M:3652 — same assertion expression; origin-scope caveat below |
| 2707 | <code>terminal_history[&#x27;participant_settlement&#x27;][0][&#x27;facts&#x27;][&#x27;terminal_settlement&#x27;] == &#x27;settled&#x27;</code> | E | M:3653 — same assertion expression; origin-scope caveat below |
| 2726 | <code>pytest.raises(HeadlessApiError)</code> | A | Q:3004–3014 checks refusal after retirement; test_native_execution_path.py:3905 checks a never-issued context. |
| 2728 | <code>retired_query.value.code == &#x27;execution_operation_context_unavailable&#x27;</code> | A | Q:3004–3014 checks execution_operation_context_unavailable after retirement. |
| 2732 | <code>persisted_scale_control[&#x27;control_generation&#x27;] == scaled[&#x27;accepted&#x27;][&#x27;control_generation&#x27;]</code> | E | M:3665 — same assertion expression; origin-scope caveat below |
| 2736 | <code>persisted_scale_control[&#x27;requested_scale&#x27;] == 0.5</code> | E | M:3669 — same assertion expression; origin-scope caveat below |
| 2747 | <code>controlled_recovery is None</code> | E | M:3680 — same assertion expression; origin-scope caveat below |
| 2748 | <code>headless.stop_fault()[&#x27;stop_fault&#x27;] is None</code> | E | M:3681 — same assertion after controlled Stop, not gripper Stop; origin-scope caveat below |
| 2765 | <code>malformed_response.status_code == 200</code> | E | H:2433 — same assertion expression; origin-scope caveat below |
| 2767 | <code>malformed_acceptance[&#x27;request_id&#x27;] == &#x27;malformed-stop.native-malformed-stop&#x27;</code> | E | H:2435 — same assertion expression; origin-scope caveat below |
| 2770 | <code>malformed_acceptance[&#x27;resolution_disposition&#x27;] == &#x27;promoted_to_full&#x27;</code> | E | H:2438 — same assertion expression; origin-scope caveat below |
| 2771 | <code>malformed_acceptance[&#x27;resolution_reason&#x27;] == &#x27;malformed_ordinary_intent&#x27;</code> | E | H:2439 — same assertion expression; origin-scope caveat below |
| 2775 | <code>malformed_terminal[&#x27;state&#x27;] == &#x27;stopped&#x27;</code> | E | H:2441 — same assertion expression; origin-scope caveat below |
| 2776 | <code>malformed_terminal[&#x27;active_stop_id&#x27;] == malformed_acceptance[&#x27;stop_id&#x27;]</code> | E | H:2442 — same assertion expression; origin-scope caveat below |
| 2786 | <code>malformed_row is not None</code> | E | H:2452 — same assertion expression; origin-scope caveat below |
| 2788 | <code>malformed_payload[&#x27;acceptance&#x27;][&#x27;malformed_ingress_reason&#x27;] == &#x27;recognized_stop_payload_invalid&#x27;</code> | E | H:2454 — same assertion expression; origin-scope caveat below |
| 2791 | <code>malformed_payload[&#x27;acceptance&#x27;][&#x27;normalized_request_digest&#x27;] is None</code> | E | H:2457 — same assertion expression; origin-scope caveat below |
| 2792 | <code>malformed_payload[&#x27;intent&#x27;] == {&#x27;intent_kind&#x27;: &#x27;malformed_ingress&#x27;, &#x27;value&#x27;: None}</code> | E | H:2458 — same assertion expression; origin-scope caveat below |
| 2804 | <code>recovered_result is not None</code> | E | H:2491 — same assertion expression; origin-scope caveat below |
| 2805 | <code>recovered_result[&#x27;accepted&#x27;] is True</code> | E | H:2492 — same assertion expression; origin-scope caveat below |
| 2806 | <code>recovered_result[&#x27;latch&#x27;][&#x27;state&#x27;] == &#x27;cleared&#x27;</code> | E | H:2493 — same assertion expression; origin-scope caveat below |
| 2807 | <code>headless.stop_fault()[&#x27;stop_fault&#x27;] is None</code> | A | D:2677 — restored post-recovery query; origin-scope caveat below |
| 2816 | <code>len(clear_rows) == 1</code> | E | H:2503 — same assertion expression; origin-scope caveat below |
| 2818 | <code>durable_clear[0] == &#x27;manyforge.internal.stop-journal-fault-clear-projection.v2&#x27;</code> | E | H:2505 — same assertion expression; origin-scope caveat below |
| 2821 | <code>durable_clear[2][0] == recovered_result[&#x27;request_id&#x27;]</code> | E | H:2508 — same assertion expression; origin-scope caveat below |
| 2825 | <code>len(clear_page[&#x27;events&#x27;]) == 1</code> | E | H:2510 — same assertion expression; origin-scope caveat below |
| 2827 | <code>clear_event[&#x27;record_kind&#x27;] == &#x27;stop_fault_cleared&#x27;</code> | E | H:2512 — same assertion expression; origin-scope caveat below |
| 2828 | <code>clear_event[&#x27;request_id&#x27;] == recovered_result[&#x27;request_id&#x27;]</code> | E | H:2513 — same assertion expression; origin-scope caveat below |
| 2829 | <code>clear_event[&#x27;operation_id&#x27;] is None</code> | E | H:2514 — same assertion expression; origin-scope caveat below |
| 2830 | <code>clear_event[&#x27;operation_revision&#x27;] is None</code> | E | H:2515 — same assertion expression; origin-scope caveat below |
| 2832 | <code>clear_history[&#x27;api_version&#x27;] == &#x27;manyforge.stop_fault_clear_projection.v1&#x27;</code> | E | H:2517 — same assertion expression; origin-scope caveat below |
| 2835 | <code>clear_history[&#x27;fault_latch&#x27;][&#x27;latch_id&#x27;] == recovered_result[&#x27;latch&#x27;][&#x27;latch_id&#x27;]</code> | E | H:2518 — same assertion expression; origin-scope caveat below |
| 2838 | <code>clear_history[&#x27;fault_latch&#x27;][&#x27;state&#x27;] == &#x27;cleared&#x27;</code> | E | H:2521 — same assertion expression; origin-scope caveat below |
| 2839 | <code>clear_history[&#x27;fault_latch&#x27;][&#x27;recovery_status&#x27;] == &#x27;cleared&#x27;</code> | E | H:2522 — same assertion expression; origin-scope caveat below |
| 2840 | <code>clear_history[&#x27;result&#x27;][&#x27;accepted&#x27;] is True</code> | E | H:2523 — same assertion expression; origin-scope caveat below |
| 2846 | <code>len(largest_page[&#x27;events&#x27;]) == 16</code> | A | Q:2721–2728 checks a full 16-event page in the manual-handover qualification workflow, not the two replacement tests. |
| 2847 | <code>largest_page[&#x27;has_more&#x27;] is True</code> | A | Q:2721–2728 checks continuation in the manual-handover qualification workflow, not the two replacement tests. |
| 2848 | <code>largest_page[&#x27;next_after_sequence&#x27;] == largest_page[&#x27;events&#x27;][-1][&#x27;sequence&#x27;]</code> | A | Q:2721–2728 checks the next cursor in the manual-handover qualification workflow, not the two replacement tests. |
| 2863 | <code>(execution_run_root.stat().st_dev, execution_run_root.stat().st_ino) == run_root_identity</code> | E | M:3692 — same assertion expression |
| 2867 | <code>not (execution_run_root / &#x27;execution-command.sock&#x27;).exists()</code> | E | M:3696 — same assertion expression |
| 2868 | <code>not (execution_run_root / &#x27;execution-observation.sock&#x27;).exists()</code> | E | M:3697 — same assertion expression |
| 2872 | <code>(durable_journal.stat().st_dev, durable_journal.stat().st_ino) == durable_identity</code> | E | M:3701 — same assertion expression |
| 2875 | <code>(execution_run_root.stat().st_dev, execution_run_root.stat().st_ino) == run_root_identity</code> | E | M:3704 — same assertion after restart |
| 2879 | <code>(execution_run_root / &#x27;execution-command.sock&#x27;).is_socket()</code> | E | M:3708 — same assertion expression |
| 2880 | <code>(execution_run_root / &#x27;execution-observation.sock&#x27;).is_socket()</code> | E | M:3709 — same assertion expression |
| 2882 | <code>restarted[&#x27;owner_epoch&#x27;] != first_status[&#x27;owner_epoch&#x27;]</code> | A | M:3711 — owner epoch nesting |
| 2883 | <code>pytest.raises(HeadlessClientError)</code> | A | M:3712 — replay after restart |
| 2885 | <code>stale.value.code == &#x27;stale_request_owner_epoch&#x27;</code> | E | M:3714 — same assertion expression |
| 2886 | <code>stale.value.status_code == 409</code> | E | M:3715 — same assertion expression |
| 2888 | <code>retained[&#x27;operation&#x27;] == acknowledged_projection[&#x27;operation&#x27;]</code> | E | M:3717 — same assertion expression |
