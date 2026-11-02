from glob import glob
from setuptools import find_packages, setup

package_name = "manyforge_ros_bringup"

setup(
    name=package_name,
    version="0.2.0",
    packages=find_packages(exclude=["test"]),
    data_files=[
        ("share/ament_index/resource_index/packages", [f"resource/{package_name}"]),
        (f"share/{package_name}", ["package.xml"]),
        (
            f"share/{package_name}/config",
            glob("config/*.yaml") + glob("config/*.json"),
        ),
        (f"share/{package_name}/launch", glob("launch/*.launch.py")),
        (f"share/{package_name}/urdf", glob("urdf/*")),
    ],
    install_requires=["manyforge_headless_api>=0.2.0,<0.3.0", "setuptools"],
    tests_require=["pytest"],
    zip_safe=True,
    maintainer="Marco Pastorio",
    maintainer_email="pastoriomarco@gmail.com",
    description="ROS 2 bringup helpers for ManyForge integration targets.",
    license="Apache-2.0",
    entry_points={
        "console_scripts": [
            "isaac_gripper_action_bridge = manyforge_ros_bringup.isaac_gripper_action_bridge:main",
            "pointcloud_observer_bridge = manyforge_ros_bringup.pointcloud_observer_bridge:main",
            "isaac_demo_supervisor = manyforge_ros_bringup.isaac_demo_supervisor:main",
            "pose_source_snapshot_publisher = manyforge_ros_bringup.pose_source_snapshot_publisher:main",
            "ai_model_probe = manyforge_ros_bringup.ai_model_probe:main",
            "sim_plc = manyforge_ros_bringup.sim_plc:main",
        ],
    },
)
