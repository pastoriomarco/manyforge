if(NOT DEFINED MANYFORGE_LINK_MAP OR
   NOT EXISTS "${MANYFORGE_LINK_MAP}")
  message(FATAL_ERROR "Manager admission link map is absent")
endif()
if(NOT DEFINED MANYFORGE_BINARY OR
   NOT EXISTS "${MANYFORGE_BINARY}")
  message(FATAL_ERROR "Manager admission boundary binary is absent")
endif()
if(NOT DEFINED MANYFORGE_NM OR
   NOT EXISTS "${MANYFORGE_NM}")
  message(FATAL_ERROR "CMake did not provide a usable nm executable")
endif()

file(READ "${MANYFORGE_LINK_MAP}" link_map)
execute_process(
  COMMAND "${MANYFORGE_NM}" -C "${MANYFORGE_BINARY}"
  RESULT_VARIABLE nm_result
  OUTPUT_VARIABLE symbols
  ERROR_VARIABLE nm_error)
if(NOT nm_result EQUAL 0)
  message(FATAL_ERROR "nm failed for Manager boundary: ${nm_error}")
endif()

foreach(required_archive IN ITEMS
    "libmanyforge_deployment_admission.a"
    "libmanyforge_deployment_verification_core.a")
  string(FIND "${link_map}" "${required_archive}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR
      "Manager boundary omitted required archive ${required_archive}")
  endif()
endforeach()

foreach(forbidden_archive_regex IN ITEMS
    "[/]libmanyforge_deployment[.]a[(]"
    "[/]libmanyforge_deployment_canonical_package_generator[.]a[(]")
  string(REGEX MATCH "${forbidden_archive_regex}" found "${link_map}")
  if(NOT found STREQUAL "")
    message(FATAL_ERROR
      "Manager boundary linked a forbidden compiler/generator archive: "
      "${found}")
  endif()
endforeach()

foreach(required_symbol IN ITEMS
    "FilesystemCanonicalEmptyRetainedBundleSource::load_verified"
    "verify_package_admission_profile"
    "FilesystemVerifiedRetainedBundleSource::get_verified")
  string(FIND "${symbols}" "${required_symbol}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR
      "Manager boundary omitted required admission symbol ${required_symbol}")
  endif()
endforeach()

foreach(forbidden_symbol IN ITEMS
    "BuildQualifiedRetentionAuthority"
    "RetainedBundleMaterializer"
    "FilesystemRawArtifactStore"
    "FilesystemRetainedBundleRepository"
    "build_qualified_product_contract_registry"
    "build_and_retain_product"
    "compile_cell_ir"
    "compose_cell_occurrences"
    "generate_canonical_empty_package"
    "posix_durability::sync_directory"
    "posix_durability::ensure_owned_plain_directory"
    "posix_durability::ScopedAdvisoryLock"
    "posix_durability::ProcessAffineLockFile"
    "posix_durability::publish_immutable_file_no_replace")
  string(FIND "${symbols}" "${forbidden_symbol}" found)
  if(NOT found EQUAL -1)
    message(FATAL_ERROR
      "Manager boundary contains forbidden compiler/writer symbol "
      "${forbidden_symbol}")
  endif()
endforeach()
