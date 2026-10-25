cmake_minimum_required(VERSION 3.25)

project(datadam VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DATADAM_BUILD_TESTS "Build unit tests" ON)
set(DATADAM_SANITIZE "" CACHE STRING "Sanitizers to build with (GCC/Clang): address,undefined or thread")

find_package(asio CONFIG REQUIRED)
find_package(RocksDB CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)
# LuaJIT (Lua 5.1, as Redis scripts expect); vcpkg ships no CMake config for it.
find_path(LUAJIT_INCLUDE_DIR luajit.h PATH_SUFFIXES luajit luajit-2.1 REQUIRED)
find_library(LUAJIT_LIBRARY_RELEASE NAMES lua51 luajit-5.1 luajit
             PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(LUAJIT_LIBRARY_DEBUG NAMES lua51 luajit-5.1 luajit
             PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
if(LUAJIT_LIBRARY_DEBUG)
  set(LUAJIT_LIBRARY optimized ${LUAJIT_LIBRARY_RELEASE} debug ${LUAJIT_LIBRARY_DEBUG})
else()
  set(LUAJIT_LIBRARY ${LUAJIT_LIBRARY_RELEASE})
endif()

add_library(datadam_core STATIC
  src/util/strings.cpp
  src/util/log.cpp
  src/util/sha256.cpp
  src/config/config.cpp
  src/mem/size_classes.cpp
  src/mem/page_arena.cpp
  src/mem/slab_allocator.cpp
  src/cache/object_cache.cpp
  src/storage/record.cpp
  src/storage/rocks_storage.cpp
  src/index/index_def.cpp
  src/index/index_registry.cpp
  src/index/search_query.cpp
  src/db/element_set.cpp
  src/db/database.cpp
  src/db/change_feed.cpp
  src/db/index_ops.cpp
  src/db/search_ops.cpp
  src/server/resp.cpp
  src/server/redis_commands.cpp
  src/server/server_context.cpp
  src/server/cmd_scan.cpp
  src/server/cmd_acl.cpp
  src/server/cmd_cdc.cpp
  src/server/cmd_repl.cpp
  src/server/cmd_json.cpp
  src/json/json.cpp
  src/json/json_path.cpp
  src/server/replication.cpp
  src/server/replica_link.cpp
  src/server/acl.cpp
  src/server/command_meta.cpp
  src/server/cmd_hash.cpp
  src/server/cmd_set.cpp
  src/server/cmd_zset.cpp
  src/server/cmd_list.cpp
  src/server/cmd_index.cpp
  src/server/cmd_search.cpp
  src/server/cmd_tx.cpp
  src/server/cmd_pubsub.cpp
  src/server/cmd_script.cpp
  src/server/scripting.cpp
  src/server/pubsub.cpp
  src/server/admin.cpp
  src/server/cmd_admin.cpp
  src/server/memcache_protocol.cpp
  src/server/server.cpp
  src/server/read_pool.cpp
  src/server/tls.cpp
)

target_include_directories(datadam_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(datadam_core PUBLIC
  DATADAM_VERSION="${PROJECT_VERSION}"
  ASIO_STANDALONE
  ASIO_NO_DEPRECATED
)
target_link_libraries(datadam_core PUBLIC asio::asio RocksDB::rocksdb OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json
                      Threads::Threads ${LUAJIT_LIBRARY} ${CMAKE_DL_LIBS})
target_include_directories(datadam_core PRIVATE ${LUAJIT_INCLUDE_DIR})

if(WIN32)
  target_compile_definitions(datadam_core PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(MSVC)
  target_compile_options(datadam_core PUBLIC /W4 /permissive- /utf-8 /Zc:__cplusplus /bigobj /EHsc
    /wd4324)  # structure padded due to alignas: intended for cache-line separation
else()
  target_compile_options(datadam_core PUBLIC -Wall -Wextra -Wpedantic)
  if(DATADAM_SANITIZE)
    # Every UB report is fatal, so a test run fails on the first one.
    target_compile_options(datadam_core PUBLIC -fsanitize=${DATADAM_SANITIZE} -fno-sanitize-recover=all
      -fno-omit-frame-pointer -g)
    target_link_options(datadam_core PUBLIC -fsanitize=${DATADAM_SANITIZE})
  endif()
endif()

add_executable(datadam src/main.cpp)
target_link_libraries(datadam PRIVATE datadam_core)

if(DATADAM_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  add_executable(datadam_tests
    tests/strings_test.cpp
    tests/slab_allocator_test.cpp
    tests/object_cache_test.cpp
    tests/resp_test.cpp
    tests/database_test.cpp
    tests/collections_test.cpp
    tests/index_test.cpp
    tests/element_set_test.cpp
    tests/admin_test.cpp
    tests/acl_test.cpp
    tests/tls_test.cpp
    tests/cdc_test.cpp
    tests/replication_test.cpp
    tests/json_test.cpp
    tests/search_test.cpp
    tests/realtime_test.cpp
    tests/ops_test.cpp
    tests/script_test.cpp
  )
  target_link_libraries(datadam_tests PRIVATE datadam_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(datadam_tests DISCOVERY_MODE PRE_TEST)
endif()
