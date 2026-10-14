# Cross-platform build of LinuxSCP (Linux + Windows).
#
#   Linux:   cmake -S . -B build && cmake --build build
#            (the supported packaging build remains the plain Makefile:
#             make -C source/linux -- this produces the same binary)
#   Windows: cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#            or with MinGW: cmake -S . -B build -G "MinGW Makefiles"
#            (see docs/windows.md and windows/build.cmd)
#
# The front end in source/linux is one code base for both platforms.
# The difference is the SFTP transport: on Linux the native PuTTY
# engine (source/putty + libs/putty/unix) is linked, as in the
# Makefile build; on Windows all protocols, SFTP included, go through
# the curl backend (curl.exe ships with Windows), and the PuTTY-only
# translation units are replaced by SftpClientStub.cpp. Everything
# else -- commands, scripting, local file handling -- is identical.

cmake_minimum_required(VERSION 3.16)
project(LinuxSCP VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The vendored PuTTY core (and the Makefile build) is compiled without
# NDEBUG: parts of the core sit behind assert-enabled code paths, and
# defining NDEBUG changes which code is generated (it even leaves an
# undefined ltime reference in logging.c). Strip it to match the
# supported Makefile build exactly.
foreach(lang C CXX)
  foreach(config RELEASE RELWITHDEBINFO MINSIZEREL)
    string(REPLACE "-DNDEBUG" "" CMAKE_${lang}_FLAGS_${config}
      "${CMAKE_${lang}_FLAGS_${config}}")
    string(REPLACE "/DNDEBUG" "" CMAKE_${lang}_FLAGS_${config}
      "${CMAKE_${lang}_FLAGS_${config}}")
  endforeach()
endforeach()

set(FRONTEND source/linux)

set(FRONTEND_SOURCES
  ${FRONTEND}/Session.cpp
  ${FRONTEND}/SessionStore.cpp
  ${FRONTEND}/SyncPlanner.cpp
  ${FRONTEND}/Editor.cpp
  ${FRONTEND}/ProtocolClient.cpp
  ${FRONTEND}/Script.cpp
  ${FRONTEND}/Console.cpp
  ${FRONTEND}/LocalFs.cpp
  ${FRONTEND}/PathUtil.cpp
  ${FRONTEND}/Platform.cpp
  ${FRONTEND}/Texts.cpp
  ${FRONTEND}/TextMode.cpp
)

if(WIN32)
  list(APPEND FRONTEND_SOURCES
    ${FRONTEND}/SftpClientStub.cpp
    ${FRONTEND}/HashUtilStub.cpp
  )
else()
  list(APPEND FRONTEND_SOURCES
    ${FRONTEND}/PuttyGlue.cpp
    ${FRONTEND}/SftpClient.c
    ${FRONTEND}/HashUtil.c
  )
endif()

add_library(linuxscp-frontend STATIC ${FRONTEND_SOURCES})
target_include_directories(linuxscp-frontend PUBLIC ${FRONTEND})

add_executable(linuxscp ${FRONTEND}/main.cpp)
target_link_libraries(linuxscp PRIVATE linuxscp-frontend)
if(WIN32)
  set_target_properties(linuxscp PROPERTIES OUTPUT_NAME "linuxscp")
  # Application manifest: long-path awareness + UTF-8 code page.
  if(MSVC)
    target_sources(linuxscp PRIVATE windows/linuxscp.manifest)
  elseif(MINGW)
    target_sources(linuxscp PRIVATE windows/linuxscp.rc)
    set_source_files_properties(windows/linuxscp.rc PROPERTIES
      OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/windows/linuxscp.manifest)
  endif()
endif()

enable_testing()
add_executable(linuxscp-unittest ${FRONTEND}/tests/UnitTest.cpp)
target_link_libraries(linuxscp-unittest PRIVATE linuxscp-frontend)
add_test(NAME unit-tests COMMAND linuxscp-unittest)

# Server-free script regressions and the offline upstream gap inventory.
# Python is optional for C++-only builds; CI always provides it.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME script-tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/${FRONTEND}/tests/ScriptTest.py
    $<TARGET_FILE:linuxscp>)
  add_test(NAME parity-checker-tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/tests/test_parity_inventory.py)
  add_test(NAME parity-inventory COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_upstream_parity.py)
endif()

