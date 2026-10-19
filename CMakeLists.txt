# BFA Remote Bridge: the platform-neutral core (src/core) with its tests, built on Linux and Windows, and the SKSE
# plugin, BFARemoteBridge.dll (src/plugin), built only on Windows with MSVC through the "windows" preset
# (CommonLibSSE-NG's MIT line and spdlog through vcpkg).
#
# Linux:
#   cmake -S . -B build -DBFARB_PYTHON=<a python with onnxruntime==1.24.4, onnx and numpy>
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# Without BFARB_PYTHON (or with a python lacking those packages) the tests that need a real runtime and the reference
# server are skipped. BFARB_BFA_MODELS=<folder holding BFA's performance.onnx and network_fp16.onnx> and
# BFARB_BFA_DLL=<BFA's BetterFacialAnimations.dll>, in the environment when the tests run, add BFA's own files
# (never committed) to them.
cmake_minimum_required(VERSION 3.21)
project(BFARemoteBridge VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(BFARB_WERROR "Treat compiler warnings as errors" ON)
option(BFARB_PLUGIN_DLL "Build BFARemoteBridge.dll (Windows, MSVC, CommonLibSSE-NG through vcpkg)" OFF)
option(BFARB_SANITIZE "Build the core and tests with AddressSanitizer and UndefinedBehaviorSanitizer (GCC, Clang)" OFF)
option(BFARB_TSAN "Build the core and tests with ThreadSanitizer (GCC, Clang; Linux only)" OFF)
if(BFARB_TSAN AND BFARB_SANITIZE)
  message(FATAL_ERROR "BFARB_TSAN and BFARB_SANITIZE can't be on in the same build: use a build folder for each.")
endif()

# ---------------------------------------------------------------------------- warnings
add_library(bfarb_options INTERFACE)
if(MSVC)
  target_compile_options(bfarb_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  if(BFARB_WERROR)
    target_compile_options(bfarb_options INTERFACE /WX)
  endif()
  # No build folder in a built file: /d1trimfile cuts the source root from __FILE__ (what SKSE::log writes), and
  # /PDBALTPATH puts only the .pdb's name in the DLL.
  file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/" BFARB_SOURCE_ROOT_NATIVE)
  target_compile_options(bfarb_options INTERFACE "/d1trimfile:${BFARB_SOURCE_ROOT_NATIVE}")
  target_link_options(bfarb_options INTERFACE "/PDBALTPATH:%_PDB%")
else()
  target_compile_options(bfarb_options INTERFACE -Wall -Wextra -Wpedantic)
  if(BFARB_WERROR)
    target_compile_options(bfarb_options INTERFACE -Werror)
  endif()
  if(BFARB_SANITIZE)
    target_compile_options(bfarb_options INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
    target_link_options(bfarb_options INTERFACE -fsanitize=address,undefined)
    # GCC 13 under AddressSanitizer reports false out-of-bounds reads inside the standard library's own code (a
    # vector's copy of a one-item initialiser list), which -Werror would turn into failed builds.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      target_compile_options(bfarb_options INTERFACE -Wno-array-bounds -Wno-stringop-overread -Wno-maybe-uninitialized)
    endif()
  endif()
  if(BFARB_TSAN)
    target_compile_options(bfarb_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options(bfarb_options INTERFACE -fsanitize=thread)
  endif()
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------- core
add_library(bfarb_core STATIC
  src/core/Bridge.cpp
  src/core/Client.cpp
  src/core/PeImage.cpp
  src/core/Protocol.cpp
  src/core/Scrub.cpp
  src/core/Settings.cpp
  src/core/Sha256.cpp
  src/core/Status.cpp)
target_include_directories(bfarb_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# ONNX Runtime 1.24.4's C API headers (MIT), vendored; only their declarations are used.
target_include_directories(bfarb_core SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")
target_compile_definitions(bfarb_core PUBLIC BFARB_VERSION="${PROJECT_VERSION}")
target_link_libraries(bfarb_core PUBLIC Threads::Threads PRIVATE bfarb_options)

# ---------------------------------------------------------------------------- tests (Linux and other POSIX systems)
if(NOT WIN32)
  enable_testing()
  set(BFARB_PYTHON "" CACHE FILEPATH "Python with onnxruntime==1.24.4, onnx and numpy, for the runtime tests and the reference server")

  add_executable(bfarb_tests
    tests/ClientTests.cpp
    tests/CoreTests.cpp
    tests/ForwardTests.cpp
    tests/ProtocolTests.cpp
    tests/TestMain.cpp)
  target_compile_definitions(bfarb_tests PRIVATE BFARB_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
  if(BFARB_TSAN AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC's ThreadSanitizer doesn't see std::timed_mutex's timed locks: the test program routes them through a lock it
    # sees (tests/TsanClocklock.cpp).
    target_sources(bfarb_tests PRIVATE tests/TsanClocklock.cpp)
  endif()
  target_link_libraries(bfarb_tests PRIVATE bfarb_core bfarb_options ${CMAKE_DL_LIBS})

  set(BFARB_TEST_ENV "BFARB_REPO_ROOT=${CMAKE_CURRENT_SOURCE_DIR}")
  if(BFARB_PYTHON)
    execute_process(
      COMMAND "${BFARB_PYTHON}" -I -c "import onnxruntime, os, glob; print(glob.glob(os.path.join(os.path.dirname(onnxruntime.__file__), 'capi', 'libonnxruntime.so*'))[0]); print(onnxruntime.__version__)"
      OUTPUT_VARIABLE BFARB_ORT_PROBE OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE BFARB_ORT_RC)
    if(BFARB_ORT_RC EQUAL 0)
      string(REPLACE "\n" ";" BFARB_ORT_PROBE "${BFARB_ORT_PROBE}")
      list(GET BFARB_ORT_PROBE 0 BFARB_ORT_LIBRARY)
      list(GET BFARB_ORT_PROBE 1 BFARB_ORT_VERSION)
      message(STATUS "BFA Remote Bridge: ONNX Runtime ${BFARB_ORT_VERSION} at ${BFARB_ORT_LIBRARY}")
      if(NOT BFARB_ORT_VERSION STREQUAL "1.24.4")
        message(WARNING "The runtime tests are written for onnxruntime 1.24.4, not ${BFARB_ORT_VERSION}")
      endif()
      add_test(NAME make_test_models
        COMMAND "${BFARB_PYTHON}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_test_model.py" "${CMAKE_CURRENT_BINARY_DIR}/models")
      set_tests_properties(make_test_models PROPERTIES FIXTURES_SETUP models TIMEOUT 120)
      list(APPEND BFARB_TEST_ENV
        "BFARB_ORT_LIBRARY=${BFARB_ORT_LIBRARY}"
        "BFARB_PYTHON=${BFARB_PYTHON}"
        "BFARB_SERVER=${CMAKE_CURRENT_SOURCE_DIR}/tools/reference_server.py"
        "BFARB_MODELS=${CMAKE_CURRENT_BINARY_DIR}/models")
      add_test(NAME reference_server_tests
        COMMAND "${BFARB_PYTHON}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_reference_server.py" "${CMAKE_CURRENT_BINARY_DIR}/models")
      set_tests_properties(reference_server_tests PROPERTIES FIXTURES_REQUIRED models TIMEOUT 300)
    else()
      message(WARNING "BFARB_PYTHON can't import onnxruntime: the runtime tests are skipped")
    endif()
  endif()
  add_test(NAME core_tests COMMAND bfarb_tests)
  set_tests_properties(core_tests PROPERTIES ENVIRONMENT "${BFARB_TEST_ENV}" TIMEOUT 600)
  if(BFARB_ORT_LIBRARY)
    set_tests_properties(core_tests PROPERTIES FIXTURES_REQUIRED models)
  endif()
endif()

# ---------------------------------------------------------------------------- the SKSE plugin (Windows)
if(BFARB_PLUGIN_DLL)
  if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "BFARemoteBridge.dll builds on Windows with MSVC: use the \"windows\" preset.")
  endif()
  find_package(CommonLibSSE CONFIG REQUIRED)
  find_package(spdlog CONFIG REQUIRED)
  # Address Library, no game structures used: SE 1.5.97 and AE 1.6.x (1.6.1170 among them) alike; VR is refused at load.
  add_commonlibsse_plugin(BFARemoteBridge
    NAME BFARemoteBridge
    AUTHOR "Pt0l3my"
    VERSION ${PROJECT_VERSION}
    SOURCES
      src/plugin/Hook.cpp
      src/plugin/Main.cpp
      src/plugin/SkyrimNet.cpp
      src/plugin/WinsockTransport.cpp)
  target_compile_definitions(BFARemoteBridge PRIVATE _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(BFARemoteBridge PRIVATE /Zc:preprocessor /EHsc)
  target_precompile_headers(BFARemoteBridge PRIVATE src/plugin/PCH.h)
  target_include_directories(BFARemoteBridge PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(BFARemoteBridge PRIVATE bfarb_core bfarb_options spdlog::spdlog ws2_32)
endif()
