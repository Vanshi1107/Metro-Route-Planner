cmake_minimum_required(VERSION 3.21)
project(metro_planner VERSION 0.1.0 LANGUAGES C CXX)   # C is needed to compile SQLite

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single-config generators (Ninja, Makefiles) need a default build type.
# Visual Studio generators choose it at build time with:  --config Release
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(METRO_USE_SYSTEM_SQLITE
       "Use SQLite installed on the system instead of third_party/sqlite3 (mostly for Linux)" OFF)

if(NOT WIN32)
  find_package(Threads REQUIRED)
endif()

# ---- warning flags, applied only to OUR code (not to SQLite) -----------------------------
add_library(metro_warnings INTERFACE)
if(MSVC)
  target_compile_options(metro_warnings INTERFACE /W4 /utf-8)
else()
  target_compile_options(metro_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ---- SQLite -------------------------------------------------------------------------------
if(METRO_USE_SYSTEM_SQLITE)
  find_package(SQLite3 REQUIRED)
  add_library(metro_sqlite INTERFACE)
  target_link_libraries(metro_sqlite INTERFACE SQLite::SQLite3)
else()
  set(METRO_SQLITE_DIR "${PROJECT_SOURCE_DIR}/third_party/sqlite3")
  if(NOT EXISTS "${METRO_SQLITE_DIR}/sqlite3.c" OR NOT EXISTS "${METRO_SQLITE_DIR}/sqlite3.h")
    message(FATAL_ERROR
      "SQLite source files not found.\n"
      "Download 'sqlite-amalgamation-*.zip' from https://www.sqlite.org/download.html,\n"
      "unzip it, and copy sqlite3.c and sqlite3.h into:\n"
      "    ${METRO_SQLITE_DIR}\n"
      "Then run the cmake configure command again.")
  endif()
  add_library(metro_sqlite STATIC "${METRO_SQLITE_DIR}/sqlite3.c")
  target_include_directories(metro_sqlite PUBLIC "${METRO_SQLITE_DIR}")
  target_compile_definitions(metro_sqlite PRIVATE SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_THREADSAFE=1)
  if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(metro_sqlite PUBLIC Threads::Threads)
  endif()
  if(MSVC)
    target_compile_options(metro_sqlite PRIVATE /W0)   # silence third-party warnings
  else()
    target_compile_options(metro_sqlite PRIVATE -w)
  endif()
endif()

# ---- our libraries -------------------------------------------------------------------------
add_library(metro_common STATIC src/common/csv.cpp src/common/db.cpp src/common/text.cpp)
target_include_directories(metro_common PUBLIC src)
target_link_libraries(metro_common PUBLIC metro_sqlite PRIVATE metro_warnings)

add_library(metro_gtfs STATIC src/gtfs/inspect.cpp)
target_link_libraries(metro_gtfs PUBLIC metro_common PRIVATE metro_warnings)

add_library(metro_importer STATIC src/import/importer.cpp)
target_link_libraries(metro_importer PUBLIC metro_gtfs PRIVATE metro_warnings)

add_library(metro_route_lib STATIC src/route/network.cpp src/route/router.cpp src/route/fare.cpp src/route/planner.cpp src/route/verify.cpp src/route/present.cpp)
target_link_libraries(metro_route_lib PUBLIC metro_gtfs PRIVATE metro_warnings)

# ---- accounts, sessions and saved journeys -----------------------------------------------------
# Password hashing uses the operating system's own cryptography on Windows (CNG) and a portable version elsewhere.
set(METRO_AUTH_SOURCES src/auth/sha256.cpp src/auth/password.cpp src/auth/accounts.cpp src/auth/journeys.cpp)
if(WIN32)
  list(APPEND METRO_AUTH_SOURCES src/auth/crypto_win.cpp)
else()
  list(APPEND METRO_AUTH_SOURCES src/auth/crypto_portable.cpp)
endif()
add_library(metro_accounts STATIC ${METRO_AUTH_SOURCES})
target_link_libraries(metro_accounts PUBLIC metro_route_lib PRIVATE metro_warnings)
if(WIN32)
  target_link_libraries(metro_accounts PUBLIC bcrypt)
endif()

# ---- the web layer: request handlers (no network needed) ------------------------------------------------------
add_library(metro_web_lib STATIC src/web/json.cpp src/web/http.cpp src/web/api.cpp)
target_link_libraries(metro_web_lib PUBLIC metro_accounts PRIVATE metro_warnings)

# ---- command-line tools -----------------------------------------------------------------------
add_executable(metro_db src/tools/metro_db.cpp)
target_link_libraries(metro_db PRIVATE metro_common metro_warnings)

add_executable(gtfs_inspect src/tools/gtfs_inspect.cpp)
target_link_libraries(gtfs_inspect PRIVATE metro_gtfs metro_warnings)

add_executable(metro_import src/tools/metro_import.cpp)
target_link_libraries(metro_import PRIVATE metro_importer metro_warnings)

add_executable(metro_route src/tools/metro_route.cpp)
target_link_libraries(metro_route PRIVATE metro_route_lib metro_warnings)

add_executable(metro_user src/tools/metro_user.cpp)
target_link_libraries(metro_user PRIVATE metro_accounts metro_warnings)

# ---- tests (run with: ctest) --------------------------------------------------------------------
enable_testing()

add_executable(test_csv tests/test_csv.cpp)
target_link_libraries(test_csv PRIVATE metro_common metro_warnings)
add_test(NAME csv COMMAND test_csv)

add_executable(test_db tests/test_db.cpp)
target_link_libraries(test_db PRIVATE metro_common metro_warnings)
add_test(NAME db COMMAND test_db "${PROJECT_SOURCE_DIR}/db/schema.sql")

add_executable(test_inspect tests/test_inspect.cpp)
target_link_libraries(test_inspect PRIVATE metro_gtfs metro_warnings)
add_test(NAME inspect COMMAND test_inspect "${PROJECT_SOURCE_DIR}/tests/data/mini_gtfs")

add_executable(test_import tests/test_import.cpp)
target_link_libraries(test_import PRIVATE metro_importer metro_warnings)
add_test(NAME import COMMAND test_import "${PROJECT_SOURCE_DIR}/db/schema.sql"
         "${PROJECT_SOURCE_DIR}/tests/data/import_gtfs" "${PROJECT_SOURCE_DIR}/tests/data/import_config")

add_executable(test_router tests/test_router.cpp)
target_link_libraries(test_router PRIVATE metro_route_lib metro_importer metro_warnings)
add_test(NAME router COMMAND test_router "${PROJECT_SOURCE_DIR}/db/schema.sql"
         "${PROJECT_SOURCE_DIR}/tests/data/import_gtfs" "${PROJECT_SOURCE_DIR}/tests/data/import_config")

add_executable(test_fare tests/test_fare.cpp)
target_link_libraries(test_fare PRIVATE metro_route_lib metro_importer metro_warnings)
add_test(NAME fare COMMAND test_fare "${PROJECT_SOURCE_DIR}/db/schema.sql"
         "${PROJECT_SOURCE_DIR}/tests/data/import_gtfs" "${PROJECT_SOURCE_DIR}/tests/data/import_config")

add_executable(test_crypto tests/test_crypto.cpp)
target_link_libraries(test_crypto PRIVATE metro_accounts metro_warnings)
add_test(NAME crypto COMMAND test_crypto)

add_executable(test_accounts tests/test_accounts.cpp)
target_link_libraries(test_accounts PRIVATE metro_accounts metro_warnings)
add_test(NAME accounts COMMAND test_accounts "${PROJECT_SOURCE_DIR}/db/schema.sql")

add_executable(test_journeys tests/test_journeys.cpp)
target_link_libraries(test_journeys PRIVATE metro_accounts metro_importer metro_warnings)
add_test(NAME journeys COMMAND test_journeys "${PROJECT_SOURCE_DIR}/db/schema.sql"
         "${PROJECT_SOURCE_DIR}/tests/data/import_gtfs" "${PROJECT_SOURCE_DIR}/tests/data/import_config")

add_executable(test_web tests/test_web.cpp)
target_link_libraries(test_web PRIVATE metro_web_lib metro_importer metro_warnings)
if(NOT WIN32)
  target_link_libraries(test_web PRIVATE Threads::Threads)
endif()
add_test(NAME web COMMAND test_web "${PROJECT_SOURCE_DIR}/db/schema.sql"
         "${PROJECT_SOURCE_DIR}/tests/data/import_gtfs" "${PROJECT_SOURCE_DIR}/tests/data/import_config")

# ---- the web server itself: only built if cpp-httplib has been downloaded (see README.md) -----------------------
if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/httplib/httplib.h")
  add_executable(metro_web src/tools/metro_web.cpp)
  target_include_directories(metro_web SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/httplib")
  target_link_libraries(metro_web PRIVATE metro_web_lib metro_warnings)
  if(WIN32)
    target_compile_definitions(metro_web PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(metro_web PRIVATE ws2_32)
  else()
    target_link_libraries(metro_web PRIVATE Threads::Threads)
  endif()
else()
  message(STATUS "metro_web will not be built: third_party/httplib/httplib.h is missing (see README.md)")
endif()

add_executable(test_planner tests/test_planner.cpp)
target_link_libraries(test_planner PRIVATE metro_route_lib metro_importer metro_warnings)
add_test(NAME planner COMMAND test_planner "${PROJECT_SOURCE_DIR}/db/schema.sql"
         "${PROJECT_SOURCE_DIR}/tests/data/import_gtfs" "${PROJECT_SOURCE_DIR}/tests/data/import_config")
