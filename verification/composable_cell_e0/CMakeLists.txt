cmake_minimum_required(VERSION 3.18)

# The buildable proof remains at the exact source commit named below.  Keeping
# this current-branch entry point runnable would accidentally compile pinned E0
# evidence against the post-relocation API and turn a historical proof into a
# compatibility surface.
message(FATAL_ERROR
  "composable_cell_runtime_unavailable: the E0 Python-boundary proof is "
  "historical evidence pinned to ManyForge commit "
  "637894c460172b47faf64c7ece2e865bb8d69593; current-branch activation "
  "returns only at the atomic I2 Python/compiler binding cutover")

# CMake inserts an implicit C/CXX-enabling project at the top of a file with no
# literal direct project() call.  This deliberately unreachable no-language
# sentinel suppresses that insertion while keeping project hooks behind the
# fatal boundary above.
project(manyforge_e0_historical_proof LANGUAGES NONE)
