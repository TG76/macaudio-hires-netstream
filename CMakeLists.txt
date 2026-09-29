cmake_minimum_required(VERSION 3.12.0)

project(FocalHiRes CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Codesign ID (empty = no signing, "-" = ad-hoc)
set(CODESIGN_ID "-" CACHE STRING "Codesign ID")

set(DRIVER_NAME "FocalHiRes")
set(DRIVER_VERSION "1.0.0")
set(DRIVER_COPYRIGHT "Copyright (c) 2026 Tim Grundmann")
set(DRIVER_IDENTIFIER "com.tg.focalhires")
set(DRIVER_UID "A7B3C1D2-E4F5-6789-ABCD-EF0123456789")
set(DRIVER_ENTRYPOINT "FocalHiResEntryPoint")

message(STATUS "Driver name - ${DRIVER_NAME}")
message(STATUS "Driver version - ${DRIVER_VERSION}")
message(STATUS "Driver identifier - ${DRIVER_IDENTIFIER}")
message(STATUS "Driver codesign ID - ${CODESIGN_ID}")

# libASPL (git submodule under external/, built as ExternalProject)
set(LIBASPL_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/external/libASPL)
if(NOT EXISTS ${LIBASPL_SOURCE_DIR}/CMakeLists.txt)
  message(FATAL_ERROR "libASPL fehlt (Submodule): git submodule update --init")
endif()

set(LIBASPL_TARGET FocalHiRes_libASPL)
include(ExternalProject)
ExternalProject_Add(${LIBASPL_TARGET}
  SOURCE_DIR ${LIBASPL_SOURCE_DIR}
  BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/libASPL-build
  INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/libASPL-prefix
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
  LOG_DOWNLOAD YES
  LOG_CONFIGURE YES
  LOG_BUILD YES
  LOG_INSTALL YES
  )

# Build as Mach-O loadable bundle (.so)
add_library(${DRIVER_NAME} MODULE
  "src/Driver.cpp"
  )

add_dependencies(${DRIVER_NAME} ${LIBASPL_TARGET})
target_include_directories(${DRIVER_NAME}
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/libASPL-prefix/include
  )
target_link_libraries(${DRIVER_NAME}
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/libASPL-prefix/lib/libASPL.a
  )

# CoreFoundation
find_library(LIB_CoreFoundation CoreFoundation REQUIRED)
target_link_libraries(${DRIVER_NAME}
  PRIVATE ${LIB_CoreFoundation}
  )

# Pack into .driver bundle
set_target_properties(${DRIVER_NAME} PROPERTIES
  OUTPUT_NAME "${DRIVER_NAME}"
  BUNDLE true
  BUNDLE_EXTENSION "driver"
  PREFIX ""
  SUFFIX ""
  MACOSX_BUNDLE ON
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_BUNDLE_NAME "${DRIVER_NAME}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${DRIVER_VERSION}"
  MACOSX_BUNDLE_COPYRIGHT "${DRIVER_COPYRIGHT}"
  MACOSX_BUNDLE_GUI_IDENTIFIER "${DRIVER_IDENTIFIER}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${DRIVER_VERSION}"
  )

# Code sign
if(NOT CODESIGN_ID STREQUAL "")
  add_custom_command(
    TARGET ${DRIVER_NAME}
    POST_BUILD
    COMMENT "Signing ${DRIVER_NAME}.driver with ID ${CODESIGN_ID}"
    VERBATIM
    COMMAND
      codesign --force -s "${CODESIGN_ID}"
        "${CMAKE_CURRENT_BINARY_DIR}/${DRIVER_NAME}.driver"
    )
endif()
