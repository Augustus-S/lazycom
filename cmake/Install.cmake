# Keep the vendored serial library private in both system packages and bundles.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   LAZYCOM_USE_SYSTEM_LIBSERIALPORT OR LAZYCOM_USE_SYSTEM_DEPS)
  return()
endif()

# Bundles keep the portable lib layout; native packages can select lib64.
set(CMAKE_INSTALL_LIBDIR "lib" CACHE PATH "Object code libraries")
include(GNUInstallDirs)
file(RELATIVE_PATH lazycom_library_from_bindir
  "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}/lazycom"
)
set_target_properties(lazycom PROPERTIES
  INSTALL_RPATH "$ORIGIN/${lazycom_library_from_bindir}"
)
install(TARGETS lazycom
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime
)
install(DIRECTORY "${LAZYCOM_LIBSERIALPORT_PREFIX}/lib/"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/lazycom" COMPONENT Runtime USE_SOURCE_PERMISSIONS
  FILES_MATCHING PATTERN "libserialport.so*"
)

install(DIRECTORY third_party/libserialport
  DESTINATION "${CMAKE_INSTALL_DATADIR}/lazycom/source" COMPONENT Runtime
  USE_SOURCE_PERMISSIONS
)
install(FILES LICENSE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/lazycom/licenses" COMPONENT Runtime
)
install(FILES third_party/libserialport/COPYING
  DESTINATION "${CMAKE_INSTALL_DATADIR}/lazycom/licenses" COMPONENT Runtime
  RENAME libserialport-COPYING
)
install(FILES third_party/ftxui/LICENSE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/lazycom/licenses" COMPONENT Runtime
  RENAME FTXUI-LICENSE
)
install(DIRECTORY include/dependencies/licenses/
  DESTINATION "${CMAKE_INSTALL_DATADIR}/lazycom/licenses" COMPONENT Runtime
)
if(LAZYCOM_BUILD_DIAGNOSTICS)
  install(FILES third_party/spdlog/LICENSE
    DESTINATION "${CMAKE_INSTALL_DATADIR}/lazycom/licenses" COMPONENT Runtime
    RENAME spdlog-LICENSE
  )
  # fmt's full license notice is embedded at the start of this vendored header.
  install(FILES third_party/spdlog/include/spdlog/fmt/bundled/format.h
    DESTINATION "${CMAKE_INSTALL_DATADIR}/lazycom/licenses" COMPONENT Runtime
    RENAME fmt-license-and-header.h
  )
endif()
install(FILES include/dependencies/DEPENDENCIES.lock
  DESTINATION "${CMAKE_INSTALL_DATADIR}/lazycom" COMPONENT Runtime
)
install(FILES docs/releases.md Plan.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/lazycom" COMPONENT Runtime
)
install(FILES packaging/lazycom.desktop
  DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT Runtime
)
install(FILES packaging/lazycom.svg
  DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps" COMPONENT Runtime
)

include("${CMAKE_CURRENT_LIST_DIR}/Packaging.cmake")
