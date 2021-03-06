# Copyright (c) 2009, 2017, Oracle and/or its affiliates. All rights reserved.
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA 

# We support different versions of SSL:
# - "bundled" uses source code in <source dir>/extra/yassl
# - "system"  (typically) uses headers/libraries in /usr/lib and /usr/lib64
# - a custom installation of openssl can be used like this
#     - cmake -DCMAKE_PREFIX_PATH=</path/to/custom/openssl> -DWITH_SSL="system"
#   or
#     - cmake -DWITH_SSL=</path/to/custom/openssl>
#
# The default value for WITH_SSL is "bundled"
# set in cmake/build_configurations/feature_set.cmake
#
# For custom build/install of openssl, see the accompanying README and
# INSTALL* files. When building with gcc, you must build the shared libraries
# (in addition to the static ones):
#   ./config --prefix=</path/to/custom/openssl> --shared; make; make install
# On some platforms (mac) you need to choose 32/64 bit architecture.
# Build/Install of openssl on windows is slightly different: you need to run
# perl and nmake. You might also need to
#   'set path=</path/to/custom/openssl>\bin;%PATH%
# in order to find the .dll files at runtime.

SET(WITH_SSL_DOC "bundled (use yassl)")
SET(WITH_SSL_DOC
  "${WITH_SSL_DOC}, yes (prefer os library if present, otherwise use bundled)")
SET(WITH_SSL_DOC
  "${WITH_SSL_DOC}, system (use os library)")
SET(WITH_SSL_DOC
  "${WITH_SSL_DOC}, </path/to/custom/installation>")

MACRO (CHANGE_SSL_SETTINGS string)
  SET(WITH_SSL ${string} CACHE STRING ${WITH_SSL_DOC} FORCE)
ENDMACRO()

MACRO (MYSQL_USE_BUNDLED_SSL)
  SET(INC_DIRS 
    ${CMAKE_SOURCE_DIR}/extra/yassl/include
    ${CMAKE_SOURCE_DIR}/extra/yassl/taocrypt/include
  )
  SET(SSL_LIBRARIES  yassl taocrypt)
  IF(CMAKE_SYSTEM_NAME MATCHES "SunOS")
    SET(SSL_LIBRARIES ${SSL_LIBRARIES} ${LIBSOCKET})
  ENDIF()
  SET(SSL_INCLUDE_DIRS ${INC_DIRS})
  SET(SSL_INTERNAL_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/extra/yassl/taocrypt/mySTL)
  SET(SSL_DEFINES "-DHAVE_YASSL -DYASSL_PREFIX -DHAVE_OPENSSL -DMULTI_THREADED")
  CHANGE_SSL_SETTINGS("bundled")
  ADD_SUBDIRECTORY(extra/yassl)
  ADD_SUBDIRECTORY(extra/yassl/taocrypt)
  GET_TARGET_PROPERTY(src yassl SOURCES)
  FOREACH(file ${src})
    SET(SSL_SOURCES ${SSL_SOURCES} ${CMAKE_SOURCE_DIR}/extra/yassl/${file})
  ENDFOREACH()
  GET_TARGET_PROPERTY(src taocrypt SOURCES)
  FOREACH(file ${src})
    SET(SSL_SOURCES ${SSL_SOURCES}
      ${CMAKE_SOURCE_DIR}/extra/yassl/taocrypt/${file})
  ENDFOREACH()
ENDMACRO()

