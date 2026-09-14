find_package(Threads REQUIRED)

add_library(lazycom_header_dependencies INTERFACE)
target_include_directories(lazycom_header_dependencies SYSTEM INTERFACE
  "${PROJECT_SOURCE_DIR}/include/dependencies"
)

if(LAZYCOM_USE_SYSTEM_DEPS)
  find_package(ftxui 6.1.9 CONFIG REQUIRED)
else()
  set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_TESTS_FUZZER OFF CACHE BOOL "" FORCE)
  set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/ftxui SYSTEM EXCLUDE_FROM_ALL)
endif()

if(LAZYCOM_BUILD_DIAGNOSTICS)
  if(LAZYCOM_USE_SYSTEM_DEPS)
    find_package(spdlog 1.15.3 CONFIG REQUIRED)
  else()
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
    set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/spdlog SYSTEM EXCLUDE_FROM_ALL)
  endif()
endif()

if(LAZYCOM_USE_SYSTEM_LIBSERIALPORT)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SYSTEM_LIBSERIALPORT REQUIRED IMPORTED_TARGET "libserialport>=0.1.2")
  add_library(libserialport::libserialport ALIAS PkgConfig::SYSTEM_LIBSERIALPORT)
else()
  include(ExternalProject)
  find_program(LAZYCOM_MAKE_EXECUTABLE NAMES gmake make REQUIRED)

  set(LAZYCOM_LIBSERIALPORT_PREFIX "${CMAKE_BINARY_DIR}/_deps/libserialport")
  set(LAZYCOM_LIBSERIALPORT_LIBRARY "${LAZYCOM_LIBSERIALPORT_PREFIX}/lib/libserialport.so")

  set(lazycom_libserialport_cflags "")
  set(lazycom_libserialport_ldflags "")
  if(LAZYCOM_ENABLE_ASAN)
    list(APPEND lazycom_libserialport_cflags
      -fsanitize=address -fno-omit-frame-pointer
    )
    list(APPEND lazycom_libserialport_ldflags -fsanitize=address)
  endif()
  if(LAZYCOM_ENABLE_UBSAN)
    list(APPEND lazycom_libserialport_cflags
      -fsanitize=undefined -fno-omit-frame-pointer
    )
    list(APPEND lazycom_libserialport_ldflags -fsanitize=undefined)
  endif()
  if(LAZYCOM_ENABLE_TSAN)
    list(APPEND lazycom_libserialport_cflags
      -fsanitize=thread -fno-omit-frame-pointer
    )
    list(APPEND lazycom_libserialport_ldflags -fsanitize=thread)
  endif()
  if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    list(APPEND lazycom_libserialport_cflags
      ${LAZYCOM_LIBSERIALPORT_RELEASE_CFLAGS}
    )
    list(APPEND lazycom_libserialport_ldflags
      ${LAZYCOM_LIBSERIALPORT_RELEASE_LDFLAGS}
    )
  endif()

  set(lazycom_libserialport_environment "CC=${CMAKE_C_COMPILER}")
  if(lazycom_libserialport_cflags)
    list(REMOVE_DUPLICATES lazycom_libserialport_cflags)
    string(JOIN " " lazycom_libserialport_cflags_string
      ${lazycom_libserialport_cflags}
    )
    list(APPEND lazycom_libserialport_environment
      "CFLAGS=${lazycom_libserialport_cflags_string}"
    )
  endif()
  if(lazycom_libserialport_ldflags)
    list(REMOVE_DUPLICATES lazycom_libserialport_ldflags)
    string(JOIN " " lazycom_libserialport_ldflags_string
      ${lazycom_libserialport_ldflags}
    )
    list(APPEND lazycom_libserialport_environment
      "LDFLAGS=${lazycom_libserialport_ldflags_string}"
    )
  endif()

  # Use the shipped Autotools outputs even when checkout timestamps put their
  # inputs later. Regeneration would require the upstream maintainer toolchain
  # and modify the shared vendored source tree, including during make install.
  set(lazycom_libserialport_make "${LAZYCOM_MAKE_EXECUTABLE}")
  set(lazycom_libserialport_recursive_flags "")
  foreach(lazycom_generated_file IN ITEMS
      aclocal.m4 configure Makefile.in config.h.in)
    set(lazycom_old_file
      "--old-file=${PROJECT_SOURCE_DIR}/third_party/libserialport/${lazycom_generated_file}"
    )
    list(APPEND lazycom_libserialport_make "${lazycom_old_file}")
    string(REPLACE "'" "'\\''" lazycom_old_file "${lazycom_old_file}")
    string(APPEND lazycom_libserialport_recursive_flags " '${lazycom_old_file}'")
  endforeach()
  # GNU Make does not propagate --old-file through MAKEFLAGS to sub-makes.
  list(APPEND lazycom_libserialport_make
    "AM_MAKEFLAGS=${lazycom_libserialport_recursive_flags}")

  ExternalProject_Add(lazycom_libserialport_external
    SOURCE_DIR "${PROJECT_SOURCE_DIR}/third_party/libserialport"
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/libserialport-build"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND
      "${CMAKE_COMMAND}" -E env ${lazycom_libserialport_environment}
      "${PROJECT_SOURCE_DIR}/third_party/libserialport/configure"
      "--prefix=${LAZYCOM_LIBSERIALPORT_PREFIX}"
      "--libdir=${LAZYCOM_LIBSERIALPORT_PREFIX}/lib"
      --enable-shared
      --disable-static
    BUILD_COMMAND ${lazycom_libserialport_make}
    INSTALL_COMMAND ${lazycom_libserialport_make} install
    BUILD_BYPRODUCTS "${LAZYCOM_LIBSERIALPORT_LIBRARY}"
  )

  add_library(lazycom_bundled_libserialport SHARED IMPORTED GLOBAL)
  set_target_properties(lazycom_bundled_libserialport PROPERTIES
    IMPORTED_LOCATION "${LAZYCOM_LIBSERIALPORT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/third_party/libserialport"
  )
  add_dependencies(lazycom_bundled_libserialport lazycom_libserialport_external)
  add_library(libserialport::libserialport ALIAS lazycom_bundled_libserialport)
endif()

if(BUILD_TESTING AND LAZYCOM_BUILD_TESTS)
  if(LAZYCOM_USE_SYSTEM_DEPS)
    find_package(Catch2 3.8.1 CONFIG REQUIRED)
  else()
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
    set(CATCH_DEVELOPMENT_BUILD OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/catch2 SYSTEM EXCLUDE_FROM_ALL)
  endif()
endif()
