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
