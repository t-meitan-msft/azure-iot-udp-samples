# Copyright (c) Microsoft Corporation. All rights reserved.
# SPDX-License-Identifier: MIT

cmake_minimum_required (VERSION 3.10)

PROJECT(azure-iot-udp-samples)

add_subdirectory(${PROJECT_SOURCE_DIR}/lib/azure-sdk-for-c)
add_subdirectory(${PROJECT_SOURCE_DIR}/lib/paho.mqtt-sn.embedded-c/MQTTSNPacket/src)
add_subdirectory(${PROJECT_SOURCE_DIR}/lib/paho.mqtt-sn.embedded-c/MQTTSNPacket/samples)

add_executable(azure-iot-udp-samples ${PROJECT_SOURCE_DIR}/src/paho_iot_hub_telemetry_example.c)

link_directories(${PROJECT_SOURCE_DIR}/lib)

target_link_libraries(azure-iot-udp-samples az_iot_common az_core az_iot_hub MQTTSNPacketClient transport)

target_include_directories(azure-iot-udp-samples PUBLIC
                          ${PROJECT_SOURCE_DIR}/lib/azure-sdk-for-c
                          "${PROJECT_SOURCE_DIR}/lib/paho.mqtt-sn.embedded-c/MQTTSNPacket/src"
                          "${PROJECT_SOURCE_DIR}/lib/paho.mqtt-sn.embedded-c/MQTTSNPacket/samples"
                          )
