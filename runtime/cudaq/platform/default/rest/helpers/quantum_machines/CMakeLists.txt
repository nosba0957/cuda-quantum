# ============================================================================ #
# Copyright (c) 2022 - 2026 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #
target_sources(cudaq-rest-qpu PRIVATE QuantumMachinesServerHelper.cpp)
add_target_config(quantum_machines)

add_library(cudaq-serverhelper-quantum_machines SHARED QuantumMachinesServerHelper.cpp )
target_link_libraries(cudaq-serverhelper-quantum_machines
  PUBLIC
    cudaq-common
    cudaq-logger
)
install(TARGETS cudaq-serverhelper-quantum_machines DESTINATION lib)

# The OPX1000 executor talks gRPC to a QOP over libcurl HTTP/2 and needs
# protobuf-lite plus protoc. Both are optional: without them the target keeps
# only the cloud REST helper above.
find_package(Protobuf QUIET)
if (NOT Protobuf_FOUND OR NOT TARGET CURL::libcurl)
  message(STATUS "quantum_machines: no protobuf or curl, skipping the OPX gRPC executor")
  return()
endif()

set(QM_PB_DIR ${CMAKE_CURRENT_BINARY_DIR}/qm_pb)
add_custom_command(
  OUTPUT ${QM_PB_DIR}/qm_min.pb.cc ${QM_PB_DIR}/qm_min.pb.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${QM_PB_DIR}
  COMMAND ${Protobuf_PROTOC_EXECUTABLE} --cpp_out=${QM_PB_DIR}
          -I${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/qm_min.proto
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/qm_min.proto
  COMMENT "Generating qm_min.pb.cc")

# An OBJECT library, not a static one: CUDAQ_REGISTER_TYPE's registration object
# is referenced by nothing and an archive member would be dropped.
add_library(cudaq-qm-grpc OBJECT
  GrpcCurl.cpp
  QuaResults.cpp
  QMExecutor.cpp
  ${QM_PB_DIR}/qm_min.pb.cc)
set_target_properties(cudaq-qm-grpc PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(cudaq-qm-grpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${QM_PB_DIR})
target_link_libraries(cudaq-qm-grpc PUBLIC
  cudaq-common
  cudaq-logger
  protobuf::libprotobuf-lite
  CURL::libcurl)
# protoc output is not ours to keep warning-free.
set_source_files_properties(${QM_PB_DIR}/qm_min.pb.cc PROPERTIES COMPILE_OPTIONS -w)

target_link_libraries(cudaq-rest-qpu PRIVATE cudaq-qm-grpc)
target_link_libraries(cudaq-serverhelper-quantum_machines PRIVATE cudaq-qm-grpc)

# The OPNIC transport, through QM's opnic SDK (CPU streams, so no CUDA). Its own
# plugin, loaded by QMExecutor only when OPNIC is selected, so nothing else
# needs libopnic at run time. Optional, but loud when missing.
#
# The SDK's headers keep `static_assert(false)` in its unused CUDA branches,
# which GCC accepts only from 13 on, so the plugin is compiled by a GCC 13+
# found here rather than by the project's compiler. Its interface is one
# extern "C" function and a class of std::vectors, so the mix is safe.
find_path(OPNIC_INCLUDE_DIR opnic/opnic.hpp)
find_library(OPNIC_LIBRARY opnic)
find_program(OPNIC_CXX NAMES g++-14 g++-13 g++)
if (OPNIC_CXX)
  execute_process(COMMAND ${OPNIC_CXX} -dumpversion
                  OUTPUT_VARIABLE OPNIC_CXX_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
if (OPNIC_INCLUDE_DIR AND OPNIC_LIBRARY AND OPNIC_CXX_VERSION VERSION_GREATER_EQUAL 13)
  set(OPNIC_PLUGIN ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libcudaq-qm-opnic.so)
  # /usr/include is already a system directory; naming it again breaks the
  # standard library's #include_next.
  set(OPNIC_INCLUDE_FLAG "")
  if (NOT OPNIC_INCLUDE_DIR STREQUAL "/usr/include")
    set(OPNIC_INCLUDE_FLAG -isystem ${OPNIC_INCLUDE_DIR})
  endif()
  add_custom_command(
    OUTPUT ${OPNIC_PLUGIN}
    COMMAND ${OPNIC_CXX} -std=c++20 -O2 -fPIC -shared -Wall -Werror
            -DQM_NO_CUDA -I${CMAKE_CURRENT_SOURCE_DIR} ${OPNIC_INCLUDE_FLAG}
            ${CMAKE_CURRENT_SOURCE_DIR}/OpnicLink.cpp -o ${OPNIC_PLUGIN}
            ${OPNIC_LIBRARY} -pthread
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/OpnicLink.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/OpnicLink.h
    COMMENT "Building the OPNIC plugin with ${OPNIC_CXX}")
  add_custom_target(cudaq-qm-opnic ALL DEPENDS ${OPNIC_PLUGIN})
  install(FILES ${OPNIC_PLUGIN} DESTINATION lib)
  message(STATUS "quantum_machines: OPNIC transport enabled (${OPNIC_LIBRARY}, "
                 "${OPNIC_CXX} ${OPNIC_CXX_VERSION})")
else()
  message(WARNING "quantum_machines: the OPNIC transport (--qm-transport opnic) "
                  "is not built: it needs QM's opnic SDK and GCC 13+ "
                  "(sdk: ${OPNIC_INCLUDE_DIR} ${OPNIC_LIBRARY}, "
                  "compiler: ${OPNIC_CXX} ${OPNIC_CXX_VERSION})")
endif()
