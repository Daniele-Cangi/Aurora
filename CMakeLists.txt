cmake_minimum_required(VERSION 3.20)
project(AuroraX LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(BUILD_FIELD          "Enable FIELD_BUILD definitions (current HAL remains experimental)" OFF)
option(USE_SODIUM           "Link against libsodium and enable real Ed25519" OFF)
option(USE_RAPTORQ          "Enable the external/legacy RaptorQ adapter" OFF)
option(RAPTORQ_HEADER_ONLY  "Use a header-only libRaptorQ integration" ON)
option(BUILD_NET_TOOLS      "Build UDP and batch research utilities" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(Threads REQUIRED)

# -------------------------------------------------------------------
# Shared headers/interface target
add_library(aurora_headers INTERFACE)
target_include_directories(aurora_headers INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(BUILD_FIELD)
  target_compile_definitions(aurora_headers INTERFACE FIELD_BUILD)
  message(WARNING
    "BUILD_FIELD is enabled, but the current HAL still contains experimental/mock "
    "device implementations. Do not treat this profile as validated hardware support."
  )
endif()

# -------------------------------------------------------------------
# libsodium: explicit opt-in for real Ed25519
set(AURORA_SODIUM_HINT_DIR "" CACHE PATH "Hint directory containing libsodium include/lib")
set(AURORA_SODIUM_ARCH "auto" CACHE STRING "Sodium architecture hint: auto|win64|win32")

if(USE_SODIUM)
  target_compile_definitions(aurora_headers INTERFACE
    AURORA_USE_REAL_CRYPTO
    PODM_USE_SODIUM
  )

  set(_SODIUM_CANDIDATE_DIRS
    ${AURORA_SODIUM_HINT_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libsodium-win64
    ${CMAKE_CURRENT_SOURCE_DIR}/libsodium-win32
    ${CMAKE_CURRENT_SOURCE_DIR}/libsodium-1.0.18-mingw/libsodium-win64
    ${CMAKE_CURRENT_SOURCE_DIR}/libsodium-1.0.18-mingw/libsodium-win32
  )

  set(_SODIUM_INC_DIRS)
  set(_SODIUM_LIB_DIRS)
  foreach(dir ${_SODIUM_CANDIDATE_DIRS})
    if(dir AND EXISTS ${dir}/include)
      list(APPEND _SODIUM_INC_DIRS ${dir}/include)
    endif()
    if(dir AND EXISTS ${dir}/lib)
      list(APPEND _SODIUM_LIB_DIRS ${dir}/lib)
    endif()
  endforeach()

  if(_SODIUM_INC_DIRS)
    list(REMOVE_DUPLICATES _SODIUM_INC_DIRS)
  endif()
  if(_SODIUM_LIB_DIRS)
    list(REMOVE_DUPLICATES _SODIUM_LIB_DIRS)
  endif()

  find_path(SODIUM_INCLUDE_DIR
    NAMES sodium.h
    PATHS ${_SODIUM_INC_DIRS}
  )
  find_library(SODIUM_LIB
    NAMES sodium libsodium
    PATHS ${_SODIUM_LIB_DIRS}
  )

  if(NOT SODIUM_INCLUDE_DIR OR NOT SODIUM_LIB)
    message(FATAL_ERROR
      "libsodium headers or library were not found. Set USE_SODIUM=OFF for the "
      "insecure research profile, install libsodium, or provide AURORA_SODIUM_HINT_DIR."
    )
  endif()

  add_library(aurora_sodium INTERFACE)
  target_include_directories(aurora_sodium INTERFACE ${SODIUM_INCLUDE_DIR})
  target_link_libraries(aurora_sodium INTERFACE ${SODIUM_LIB})
else()
  message(WARNING
    "USE_SODIUM=OFF: Aurora-X will use deterministic placeholder signatures. "
    "This build is INSECURE_TEST and must not be described as using real Ed25519."
  )
endif()

# -------------------------------------------------------------------
# RaptorQ: explicit optional/legacy integration
set(RAPTORQ_ROOT "" CACHE PATH "Root of a compatible RaptorQ SDK (include/lib or source tree)")
set(AURORA_FEC_SOURCES)

if(USE_RAPTORQ)
  set(_AURORA_RAPTORQ_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/fec/AuroraRaptorQ.cpp)
  set(_AURORA_RAPTORQ_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/src/fec/AuroraRaptorQ.hpp)

  if(NOT EXISTS ${_AURORA_RAPTORQ_SOURCE} OR NOT EXISTS ${_AURORA_RAPTORQ_HEADER})
    message(FATAL_ERROR
      "USE_RAPTORQ=ON, but the AuroraRaptorQ adapter sources are not present at "
      "src/fec/AuroraRaptorQ.cpp/.hpp. Disable USE_RAPTORQ or restore a compatible adapter."
    )
  endif()

  if(NOT RAPTORQ_ROOT AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/extern/libRaptorQ)
    set(RAPTORQ_ROOT
      ${CMAKE_CURRENT_SOURCE_DIR}/extern/libRaptorQ
      CACHE PATH "Root of a compatible RaptorQ SDK" FORCE
    )
  endif()

  if(NOT RAPTORQ_ROOT)
    message(FATAL_ERROR
      "USE_RAPTORQ=ON requires RAPTORQ_ROOT pointing to a compatible implementation."
    )
  endif()

  target_compile_definitions(aurora_headers INTERFACE
    AURORA_USE_LIBRAPTORQ=1
    AURORA_USE_RAPTORQ_REAL
  )

  add_library(aurora_raptorq INTERFACE)
  target_include_directories(aurora_raptorq INTERFACE
    ${RAPTORQ_ROOT}/src
    ${RAPTORQ_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/eigen
  )

  if(NOT RAPTORQ_HEADER_ONLY)
    find_library(RAPTORQ_LIB
      NAMES RaptorQ raptorq
      PATHS ${RAPTORQ_ROOT}/lib
    )
    if(NOT RAPTORQ_LIB)
      message(FATAL_ERROR
        "A compiled RaptorQ library was not found under RAPTORQ_ROOT/lib. "
        "Enable RAPTORQ_HEADER_ONLY or provide the library."
      )
    endif()
    target_link_libraries(aurora_raptorq INTERFACE ${RAPTORQ_LIB})
  endif()

  list(APPEND AURORA_FEC_SOURCES ${_AURORA_RAPTORQ_SOURCE})
endif()

# -------------------------------------------------------------------
# Helper for common dependencies
function(aurora_link_common target)
  target_link_libraries(${target} PRIVATE aurora_headers Threads::Threads)
  if(TARGET aurora_sodium)
    target_link_libraries(${target} PRIVATE aurora_sodium)
  endif()
  if(TARGET aurora_raptorq)
    target_link_libraries(${target} PRIVATE aurora_raptorq)
  endif()
endfunction()

# -------------------------------------------------------------------
# Main executable
add_executable(aurora_x aurora_x.cpp ${AURORA_FEC_SOURCES})
aurora_link_common(aurora_x)

# -------------------------------------------------------------------
# Tests
if(BUILD_TESTING)
  add_executable(test_aurora_organism tests/test_aurora_organism.cpp)
  aurora_link_common(test_aurora_organism)
  add_test(NAME aurora.generation_lifecycle COMMAND test_aurora_organism)

  add_executable(test_transport_contract tests/test_transport_contract.cpp)
  aurora_link_common(test_transport_contract)
  add_test(NAME aurora.transport_contract COMMAND test_transport_contract)

  add_executable(test_safety_envelope tests/test_safety_envelope.cpp)
  aurora_link_common(test_safety_envelope)
  add_test(NAME aurora.safety_envelope COMMAND test_safety_envelope)

  # Keep standard assert() checks active even in optimized test configurations.
  if(MSVC)
    target_compile_options(test_aurora_organism PRIVATE /UNDEBUG)
    target_compile_options(test_transport_contract PRIVATE /UNDEBUG)
    target_compile_options(test_safety_envelope PRIVATE /UNDEBUG)
  else()
    target_compile_options(test_aurora_organism PRIVATE -UNDEBUG)
    target_compile_options(test_transport_contract PRIVATE -UNDEBUG)
    target_compile_options(test_safety_envelope PRIVATE -UNDEBUG)
  endif()

  if(USE_RAPTORQ)
    add_executable(
      test_raptorq_adapter
      tests/test_raptorq_adapter.cpp
      ${AURORA_FEC_SOURCES}
    )
    aurora_link_common(test_raptorq_adapter)
    add_test(NAME aurora.raptorq_adapter COMMAND test_raptorq_adapter)
  endif()
endif()

# -------------------------------------------------------------------
# Optional network and batch research tools
if(BUILD_NET_TOOLS)
  set(AURORA_NET_TOOLS
    aurora_batch_test
    aurora_deadline_sweep
    aurora_selector_test
    aurora_udp_demo
    aurora_udp_extreme_bidir_test
    aurora_udp_extreme_echo_test
    aurora_udp_extreme_seq_test
    aurora_udp_extreme_test
    aurora_udp_token_seq_test
    aurora_unit_dedup_test
  )

  foreach(tool ${AURORA_NET_TOOLS})
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/${tool}.cpp")
    if(EXISTS ${src})
      add_executable(${tool} ${src} ${AURORA_FEC_SOURCES})
      aurora_link_common(${tool})
      if(WIN32)
        target_link_libraries(${tool} PRIVATE ws2_32)
      endif()
    endif()
  endforeach()

  foreach(tool
      aurora_unit_dedup_test
      aurora_deadline_sweep
      aurora_selector_test)
    if(TARGET ${tool})
      target_compile_definitions(${tool} PRIVATE AURORA_NO_MAIN)
    endif()
  endforeach()
endif()

# -------------------------------------------------------------------
# Warning levels
set(_AURORA_ALL_TARGETS
  aurora_x
  test_raptorq_adapter
  test_aurora_organism
  test_transport_contract
  test_safety_envelope
  aurora_batch_test
  aurora_deadline_sweep
  aurora_selector_test
  aurora_udp_demo
  aurora_udp_extreme_bidir_test
  aurora_udp_extreme_echo_test
  aurora_udp_extreme_seq_test
  aurora_udp_extreme_test
  aurora_udp_token_seq_test
  aurora_unit_dedup_test
)

foreach(tgt ${_AURORA_ALL_TARGETS})
  if(TARGET ${tgt})
    if(MSVC)
      target_compile_options(${tgt} PRIVATE /W4)
    else()
      target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
  endif()
endforeach()

message(STATUS "")
message(STATUS "==== Aurora-X Build Configuration ====")
message(STATUS "  C++ standard      : ${CMAKE_CXX_STANDARD}")
message(STATUS "  BUILD_FIELD       : ${BUILD_FIELD}")
message(STATUS "  USE_SODIUM        : ${USE_SODIUM}")
message(STATUS "  USE_RAPTORQ       : ${USE_RAPTORQ}")
message(STATUS "  RAPTORQ_HEADER    : ${RAPTORQ_HEADER_ONLY}")
message(STATUS "  BUILD_NET_TOOLS   : ${BUILD_NET_TOOLS}")
message(STATUS "  BUILD_TESTING     : ${BUILD_TESTING}")
message(STATUS "  Output directory  : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "======================================")