if(NOT WIN32)
  # ---- PuTTY core + POSIX platform layer (mirrors source/linux/Makefile) --
  file(GLOB_RECURSE PUTTY_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/source/putty/*.c)
  list(REMOVE_ITEM PUTTY_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/source/putty/ssh/nosharing.c)

  file(GLOB PUTTY_UNIX_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/putty/unix/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/putty/unix/utils/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/putty/unix/stubs/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/putty/utils/*.c)
  file(GLOB PUTTY_CHARSET_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/putty/charset/*.c)
  list(REMOVE_ITEM PUTTY_CHARSET_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/putty/charset/enum.c)

  add_library(putty STATIC
    ${PUTTY_CORE_SOURCES}
    ${PUTTY_UNIX_SOURCES}
    ${PUTTY_CHARSET_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/putty/ssh/sftp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/putty/ssh/sftpcommon.c
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/putty/psftpcommon.c
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/putty/pinger.c
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/putty/timing.c)

  target_compile_definitions(putty PRIVATE
    FORK_BUILD MPEXT PLATFORM_HAS_SMEMCLR NO_GSSAPI
    HAVE_AES_NI=0 HAVE_SHA_NI=0 HAVE_CLMUL=0
    HAVE_NEON_CRYPTO=0 HAVE_NEON_PMULL=0 HAVE_NEON_SHA512=0
    HAVE_SYS_TYPES_H=1 HAVE_SYS_STAT_H=1 HAVE_SYS_TIME_H=1
    HAVE_SYS_SELECT_H=1 HAVE_SYS_SOCKET_H=1 HAVE_SYS_IOCTL_H=1
    HAVE_SYS_UN_H=1 HAVE_SYS_UIO_H=1 HAVE_NETDB_H=1 HAVE_NETINET_IN_H=1
    HAVE_ARPA_INET_H=1 HAVE_POLL_H=1 HAVE_FCNTL_H=1 HAVE_TERMIOS_H=1
    HAVE_UNISTD_H=1 HAVE_PWD_H=1 HAVE_GLOB_H=1 HAVE_POLL=1
    HAVE_CLOCK_GETTIME=1 HAVE_CLOCK_MONOTONIC=1 HAVE_SETPWENT=1
    HAVE_ENDPWENT=1 HAVE_FUTIMES=1 HAVE_GETADDRINFO=1 HAVE_UTIMES=1
    HAVE_SYSCTLBYNAME=0 HAVE_SO_PEERCRED=1 HAVE_GETAUXVAL=1
    HAVE_SYS_AUXV_H=1 HAVE_ASM_HWCAP_H=1 HAVE_ELF_AUX_INFO=0
    HAVE_SYS_SYSCTL_H=0)
  target_compile_definitions(putty PUBLIC _FILE_OFFSET_BITS=64)
  set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/source/putty/be_list.c
    PROPERTIES COMPILE_DEFINITIONS APPNAME=LinuxSCP)
  target_include_directories(putty PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/source/putty
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/putty/unix
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/putty
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/putty/charset)

  find_package(Threads REQUIRED)
  target_link_libraries(putty PUBLIC Threads::Threads)
  target_link_libraries(linuxscp-frontend PUBLIC putty)
  target_compile_definitions(linuxscp-frontend PRIVATE
    FORK_BUILD MPEXT PLATFORM_HAS_SMEMCLR NO_GSSAPI
    HAVE_AES_NI=0 HAVE_SHA_NI=0 HAVE_CLMUL=0
    HAVE_NEON_CRYPTO=0 HAVE_NEON_PMULL=0 HAVE_NEON_SHA512=0)
endif()

if(MSVC)
  target_compile_options(linuxscp-frontend PRIVATE /W3 /utf-8)
else()
  target_compile_options(linuxscp-frontend PRIVATE -Wall)
endif()

# ---- install -------------------------------------------------------------
install(TARGETS linuxscp RUNTIME DESTINATION bin)
if(WIN32)
  install(FILES source/gui/linuxscp_gui.py source/gui/linuxscp-gui.bat
    DESTINATION bin)
  install(DIRECTORY source/gui/web/ DESTINATION bin/web)
else()
  install(FILES docs/linuxscp.1 DESTINATION share/man/man1)
  install(FILES debian/linuxscp.desktop DESTINATION share/applications)
  install(FILES source/gui/linuxscp_gui.py
    DESTINATION share/linuxscp/gui
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
      GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
  install(DIRECTORY source/gui/web/ DESTINATION share/linuxscp/gui/web)
endif()
