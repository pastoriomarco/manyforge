# Third-party notices: ManyForge reference ROS image

This file lists the third-party software that the ManyForge reference ROS image redistributes: the image built from
`Dockerfile.ros-lyrical-full` with its default build arguments (`ROS_BASE=source`, `ROBOPLAN_SOURCE_MODE=qualified`).
ManyForge's own source is not in the image (it is mounted at run time); the image holds Ubuntu, ROS 2 Lyrical built from
source, robot drivers, robotics libraries, a Python environment and Node.js.

For each component it gives the version or pinned reference, the licence as its owner declares it, the upstream source and
where the declaration was taken from. ManyForge never writes, assigns or infers a licence for someone else's work: where
an owner declares none, this file says **not declared upstream**; where a declaration is unclear, it names the owner to
ask (**to confirm with ...**). Licence names are quoted as declared (for example "Apache License 2.0" or "BSD" from a
`package.xml`), not translated. ManyForge itself is licensed under Apache-2.0 (see `LICENSE` and `NOTICE`).

This is a factual inventory, not legal advice.

Evidence used, in order of preference:

- **ledger**: `third_party/external_delta_ledger.v1.json` records the source identity and the SHA-256 of the licence file.
  The checkouts under `.docker/` match those digests.
- **source**: the licence file, `package.xml` `<license>` tag or other owner statement in the pinned source.
- **deb**: the Debian `copyright` file of the installed Ubuntu (or `packages.ros.org`) package.
- **wheel**: the wheel's `METADATA` (`License-Expression`/`License`) and its bundled licence files.
- **public**: an established public fact, not checked against a file here.

Versions marked *observed* are not pinned by the build; they were read from an amd64 build of this Dockerfile from
November 2026 and resolve again at each build. Items marked **to confirm** could not be established from the evidence
above.

## Licence files in the image

The owners' licence and notice files are copied **verbatim** from their sources. Products that install their own (Ubuntu
packages, most Python wheels, the xArm source tree, npm) keep them where they are. The rest are in one directory,
`/usr/share/licenses/manyforge-image/`:

| Path | Contents | Taken from |
| --- | --- | --- |
| `manyforge/` | ManyForge's `LICENSE`, `NOTICE` and this file | this repository |
| `ros-source/src/<repository>/...` | `LICENSE*`, `LICENCE*`, `COPYING*` and `NOTICE*` files of every ROS source repository with a built package: the repository root, and each built package directory up to three levels deep (test and doc directories excluded; this includes bundled code such as Fast DDS `thirdparty/`) | the ROS source tree at build time (`/opt/ros_src`, same paths) |
| `ros-source/build/<package>/...` | the same files from the upstream sources that built `*_vendor` packages fetched into their build trees (for example gz-math, sdformat, mcap, foonathan/memory) | the build tree at build time |
| `topic_based_ros2_control/LICENSE` | | the pinned checkout |
| `roboplan-closure/{roboplan,proxsuite,toppra,nanoflann,jrl-cmakemodules}/...` | `roboplan/LICENSE`, `proxsuite/LICENSE`, `toppra/LICENSE`, `nanoflann/COPYING`, `jrl-cmakemodules/LICENSE` and `jrl-cmakemodules/v2/LICENSE` | the pinned `.docker/` sources |
| `cmeel/{assimp,boost,console_bridge,octomap,tinyxml2,urdfdom,zlib}/` | the upstream licence file of each cmeel wheel that ships none | the sha256-pinned sdist of the same version (console_bridge, OctoMap, TinyXML-2, urdfdom); for Assimp, which has no sdist, `LICENSE` at the assimp v6.0.5 commit that cmeel-assimp 6.0.5 builds; for Boost and zlib, whose sdists hold only build scripts, `LICENSE_1_0.txt` at boost-1.90.0 and `LICENSE` at zlib v1.3.1 (each sha256-pinned) |
| `nodejs/LICENSE` | Node.js's `LICENSE`, with the notices of its bundled components | `/usr/local/LICENSE` of the pinned `node:22-slim` |

Other places: Ubuntu packages in `/usr/share/doc/<package>/copyright` and `/usr/share/common-licenses/`; Python wheels in
their `*.dist-info`; the xArm driver and SDK source with their `LICENSE` files in `/opt/manyforge/xarm_ws/src/`; RoboPlan's
vendored `tl::expected` with its `COPYING` in the venv `cmeel.prefix/include/tl/`; the `so101_robot_model` `LICENSE` in its
model directory; exact source records in `/opt/ros/lyrical/manyforge-source/` (ROS repository list, exact commits, build
set), `/opt/manyforge/*/UPSTREAM_COMMIT(S)` and `VENDOR_COMMIT`.

## Source of copyleft components

Decided (OD-315, notices review 2026-11-19): ManyForge does not publish its own source bundle. The corresponding source of
the GPL, LGPL and EPL components in the image is available from:

- **Ubuntu packages**: the Ubuntu archive, for the exact version installed (listed in `/var/lib/dpkg/status`). Source
  package pages: `https://launchpad.net/ubuntu/+source/<source-package>/<version>`; archive files:
  `https://archive.ubuntu.com/ubuntu/pool/<component>/<initial>/<source-package>/` (`ports.ubuntu.com/ubuntu-ports` for
  arm64 builds of the same source); or `apt-get source <package>=<version>` with Ubuntu's `deb-src` entries enabled.
