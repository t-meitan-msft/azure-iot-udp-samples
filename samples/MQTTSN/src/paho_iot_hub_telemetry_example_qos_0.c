// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

#include "azure/iot/az_iot_hub_client.h"
#include "src/MQTTSNPacket.h"
#include "transport.h"

// DO NOT MODIFY: Device ID Environment Variable Name
#define ENV_DEVICE_ID "AZ_IOT_DEVICE_ID"

// DO NOT MODIFY: IoT Hub Hostname Environment Variable Name
#define ENV_IOT_HUB_HOSTNAME "AZ_IOT_HUB_HOSTNAME"

#define QOS 0 // or 1
#define TELEMETRY_SEND_INTERVAL 1 // seconds
#define NUMBER_OF_MESSAGES 5
#define TELEMETRY_PAYLOAD \
  "{\"d\":{\"myName\":\"IoT mbed\",\"accelX\":12,\"accelY\":4,\"accelZ\":12,\"temp\":18}}"

static char topicname[128];
static char device_id[64];
static char iot_hub_hostname[128];
static az_iot_hub_client client;

char* const default_gateway_address = "127.0.0.1";
const int default_gateway_port = 10000; // use unicast port if sending a unicast packet

static void sleep_seconds(uint32_t seconds)
{
#ifdef _WIN32
  Sleep((DWORD)seconds * 1000);
#else
  sleep(seconds);
#endif
}

// Read OS environment variables using stdlib function
static az_result read_configuration_entry(
    const char* name,
    const char* env_name,
    char* default_value,
    bool hide_value,
    az_span buffer,
    az_span* out_value)
{
  printf("%s = ", name);
  char* env = getenv(env_name);

  if (env != NULL)
  {
    printf("%s\r\n", hide_value ? "***" : env);
    az_span env_span = az_span_from_str(env);
    AZ_RETURN_IF_NOT_ENOUGH_SIZE(buffer, az_span_size(env_span));
    az_span_copy(buffer, env_span);
    *out_value = az_span_slice(buffer, 0, az_span_size(env_span));
  }
  else if (default_value != NULL)
  {
    printf("%s\r\n", default_value);
    az_span default_span = az_span_from_str(default_value);
    AZ_RETURN_IF_NOT_ENOUGH_SIZE(buffer, az_span_size(default_span));
    az_span_copy(buffer, default_span);
    *out_value = az_span_slice(buffer, 0, az_span_size(default_span));
  }
  else
  {
    printf("(missing) Please set the %s environment variable.\r\n", env_name);
    return AZ_ERROR_ARG;
  }

  return AZ_OK;
}

static az_result read_configuration_and_init_client()
{
  az_span device_id_span = AZ_SPAN_FROM_BUFFER(device_id);
  AZ_RETURN_IF_FAILED(read_configuration_entry(
      ENV_DEVICE_ID, ENV_DEVICE_ID, "", false, device_id_span, &device_id_span));

  az_span iot_hub_hostname_span = AZ_SPAN_FROM_BUFFER(iot_hub_hostname);
  AZ_RETURN_IF_FAILED(read_configuration_entry(
      ENV_IOT_HUB_HOSTNAME,
      ENV_IOT_HUB_HOSTNAME,
      "",
      false,
      iot_hub_hostname_span,
      &iot_hub_hostname_span));

  // Initialize the hub client with the hub host endpoint and the default connection options
  AZ_RETURN_IF_FAILED(az_iot_hub_client_init(
      &client,
      az_span_slice(iot_hub_hostname_span, 0, (int32_t)strlen(iot_hub_hostname)),
      az_span_slice(device_id_span, 0, (int32_t)strlen(device_id)),
      NULL));

  return AZ_OK;
}

