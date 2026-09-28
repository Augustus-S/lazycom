set(LAZYCOM_PACKAGE_SUFFIX "" CACHE STRING "Prerelease suffix appended to the project version")
set(LAZYCOM_BUILD_COMMIT "unknown" CACHE STRING "Source commit of the packaged build")
set(LAZYCOM_PACKAGE_VERSION "${PROJECT_VERSION}${LAZYCOM_PACKAGE_SUFFIX}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/package-version.txt" CONTENT "${LAZYCOM_PACKAGE_VERSION}\n")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/package-commit.txt" CONTENT "${LAZYCOM_BUILD_COMMIT}\n")
install(FILES "${CMAKE_BINARY_DIR}/package-commit.txt"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/lazycom" COMPONENT Runtime RENAME COMMIT
)

set(CPACK_PACKAGE_NAME lazycom)
set(CPACK_PACKAGE_VENDOR "LazyCom contributors")
set(CPACK_PACKAGE_CONTACT "LazyCom maintainers")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Augustus-S/lazycom")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Terminal serial port assistant")
set(CPACK_PACKAGE_DESCRIPTION "A C++20 terminal interface for serial port communication on Linux.")
set(CPACK_PACKAGE_VERSION "${LAZYCOM_PACKAGE_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "lazycom-${LAZYCOM_PACKAGE_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_GENERATOR TGZ)
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};Runtime;/")
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGE_CHECKSUM SHA256)

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION utils)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_RELEASE 1)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-only AND LGPL-3.0-or-later AND MIT AND CC0-1.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Communications")
set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
set(CPACK_RPM_PACKAGE_RELOCATABLE OFF)
set(CPACK_RPM_SPEC_MORE_DEFINE
  "%define __provides_exclude_from ^/usr/${CMAKE_INSTALL_LIBDIR}/lazycom/.*$\n%define __requires_exclude ^libserialport[.]so.*$\n%define __requires_exclude_from ^/usr/${CMAKE_INSTALL_DATADIR}/lazycom/source/.*$"
)

include(CPack)