# MYSQL_CHECK_SSL
#
# Provides the following configure options:
# WITH_SSL=[yes|bundled|system|<path/to/custom/installation>]
MACRO (MYSQL_CHECK_SSL)
  IF(NOT WITH_SSL)
   IF(WIN32)
     CHANGE_SSL_SETTINGS("bundled")
   ENDIF()
  ENDIF()

  # See if WITH_SSL is of the form </path/to/custom/installation>
  FILE(GLOB WITH_SSL_HEADER ${WITH_SSL}/include/openssl/ssl.h)
  IF (WITH_SSL_HEADER)
    SET(WITH_SSL_PATH ${WITH_SSL} CACHE PATH "path to custom SSL installation")
    SET(WITH_SSL_PATH ${WITH_SSL})
  ENDIF()

  IF(WITH_SSL STREQUAL "bundled")
    MYSQL_USE_BUNDLED_SSL()
    # Reset some variables, in case we switch from /path/to/ssl to "bundled".
    IF (WITH_SSL_PATH)
      UNSET(WITH_SSL_PATH)
      UNSET(WITH_SSL_PATH CACHE)
    ENDIF()
    IF (OPENSSL_ROOT_DIR)
      UNSET(OPENSSL_ROOT_DIR)
      UNSET(OPENSSL_ROOT_DIR CACHE)
    ENDIF()
    IF (OPENSSL_INCLUDE_DIR)
      UNSET(OPENSSL_INCLUDE_DIR)
      UNSET(OPENSSL_INCLUDE_DIR CACHE)
    ENDIF()
    IF (WIN32 AND OPENSSL_APPLINK_C)
      UNSET(OPENSSL_APPLINK_C)
      UNSET(OPENSSL_APPLINK_C CACHE)
    ENDIF()
    IF (OPENSSL_LIBRARY)
      UNSET(OPENSSL_LIBRARY)
      UNSET(OPENSSL_LIBRARY CACHE)
    ENDIF()
    IF (CRYPTO_LIBRARY)
      UNSET(CRYPTO_LIBRARY)
      UNSET(CRYPTO_LIBRARY CACHE)
    ENDIF()
  ELSEIF(WITH_SSL STREQUAL "system" OR
         WITH_SSL STREQUAL "yes" OR
         WITH_SSL_PATH
         )
    # First search in WITH_SSL_PATH.
    FIND_PATH(OPENSSL_ROOT_DIR
      NAMES include/openssl/ssl.h
      NO_CMAKE_PATH
      NO_CMAKE_ENVIRONMENT_PATH
      HINTS ${WITH_SSL_PATH}
    )
    # Then search in standard places (if not found above).
    FIND_PATH(OPENSSL_ROOT_DIR
      NAMES include/openssl/ssl.h
    )

    FIND_PATH(OPENSSL_INCLUDE_DIR
      NAMES openssl/ssl.h
      HINTS ${OPENSSL_ROOT_DIR}/include
    )

    IF (WIN32)
      FIND_FILE(OPENSSL_APPLINK_C
        NAMES openssl/applink.c
        HINTS ${OPENSSL_ROOT_DIR}/include
      )
      MESSAGE(STATUS "OPENSSL_APPLINK_C ${OPENSSL_APPLINK_C}")
    ENDIF()

    # On mac this list is <.dylib;.so;.a>
    # On most platforms we still prefer static libraries, so we revert it here.
    IF (WITH_SSL_PATH AND NOT APPLE)
      LIST(REVERSE CMAKE_FIND_LIBRARY_SUFFIXES)
      MESSAGE(STATUS "suffixes <${CMAKE_FIND_LIBRARY_SUFFIXES}>")
    ENDIF()
    FIND_LIBRARY(OPENSSL_LIBRARY
                 NAMES ssl ssleay32 ssleay32MD
                 HINTS ${OPENSSL_ROOT_DIR}/lib)
    FIND_LIBRARY(CRYPTO_LIBRARY
                 NAMES crypto libeay32
                 HINTS ${OPENSSL_ROOT_DIR}/lib)
    IF (WITH_SSL_PATH AND NOT APPLE)
      LIST(REVERSE CMAKE_FIND_LIBRARY_SUFFIXES)
    ENDIF()

    # Verify version number. Version information looks like:
    #   #define OPENSSL_VERSION_NUMBER 0x1000103fL
    # Encoded as MNNFFPPS: major minor fix patch status
    FILE(STRINGS "${OPENSSL_INCLUDE_DIR}/openssl/opensslv.h"
      OPENSSL_VERSION_NUMBER
      REGEX "^#[ ]*define[\t ]+OPENSSL_VERSION_NUMBER[\t ]+0x[0-9].*"
    )
    STRING(REGEX REPLACE
      "^.*OPENSSL_VERSION_NUMBER[\t ]+0x([0-9]).*$" "\\1"
      OPENSSL_MAJOR_VERSION "${OPENSSL_VERSION_NUMBER}"
    )

    IF(OPENSSL_INCLUDE_DIR AND
       OPENSSL_LIBRARY   AND
       CRYPTO_LIBRARY      AND
       OPENSSL_MAJOR_VERSION STREQUAL "1"
      )
      SET(OPENSSL_FOUND TRUE)
      FIND_PROGRAM(OPENSSL_EXECUTABLE openssl
        DOC "path to the openssl executable")
      IF(OPENSSL_EXECUTABLE)
        SET(OPENSSL_EXECUTABLE_HAS_ZLIB 0)
        EXECUTE_PROCESS(
          COMMAND ${OPENSSL_EXECUTABLE} "list-cipher-commands"
          OUTPUT_VARIABLE stdout
          ERROR_VARIABLE  stderr
          RESULT_VARIABLE result
          OUTPUT_STRIP_TRAILING_WHITESPACE
          )
