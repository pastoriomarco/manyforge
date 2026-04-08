cmake_minimum_required(VERSION 3.14)
project(manyforge_core_py VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror -pedantic)

# Find pybind11
find_package(pybind11 CONFIG REQUIRED)

# manyforge_core directory (passed from command line)
set(MANYFORGE_CORE_DIR "${MANYFORGE_CORE_DIR}" CACHE PATH "Path to manyforge_core build")
set(MANYFORGE_CORE_INCLUDE_DIR "${MANYFORGE_CORE_INCLUDE_DIR}" CACHE PATH "Path to manyforge_core headers")

# Add manyforge_core headers (try explicit include dir, then fall back to build-dir/../include)
if(MANYFORGE_CORE_INCLUDE_DIR)
  include_directories(${MANYFORGE_CORE_INCLUDE_DIR})
else()
  include_directories(${MANYFORGE_CORE_DIR}/../include)
endif()

# Python bindings module
pybind11_add_module(manyforge_core_py
  bindings.cpp
)

# Link against the manyforge_core library
target_link_libraries(manyforge_core_py PRIVATE
  ${MANYFORGE_CORE_DIR}/libmanyforge_core.a
  pybind11::module
  pybind11::headers
)

target_include_directories(manyforge_core_py PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Install the Python module
install(TARGETS manyforge_core_py
  LIBRARY DESTINATION .
  RUNTIME DESTINATION .
)