- **Libraries bundled in Python wheels** (libgfortran and libquadmath in NumPy and SciPy, GEOS in Shapely): the upstream
  projects named in each wheel's licence file (gcc.gnu.org, libgeos.org), and the wheels' own source distributions on PyPI.
- **Source-built components** (ROS 2, RoboPlan closure, drivers): the exact upstream repositories and commits recorded in
  the image (see above) and in this repository.

## Obligations at a glance

**Copyleft components in the image**

| Component | Licence | How it is used | Status |
| --- | --- | --- | --- |
| Ubuntu packages: toolchain and utilities (bash, coreutils, gcc/g++, binutils, make, git, gnupg, cppcheck, uncrustify, flake8 plugins and others) | GPL-2.0 / GPL-3.0 (GCC runtime: GPL-3.0 WITH GCC-exception-3.1), per each package's copyright file | Unmodified Ubuntu binaries | Licence texts present (`/usr/share/doc/*/copyright`, `/usr/share/common-licenses`). Source: Ubuntu archive (above) |
| Ubuntu libraries: glibc, liborocos-kdl, liblttng-ust, libfribidi and others | LGPL-2.1+ / LGPL-3.0+ | Dynamically linked, unmodified | As above |
| Eigen (`libeigen3-dev`) | MPL-2.0 (a few files LGPL-2.1+ / BSD-3-Clause) | Headers in the image; compiled into ROS, ProxSuite, TOPPRA and RoboPlan binaries | Source form (the headers) is in the image, unmodified; licence in its copyright file |
| graphviz (Ubuntu, pulled in through rosdep) | EPL-1.0 | Unmodified binary | Licence text present; source: Ubuntu archive |
| NumPy and SciPy wheels: bundled `libgfortran`, `libquadmath` (amd64) | GPL-3.0-or-later WITH GCC-exception-3.1; LGPL-2.1-or-later | Bundled shared libraries, dynamically linked | Licence texts in each wheel's `LICENSE.txt`; source availability named there (gcc.gnu.org) |
| Shapely wheel: bundled GEOS | LGPL-2.1 | Bundled shared library, dynamically linked | Licence in `shapely-*.dist-info/licenses/LICENSE_GEOS`; source: libgeos.org |
| certifi, pathspec (Python) | MPL-2.0 | Unmodified, shipped as source (`.py`) | Source form and licence in the dist-info |
| autocommand (vendored inside setuptools) | LGPL-3.0 | Unmodified, shipped as source | Source and licence in place |
| jrl-cmakemodules 2.3.0 | top-level `LICENSE`: GNU Lesser General Public License "unless otherwise specified"; `v2/LICENSE`: BSD 3-Clause License, Inria | Build time only, for ProxSuite, which uses the `v2` API (`jrl_configure_defaults`, `jrl_option`) | Both licence files in `roboplan-closure/jrl-cmakemodules/`. Whether any jrl-cmakemodules file is installed: **to confirm** by scanning the published image |

No LGPL library is statically linked into anything the image builds. The two static libraries the build links are
GoogleTest (BSD-3-Clause) and the private TinyXML-2 11 (Zlib).

**Open items before publication**

1. **Owner confirmations** (each marked **to confirm with ...** below): RoboPlan's example robot models (Open Planning),
   `uf_ros_lib` and `xarm_description` (UFACTORY), the bundled V-HACD and Embree/TBB notices in the `vhacdx` and
   `embreex` wheels (their maintainers), and the Fast DDS `thirdparty/` code without its own licence file (eProsima).
