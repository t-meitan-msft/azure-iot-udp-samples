# Copyright (c) Microsoft Corporation. All rights reserved.
# SPDX-License-Identifier: MIT

cmake_minimum_required (VERSION 3.10)

PROJECT(azure-iot-udp-samples)

add_subdirectory(${PROJECT_SOURCE_DIR}/lib/azure-sdk-for-c)
add_subdirectory(${PROJECT_SOURCE_DIR}/lib/paho.mqtt-sn.embedded-c/MQTTSNPacket/src)

add_executable(sample_telemetry ${PROJECT_SOURCE_DIR}/src/paho_iot_hub_telemetry_example.c ${PROJECT_SOURCE_DIR}/src/transport.c)

link_directories(${PROJECT_SOURCE_DIR}/lib)

target_link_libraries(sample_telemetry PUBLIC az_iot_hub MQTTSNPacketClient)

target_include_directories(sample_telemetry PUBLIC
                          "${PROJECT_SOURCE_DIR}/lib/azure-sdk-for-c/sdk/inc"
                          "${PROJECT_SOURCE_DIR}/lib/paho.mqtt-sn.embedded-c/MQTTSNPacket"
                          )