int main(int argc, char** argv)
{
  int rc = 0;
  int udp_socket;
  unsigned char buf[500];
  int buflen = sizeof(buf);
  MQTTSN_topicid topic;
  MQTTSNString topicstr;
  int len = 0;
  int retained = 0;
  short packetid = 1;

  // Read for optional destination address and port
  char* host = argc > 1 ? argv[1] : default_gateway_address;
  int port = argc > 2 ? atoi(argv[2]) : default_gateway_port;

  MQTTSNPacket_connectData options = MQTTSNPacket_connectData_initializer;
  unsigned short topicid;

  // Create a unicast UDP socket
  udp_socket = transport_open();
  if (udp_socket < 0)
    return udp_socket;

  printf("Connecting to host '%s', port '%d'\r\n", host, port);

  // Read in the necessary environment variables and initialize the az_iot_hub_client
  if (az_failed(rc = read_configuration_and_init_client()))
  {
    printf("Failed to read configuration from environment variables, return code %d\r\n", rc);
    return rc;
  }

  options.clientID.cstring = device_id;

  // Get topic name that the IoT Hub is subscribed to
  if (az_failed(
          rc = az_iot_hub_client_telemetry_get_publish_topic(
              &client, NULL, topicname, sizeof(topicname), NULL)))
  {
    printf("Failed to get publish topic, return code %d\r\n", rc);
    return rc;
  }

  // CONNECT to MQTTSN Gateway
  len = MQTTSNSerialize_connect(buf, buflen, &options);

  if (az_failed(rc = transport_sendPacketBuffer(host, port, buf, len)))
  {
    printf("Failed to send Connect packet to the Gateway, return code %d\r\n", rc);
    return rc;
  }

  // Wait for CONNACK from the MQTTSN Gateway
  if (MQTTSNPacket_read(buf, buflen, transport_getdata) == MQTTSN_CONNACK)
  {
    int connack_rc = -1;

    if (MQTTSNDeserialize_connack(&connack_rc, buf, buflen) != 1 || connack_rc != 0)
    {
      printf("Failed to receive Connect ACK packet, return code %d\r\nExiting...\r\n", connack_rc);
      goto exit;
    }
    else
      printf("CONNACK rc %d\r\n", connack_rc);
  }
  else
  {
    printf("Failed to connect to the Gateway\r\nExiting...\r\n");
    goto exit;
  }

  // REGISTER topic name with the MQTTSN Gateway
  printf("Registering topic %s\r\n", topicname);
  topicstr.cstring = topicname;
  topicstr.lenstring.len = strlen(topicname);
  len = MQTTSNSerialize_register(buf, buflen, 0, packetid, &topicstr);

  if (az_failed(rc = transport_sendPacketBuffer(host, port, buf, len)))
  {
    printf("Failed to send Register packet to the Gateway, return code %d\r\n", rc);
    return rc;
  }

  // Wait for REGACK from the MQTTSN Gateway
  if (MQTTSNPacket_read(buf, buflen, transport_getdata) == MQTTSN_REGACK)
  {
    unsigned short submsgid;
    unsigned char returncode;

    rc = MQTTSNDeserialize_regack(&topicid, &submsgid, &returncode, buf, buflen);
    if (returncode != 0)
    {
      printf("Failed to receive Register ACK packet, return code %d\r\nExiting...\r\n", returncode);
      goto exit;
    }
    else
      printf("REGACK topic id %d\r\n", topicid);
  }
  else
  {
    printf("Failed to register topic with the Gateway\r\nExiting...\r\n");
    goto exit;
  }

  // Publish 5 messages
  for (int i = 0; i < NUMBER_OF_MESSAGES; ++i)
  {
    printf("Sending Message %d\r\n", i + 1);
    topic.type = MQTTSN_TOPIC_TYPE_NORMAL;
    topic.data.id = topicid;

    // PUBLISH
    len = MQTTSNSerialize_publish(
        buf,
        buflen,
        0,
        QOS,
        retained,
        packetid + i,
        topic,
        TELEMETRY_PAYLOAD,
        sizeof(TELEMETRY_PAYLOAD));

    if (az_failed(rc = transport_sendPacketBuffer(host, port, buf, len)))
    {
      printf("Failed to publish telemetry message %d, return code %d\r\n", i + 1, rc);
      return rc;
    }

    printf("Published rc %d for publish length %d\r\n", rc, len);

    if (QOS == 1)
    {
      // Wait for PUBACK
      if (MQTTSNPacket_read(buf, buflen, transport_getdata) == MQTTSN_PUBACK)
      {
        unsigned short packet_id, topic_id;
        unsigned char returncode;

        if (MQTTSNDeserialize_puback(&topic_id, &packet_id, &returncode, buf, buflen) != 1
            || returncode != MQTTSN_RC_ACCEPTED)
          printf("Failed to receive Publish ACK packet, return code %d\n", returncode);
        else
          printf("PUBACK received, id %d\n", packet_id);
      }
      else
      {
        printf("Failed to Acknowledge Publish packet\nExiting...\n");
        goto exit;
      }
    }

    sleep_seconds(TELEMETRY_SEND_INTERVAL); // Publish a message every second
  }

  // DISCONNECT the client
  printf("Disconnecting\r\n");
  len = MQTTSNSerialize_disconnect(buf, buflen, 0);

  if (az_failed(rc = transport_sendPacketBuffer(host, port, buf, len)))
  {
    printf("Failed to send Disconnect packet to the Gateway, return code %d\r\n", rc);
    return rc;
  }

  printf("Disconnected.\r\n");

exit:
  transport_close();

  return 0;
}