#       If previous command failed, try alternative command line (debian)
        IF(NOT result EQUAL 0)
          EXECUTE_PROCESS(
            COMMAND ${OPENSSL_EXECUTABLE} "list" "-cipher-commands"
            OUTPUT_VARIABLE stdout
            ERROR_VARIABLE  stderr
            RESULT_VARIABLE result
            OUTPUT_STRIP_TRAILING_WHITESPACE
            )
        ENDIF()
        IF(result EQUAL 0)
          STRING(REGEX REPLACE "[ \n]+" ";" CIPHER_COMMAND_LIST ${stdout})
          FOREACH(cipher_command ${CIPHER_COMMAND_LIST})
            IF(${cipher_command} STREQUAL "zlib")
              SET(OPENSSL_EXECUTABLE_HAS_ZLIB 1)
            ENDIF()
          ENDFOREACH()
          IF(OPENSSL_EXECUTABLE_HAS_ZLIB)
            MESSAGE(STATUS "The openssl command does support zlib")
          ELSE()
            MESSAGE(STATUS "The openssl command does not support zlib")
          ENDIF()
        ENDIF()
      ENDIF()
    ELSE()
      SET(OPENSSL_FOUND FALSE)
    ENDIF()

    # If we are invoked with -DWITH_SSL=/path/to/custom/openssl
    # and we have found static libraries, then link them statically
    # into our executables and libraries.
    # Adding IMPORTED_LOCATION allows MERGE_STATIC_LIBS
    # to get LOCATION and do correct dependency analysis.
    SET(MY_CRYPTO_LIBRARY "${CRYPTO_LIBRARY}")
    SET(MY_OPENSSL_LIBRARY "${OPENSSL_LIBRARY}")
    IF (WITH_SSL_PATH)
      GET_FILENAME_COMPONENT(CRYPTO_EXT "${CRYPTO_LIBRARY}" EXT)
      GET_FILENAME_COMPONENT(OPENSSL_EXT "${OPENSSL_LIBRARY}" EXT)
      IF (CRYPTO_EXT STREQUAL ".a" OR OPENSSL_EXT STREQUAL ".lib")
        SET(MY_CRYPTO_LIBRARY imported_crypto)
        ADD_LIBRARY(imported_crypto STATIC IMPORTED)
        SET_TARGET_PROPERTIES(imported_crypto
          PROPERTIES IMPORTED_LOCATION "${CRYPTO_LIBRARY}")
      ENDIF()
      IF (OPENSSL_EXT STREQUAL ".a" OR OPENSSL_EXT STREQUAL ".lib")
        SET(MY_OPENSSL_LIBRARY imported_openssl)
        ADD_LIBRARY(imported_openssl STATIC IMPORTED)
        SET_TARGET_PROPERTIES(imported_openssl
          PROPERTIES IMPORTED_LOCATION "${OPENSSL_LIBRARY}")
      ENDIF()
    ENDIF()

    MESSAGE(STATUS "OPENSSL_INCLUDE_DIR = ${OPENSSL_INCLUDE_DIR}")
    MESSAGE(STATUS "OPENSSL_LIBRARY = ${OPENSSL_LIBRARY}")
    MESSAGE(STATUS "CRYPTO_LIBRARY = ${CRYPTO_LIBRARY}")
    MESSAGE(STATUS "OPENSSL_MAJOR_VERSION = ${OPENSSL_MAJOR_VERSION}")

    INCLUDE(CheckSymbolExists)
    SET(CMAKE_REQUIRED_INCLUDES ${OPENSSL_INCLUDE_DIR})
    CHECK_SYMBOL_EXISTS(SHA512_DIGEST_LENGTH "openssl/sha.h" 
                        HAVE_SHA512_DIGEST_LENGTH)
    IF(OPENSSL_FOUND AND HAVE_SHA512_DIGEST_LENGTH)
      SET(SSL_SOURCES "")
      SET(SSL_LIBRARIES ${MY_OPENSSL_LIBRARY} ${MY_CRYPTO_LIBRARY})
      IF(CMAKE_SYSTEM_NAME MATCHES "SunOS")
        SET(SSL_LIBRARIES ${SSL_LIBRARIES} ${LIBSOCKET})
      ENDIF()
      IF(CMAKE_SYSTEM_NAME MATCHES "Linux")
        SET(SSL_LIBRARIES ${SSL_LIBRARIES} ${LIBDL})
      ENDIF()
      MESSAGE(STATUS "SSL_LIBRARIES = ${SSL_LIBRARIES}")
      SET(SSL_INCLUDE_DIRS ${OPENSSL_INCLUDE_DIR})
      SET(SSL_INTERNAL_INCLUDE_DIRS "")
      SET(SSL_DEFINES "-DHAVE_OPENSSL")
    ELSE()

      UNSET(WITH_SSL_PATH)
      UNSET(WITH_SSL_PATH CACHE)
      UNSET(OPENSSL_ROOT_DIR)
      UNSET(OPENSSL_ROOT_DIR CACHE)
      UNSET(OPENSSL_INCLUDE_DIR)
      UNSET(OPENSSL_INCLUDE_DIR CACHE)
      UNSET(OPENSSL_APPLINK_C)
      UNSET(OPENSSL_APPLINK_C CACHE)
      UNSET(OPENSSL_LIBRARY)
      UNSET(OPENSSL_LIBRARY CACHE)
      UNSET(CRYPTO_LIBRARY)
      UNSET(CRYPTO_LIBRARY CACHE)

      MESSAGE(SEND_ERROR
        "Cannot find appropriate system libraries for SSL. "
        "Make sure you've specified a supported SSL version. "
        "Consult the documentation for WITH_SSL alternatives")
    ENDIF()
  ELSE()
    MESSAGE(SEND_ERROR
      "Wrong option or path for WITH_SSL. "
      "Valid options are : ${WITH_SSL_DOC}")
  ENDIF()
