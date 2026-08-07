cmake_minimum_required(VERSION 3.28)

project(lazycom VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
include(cmake/ProjectOptions.cmake)
include(cmake/Dependencies.cmake)

add_library(lazycom_base STATIC
  src/base/error.cpp
)
target_include_directories(lazycom_base PUBLIC include)
target_link_libraries(lazycom_base
  PUBLIC lazycom_header_dependencies
  PRIVATE lazycom_project_options lazycom_project_warnings
)

add_library(lazycom_core STATIC
  src/core/build_info.cpp
)

add_library(lazycom_config STATIC
  src/config/persistence.cpp
  src/config/schema.cpp
  src/config/safe_file.cpp
)
target_include_directories(lazycom_config PUBLIC include)
target_link_libraries(lazycom_config
  PUBLIC lazycom_base
  PRIVATE lazycom_project_options lazycom_project_warnings
)

add_library(lazycom_logging STATIC
  src/logging/schema.cpp
  src/logging/session_writer.cpp
)
target_include_directories(lazycom_logging PUBLIC include)
target_link_libraries(lazycom_logging
  PUBLIC lazycom_base Threads::Threads
  PRIVATE lazycom_project_options lazycom_project_warnings
)

add_library(lazycom_data_path STATIC
  src/framing/rx_framer.cpp
  src/encoding/display.cpp
  src/model/memory_budget.cpp
  src/model/session_record.cpp
  src/model/session_sequencer.cpp
  src/model/tx_operation.cpp
)
target_include_directories(lazycom_data_path PUBLIC include)
target_link_libraries(lazycom_data_path
  PUBLIC lazycom_logging Threads::Threads
  PRIVATE lazycom_project_options lazycom_project_warnings
)

add_library(lazycom_scheduler STATIC
  src/scheduler/scheduler.cpp
)
target_include_directories(lazycom_scheduler PUBLIC include)
target_link_libraries(lazycom_scheduler
  PUBLIC lazycom_config
  PRIVATE lazycom_project_options lazycom_project_warnings
)
target_include_directories(lazycom_core PUBLIC include)
target_compile_definitions(lazycom_core PRIVATE LAZYCOM_VERSION="${PROJECT_VERSION}")
target_link_libraries(lazycom_core
  PUBLIC lazycom_base
  PRIVATE lazycom_project_options lazycom_project_warnings
)

add_library(lazycom_diagnostics STATIC
  src/diagnostics/diagnostics.cpp
)
target_include_directories(lazycom_diagnostics PUBLIC include)
target_link_libraries(lazycom_diagnostics
  PUBLIC lazycom_base
  PRIVATE lazycom_project_options lazycom_project_warnings
)
if(LAZYCOM_BUILD_DIAGNOSTICS)
  target_compile_definitions(lazycom_diagnostics PRIVATE LAZYCOM_DIAGNOSTICS_ENABLED=1)
  target_link_libraries(lazycom_diagnostics PRIVATE spdlog::spdlog)
endif()

add_library(lazycom_serial STATIC
  src/serial/backend.cpp
  src/serial/libserialport_probe.cpp
  src/serial/scanner.cpp
  src/serial/service.cpp
)
target_include_directories(lazycom_serial PUBLIC include)
target_link_libraries(lazycom_serial
  PUBLIC lazycom_base lazycom_config Threads::Threads libserialport::libserialport
  PRIVATE lazycom_project_options lazycom_project_warnings
)
if(TARGET lazycom_libserialport_external)
  add_dependencies(lazycom_serial lazycom_libserialport_external)
endif()

add_library(lazycom_app STATIC
  src/app/application.cpp
  src/app/state.cpp
)
target_include_directories(lazycom_app PUBLIC include)
target_link_libraries(lazycom_app
  PUBLIC
    lazycom_core
    lazycom_config
    lazycom_data_path
    lazycom_logging
    lazycom_scheduler
    lazycom_serial
    lazycom_diagnostics
  PRIVATE lazycom_project_options lazycom_project_warnings
)

add_library(lazycom_ui STATIC
  src/ui/tui.cpp
)
target_include_directories(lazycom_ui PUBLIC include)
target_link_libraries(lazycom_ui
  PUBLIC lazycom_app ftxui::component
  PRIVATE lazycom_project_options lazycom_project_warnings
)

add_executable(lazycom src/main.cpp)
target_link_libraries(lazycom
  PRIVATE
    lazycom_core
    lazycom_app
    lazycom_diagnostics
    lazycom_serial
    lazycom_ui
    lazycom_project_options
    lazycom_project_warnings
    ftxui::dom
    ftxui::screen
    ftxui::component
)

foreach(lazycom_target IN ITEMS
    lazycom_base
    lazycom_core
    lazycom_config
    lazycom_logging
    lazycom_data_path
    lazycom_scheduler
    lazycom_diagnostics
    lazycom_serial
    lazycom_app
    lazycom_ui
    lazycom
)
  target_link_libraries(${lazycom_target} PRIVATE lazycom_release_hardening)
endforeach()
target_link_libraries(lazycom PRIVATE lazycom_release_link_hardening)

if(BUILD_TESTING AND LAZYCOM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
