option(LAZYCOM_BUILD_TESTS "Build LazyCom tests" ON)
option(LAZYCOM_BUILD_DIAGNOSTICS "Compile internal diagnostics support" ON)
option(LAZYCOM_USE_SYSTEM_LIBSERIALPORT "Use a validated system libserialport" OFF)
option(LAZYCOM_USE_SYSTEM_DEPS "Use validated system non-header dependencies" OFF)
option(LAZYCOM_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(LAZYCOM_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(LAZYCOM_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(LAZYCOM_ENABLE_COVERAGE "Enable coverage instrumentation" OFF)
option(LAZYCOM_ENABLE_LTO "Enable interprocedural optimization" OFF)
option(LAZYCOM_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)

if(LAZYCOM_ENABLE_TSAN AND (LAZYCOM_ENABLE_ASAN OR LAZYCOM_ENABLE_UBSAN))
  message(FATAL_ERROR "TSan must be configured separately from ASan/UBSan")
endif()

if(LAZYCOM_ENABLE_COVERAGE AND LAZYCOM_ENABLE_LTO)
  message(FATAL_ERROR "Coverage and LTO cannot be enabled together")
endif()

add_library(lazycom_project_options INTERFACE)
target_compile_features(lazycom_project_options INTERFACE cxx_std_20)

if(LAZYCOM_ENABLE_ASAN)
  target_compile_options(lazycom_project_options INTERFACE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(lazycom_project_options INTERFACE -fsanitize=address)
endif()

if(LAZYCOM_ENABLE_UBSAN)
  target_compile_options(lazycom_project_options INTERFACE -fsanitize=undefined -fno-omit-frame-pointer)
  target_link_options(lazycom_project_options INTERFACE -fsanitize=undefined)
endif()

if(LAZYCOM_ENABLE_TSAN)
  target_compile_options(lazycom_project_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(lazycom_project_options INTERFACE -fsanitize=thread)
endif()

if(LAZYCOM_ENABLE_COVERAGE)
  target_compile_options(lazycom_project_options INTERFACE --coverage -O0 -g)
  target_link_options(lazycom_project_options INTERFACE --coverage)
endif()

if(LAZYCOM_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT lazycom_ipo_supported OUTPUT lazycom_ipo_error)
  if(NOT lazycom_ipo_supported)
    message(FATAL_ERROR "LTO is unavailable: ${lazycom_ipo_error}")
  endif()
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

add_library(lazycom_project_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lazycom_project_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wformat=2
    -Wundef
    -Wnon-virtual-dtor
  )
endif()

if(LAZYCOM_WARNINGS_AS_ERRORS)
  target_compile_options(lazycom_project_warnings INTERFACE -Werror)
endif()