ENDMACRO()

# If cmake is invoked with -DWITH_SSL=</path/to/custom/openssl>
# and we discover that the installation has dynamic libraries,
# then copy the dlls to runtime_output_directory, and add INSTALL them.
# Currently only relevant for Windows and Mac.
MACRO(MYSQL_CHECK_SSL_DLLS)
  IF (WITH_SSL_PATH AND (APPLE OR WIN32))
    MESSAGE(STATUS "WITH_SSL_PATH ${WITH_SSL_PATH}")
    IF(APPLE)
      GET_FILENAME_COMPONENT(CRYPTO_EXT "${CRYPTO_LIBRARY}" EXT)
      GET_FILENAME_COMPONENT(OPENSSL_EXT "${OPENSSL_LIBRARY}" EXT)
      MESSAGE(STATUS "CRYPTO_EXT ${CRYPTO_EXT}")
      IF(CRYPTO_EXT STREQUAL ".dylib" AND OPENSSL_EXT STREQUAL ".dylib")
        MESSAGE(STATUS "set HAVE_CRYPTO_DYLIB HAVE_OPENSSL_DYLIB")
        SET(HAVE_CRYPTO_DYLIB 1)
        SET(HAVE_OPENSSL_DYLIB 1)
      ENDIF()
    ENDIF()
    IF(APPLE AND HAVE_CRYPTO_DYLIB AND HAVE_OPENSSL_DYLIB)
      # CRYPTO_LIBRARY is .../lib/libcrypto.dylib
      # CRYPTO_VERSION is .../lib/libcrypto.1.0.0.dylib
      EXECUTE_PROCESS(
        COMMAND readlink "${CRYPTO_LIBRARY}" OUTPUT_VARIABLE CRYPTO_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE)
      EXECUTE_PROCESS(
        COMMAND readlink "${OPENSSL_LIBRARY}" OUTPUT_VARIABLE OPENSSL_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE)

      # Replace dependency "/Volumes/.../lib/libcrypto.1.0.0.dylib
      EXECUTE_PROCESS(
        COMMAND otool -L "${OPENSSL_LIBRARY}"
        OUTPUT_VARIABLE OTOOL_OPENSSL_DEPS)
      STRING(REPLACE "\n" ";" DEPS_LIST ${OTOOL_OPENSSL_DEPS})
      FOREACH(LINE ${DEPS_LIST})
        STRING(REGEX MATCH "(/.*/lib/${CRYPTO_VERSION})" XXXXX ${LINE})
        IF(CMAKE_MATCH_1)
          SET(OPENSSL_DEPS "${CMAKE_MATCH_1}")
        ENDIF()
      ENDFOREACH()

      GET_FILENAME_COMPONENT(CRYPTO_DIRECTORY "${CRYPTO_LIBRARY}" DIRECTORY)
      GET_FILENAME_COMPONENT(OPENSSL_DIRECTORY "${OPENSSL_LIBRARY}" DIRECTORY)
      GET_FILENAME_COMPONENT(CRYPTO_NAME "${CRYPTO_LIBRARY}" NAME)
      GET_FILENAME_COMPONENT(OPENSSL_NAME "${OPENSSL_LIBRARY}" NAME)

      SET(CRYPTO_FULL_NAME "${CRYPTO_DIRECTORY}/${CRYPTO_VERSION}")
      SET(OPENSSL_FULL_NAME "${OPENSSL_DIRECTORY}/${OPENSSL_VERSION}")

      ADD_CUSTOM_TARGET(copy_openssl_dlls ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CRYPTO_FULL_NAME}" "./${CRYPTO_VERSION}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OPENSSL_FULL_NAME}" "./${OPENSSL_VERSION}"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
          "${CRYPTO_VERSION}" "${CRYPTO_NAME}"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
          "${OPENSSL_VERSION}" "${OPENSSL_NAME}"
        COMMAND chmod +w "${CRYPTO_VERSION}" "${OPENSSL_VERSION}"
        COMMAND install_name_tool -change
        "${OPENSSL_DEPS}" "@loader_path/${CRYPTO_VERSION}" "${OPENSSL_VERSION}"

        WORKING_DIRECTORY
        "${CMAKE_BINARY_DIR}/library_output_directory/${CMAKE_CFG_INTDIR}"
        )

      # Create symlinks for plugins, see MYSQL_ADD_PLUGIN/install_name_tool
      ADD_CUSTOM_TARGET(link_openssl_dlls ALL
        COMMAND ${CMAKE_COMMAND} -E create_symlink
          "../lib/${CRYPTO_VERSION}" "${CRYPTO_VERSION}"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
          "../lib/${OPENSSL_VERSION}" "${OPENSSL_VERSION}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/plugin_output_directory"
        )
      # Create symlinks for plugins built with Xcode
      IF(NOT BUILD_IS_SINGLE_CONFIG)
        ADD_CUSTOM_TARGET(link_openssl_dlls_cmake_cfg_intdir ALL
          COMMAND ${CMAKE_COMMAND} -E create_symlink
          "../../lib/${CMAKE_CFG_INTDIR}/${CRYPTO_VERSION}" "${CRYPTO_VERSION}"
          COMMAND ${CMAKE_COMMAND} -E create_symlink
          "../../lib/${CMAKE_CFG_INTDIR}/${OPENSSL_VERSION}" "${OPENSSL_VERSION}"
          WORKING_DIRECTORY
          "${CMAKE_BINARY_DIR}/plugin_output_directory/${CMAKE_CFG_INTDIR}"
        )
      ENDIF()

      # Directory layout after 'make install' is different.
      # Create some symlinks from lib/plugin/*.dylib to ../../lib/*.dylib
      FILE(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/plugin_output_directory/plugin")
      ADD_CUSTOM_TARGET(link_openssl_dlls_for_install ALL
        COMMAND ${CMAKE_COMMAND} -E create_symlink
          "../../lib/${CRYPTO_VERSION}" "${CRYPTO_VERSION}"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
          "../../lib/${OPENSSL_VERSION}" "${OPENSSL_VERSION}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/plugin_output_directory/plugin"
        )
      # See INSTALL_DEBUG_TARGET used for installing debug versions of plugins.
      IF(EXISTS ${DEBUGBUILDDIR})
        FILE(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/plugin_output_directory/plugin/debug")
        ADD_CUSTOM_TARGET(link_openssl_dlls_for_install_debug ALL
          COMMAND ${CMAKE_COMMAND} -E create_symlink
            "../../../lib/${CRYPTO_VERSION}" "${CRYPTO_VERSION}"
          COMMAND ${CMAKE_COMMAND} -E create_symlink
            "../../../lib/${OPENSSL_VERSION}" "${OPENSSL_VERSION}"
          WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/plugin_output_directory/plugin/debug"
        )
      ENDIF()

      MESSAGE(STATUS "INSTALL ${CRYPTO_NAME} to ${INSTALL_LIBDIR}")
      MESSAGE(STATUS "INSTALL ${OPENSSL_NAME} to ${INSTALL_LIBDIR}")

      # ${CMAKE_CFG_INTDIR} does not work with Xcode INSTALL ??
      SET(SUBDIRECTORY "")
      IF(CMAKE_BUILD_TYPE AND NOT BUILD_IS_SINGLE_CONFIG AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        SET(SUBDIRECTORY "Debug")
      ELSEIF(CMAKE_BUILD_TYPE AND NOT BUILD_IS_SINGLE_CONFIG AND CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        SET(SUBDIRECTORY "RelWithDebInfo")
      ENDIF()
      INSTALL(FILES
        ${CMAKE_BINARY_DIR}/library_output_directory/${SUBDIRECTORY}/${CRYPTO_NAME}
        ${CMAKE_BINARY_DIR}/library_output_directory/${SUBDIRECTORY}/${OPENSSL_NAME}
        ${CMAKE_BINARY_DIR}/library_output_directory/${SUBDIRECTORY}/${CRYPTO_VERSION}
        ${CMAKE_BINARY_DIR}/library_output_directory/${SUBDIRECTORY}/${OPENSSL_VERSION}
        DESTINATION "${INSTALL_LIBDIR}" COMPONENT SharedLibraries
        )
      INSTALL(FILES
        ${CMAKE_BINARY_DIR}/plugin_output_directory/plugin/${CRYPTO_VERSION}
        ${CMAKE_BINARY_DIR}/plugin_output_directory/plugin/${OPENSSL_VERSION}
        DESTINATION ${INSTALL_PLUGINDIR} COMPONENT SharedLibraries
        )
      # See INSTALL_DEBUG_TARGET used for installing debug versions of plugins.
      IF(EXISTS ${DEBUGBUILDDIR})
        INSTALL(FILES
          ${CMAKE_BINARY_DIR}/plugin_output_directory/plugin/debug/${CRYPTO_VERSION}
          ${CMAKE_BINARY_DIR}/plugin_output_directory/plugin/debug/${OPENSSL_VERSION}
          DESTINATION ${INSTALL_PLUGINDIR}/debug COMPONENT SharedLibraries
          )
      ENDIF()
    ENDIF()

    IF(WIN32)
      GET_FILENAME_COMPONENT(CRYPTO_NAME "${CRYPTO_LIBRARY}" NAME_WE)
      GET_FILENAME_COMPONENT(OPENSSL_NAME "${OPENSSL_LIBRARY}" NAME_WE)
      FILE(GLOB HAVE_CRYPTO_DLL "${WITH_SSL_PATH}/bin/${CRYPTO_NAME}.dll")
      FILE(GLOB HAVE_OPENSSL_DLL "${WITH_SSL_PATH}/bin/${OPENSSL_NAME}.dll")
      MESSAGE(STATUS "HAVE_CRYPTO_DLL ${HAVE_CRYPTO_DLL}")
      MESSAGE(STATUS "HAVE_OPENSSL_DLL ${HAVE_OPENSSL_DLL}")
      IF(HAVE_CRYPTO_DLL AND HAVE_OPENSSL_DLL)
        ADD_CUSTOM_TARGET(copy_openssl_dlls ALL
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${WITH_SSL_PATH}/bin/${CRYPTO_NAME}.dll"
          "${CMAKE_BINARY_DIR}/runtime_output_directory/${CMAKE_CFG_INTDIR}/${CRYPTO_NAME}.dll"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${WITH_SSL_PATH}/bin/${OPENSSL_NAME}.dll"
          "${CMAKE_BINARY_DIR}/runtime_output_directory/${CMAKE_CFG_INTDIR}/${OPENSSL_NAME}.dll"
          )
        MESSAGE(STATUS
          "INSTALL ${WITH_SSL_PATH}/bin/${CRYPTO_NAME}.dll to ${INSTALL_BINDIR}")
        MESSAGE(STATUS
          "INSTALL ${WITH_SSL_PATH}/bin/${OPENSSL_NAME}.dll to ${INSTALL_BINDIR}")
        INSTALL(FILES
          "${WITH_SSL_PATH}/bin/${CRYPTO_NAME}.dll"
          "${WITH_SSL_PATH}/bin/${OPENSSL_NAME}.dll"
          DESTINATION "${INSTALL_BINDIR}" COMPONENT SharedLibraries)
      ELSE()
        MESSAGE(STATUS "Cannot find SSL dynamic libraries")
      ENDIF()
    ENDIF()
  ENDIF()
ENDMACRO()
