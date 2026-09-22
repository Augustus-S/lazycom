# Keep the vendored serial library private in both system packages and bundles.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   LAZYCOM_USE_SYSTEM_LIBSERIALPORT OR LAZYCOM_USE_SYSTEM_DEPS)
  return()
endif()

set_target_properties(lazycom PROPERTIES INSTALL_RPATH "$ORIGIN/../lib/lazycom")
install(TARGETS lazycom RUNTIME DESTINATION bin COMPONENT Runtime)
install(DIRECTORY "${LAZYCOM_LIBSERIALPORT_PREFIX}/lib/"
  DESTINATION lib/lazycom COMPONENT Runtime
  FILES_MATCHING PATTERN "libserialport.so*"
)

install(DIRECTORY third_party/libserialport
  DESTINATION share/lazycom/source COMPONENT Runtime USE_SOURCE_PERMISSIONS
)
install(FILES third_party/libserialport/COPYING
  DESTINATION share/lazycom/licenses COMPONENT Runtime RENAME libserialport-COPYING
)
install(FILES third_party/ftxui/LICENSE
  DESTINATION share/lazycom/licenses COMPONENT Runtime RENAME FTXUI-LICENSE
)
install(DIRECTORY include/dependencies/licenses/
  DESTINATION share/lazycom/licenses COMPONENT Runtime
)
if(LAZYCOM_BUILD_DIAGNOSTICS)
  install(FILES third_party/spdlog/LICENSE
    DESTINATION share/lazycom/licenses COMPONENT Runtime RENAME spdlog-LICENSE
  )
  # fmt's full license notice is embedded at the start of this vendored header.
  install(FILES third_party/spdlog/include/spdlog/fmt/bundled/format.h
    DESTINATION share/lazycom/licenses COMPONENT Runtime RENAME fmt-license-and-header.h
  )
endif()
install(FILES include/dependencies/DEPENDENCIES.lock
  DESTINATION share/lazycom COMPONENT Runtime
)
install(FILES docs/releases.md Plan.md
  DESTINATION share/lazycom COMPONENT Runtime
)
install(FILES packaging/lazycom.desktop
  DESTINATION share/applications COMPONENT Runtime
)
install(FILES packaging/lazycom.svg
  DESTINATION share/icons/hicolor/scalable/apps COMPONENT Runtime
)

include("${CMAKE_CURRENT_LIST_DIR}/Packaging.cmake")
