#!/bin/bash
# Smoke check of a ROS Lyrical image (source-built or apt base). Run it inside
# a throwaway container of the image, through the image entrypoint when it has
# one so the checked environment is the one ROS processes get:
#   docker run --rm -e ROS_DOMAIN_ID=91 -v "$PWD/scripts/ros:/smoke:ro" <image> \
#     bash /smoke/smoke-ros-lyrical-base.sh
# Checks: ros2 doctor, key package versions against the ManyForge floors
# (qualified level of 2026-11-08: JTC 6.10.1, ros2_control 6.9.0), no
# ROS or ROS-workspace library linking venv/cmeel libraries (RUNPATH and ldd
# under the current environment), robot_state_publisher with an xacro'd Lite 6
# URDF, ros2_control_node with mock hardware loading a position-only JTC
# (~/speed_scaling_input), and a publisher/subscriber pair.
set -uo pipefail
export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-91}"
export ROS_AUTOMATIC_DISCOVERY_RANGE="${ROS_AUTOMATIC_DISCOVERY_RANGE:-LOCALHOST}"
if [ "${ROS_DOMAIN_ID}" = 0 ]; then echo "refusing ROS_DOMAIN_ID=0" >&2; exit 2; fi
if [ -z "${AMENT_PREFIX_PATH:-}" ]; then
    set +u; source "/opt/ros/${ROS_DISTRO:-lyrical}/setup.bash"
    for overlay in /opt/manyforge/topic_based_ros2_control/install /opt/manyforge/xarm_ws/install; do
        [ -f "${overlay}/local_setup.bash" ] && source "${overlay}/local_setup.bash"
    done
    set -u
fi
work=$(mktemp -d)
failures=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; failures=$((failures + 1)); }
cleanup() { kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "${work}"; }
trap cleanup EXIT

