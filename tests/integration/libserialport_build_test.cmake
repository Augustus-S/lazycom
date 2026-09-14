cmake_minimum_required(VERSION 3.28)

file(REMOVE_RECURSE "${TEST_ROOT}")
set(fixture "${TEST_ROOT}/source")
file(MAKE_DIRECTORY "${fixture}/third_party")
file(COPY "${LAZYCOM_SOURCE_DIR}/third_party/libserialport"
  DESTINATION "${fixture}/third_party")
file(CREATE_LINK "${LAZYCOM_SOURCE_DIR}/third_party/ftxui"
  "${fixture}/third_party/ftxui" SYMBOLIC)
file(WRITE "${fixture}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.28)
project(libserialport_build_regression LANGUAGES C CXX)
set(LAZYCOM_BUILD_DIAGNOSTICS OFF)
include(\"${LAZYCOM_SOURCE_DIR}/cmake/Dependencies.cmake\")
")

set(serial_source "${fixture}/third_party/libserialport")
set(generated_files aclocal.m4 configure Makefile.in config.h.in)
foreach(name IN LISTS generated_files)
  file(SHA256 "${serial_source}/${name}" "before_${name}")
endforeach()
# A source checkout can make Autotools inputs newer than shipped outputs.
file(GLOB macro_files "${serial_source}/autostuff/*.m4")
file(TOUCH ${macro_files}
  "${serial_source}/configure.ac" "${serial_source}/Makefile.am")

set(unavailable_tool "${TEST_ROOT}/unavailable-autotools")
file(WRITE "${unavailable_tool}" "#!/bin/sh\nexit 1\n")
file(CHMOD "${unavailable_tool}" PERMISSIONS
  OWNER_READ OWNER_WRITE OWNER_EXECUTE)

foreach(generator IN ITEMS Ninja "Unix Makefiles")
  string(REPLACE " " "-" directory "${generator}")
  set(binary "${TEST_ROOT}/${directory}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
      "ACLOCAL=${unavailable_tool}" "AUTOMAKE=${unavailable_tool}"
      "AUTOCONF=${unavailable_tool}" "AUTOHEADER=${unavailable_tool}"
      "${CMAKE_COMMAND}" -S "${fixture}" -B "${binary}" -G "${generator}"
      "-DCMAKE_C_COMPILER=${TEST_C_COMPILER}"
      "-DCMAKE_CXX_COMPILER=${TEST_CXX_COMPILER}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
  )
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "${generator} configure failed:\n${output}\n${error}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
      "ACLOCAL=${unavailable_tool}" "AUTOMAKE=${unavailable_tool}"
      "AUTOCONF=${unavailable_tool}" "AUTOHEADER=${unavailable_tool}"
      "${CMAKE_COMMAND}" --build "${binary}"
      --target lazycom_libserialport_external --parallel 2
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
  )
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "${generator} build/install failed:\n${output}\n${error}")
  endif()
  if(NOT EXISTS "${binary}/_deps/libserialport/lib/libserialport.so")
    message(FATAL_ERROR "${generator} did not install the shared library")
  endif()
  foreach(name IN LISTS generated_files)
    file(SHA256 "${serial_source}/${name}" after)
    if(NOT after STREQUAL before_${name})
      message(FATAL_ERROR "${generator} modified vendored ${name}")
    endif()
  endforeach()
endforeach()
