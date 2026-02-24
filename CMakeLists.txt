cmake_minimum_required(VERSION 3.5)

# Derive the package version from the git tag so pushing a new vX.Y.Z tag is
# the only step needed to update the version everywhere (binary, config.h,
# man page, CPack .deb filename).
#
# Priority:
#  1. GITHUB_REF_NAME env var  -- set by GitHub Actions on tag-push events
#                                  (e.g. "v8.1.3"); reliable in shallow clones.
#  2. `git describe`            -- works for local builds where full tag history
#                                  is present.
#  3. Fallback: "0.0.0"         -- building outside any git repo (tarball, etc.)
if(DEFINED ENV{GITHUB_REF_NAME} AND "$ENV{GITHUB_REF_NAME}" MATCHES "^v[0-9]")
  set(_GIT_TAG "$ENV{GITHUB_REF_NAME}")
else()
  execute_process(
    COMMAND git describe --tags --match "v*" --abbrev=0
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE _GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()
string(REGEX REPLACE "^v" "" FBPANEL_VERSION "${_GIT_TAG}")
if(NOT FBPANEL_VERSION OR FBPANEL_VERSION STREQUAL "")
  set(FBPANEL_VERSION "0.0.0")
endif()

project(fbpanel VERSION ${FBPANEL_VERSION} LANGUAGES C)
set(CMAKE_VERBOSE_MAKEFILE OFF)
set(CMAKE_COLOR_MAKEFILE OFF)

# ---------------------------------------------------------------------------
# Build type
# ---------------------------------------------------------------------------
# Default to Release when the caller has not specified a build type.
# Accepted values: Release  Debug  RelWithDebInfo  MinSizeRel
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING
      "Build type (Release Debug RelWithDebInfo MinSizeRel)" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Improve the compiler flags for Debug and RelWithDebInfo over CMake's
# minimal built-in defaults ("-g" and "-O2 -g -DNDEBUG" respectively).
# These are only applied when the variable still holds CMake's factory
# default value, so an explicit -DCMAKE_C_FLAGS_DEBUG="..." is honoured.
if(CMAKE_C_FLAGS_DEBUG STREQUAL "-g")
  set(CMAKE_C_FLAGS_DEBUG "-O0 -g3 -Wall -Wextra" CACHE STRING
      "C compiler flags for Debug builds" FORCE)
endif()
if(CMAKE_C_FLAGS_RELWITHDEBINFO STREQUAL "-O2 -g -DNDEBUG")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g3 -DNDEBUG" CACHE STRING
      "C compiler flags for RelWithDebInfo builds" FORCE)
endif()

# Strip installed binaries for Release/MinSizeRel; keep symbols for
# Debug and RelWithDebInfo so the installed package is debuggable.
if(CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
  set(CMAKE_INSTALL_DO_STRIP FALSE)
else()
  set(CMAKE_INSTALL_DO_STRIP TRUE)
endif()

include(GNUInstallDirs)
find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MODULES REQUIRED gmodule-2.0 gtk+-2.0 )

# we need this header in order to build target
configure_file ( "${PROJECT_SOURCE_DIR}/config.h.in" "${PROJECT_SOURCE_DIR}/config.h")
configure_file ( "${PROJECT_SOURCE_DIR}/version.in" "${PROJECT_SOURCE_DIR}/version")

