add_executable(lazycom_tests
  unit/application_test.cpp
  unit/base_error_test.cpp
  unit/config_safe_file_test.cpp
  unit/config_schema_test.cpp
  unit/data_path_test.cpp
  unit/dependency_smoke_test.cpp
  unit/logging_persistence_test.cpp
  unit/logging_schema_test.cpp
  unit/scheduler_test.cpp
  unit/serial_service_test.cpp
  unit/signals_completion_test.cpp
  unit/state_model_test.cpp
  unit/ui_routing_test.cpp
  integration/logging_files_test.cpp
)
target_compile_definitions(lazycom_tests PRIVATE CATCH_CONFIG_NO_COUNTER)
target_include_directories(lazycom_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
if(LAZYCOM_BUILD_DIAGNOSTICS)
  target_compile_definitions(lazycom_tests PRIVATE LAZYCOM_EXPECT_DIAGNOSTICS=1)
endif()
target_link_libraries(lazycom_tests
  PRIVATE
    Catch2::Catch2WithMain
    lazycom_base
    lazycom_app
    lazycom_config
    lazycom_core
    lazycom_data_path
    lazycom_diagnostics
    lazycom_logging
    lazycom_scheduler
    lazycom_serial
    lazycom_ui
    lazycom_project_options
    lazycom_project_warnings
)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/third_party/catch2/extras")
include(Catch)
catch_discover_tests(lazycom_tests)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(NOT LAZYCOM_USE_SYSTEM_LIBSERIALPORT)
    add_test(NAME bundled_libserialport_build
      COMMAND "${CMAKE_COMMAND}"
        "-DLAZYCOM_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/libserialport-build-regression"
        "-DTEST_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DTEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/libserialport_build_test.cmake"
    )
    set_tests_properties(bundled_libserialport_build PROPERTIES
      LABELS "integration;build" TIMEOUT 120
    )
  endif()

  add_executable(lazycom_serial_backend_viability_tests
    integration/serial_backend_viability_test.cpp
    integration/serial_owner_pty_test.cpp
  )
  target_compile_definitions(lazycom_serial_backend_viability_tests
    PRIVATE CATCH_CONFIG_NO_COUNTER _GNU_SOURCE
  )
  target_link_libraries(lazycom_serial_backend_viability_tests
    PRIVATE
      Catch2::Catch2WithMain
      lazycom_serial
      lazycom_project_options
      lazycom_project_warnings
  )
  catch_discover_tests(lazycom_serial_backend_viability_tests
    PROPERTIES LABELS "integration;serial"
  )
endif()