2. **Verbatim licence files that do not exist upstream** (nothing to copy; the owner's declaration is reported as is):
   ackermann_msgs, angles, filters and yaml_cpp_vendor ship no licence file (their `package.xml` declares the licence;
   angles and filters carry the licence text in their source headers, which are installed). The Assimp licence file is
   the top-level one; licences of Assimp's bundled `contrib/` code compiled into the cmeel-assimp wheel are not copied.
3. **Scan of the first published image** for: installed jrl-cmakemodules files, and the actual contents of
   `ros-source/build/` (which vendor packages fetched upstream source rather than using an Ubuntu package).

**Development tools in the runtime image (kept)**

The ROS build targets (`third_party/ros_lyrical/packages.txt`) include `ros_core`, which exec-depends on `ament_lint_auto`
and `ament_lint_common`, and the `ament_cmake_*` linter packages themselves; their exec dependencies bring in uncrustify,
cppcheck, flake8 and its plugins, pydocstyle and similar tools through rosdep. The image keeps them because ManyForge's ROS suites, which run in this image,
use the ament linters: `test-all.sh` builds `manyforge_xarm_control` with `BUILD_TESTING=ON` in the `ros` suite, whose
`ament_lint_auto_find_test_dependencies()` registers the `ament_copyright`, `ament_cpplint` and `ament_uncrustify` tests
that its CTest run executes, and the `ros`/`ros-e2e` suites colcon-build `manyforge_perception_interfaces` with
`BUILD_TESTING=ON`, which requires `ament_cmake_copyright` and `ament_cmake_lint_cmake`.

**Attribution required by bundled components** (Pillow wheel):

- Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved.
- This software is based in part on the work of the Independent JPEG Group.

## OS base

Base image: `ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55`. Ubuntu packages
come from the Ubuntu 24.04 archive at build time; their versions are not pinned. Each package's licence is in
`/usr/share/doc/<package>/copyright` in the image, and the installed package list is `/var/lib/dpkg/status`.

| Component | Version (observed) | Licence | Source | Evidence |
| --- | --- | --- | --- | --- |
| Ubuntu 24.04 base system | digest above | Various (GPL-2.0+, LGPL-2.1+, MIT, BSD and others per package) | https://ubuntu.com, https://launchpad.net/ubuntu | deb |
| ros2-apt-source (apt source and keyring for `packages.ros.org`) | 1.2.0, sha256-pinned | Apache-2.0, Copyright 2025 Open Robotics | https://github.com/ros-infrastructure/ros-apt-source | deb (`/usr/share/doc/ros2-apt-source/copyright`) |
| GCC toolchain (`build-essential`, gcc/g++ 13, libstdc++) | 13.3 | GPL-3.0-or-later; runtime libraries GPL-3.0 WITH GCC-exception-3.1 | https://gcc.gnu.org | deb, public |
| GNU C Library | 2.39 | LGPL-2.1-or-later | https://www.gnu.org/software/libc | public |
| Python | 3.12.3 | PSF-2.0 | https://www.python.org | public |
| python3-pip, python3-venv | 24.0 / 3.12.3 | MIT / PSF-2.0 | https://pip.pypa.io | deb, wheel |
| CMake | 3.28.3 | BSD-3-Clause | https://cmake.org | deb |
| Ninja | 1.11.1 | Apache-2.0 | https://ninja-build.org | deb |
| Git | 2.43.0 | GPL-2.0-only | https://git-scm.com | deb |
| curl | 8.5.0 | curl | https://curl.se | deb |
| GnuPG | 2.4.4 | GPL-3.0-or-later | https://gnupg.org | deb |
| ca-certificates | 20260816 | MPL-2.0 (certificate data), GPL-2.0-or-later (scripts) | https://launchpad.net/ubuntu/+source/ca-certificates | deb |
| GoogleTest / GoogleMock (also built as static libraries into `/usr/lib`) | 1.14.0 | BSD-3-Clause | https://github.com/google/googletest | deb |
| Boost (filesystem, serialization, system, python, chrono, thread, date-time) | 1.83 | BSL-1.0 | https://www.boost.org | deb |
| Eigen | 3.4.0 | MPL-2.0 (some files LGPL-2.1+ / BSD-3-Clause) | https://eigen.tuxfamily.org | deb |
| double-conversion | 3.3.0 | BSD-3-Clause | https://github.com/google/double-conversion | deb |
| Expat | 2.6.1 | MIT | https://libexpat.github.io | deb |
| OpenSSL (libssl) | 3.0.13 | Apache-2.0 | https://www.openssl.org | deb |
| SQLite | 3.45.1 | blessing (public domain) | https://sqlite.org | deb |
| utf8proc | 2.9.0 | MIT AND Unicode-DFS-2016 | https://github.com/JuliaStrings/utf8proc | deb |
| yaml-cpp | 0.8.0 | MIT | https://github.com/jbeder/yaml-cpp | deb |
| urdfdom (system) | 4.0.0 | BSD-3-Clause | https://github.com/ros/urdfdom | deb |
| OctoMap (system) | 1.9.7 | BSD-3-Clause | https://github.com/OctoMap/octomap | deb |
| colcon (`python3-colcon-common-extensions`, from `packages.ros.org`) | 0.3.0 | Apache-2.0 | https://github.com/colcon | public |
| rosdep-resolved system dependencies of the ROS source set | listed in `/opt/ros/lyrical/manyforge-source/rosdep-apt.txt` | per package | Ubuntu archive | deb |

<details>
<summary>Copyleft Ubuntu packages that rosdep installs for the ROS source set (observed)</summary>

Among the rosdep dependencies, these packages are worth naming because their licence is copyleft. The list was observed
in a build that installed the same ROS packages; the authoritative list for a given image is `rosdep-apt.txt` above.

| Package | Licence | Why it is there |
| --- | --- | --- |
| uncrustify 0.78.1 | GPL-2.0-or-later | `uncrustify_vendor` / `ament_uncrustify` (kept: used by the `ros` suite) |
| cppcheck 2.13.0 | GPL-3.0-or-later | `ament_cppcheck` (kept with the ament linters) |
| python3-flake8-builtins, python3-flake8-deprecated | GPL-2.0 | `ament_flake8` (kept with the ament linters) |
| python3-flake8-import-order | LGPL-3.0 | `ament_flake8` (kept with the ament linters) |
| liborocos-kdl 1.5.1, python3-pykdl | LGPL-2.1-or-later | `kdl_parser`, `tf2_kdl`, `tf2_eigen_kdl` |
| liblttng-ust 2.13.7, lttng-tools | LGPL-2.1 (libraries), GPL-2.0 (tools) | `tracetools` |
| graphviz 2.42.2 | EPL-1.0 | `tf2_tools` |
| libtinyxml2 10.0.0 | Zlib | Fast DDS, urdfdom, sdformat |
| libspdlog 1.12.0, libfmt 9.1.0 | MIT | `spdlog_vendor`, `rcl_logging_spdlog` |
| libbullet 3.24 | Zlib | `tf2_bullet` |

</details>

## ROS 2

ROS 2 Lyrical is built from source in the image. The source set is the release set of rosdistro sync `lyrical/2026-10-05`
(ros/rosdistro `df04dc94976c3fe616c39dec8851bc375c1d4fc1`) in `third_party/ros_lyrical/ros-lyrical.repos`, with the
version floors in `floors.repos`. The build installs the packages listed in `third_party/ros_lyrical/packages.txt` and
their dependencies into `/opt/ros/lyrical`. Repositories that are not in that dependency closure are fetched but not
installed; the exact build set is recorded in `/opt/ros/lyrical/manyforge-source/build-set.txt`. The repositories in
`exclude.txt` (Cyclone DDS, iceoryx, Connext, Zenoh) are not built. No ROS source is modified.

Licence evidence for the ROS packages is each package's `package.xml` `<license>` tag in the pinned source, quoted as
declared (`|` separates a package's several tags). The repositories' own licence files are in the image under
`/usr/share/licenses/manyforge-image/ros-source/`. All 92 repositories of `ros-lyrical.repos` have at least one package in
the build set (`colcon --packages-up-to` includes the test dependencies of the targets).

| Declared licence (`package.xml`) | Repositories (GitHub org/repo and version) |
| --- | --- |
| Apache License 2.0 | 48 repositories; see the first list below |
| Apache 2.0 | eProsima/Fast-CDR v2.3.6, eProsima/Fast-DDS v3.6.2, ros2/ros2_tracing 8.10.2, ros/sdformat_urdf 2.1.1 |
| Apache-2.0 | ros-controls/ros2_control_cmake 1.0.0 (version floor) |
| BSD | 14 repositories; see the second list below |
| BSD-3-Clause | ros-controls/control_msgs 6.10.0, ros/diagnostics 4.4.7, PickNikRobotics/generate_parameter_library 1.3.0, ros-controls/realtime_tools 5.4.0, PickNikRobotics/RSL 1.3.0 |
| MIT | pal-robotics/backward_ros 1.0.8, pal-robotics/pal_statistics 2.8.2 |
| BSL-1.0 | PickNikRobotics/cpp_polyfills 2.0.3 (`tcb_span`, the only package in this release) |
| Several licences in one package or repository | see the third list below |

<details>
<summary>"Apache License 2.0" repositories (48)</summary>

ament/ament_cmake 2.8.8, ros2/ament_cmake_ros 0.15.8, ament/ament_index 1.13.3, ament/ament_package 0.18.3,
ros-controls/control_toolbox 6.4.0, ros2/eigen3_cmake_module 0.5.1, ros2/example_interfaces 0.14.1,
ament/google_benchmark_vendor 0.7.0, gazebo-release/gz_cmake_vendor 0.4.5, gazebo-release/gz_math_vendor 0.4.5,
gazebo-release/gz_tools_vendor 0.2.2, gazebo-release/gz_utils_vendor 0.4.1, ros-tooling/keyboard_handler 0.5.1,
ros-controls/kinematics_interface 2.4.2, ros2/launch_ros 0.29.9, ros-tooling/libstatistics_collector 2.1.2,
osrf/osrf_testing_tools_cpp 2.3.1, ros2/performance_test_fixture 0.4.1, ros2/rcl 10.4.5, ros2/rcl_interfaces 2.4.5,
ros2/rcl_logging 3.4.1, ros2/rclcpp 32.0.3, ros2/rclpy 10.0.11, ros2/rcutils 7.1.3, ros2/rmw 7.10.2,
ros2/rmw_dds_common 6.0.0, ros2/rmw_fastrtps 9.4.10, ros2/rmw_implementation 3.1.6, ros-controls/ros2_controllers 6.10.1
(version floor; includes joint_trajectory_controller and parallel_gripper_controller), ros2/ros2cli_common_extensions
0.5.2, ros/ros_environment 4.5.1, ros2/ros_testing 0.9.1, ros2/ros_workspace 1.0.3, ros2/rosidl 5.2.1, ros2/rosidl_core
0.4.3, ros2/rosidl_defaults 1.8.1, ros2/rosidl_dynamic_typesupport 0.4.1, ros2/rosidl_dynamic_typesupport_fastrtps 0.5.1,
ros2/rosidl_python 0.27.2, ros2/rosidl_runtime_py 0.15.2, ros2-rust/rosidl_rust 0.5.0, ros2/rosidl_typesupport 3.4.2,
ros2/rosidl_typesupport_fastrtps 3.9.7, ros2/rpyutils 0.7.2, gazebo-release/sdformat_vendor 0.3.4,
ros-simulation/simulation_interfaces 2.1.0, ros2/test_interface_files 0.14.1, ros2/variants 0.13.0.

</details>

<details>
<summary>"BSD" repositories (14)</summary>

ros-drivers/ackermann_msgs 2.0.2, ros/angles 1.16.1, ros/class_loader 2.9.4, ros/filters 2.2.2, ros2/geometry2 0.45.10,
ros/kdl_parser 3.0.1, PickNikRobotics/launch_param_builder 0.1.1, ros2/message_filters 7.4.2, ros/robot_state_publisher
3.5.6, ros2/unique_identifier_msgs 2.8.1, ros2/urdf 2.13.2, ros/urdfdom 6.0.0, ros/urdfdom_headers 3.0.0, ros/xacro 2.1.1.

These `package.xml` files say "BSD" without a clause count. The licence text is in each repository's licence file in the
image, except ackermann_msgs, angles and filters, which ship none (**not declared upstream** beyond the `package.xml`
tag; angles and filters carry a BSD licence text in their source headers).

</details>

<details>
<summary>Several licences in one package or repository</summary>

| Repository | Declared licences |
| --- | --- |
| ament/ament_lint 0.20.6 | "Apache License 2.0" (22 packages); `ament_cpplint`: "Apache License 2.0 \| BSD"; `ament_pep257`: "Apache License 2.0 \| MIT" |
| ros2/common_interfaces 5.9.3 | "Apache License 2.0" (11 packages); `sensor_msgs_py`: "BSD" |
| ros2/launch 3.9.8 | "Apache License 2.0"; `launch_testing_ament_cmake`: "Apache License 2.0 \| BSD" |
| ros/pluginlib 5.8.5 | `pluginlib`: "BSD"; `ros2plugin`: "Apache License 2.0" |
| ros-controls/ros2_control 6.10.1 | "Apache License 2.0" (9 packages); `controller_manager_msgs`: "BSD" |
| ros2/ros2cli 0.40.9 | "Apache License 2.0" (14 packages); `ros2topic`: "Apache License 2.0 \| BSD-3-Clause" |
| ros2/rosbag2 0.33.4 | "Apache License 2.0" (15 packages); `rosbag2_storage_mcap`, `rosbag2_test_msgdefs`: "Apache-2.0"; `rosbag2_compression_zstd`: "Apache 2.0" |
| ros2/sros2 0.16.6 | `sros2`: "Apache License 2.0"; `sros2_cmake`: "Apache 2.0" |
| ros2/rcpputils 2.14.5 | "Apache License 2.0 \| BSD-3-Clause" |
| ros-perception/vision_msgs 4.2.0 | "Apache License 2.0 \| Apache License 2.0" |
| ros2/console_bridge_vendor 1.9.1 | "Apache License 2.0 \| BSD" (console_bridge itself: the Ubuntu package) |
| eProsima/foonathan_memory_vendor v1.4.1 | "Apache License 2.0 \| zlib License" (foonathan/memory, fetched and built by the vendor package) |
| ros2/libyaml_vendor 1.8.1 | "Apache License 2.0 \| MIT" |
| ros2/mimick_vendor 0.9.0 | "Apache License 2.0 \| MIT" |
| ros2/spdlog_vendor 1.8.0 | "Apache License 2.0 \| MIT" |
| ros2/yaml_cpp_vendor 9.2.1 | "Apache License 2.0 \| MIT" (comments: package contents Apache 2.0, yaml-cpp MIT); no licence file upstream |
| ament/uncrustify_vendor 3.2.0 | "Apache License 2.0 \| GNU General Public License v2.0" (uncrustify: the Ubuntu package) |

Vendor packages that build upstream code fetched at build time, instead of an Ubuntu package, install that code under its
owner's licence; the licence files found in those fetched sources are in `ros-source/build/<package>/`.

Fast DDS bundles code in `thirdparty/` (git submodules are not fetched; Fast DDS uses the Ubuntu packages instead): `boost`
(`LICENSE.TXT`) and `filewatch` (`LICENSE`) ship licence files, which are in the image. `optionparser`, `taocpp-pegtl` and
`nlohmann-json` ship no separate licence file; their headers state their licences (`nlohmann/json.hpp`:
`SPDX-License-Identifier: MIT`; `pegtl.hpp`: "Distributed under the Boost Software License, Version 1.0"; the optionparser
headers: eProsima's Apache License 2.0 header). Whether they are installed or only compiled in: **to confirm with
eProsima**.

</details>

## Controllers and drivers

These are built in a clean stage against the ROS install above and copied into `/opt/manyforge`.

| Component | Version / ref | Licence | Source | Evidence |
| --- | --- | --- | --- | --- |
| topic_based_ros2_control | `aa9f9ef9525a60f8becea322537dd1d6d1bf9123` | BSD-3-Clause (`package.xml`); repository `LICENSE` in `/usr/share/licenses/manyforge-image/topic_based_ros2_control/` | https://github.com/PickNikRobotics/topic_based_ros2_control | source |
| UFACTORY xArm ROS 2 driver (`xarm_ros2`; packages up to `xarm_controller`, `xarm_api`, `uf_ros_lib`; full source kept in `/opt/manyforge/xarm_ws/src`) | `3654c9afef9c51c3bd6723d65ccecca166bc38b0`, branch `lyrical-no-gazebo` of the fork https://github.com/pastoriomarco/xarm_ros2 | `package.xml`: "BSD" (`xarm_api`, `xarm_controller`, `xarm_msgs`, `xarm_sdk`), "TODO: License declaration" (`uf_ros_lib`); repository `LICENSE`: a BSD licence text, Copyright (c) 2018, UFACTORY Inc. | https://github.com/xArm-Developer/xarm_ros2 | source |
| xArm C++ SDK (submodule `xarm_sdk/cxx`) | `f36794320a285c0b4bf1cd8ef181cbff6e3b794a` | repository `LICENSE`: a BSD licence text, Copyright (c) 2018, UFACTORY Inc. | https://github.com/xArm-Developer/xArm-CPLUS-SDK | source |
| xarm_description (description data, `/opt/manyforge/xarm_description`) | `3654c9afef9c51c3bd6723d65ccecca166bc38b0` | `package.xml`: "TODO: License declaration"; repository `LICENSE` as above (in `/opt/manyforge/xarm_ws/src/xarm_ros2/`, same commit) | same fork | source |

`uf_ros_lib` and `xarm_description` declare no licence in `package.xml` (**not declared upstream** at package level);
whether the repository `LICENSE` covers them is **to confirm with UFACTORY**. The fork's own changes are by the ManyForge
maintainer under the repository's licence.

The ros2_controllers 6.10.1, realtime_tools 5.4.0, ros2_control_cmake 1.0.0 and rmw_fastrtps 9.4.11 overlays in the
Dockerfile belong to the `ROS_BASE=apt` variant only. In the default image these come from the ROS source set above.

## Robotics libraries

| Component | Version / ref | Licence | Where in the image | Source | Evidence |
| --- | --- | --- | --- | --- | --- |
| RoboPlan (roboplan, _common, _core, _simple_ik, _oink, _toppra, _rrt, _cartesian_planning, _example_models) | 0.8.0, `2a6b447e648b1a3337f10cbab995cda0c0a71d55`, with ManyForge's patches in `third_party/roboplan/patches/` (MIT) | MIT, Copyright (c) 2025-2026 Open Planning | venv (`/opt/manyforge/venv`); licence file in `roboplan-closure/roboplan/` | https://github.com/open-planning/roboplan | ledger (license sha256 `eb1aefed…`) |
| RoboPlan vendored `tl::expected` | in `roboplan_core/third_party/tl` | CC0-1.0 | headers in venv `cmeel.prefix/include/tl` | https://github.com/TartanLlama/expected | source |
| RoboPlan vendored TinyXML-2 | in `roboplan_core/third_party/tinyxml2` | Zlib | not used: the build points RoboPlan at the private TinyXML-2 11 below | https://github.com/leethomason/tinyxml2 | source |
| ProxSuite | 0.8.2, `6ae71b84730bf30453d70764073d186f5fe7ad69`, built without serialization | BSD-2-Clause, Copyright (c) 2022-2026 Inria | `/usr/local`; licence file in `roboplan-closure/proxsuite/` | https://github.com/Simple-Robotics/proxsuite | ledger (`46a9320a…`) |
| jrl-cmakemodules | 2.3.0, `984234828e97557b82d30262ffa3a851b6be2383` | top-level `LICENSE`: GNU Lesser General Public License "unless otherwise specified"; `v2/LICENSE`: BSD 3-Clause License, Inria | build time only (ProxSuite uses the `v2` API); both licence files in `roboplan-closure/jrl-cmakemodules/` | https://github.com/jrl-umi3218/jrl-cmakemodules | ledger (`09ba2945…`), source (`v2/LICENSE`) |
| TOPPRA | 0.6.10, `8121e9af72f5aa589b515aa15e0dc408344c3220` | MIT, Copyright (c) 2017 Hung Pham | `/usr/local`; licence file in `roboplan-closure/toppra/` | https://github.com/hungpham2511/toppra | ledger (`80d94151…`) |
| nanoflann | 2.0.1, `18f4edf753e813644a7a12711a86139777a8ef00` | BSD-2-Clause (`package.xml`); `COPYING`: Software License Agreement (BSD License) | compiled into RoboPlan RRT; `COPYING` in `roboplan-closure/nanoflann/` | https://github.com/jlblancoc/nanoflann | ledger (`12e2e0ba…`) |
| TinyXML-2 (private static copy) | 11.0.0, from the sha256-pinned `cmeel_tinyxml2-11.0.0` sdist | Zlib | `/opt/manyforge/private-tinyxml2-11`, linked into urdfdom 6 and RoboPlan; licence file in `cmeel/tinyxml2/` | https://github.com/leethomason/tinyxml2 | wheel |
| urdfdom (rebuilt against the private TinyXML-2) | 6.0.0 (+ urdfdom_headers 3.0.0), from sha256-pinned cmeel sdists | BSD-3-Clause (wheel); "BSD" (upstream `package.xml`) | venv `cmeel.prefix/lib`; licence file in `cmeel/urdfdom/` | https://github.com/ros/urdfdom | wheel, source |

<details>
<summary>RoboPlan example robot models (installed with roboplan_example_models)</summary>

The models stay in the image: ManyForge uses them. `manyforge_roboplan`'s tests (`test_roboplan_candidate_computer`,
`test_roboplan_scene_generation`, `test_process_contained_candidate_client`, run by the `ros` and RoboPlan suites in this
image) load `ur_robot_model`, `scripts/ci/check-roboplan-dependency-stack.py` checks the package at image build, and
RoboPlan's own `roboplan` package depends on it.

The package declares MIT (`package.xml`, `pyproject.toml`), but the models are derived from third-party robot
descriptions, and the package names no per-model licence except where shown. Where nothing is declared, the licence is
**not declared upstream**; ask Open Planning (RoboPlan) for each model's terms (**to confirm with Open Planning**).

| Model directory | Upstream named in the package | Licence |
| --- | --- | --- |
| `franka_robot_model` | frankarobotics/franka_description | not declared upstream; **to confirm with Open Planning** |
| `kinova_robot_model` | Kinovarobotics/ros2_kortex; Robotiq meshes from PickNikRobotics/ros2_robotiq_gripper | not declared upstream; **to confirm with Open Planning** (ros2_robotiq_gripper itself declares BSD-3-Clause, see `NOTICE`) |
| `reachback_robot_model` | clearpathrobotics/clearpath_community_demos and clearpath_common; Doosan meshes from DoosanRobotics/doosan-robot2; gripper "from the OnRobot website" | not declared upstream; **to confirm with Open Planning** (OnRobot terms unknown) |
| `so101_robot_model` | TheRobotStudio/SO-ARM100 at `608122e9ac330a753735f2e18aee73338e9ac407` | Apache-2.0 (`LICENSE` in the model directory) |
| `stretch4_robot_model` | not stated | not declared upstream; **to confirm with Open Planning** |
| `tiago_pro_robot_model` | not stated | not declared upstream; **to confirm with Open Planning** |
| `ur_robot_model` | not stated | not declared upstream; **to confirm with Open Planning** |
| `pointclouds/example_point_cloud.ply` | not stated | not declared upstream; **to confirm with Open Planning** |

</details>

## Python packages

The application environment `/opt/manyforge/venv` uses `--system-site-packages`, so it also sees the Ubuntu Python
packages, which are covered by their Debian copyright files. The build pins the packages in the first table.
Dependencies it pulls in are not pinned; the second table lists those observed in an amd64 build. Pip records each
package's licence files in its `*.dist-info` directory, except where noted.

**Pinned**

| Package | Version | Licence | Upstream | Evidence / notes |
| --- | --- | --- | --- | --- |
| pin, libpinocchio (Pinocchio) | 4.1.0 | BSD-2-Clause, per the licence text in the wheel (the wheel metadata says BSD-3-Clause) | https://github.com/stack-of-tasks/pinocchio | wheel |
| coal, libcoal (formerly HPP-FCL) | 3.0.3 | BSD-3-Clause | https://github.com/coal-library/coal | wheel |
| eigenpy | 3.13.0 | BSD-2-Clause | https://github.com/stack-of-tasks/eigenpy | wheel |
| cmeel | 0.60.0 | BSD-2-Clause | https://github.com/cmake-wheel/cmeel | wheel |
| cmeel-assimp (Assimp) | 6.0.5 | BSD-3-Clause | https://github.com/assimp/assimp | wheel metadata; upstream `LICENSE` (assimp v6.0.5) in `cmeel/assimp/` |
| cmeel-boost (Boost) | 1.90.0 | BSL-1.0 | https://www.boost.org | wheel metadata; upstream `LICENSE_1_0.txt` (boost-1.90.0) in `cmeel/boost/` |
| cmeel-console-bridge (console_bridge) | 1.0.2.3 | the wheel declares Zlib; upstream ros/console_bridge declares "3-Clause BSD" (`QUALITY_DECLARATION.md`) and ships a BSD licence text in `LICENSE`. Both are reported; ManyForge does not choose | https://github.com/ros/console_bridge | wheel metadata, source; upstream `LICENSE` (from the 1.0.2.3 sdist) in `cmeel/console_bridge/` |
| cmeel-octomap (OctoMap) | 1.10.0 | BSD-3-Clause | https://github.com/OctoMap/octomap | wheel metadata; upstream `octomap/LICENSE.txt` (from the 1.10.0 sdist) in `cmeel/octomap/` |
| cmeel-qhull (Qhull) | 8.0.2.1 | Qhull | http://www.qhull.org | wheel (`COPYING.txt`) |
| cmeel-tinyxml2 (TinyXML-2) | 11.0.0 | Zlib | https://github.com/leethomason/tinyxml2 | wheel metadata; upstream `LICENSE.txt` (from the 11.0.0 sdist) in `cmeel/tinyxml2/` |
| cmeel-urdfdom (urdfdom) | 6.0.0 | BSD-3-Clause | https://github.com/ros/urdfdom | wheel metadata; upstream `LICENSE` (from the 6.0.0 sdist) in `cmeel/urdfdom/` |
| cmeel-zlib (zlib) | 1.3.1 | Zlib | https://zlib.net | wheel metadata; upstream `LICENSE` (zlib v1.3.1) in `cmeel/zlib/` |
| numpy | 2.3.5 | BSD-3-Clause; bundles OpenBLAS and LAPACK (BSD-3-Clause), libgfortran (GPL-3.0-or-later WITH GCC-exception-3.1), libquadmath (LGPL-2.1-or-later, amd64) | https://numpy.org | wheel `LICENSE.txt` |
| pytest | 9.0.3 | MIT | https://pytest.org | wheel |
| py_trees | 2.4.0 | BSD-3-Clause | https://github.com/splintered-reality/py_trees | wheel |
| pybind11 | 3.0.3 | BSD-3-Clause | https://github.com/pybind/pybind11 | wheel |
| PyYAML | 6.0.3 | MIT | https://pyyaml.org | wheel |
| rfc8785 (also installed for the system Python) | 0.1.4 | Apache-2.0 | https://github.com/trailofbits/rfc8785.py | wheel |
| Pillow | 10.2.0 | HPND (MIT-CMU); bundles FreeType (FTL), libjpeg-turbo (IJG/BSD-3-Clause/Zlib), libpng, libtiff, libwebp, OpenJPEG, LCMS2, HarfBuzz, raqm, Brotli, bzip2, xz, zlib and others | https://python-pillow.org | wheel `LICENSE` (attributions above) |
| trimesh | 4.11.5 | MIT | https://github.com/mikedh/trimesh | wheel |
| yourdfpy | 0.0.60 | MIT | https://github.com/clemense/yourdfpy | wheel |
| nanobind | 2.12.0 | BSD-3-Clause | https://github.com/wjakob/nanobind | wheel |
| scikit-build-core | 0.12.2 | Apache-2.0 | https://github.com/scikit-build/scikit-build-core | wheel |
| FastAPI | 0.135.3 | MIT | https://github.com/fastapi/fastapi | wheel |
| Uvicorn (`[standard]`) | 0.44.0 | BSD-3-Clause | https://github.com/encode/uvicorn | wheel |
| HTTPX | 0.28.1 | BSD-3-Clause | https://github.com/encode/httpx | wheel |
| httpx2 | 2.12.0 | BSD-3-Clause | https://pypi.org/project/httpx2/ | wheel |
| websockets | 16.0 | BSD-3-Clause | https://github.com/python-websockets/websockets | wheel |
| MCP Python SDK (`mcp`) | 1.27.0 | MIT | https://github.com/modelcontextprotocol/python-sdk | wheel |
| pip, setuptools (venv) | 26.2.2 (observed), 79.0.1 (`<80`) | MIT | https://pypa.io | wheel; setuptools vendors autocommand (LGPL-3.0) and MIT/Apache-2.0/PSF-2.0 helpers |

**Unpinned dependencies (observed)**

| Licence | Packages |
| --- | --- |
| MIT | annotated-doc 0.0.5, annotated-types 0.8.0, anyio 4.16.0, attrs 26.1.0, charset-normalizer 3.5.2, colorlog 6.12.0, h11 0.16.0, httptools 0.9.0, httpx-sse 0.4.3, jsonschema 4.26.0, jsonschema-specifications 2025.9.1, pluggy 1.7.0, pydantic 2.14.2, pydantic-core 2.50.2, pydantic-settings 2.16.2, pydot 4.0.1, PyJWT 2.15.1, referencing 0.37.0, rpds-py 2026.9.1, rtree 1.4.1 (bundles libspatialindex, MIT), svg.path 7.1, truststore 0.10.5, typing-inspection 0.4.4, watchfiles 1.3.0, vhacdx 0.1.1 (bundles V-HACD; its notice: **to confirm with the vhacdx maintainers**) |
| MIT OR Apache-2.0 | uvloop 0.23.0 (bundles libuv, MIT) |
| BSD-3-Clause | click 8.5.0, httpcore 1.0.9, httpcore2 2.12.0, idna 3.20, networkx 3.7, pycollada 0.9.3, python-dotenv 1.2.4, scipy 1.18.1 (bundles OpenBLAS/LAPACK BSD-3-Clause, libgfortran GPL-3.0-or-later WITH GCC-exception-3.1, libquadmath LGPL-2.1-or-later on amd64), shapely 2.2.0 (bundles GEOS, LGPL-2.1), sse-starlette 3.6.0, starlette 1.10.0 |
| BSD-2-Clause | xxhash 4.0.2 |
| Apache-2.0 | manifold3d 3.5.4, python-multipart 0.0.33 |
| ISC | mapbox_earcut 2.1.0 |
| PSF-2.0 | typing_extensions 4.16.0 |
| MPL-2.0 | certifi 2026.10.23, pathspec 1.1.1 |
| BSD-3-Clause (licence text; bundled Embree/TBB notices **to confirm with the embreex maintainers**) | embreex 4.4.0.post1 |

These are mainly trimesh's `[easy]` extras, pulled in by yourdfpy, plus the FastAPI, Uvicorn and MCP dependencies.
yourdfpy's `lxml` and `six` are satisfied by the Ubuntu packages `python3-lxml` and `python3-six`.

## Node.js

| Component | Version | Licence | Source | Evidence |
| --- | --- | --- | --- | --- |
| Node.js (the `node` binary), copied from the official `node:22-slim` image | v22.23.5 (`node:22-slim` pinned by digest `sha256:e9f565d32859f0cbfb52125eafe8be39390f24dac302dc3cdf566e8ba3ab21ee`) | MIT; bundled components (V8, OpenSSL, ICU, libuv, llhttp, zlib and others) under the licences listed in Node's `LICENSE` | https://nodejs.org | source (`LICENSE`, copied verbatim to `/usr/share/licenses/manyforge-image/nodejs/`) |
| npm (`/usr/local/lib/node_modules/npm`) | 10.9.9 (observed) | Artistic-2.0; its bundled dependencies under their own licences (files in the same directory) | https://github.com/npm/cli | source (`LICENSE` in the directory) |

ManyForge's Composer frontend and its npm dependencies are not in the image.