echo "== environment"
echo "ROS_DISTRO=${ROS_DISTRO:-} ROS_DOMAIN_ID=${ROS_DOMAIN_ID} RMW_IMPLEMENTATION=${RMW_IMPLEMENTATION:-<default>}"
echo "LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}"
grep -h . /etc/ld.so.conf.d/*.conf 2>/dev/null | sed 's/^/ld.so.conf: /'

echo "== ros2 doctor --report"
if timeout 60 ros2 doctor --report > "${work}/doctor.txt" 2>&1; then
    grep -E '^(distribution name|distribution status|middleware name|platform name)' "${work}/doctor.txt"
    grep -q '^distribution name *: lyrical' "${work}/doctor.txt" && pass "doctor: lyrical" || fail "doctor: distribution"
else
    cat "${work}/doctor.txt"; fail "ros2 doctor --report"
fi

echo "== package versions"
version_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]; }
while read -r pkg floor; do
    v=$(ros2 pkg xml "${pkg}" -t version 2>/dev/null)
    if [ -z "${v}" ]; then fail "${pkg} missing"; continue; fi
    if [ "${floor}" = - ] || version_ge "${v}" "${floor}"; then pass "${pkg} ${v} (floor ${floor})"
    else fail "${pkg} ${v} < ${floor}"; fi
done <<'EOF'
rmw_fastrtps_cpp 9.4.10
rmw_fastrtps_shared_cpp 9.4.10
joint_trajectory_controller 6.10.1
ros2_controllers 6.9.0
controller_manager 6.9.0
hardware_interface 6.9.0
realtime_tools 5.4.0
ros2_control_cmake 1.0.0
parallel_gripper_controller 6.10.1
robot_state_publisher -
rclcpp -
rclpy -
xarm_controller -
topic_based_ros2_control -
EOF
ls "/opt/ros/${ROS_DISTRO:-lyrical}"/lib/libfastdds.so.* 2>/dev/null | sed 's/^/Fast DDS library: /'
echo "rmw_fastrtps_shared_cpp library: $(ros2 pkg prefix rmw_fastrtps_shared_cpp)/lib/librmw_fastrtps_shared_cpp.so"

echo "== library closure (no venv/cmeel libraries)"
bad='/opt/manyforge/venv|cmeel\.prefix|site-packages/lib'
: > "${work}/closure.txt"
n=0
for root in "/opt/ros/${ROS_DISTRO:-lyrical}" /opt/manyforge/xarm_ws/install /opt/manyforge/topic_based_ros2_control/install \
            /opt/manyforge/control_overlay /opt/manyforge/rmw_fastrtps_overlay; do
    [ -d "${root}" ] || continue
    while IFS= read -r -d '' f; do
        head -c 4 "${f}" 2>/dev/null | grep -q $'\x7fELF' || continue
        n=$((n + 1))
        readelf -d "${f}" 2>/dev/null | grep -E 'R(UN)?PATH' | grep -E "${bad}" | sed "s#^#${f}: #" >> "${work}/closure.txt"
        ldd "${f}" 2>/dev/null | grep -E "=> .*(${bad})|not found" | sed "s#^#${f}: #" >> "${work}/closure.txt"
        # Workspace plugins load by dlopen, so an unresolved symbol only shows at controller start.
        case "${root}:${f}" in
            /opt/ros/*) ;;
            *.so) ldd -r "${f}" 2>/dev/null | grep 'undefined symbol' | sed "s#^#${f}: #" >> "${work}/closure.txt" ;;
        esac
    done < <(find "${root}" -type f \( -name '*.so*' -o -perm -u+x \) -print0)
done
if [ -s "${work}/closure.txt" ]; then
    head -40 "${work}/closure.txt"; fail "library closure: $(wc -l < "${work}/closure.txt") offending entries in ${n} ELF files"
else
    pass "library closure: ${n} ELF files, none resolve into venv/cmeel, none missing, no unresolved workspace symbols"
fi

echo "== robot_state_publisher with Lite 6 URDF"
desc=$(ros2 pkg prefix xarm_description 2>/dev/null)/share/xarm_description/urdf/xarm_device.urdf.xacro
if xacro "${desc}" dof:=6 robot_type:=lite ros2_control_plugin:=mock_components/GenericSystem > "${work}/lite6.urdf" 2> "${work}/xacro.err"; then
    pass "xacro lite6 ($(grep -c '<joint ' "${work}/lite6.urdf") joints)"
else
    cat "${work}/xacro.err"; fail "xacro lite6"
fi
ros2 run robot_state_publisher robot_state_publisher --ros-args \
    -p robot_description:="$(cat "${work}/lite6.urdf")" > "${work}/rsp.log" 2>&1 &
if timeout 20 ros2 topic echo --once --qos-durability transient_local --qos-reliability reliable /tf_static tf2_msgs/msg/TFMessage > "${work}/tf_static.txt" 2>&1 && grep -q child_frame_id "${work}/tf_static.txt"; then
    pass "robot_state_publisher publishes /tf_static"
else
    cat "${work}/rsp.log"; fail "robot_state_publisher /tf_static"
fi
timeout 20 ros2 topic echo --once --qos-durability transient_local --qos-reliability reliable --field data /robot_description std_msgs/msg/String > "${work}/desc.txt" 2>&1 \
    && grep -q '<robot' "${work}/desc.txt" && pass "/robot_description" || fail "/robot_description"

echo "== ros2_control_node with mock hardware and position-only JTC"
cat > "${work}/controllers.yaml" <<'EOF'
controller_manager:
  ros__parameters:
    update_rate: 100
    joint_state_broadcaster:
      type: joint_state_broadcaster/JointStateBroadcaster
    joint_trajectory_controller:
      type: joint_trajectory_controller/JointTrajectoryController
joint_trajectory_controller:
  ros__parameters:
    joints: [joint1, joint2, joint3, joint4, joint5, joint6]
    command_interfaces: [position]
    state_interfaces: [position, velocity]
EOF
ros2 run controller_manager ros2_control_node --ros-args --params-file "${work}/controllers.yaml" \
    > "${work}/cm.log" 2>&1 &
if timeout 60 ros2 run controller_manager spawner joint_state_broadcaster joint_trajectory_controller \
        --param-file "${work}/controllers.yaml" --controller-manager-timeout 30 > "${work}/spawner.log" 2>&1; then
    pass "spawned joint_state_broadcaster + joint_trajectory_controller"
else
    cat "${work}/spawner.log"; tail -30 "${work}/cm.log"; fail "spawner"
fi
timeout 20 ros2 control list_controllers 2>&1 | sed 's/^/  /'
timeout 20 ros2 topic list > "${work}/topics.txt" 2>&1
if grep -qx /joint_trajectory_controller/speed_scaling_input "${work}/topics.txt"; then
    pass "/joint_trajectory_controller/speed_scaling_input exists"
else
    sed 's/^/  /' "${work}/topics.txt"; fail "speed_scaling_input missing"
fi
timeout 20 ros2 topic echo --once /joint_states sensor_msgs/msg/JointState > /dev/null 2>&1 && pass "/joint_states" || fail "/joint_states"
grep -iE 'error|exception|bad_alloc' "${work}/cm.log" | head -5

echo "== publisher/subscriber"
ros2 topic pub -r 5 /manyforge_smoke std_msgs/msg/String '{data: smoke}' > /dev/null 2>&1 &
timeout 20 ros2 topic echo --once --field data /manyforge_smoke std_msgs/msg/String > "${work}/echo.txt" 2>&1
grep -qx smoke "${work}/echo.txt" && pass "talker/listener" || { cat "${work}/echo.txt"; fail "talker/listener"; }

echo "== result: ${failures} failure(s)"
exit "${failures}"
