cmake_minimum_required(VERSION 3.16)
project(lmdb-lws-backup VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# --- vendored LMDB (0.9.33), built static ---------------------------------
add_library(lmdb_vendored STATIC
  third_party/liblmdb/mdb.c
  third_party/liblmdb/midl.c)
target_include_directories(lmdb_vendored PUBLIC third_party/liblmdb)
# Upstream C sources are not warning-clean; keep our own warnings meaningful.
target_compile_options(lmdb_vendored PRIVATE -w)
find_package(Threads REQUIRED)
target_link_libraries(lmdb_vendored PUBLIC Threads::Threads)

# --- libsodium: prefer the locally built static lib, else system ----------
set(SODIUM_LOCAL_LIB ${CMAKE_SOURCE_DIR}/third_party/local/lib/libsodium.a)
set(SODIUM_LOCAL_INC ${CMAKE_SOURCE_DIR}/third_party/local/include)
if(EXISTS ${SODIUM_LOCAL_LIB})
  add_library(sodium STATIC IMPORTED)
  set_target_properties(sodium PROPERTIES
    IMPORTED_LOCATION ${SODIUM_LOCAL_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${SODIUM_LOCAL_INC})
  message(STATUS "Using vendored static libsodium: ${SODIUM_LOCAL_LIB}")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
  add_library(sodium ALIAS PkgConfig::SODIUM)
  message(STATUS "Using system libsodium")
endif()

# --- vendored toml++ (header-only) ----------------------------------------
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus INTERFACE third_party/tomlplusplus)

# --- core library ----------------------------------------------------------
add_library(lwsbk_core STATIC
  src/log.cpp
  src/hot_copy.cpp
  src/crypto.cpp
  src/manifest.cpp
  src/retention.cpp
  src/config.cpp
  src/backup.cpp
  src/scheduler.cpp
  src/alert.cpp
  src/cli.cpp)
target_include_directories(lwsbk_core PUBLIC src)
target_link_libraries(lwsbk_core PUBLIC lmdb_vendored sodium tomlplusplus)

# --- main binary -----------------------------------------------------------
add_executable(lmdb-lws-backup src/main.cpp)
target_link_libraries(lmdb-lws-backup PRIVATE lwsbk_core)
# Static C++/gcc runtime so the binary only needs libc at run time.
target_link_options(lmdb-lws-backup PRIVATE -static-libstdc++ -static-libgcc)

install(TARGETS lmdb-lws-backup RUNTIME DESTINATION bin)

enable_testing()
add_subdirectory(tests)