file(GLOB FBPANEL_SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} panel/*.c)
file(GLOB FBPANEL_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} panel/*.h)

# setup fbpanel itself
add_executable            (fbpanel        ${FBPANEL_SOURCES} ${FBPANEL_HEADERS})
target_compile_options    (fbpanel        PRIVATE -MMD)
target_include_directories(fbpanel        PUBLIC  panel . )
target_include_directories(fbpanel SYSTEM PRIVATE ${X11_INCLUDE_DIRS} ${MODULES_INCLUDE_DIRS})
# export symbols during link process of fbpanel because plugins use fbpanel binary as library.
target_link_libraries     (fbpanel        PRIVATE -lm ${X11_LIBRARIES} ${MODULES_LIBRARIES} -Wl,--export-dynamic)

# make a list of fbpanel plugins (volume removed; replaced by alsa plugin below)
set(PLUGINS battery batterytext cpu deskno genmon image mem2 meter pager space tclock chart dclock deskno2 icons launchbar mem menu net separator taskbar tray user wincmd brightness cpufreq diskio diskspace loadavg swap thermal windowtitle xrandr xkill timer clipboard windowlist capslock kbdlayout)

foreach(PLUGIN ${PLUGINS})
    file(GLOB PLUGIN_SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} plugins/${PLUGIN}/*.c)
    file(GLOB PLUGIN_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} plugins/${PLUGIN}/*.h)
    add_library               (${PLUGIN}        SHARED  ${PLUGIN_SOURCES} ${PLUGIN_HEADERS} ${FBPANEL_HEADERS})
    target_include_directories(${PLUGIN}        PUBLIC  panel .)
# put system here to suppress deprecation warns relating gtk2 and newer glib
    target_include_directories(${PLUGIN} SYSTEM PRIVATE ${MODULES_INCLUDE_DIRS})
#    target_link_libraries     (${PLUGIN}        PRIVATE ${X11_LIBRARIES} ${MODULES_LIBRARIES} -Wl,--no-undefined)
    target_link_libraries     (${PLUGIN}        PRIVATE ${X11_LIBRARIES} ${MODULES_LIBRARIES} -Wl,--warn-unresolved-symbols)
    target_compile_definitions(${PLUGIN}        PUBLIC PLUGIN)
    target_compile_options    (${PLUGIN}        PUBLIC -pthread -MMD)
endforeach()

# ---------------------------------------------------------------------------
# alsa plugin -- requires libasound2 (optional; skipped if not installed)
# ---------------------------------------------------------------------------
pkg_check_modules(ALSA alsa)
if(ALSA_FOUND)
    file(GLOB ALSA_SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} plugins/alsa/*.c)
    add_library               (alsa        SHARED  ${ALSA_SOURCES} ${FBPANEL_HEADERS})
    target_include_directories(alsa        PUBLIC  panel .)
    target_include_directories(alsa SYSTEM PRIVATE ${MODULES_INCLUDE_DIRS} ${ALSA_INCLUDE_DIRS})
    target_link_libraries     (alsa        PRIVATE ${X11_LIBRARIES} ${MODULES_LIBRARIES} ${ALSA_LIBRARIES} -Wl,--warn-unresolved-symbols)
    target_compile_definitions(alsa        PUBLIC  PLUGIN)
    target_compile_options    (alsa        PUBLIC  -pthread -MMD)
    install(PROGRAMS "${PROJECT_BINARY_DIR}/libalsa.so" DESTINATION "${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME}")
    message(STATUS "alsa plugin: enabled (libasound ${ALSA_VERSION})")
else()
    message(STATUS "alsa plugin: disabled (libasound not found)")
endif()

# workout manpage
set(DATADIR "${CMAKE_INSTALL_FULL_DATADIR}/${PROJECT_NAME}/config")
configure_file (
    "${PROJECT_SOURCE_DIR}/data/man/fbpanel.1.in"
    "${PROJECT_BINARY_DIR}/fbpanel.1"
)

# workout default config
file(READ "${PROJECT_SOURCE_DIR}/data/config/default.in" defaultconfig)
string(REPLACE "%%datadir%%" "${CMAKE_INSTALL_FULL_DATAROOTDIR}" defaultconfig ${defaultconfig})
string(REPLACE "@libexecdir@" "${CMAKE_INSTALL_FULL_LIBEXECDIR}/${PROJECT_NAME}" defaultconfig ${defaultconfig})
file(WRITE "${PROJECT_BINARY_DIR}/default" ${defaultconfig})

# workout pager config
configure_file (
    "${PROJECT_SOURCE_DIR}/data/config/pager.in"
    "${PROJECT_BINARY_DIR}/pager"
)

# workout make_profile script
set(datadir "${CMAKE_INSTALL_FULL_DATAROOTDIR}/${PROJECT_NAME}")
configure_file (
    "${PROJECT_SOURCE_DIR}/exec/make_profile.in"
    "${PROJECT_BINARY_DIR}/make_profile"
    @ONLY
)

# create "make install" rules in one place
install(PROGRAMS "${PROJECT_BINARY_DIR}/${PROJECT_NAME}" DESTINATION "${CMAKE_INSTALL_BINDIR}")
foreach(PLUGIN ${PLUGINS})
    install(PROGRAMS "${PROJECT_BINARY_DIR}/lib${PLUGIN}.so" DESTINATION "${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME}")
endforeach()
set(DOCS CHANGELOG.md LICENSE CREDITS INSTALL.md README.md)
foreach(DOC ${DOCS})
    install(FILES "${PROJECT_SOURCE_DIR}/${DOC}" DESTINATION "${CMAKE_INSTALL_DOCDIR}-${PROJECT_VERSION}")
endforeach()
install(FILES "${PROJECT_BINARY_DIR}/${PROJECT_NAME}.1" DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/data/images" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}" PATTERN "Makefile" EXCLUDE)
install(FILES "${PROJECT_BINARY_DIR}/default" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}/config")
install(FILES "${PROJECT_BINARY_DIR}/pager" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}/config")
install(PROGRAMS "${PROJECT_SOURCE_DIR}/exec/xlogout" DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/${PROJECT_NAME}")
install(PROGRAMS "${PROJECT_BINARY_DIR}/make_profile" DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/${PROJECT_NAME}")
install(FILES "${PROJECT_SOURCE_DIR}/po/fr_FR.UTF-8.mo" DESTINATION "${CMAKE_INSTALL_FULL_DATADIR}/locale/fr/LC_MESSAGES" RENAME "${PROJECT_NAME}.mo")
install(FILES "${PROJECT_SOURCE_DIR}/po/ru_RU.UTF-8.mo" DESTINATION "${CMAKE_INSTALL_FULL_DATADIR}/locale/ru/LC_MESSAGES" RENAME "${PROJECT_NAME}.mo")
install(FILES "${PROJECT_SOURCE_DIR}/po/it_IT.UTF-8.mo" DESTINATION "${CMAKE_INSTALL_FULL_DATADIR}/locale/it/LC_MESSAGES" RENAME "${PROJECT_NAME}.mo")

# ---------------------------------------------------------------------------
# CPack — Debian packaging
# ---------------------------------------------------------------------------
# The package name carries a suffix for non-Release builds so that debug
# packages are recognised as distinct by dpkg and can co-exist with (or
# replace) the release package without confusing the package manager:
#
#   Release / MinSizeRel  →  fbpanel_<ver>_<arch>.deb
#   Debug                 →  fbpanel-dbg_<ver>_<arch>.deb
#   RelWithDebInfo        →  fbpanel-dbgsym_<ver>_<arch>.deb
#
# Build a debug .deb:
#   cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/usr
#   make -C build-debug -j$(nproc)
#   cd build-debug && cpack -G DEB
#
# Build a RelWithDebInfo .deb:
#   cmake -B build-rwdi -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr
#   make -C build-rwdi -j$(nproc)
#   cd build-rwdi && cpack -G DEB

string(TOUPPER "${CMAKE_BUILD_TYPE}" _BUILD_TYPE_UPPER)
if(_BUILD_TYPE_UPPER STREQUAL "DEBUG")
  set(CPACK_PACKAGE_NAME "${PROJECT_NAME}-dbg")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "Lightweight GTK2 panel for the Linux desktop (debug build, -O0 -g3)")
elseif(_BUILD_TYPE_UPPER STREQUAL "RELWITHDEBINFO")
  set(CPACK_PACKAGE_NAME "${PROJECT_NAME}-dbgsym")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "Lightweight GTK2 panel for the Linux desktop (optimised with debug symbols)")
else()
  set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "Lightweight GTK2 panel for the Linux desktop")
endif()

# Default generator for local use; CI overrides with: cpack -G DEB  or  cpack -G RPM
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_CONTACT "Brett Kuskie <fullaxx@gmail.com>")

# --- Debian / Ubuntu packaging ---
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Brett Kuskie <fullaxx@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "x11")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

# --- RPM packaging (Fedora / RHEL / etc.) ---
set(CPACK_RPM_PACKAGE_LICENSE   "GPL-2.0+")
set(CPACK_RPM_PACKAGE_GROUP     "User Interface/X")
set(CPACK_RPM_PACKAGE_URL       "https://github.com/Fullaxx/fbpanel")
set(CPACK_RPM_PACKAGE_AUTOREQ   ON)
set(CPACK_RPM_FILE_NAME         "RPM-DEFAULT")
set(CPACK_RPM_PACKAGE_DESCRIPTION
    "fbpanel is a lightweight GTK2 desktop panel for the Linux desktop.\n"
    "It supports a plugin-based architecture with applets for taskbar,\n"
    "system tray, clock, battery, CPU, memory, network monitors, and more.")

include(CPack)
