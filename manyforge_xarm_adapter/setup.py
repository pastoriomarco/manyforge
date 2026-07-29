from glob import glob

from setuptools import find_packages
from setuptools import setup


package_name = 'manyforge_xarm_adapter'

setup(
    name=package_name,
    version='0.1.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        (
            'share/ament_index/resource_index/packages',
            [f'resource/{package_name}'],
        ),
        (f'share/{package_name}', ['package.xml', 'README.md']),
        (f'share/{package_name}/config', glob('config/*.yaml')),
        (f'share/{package_name}/launch', glob('launch/*.launch.py')),
    ],
    install_requires=['setuptools'],
    tests_require=['pytest'],
    zip_safe=True,
    maintainer='Marco Pastorio',
    maintainer_email='pastoriomarco@gmail.com',
    description=(
        'Dedicated xArm binding, model-generation, and observation adapter '
        'for ManyForge.'
    ),
    license='Apache-2.0',
    entry_points={
        'console_scripts': [
            (
                'manyforge-xarm-binding-manifest = '
                'manyforge_xarm_adapter.binding_manifest:main'
            ),
            (
                'manyforge-xarm-generate-fixed-model = '
                'manyforge_xarm_adapter.model_generation:main'
            ),
            (
                'manyforge-xarm-validate-controller-binding = '
                'manyforge_xarm_adapter.controller_binding_validation:main'
            ),
            (
                'manyforge-xarm-validate-live-controller-parameters = '
                'manyforge_xarm_adapter.'
                'live_controller_parameter_validation:main'
            ),
            (
                'manyforge-xarm-read-only-observer = '
                'manyforge_xarm_adapter.read_only_observer:main'
            ),
            (
                'manyforge-xarm-resolve-supervised-runtime = '
                'manyforge_xarm_adapter.supervised_runtime_binding:main'
            ),
            (
                'manyforge-xarm-run-execution-admission-provider = '
                'manyforge_xarm_adapter.'
                'execution_admission_provider_launch:main'
            ),
            (
                'manyforge-xarm-resolve-real-robot-deployment = '
                'manyforge_xarm_adapter.real_robot_deployment:main'
            ),
            (
                'manyforge-xarm-capture-live-evidence = '
                'manyforge_xarm_adapter.live_evidence:main'
            ),
            (
                'manyforge-xarm-capture-supervised-command = '
                'manyforge_xarm_adapter.supervised_command_evidence:main'
            ),
            (
                'manyforge-xarm-capture-controller-gate = '
                'manyforge_xarm_adapter.controller_gate_evidence:main'
            ),
        ],
    },
)
