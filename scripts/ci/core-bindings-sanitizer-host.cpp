#include <Python.h>

#include <cstring>
#include <stdexcept>

namespace {

constexpr char kExceptionPreflight[] =
    "manyforge binding sanitizer host exception preflight";

bool exception_runtime_is_ready() {
  try {
    throw std::runtime_error(kExceptionPreflight);
  } catch (const std::runtime_error& error) {
    return std::strcmp(error.what(), kExceptionPreflight) == 0;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (!exception_runtime_is_ready()) {
    return 125;
  }
  return Py_BytesMain(argc, argv);
}
