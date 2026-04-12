# ManyForge Workspace Guide

## C++ Core Conventions

These conventions apply to all `manyforge_core` and `manyforge_roboplan` code.

### Namespace
```cpp
namespace manyforge::core { ... }
```

### Directory Layout
```
manyforge_core/
├── include/manyforge_core/
│   ├── types/          # DTOs and enums
│   ├── ports/          # Abstract interfaces (IPlannerBackend, etc.)
│   ├── time/           # IClock, IScheduler
│   ├── cache/          # Trajectory cache (Phase 1)
│   └── collision/      # Collision policy (Phase 1)
├── src/                # Implementation files (.cpp)
└── test/               # Google Test files
```

### Include Paths
```cpp
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/ports/iplanner_backend.hpp>
#include <manyforge_core/time/iclock.hpp>
```

### Error Handling
- `Result<T>` for fallible operations
- `ErrorCode` enum for error codes
- No exceptions in core

### C++ Standard
- C++20
- `#pragma once` for include guards
- No ROS headers (`rclcpp`, `rosidl`, `rmw`)
- No `std::this_thread::sleep_for` in core

### Naming
- `snake_case` for files, functions, variables, namespaces
- `PascalCase` for types, classes, enums, structs
- `k` prefix for enum values: `kSuccess`, `kIdle`
- `I` prefix for interfaces: `IClock`, `IPlannerBackend`

### Build
- CMakeLists.txt changes are allowed when required; keep them intentional and requalify affected suites
- Build: `mkdir -p build && cd build && cmake .. && make -j$(nproc)`
- Test: `cd build && ctest --output-on-failure`
- Flags: `-Wall -Wextra -Werror -pedantic`

### Frozen Contract Headers (do NOT modify)
All files under:
- `include/manyforge_core/types/`
- `include/manyforge_core/ports/`
- `include/manyforge_core/time/iclock.hpp`
- `include/manyforge_core/time/ischeduler.hpp`
