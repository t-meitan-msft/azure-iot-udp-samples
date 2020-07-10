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

#include "MQTTSNPacket.h"
#include "azure/iot/az_iot_hub_client.h"
#include "transport.h"

// DO NOT MODIFY: Device ID Environment Variable Name
#define ENV_DEVICE_ID "AZ_IOT_DEVICE_ID"

// DO NOT MODIFY: IoT Hub Hostname Environment Variable Name
#define ENV_IOT_HUB_HOSTNAME "AZ_IOT_HUB_HOSTNAME"

// DO NOT MODIFY: MQTTSN Gateway IP Address Environment Variable Name
#define ENV_MQTTSN_GATEWAY_ADDRESS "MQTTSN_GATEWAY_ADDRESS"

// DO NOT MODIFY: MQTTSN Gateway gateway_port Environment Variable Name
#define ENV_MQTTSN_GATEWAY_PORT "MQTTSN_GATEWAY_PORT"

#define DEFAULT_GATEWAY_ADDRESS "127.0.0.1"
#define DEFAULT_GATEWAY_PORT "10000"
#define TELEMETRY_SEND_INTERVAL 1 // seconds
#define NUMBER_OF_MESSAGES 5
#define TELEMETRY_PAYLOAD \
  "{\"d\":{\"myName\":\"IoT mbed\",\"accelX\":12,\"accelY\":4,\"accelZ\":12,\"temp\":18}}"

#ifdef AZ_TELEMETRY_QOS_0
#undef ENABLE_PUBACK // default to qos 1 and enable puback if QoS 1
#else
#define ENABLE_PUBACK
#endif

static char topic_name[128];
static unsigned char scratch_buffer[128];

typedef struct iothub_client_context_tag
{
  char gateway_address[16];
  int gateway_port;
  char device_id[64];
  unsigned short telemetry_topic_id;
  az_iot_hub_client client;
  short packet_id;
} iothub_client_context;

static void sleep_seconds(uint32_t seconds)
{
#ifdef _WIN32
  Sleep((DWORD)seconds * 1000);
#else
  sleep(seconds);
#endif
}

/*
 * Read OS environment variables using stdlib function
 */
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

/*
 * Read configurations and initialize Azure IoT Hub Client
 */
static az_result read_configuration_and_init_client(
    az_iot_hub_client* client,
    char* device_id, int device_id_len, char* gateway_address, int gateway_address_len, int* gateway_port)
{
  // Read Device ID configuration
  az_span device_id_span = az_span_init(device_id, device_id_len);
  AZ_RETURN_IF_FAILED(read_configuration_entry(
      ENV_DEVICE_ID, ENV_DEVICE_ID, "", false, device_id_span, &device_id_span));

  // Read Gateway IP address configuration
  az_span gateway_address_span = az_span_init(gateway_address, gateway_address_len);
  AZ_RETURN_IF_FAILED(read_configuration_entry(
      ENV_MQTTSN_GATEWAY_ADDRESS,
      ENV_MQTTSN_GATEWAY_ADDRESS,
      DEFAULT_GATEWAY_ADDRESS,
      false,
      gateway_address_span,
      &gateway_address_span));

  // Read IoT Hub Hostname configuration
  az_span iot_hub_hostname_span = AZ_SPAN_FROM_BUFFER(scratch_buffer);
  AZ_RETURN_IF_FAILED(read_configuration_entry(
      ENV_IOT_HUB_HOSTNAME,
      ENV_IOT_HUB_HOSTNAME,
      "",
      false,
      iot_hub_hostname_span,
      &iot_hub_hostname_span));  

  // Initialize the hub client with the hub host endpoint and the default connection options
  AZ_RETURN_IF_FAILED(az_iot_hub_client_init(
      client,
      az_span_slice(iot_hub_hostname_span, 0, (int32_t)strlen(scratch_buffer)),
      az_span_slice(device_id_span, 0, (int32_t)strlen(device_id)),
      NULL));

  // Read Gateway port number
  az_span gateway_port_span = AZ_SPAN_FROM_BUFFER(scratch_buffer);
  AZ_RETURN_IF_FAILED(read_configuration_entry(
      ENV_MQTTSN_GATEWAY_PORT,
      ENV_MQTTSN_GATEWAY_PORT,
      DEFAULT_GATEWAY_PORT,
      false,
      gateway_port_span,
      &gateway_port_span));

  AZ_RETURN_IF_FAILED(
      az_span_atou32(gateway_port_span, gateway_port));

  return AZ_OK;
}

/*
 * Read the Environment Variables and and initialize the az_iot_hub_client
 */
static int init_client_context(iothub_client_context* ctx)
{
  int rc;
  ctx->packet_id = 0;

  if (az_failed(rc = read_configuration_and_init_client(&ctx->client, ctx->device_id, sizeof(ctx->device_id), ctx->gateway_address, sizeof(ctx->gateway_address), &ctx->gateway_port)))
  {
    printf("Failed to read configuration from environment variables, return code %d\r\n", rc);
    return rc;
  }

  return 0;
}

/*
 * First 10 attempts: try within 3 seconds
 * Next 10 attempts: retry after every 1 minute
 * After 20 attempts: retry every 10 minutes
 */
int get_connection_timeout(int attempt_number)
{
  return (attempt_number < 10) ? 3 : (attempt_number < 20) ? 60 : 600;
}

/*
 * 1. Create CONNECT packet
 * 2. Send CONNECT packet to the MQTTSN Gateway
 */
static int send_connect(iothub_client_context* ctx, MQTTSNPacket_connectData* options)
{
  int rc;
  int len;

  // 1. Create CONNECT packet
  if ((len = MQTTSNSerialize_connect(scratch_buffer, sizeof(scratch_buffer), options)) == 0)
  {
    printf("Failed to serialize CONNECT packet, return code %d\r\n", len);
    return -1;
  }

  // 2. Send CONNECT packet to the MQTTSN Gateway
  if ((rc = transport_sendPacketBuffer(ctx->gateway_address, ctx->gateway_port, scratch_buffer, len)) != 0)
  {
    printf("Failed to send CONNECT packet to the Gateway, return code %d\r\n", rc);
    return rc;
  }

  return 0;
}

/*
 * Wait for CONNACK packet from the MQTTSN Gateway
 */
static int receive_connack()
{
  int rc;

  if (MQTTSNPacket_read(scratch_buffer, sizeof(scratch_buffer), transport_getdata)
      == MQTTSN_CONNACK)
  {
    if (MQTTSNDeserialize_connack(&rc, scratch_buffer, sizeof(scratch_buffer)) != 1 || rc != 0)
    {
      printf("Failed to deserialize CONNACK packet, return code %d\r\n", rc);
      rc = -1;
    }
    else
    {
      printf("Successfully received CONNACK\r\n");
    }

    return rc;
  }
  else
  {
    printf("Failed to receive CONNACK packet\r\n");
    return -1;
  }

  return 0;
}

/*
 * 1. Open transport
 * 2. Attempt connecting to Gateway with some backoff
 */
static int connect_device(iothub_client_context* ctx)
{
  int rc;
  int len;
  int retry_attempt = 0;

  // 1. Open unicast UDP transport
  if ((rc = transport_open()) < 0)
  {
    printf("Failed to open transport, return code %d\r\n", rc);
    return rc;
  }

  // 2. Attempt connecting to Gateway with some backoff
  MQTTSNPacket_connectData options = MQTTSNPacket_connectData_initializer;
  options.clientID.cstring = ctx->device_id;

  do
  {
    if ((rc = send_connect(ctx, &options)) != 0)
    {
      printf("Failed to send CONNECT packet to Gateway for device ID = %s, return code = %d\r\n", ctx->device_id, rc);
    }
    else if ((rc = receive_connack()) != 0)
    {
      printf("Failed to receive CONNACK packet from Gateway for device ID = %s, return code = %d\r\n", ctx->device_id, rc);
    }

    if (rc != 0)
    {
      int timeout = get_connection_timeout(++retry_attempt);
      printf("Retry attempt number %d waiting %d\n", retry_attempt, timeout);

      sleep_seconds(timeout);
    }

  } while (rc != 0);

  return 0;
}

/*
 * 1. Create REGISTER packet
 * 2. Send REGISTER packet to the MQTTSN Gateway
 */
static int send_topic_registration(iothub_client_context* ctx, MQTTSNString* topic_str)
{
  int rc;
  int len;

  // 1. Create REGISTER packet (by registering the topic name with the MQTTSN Gateway)
  printf("Registering topic %s\r\n", topic_name);

  topic_str->cstring = topic_name;
  topic_str->lenstring.len = strlen(topic_name);

  if ((len = MQTTSNSerialize_register(
           scratch_buffer, sizeof(scratch_buffer), 0, ctx->packet_id, topic_str))
      == 0)
  {
    printf("Failed to serialize REGISTER packet, return code %d\r\n", len);
    return len;
  }

  // 2. Send REGISTER packet to the MQTTSN Gateway
  if ((rc = transport_sendPacketBuffer(ctx->gateway_address, ctx->gateway_port, scratch_buffer, len)) != 0)
  {
    printf("Failed to send REGISTER packet to the Gateway, return code %d\r\n", rc);
    return rc;
  }

  return 0;
}

/*
 * 1. Wait for REGACK packet from the MQTTSN Gateway
 * 2. Save received topic ID
 */
static int receive_topic_registration_ack(iothub_client_context* ctx, unsigned short* topic_id)
{
  int len;

  // 1. Wait for REGACK packet from the MQTTSN Gateway
  if (MQTTSNPacket_read(scratch_buffer, sizeof(scratch_buffer), transport_getdata) == MQTTSN_REGACK)
  {
    unsigned short sub_msg_id;
    unsigned char return_code;

    // 2. Save received topic ID
    if (MQTTSNDeserialize_regack(
            topic_id, &sub_msg_id, &return_code, scratch_buffer, sizeof(scratch_buffer))
            != 1
        || return_code != 0)
    {
      printf("Failed to deserialize REGACK packet, return code %d\r\n", return_code);
      return -1;
    }
    else
    {
      printf("Successfully received REGACK for topic id = %d \r\n", *topic_id);
    }
  }
  else
  {
    printf("Failed to receive REGACK\r\n");
    return -1;
  }

  return 0;
}

/*
 * 1. Send registration for long topic name to Gateway
 * 2. Receive registration ack
 * 3. Retry with backoff if fail to register
 */
static int register_topic(
    iothub_client_context* ctx,
    char* topic_name,
    int topic_len,
    unsigned short* topic_id)
{
  int rc;
  int len;
  int retry_attempt = 0;

  MQTTSNString topic_str;
  topic_str.cstring = topic_name;
  topic_str.lenstring.len = topic_len;

  do
  {
    if ((rc = send_topic_registration(ctx, &topic_str)) != 0)
    {
      printf("Failed to send REGISTER packet with Gateway the topic name = %s, return code = %d\r\n", topic_name, rc);
    }
    else if ((rc = receive_topic_registration_ack(ctx, topic_id)) != 0)
    {
      printf("Failed to receive REGACK packet from Gateway for topic name = %s, return code = %d\r\n", topic_name, rc);
    }

    if (rc != 0)
    {
      int timeout = get_connection_timeout(++retry_attempt);
      printf("Retry attempt number %d waiting %d\n", retry_attempt, timeout);

      sleep_seconds(timeout);
    }

  } while (rc != 0);

  return 0;
}

/*
 * 1. Create PUBLISH packet
 * 2. Send PUBLISH packet to the MQTTSN Gateway
 */
static int send_publish(iothub_client_context* ctx, unsigned char* payload, int payload_size)
{
  int rc;
  int len;
  int retained = 0;
  MQTTSN_topicid topic;
  int qos;

#ifdef ENABLE_PUBACK
  qos = 1;
#else
  qos = 0;
#endif

  topic.type = MQTTSN_TOPIC_TYPE_NORMAL;
  topic.data.id = ctx->telemetry_topic_id;

  // 1. Create PUBLISH packet
  if ((len = MQTTSNSerialize_publish(
           scratch_buffer,
           sizeof(scratch_buffer),
           0,
           qos,
           retained,
           ctx->packet_id,
           topic,
           payload,
           payload_size))
      == 0)
  {
    printf("Failed to serialize PUBLISH packet, return code %d\r\n", len);
    return len;
  }

  // 2. Send PUBLISH packet to the MQTTSN Gateway
  if ((rc = transport_sendPacketBuffer(ctx->gateway_address, ctx->gateway_port, scratch_buffer, len)) != 0)
  {
    printf(
        "Failed to send PUBLISH packet with packet id = %d, return code %d\r\n",
        ctx->packet_id,
        rc);

    return rc;
  }

  printf("Successfully published telemetry payload of length = %d\r\n", len);

  return 0;
}

#ifdef ENABLE_PUBACK
/*
 * 1. Wait for PUBACK packet from the MQTTSN Gateway
 * 2. Validate packet ID
 */
static int receive_puback(iothub_client_context* ctx, unsigned short* packet_id)
{
  unsigned short packet_id_received;

  // 1. Wait for PUBACK packet from the MQTTSN Gateway
  if (MQTTSNPacket_read(scratch_buffer, sizeof(scratch_buffer), transport_getdata) == MQTTSN_PUBACK)
  {
    unsigned short topic_id;
    unsigned char return_code;

    if (MQTTSNDeserialize_puback(
            &topic_id, &packet_id_received, &return_code, scratch_buffer, sizeof(scratch_buffer))
            != 1
        || return_code != MQTTSN_RC_ACCEPTED)
    {
      printf(
          "Failed to deserialize PUBACK packet ID = %hu, return code %d\r\n",
          packet_id_received,
          return_code);
      return -1;
    }
    else
    {
      printf("Successfully received PUBACK for packet ID = %hu\r\n", packet_id_received);
    }
  }
  else
  {
    printf("Failed to receive PUBACK packet\r\n");
    return -1;
  }

  // 2. Validate packet ID
  if (*packet_id != packet_id_received)
  {
    printf("Failed to receive PUBACK packet for the requested packet ID = %hu\r\n", *packet_id);
    return -1;
  }

  return 0;
}
#endif

/*
 * 1. Get new message ID
 * 2. Publish message
 * 3. Wait for puback if enabled (QoS 1)
 */
static int send_telemetry(iothub_client_context* ctx, unsigned char* payload, int payload_size)
{
  int rc;
  int len;

  // 1. Get new message ID
  ctx->packet_id++;

  // 2. Publish message
  if ((rc = send_publish(ctx, payload, payload_size)) != 0)
  {
    printf(
        "Failed to send PUBLISH packet for payload = %s, payload size = %d\r\n",
        payload,
        payload_size);
    return rc;
  }

  // 3. Wait for puback if enabled (QoS 1)
#ifdef ENABLE_PUBACK
  else if ((rc = receive_puback(ctx, &ctx->packet_id)) != 0)
  {
    printf(
        "Failed to receive PUBACK packet for payload = %s, payload size = %d\r\n",
        payload,
        payload_size);
    return rc;
  }
#endif

  return 0;
}

/*
 * 1. Get telemetry topic name from the Azure IoT Hub
 * 2. Register the topic with the Gateway to get topic ID
 * 3. Send sample telemetry messages
 */
static int send_sample_telemetry_messages(iothub_client_context* ctx)
{
  int rc;
  int len;
  int retry_attempt = 0;

  // 1. Get telemetry topic name from the Azure IoT Hub
  if (az_failed(
          rc = az_iot_hub_client_telemetry_get_publish_topic(
              &ctx->client, NULL, topic_name, sizeof(topic_name), (size_t*)&len)))
  {
    printf("Failed to get publish topic, return code %d\r\n", rc);
    return rc;
  }

  // 2. Register the topic with the Gateway to get topic ID
  if ((rc = register_topic(ctx, topic_name, len, &ctx->telemetry_topic_id)) != 0)
  {
    printf("Failed to register telemetry topic, return code %d\r\n", rc);
    return rc;
  }

  // 3. Send sample telemetry messages
  for (int i = 0; i < NUMBER_OF_MESSAGES; ++i)
  {
    printf("Sending Message %d\r\n", i + 1);

    // Attempt sending messages with some backoff
    if ((rc = send_telemetry(ctx, TELEMETRY_PAYLOAD, sizeof(TELEMETRY_PAYLOAD)) != 0))
    {
      int timeout = get_connection_timeout(++retry_attempt);
      printf("Retry attempt number %d waiting %d\n", retry_attempt, timeout);

      sleep_seconds(timeout);

      continue;
    }

    retry_attempt = 0;

    // Publish messages at an interval
    sleep_seconds(TELEMETRY_SEND_INTERVAL);
  }

  return 0;
}

/*
 * 1. Send Disconnect packet to the Gateway
 * 2. Close the transport
 */
static int disconnect_device(iothub_client_context* ctx)
{
  int rc;
  int len;

  // 1. Send Disconnect packet to the Gateway
  printf("Disconnecting\r\n");

  if ((len = MQTTSNSerialize_disconnect(scratch_buffer, sizeof(scratch_buffer), 0)) == 0)
  {
    printf("Failed to serialize Disconnect packet, return code %d\r\n", len);
    return -1;
  }

  if ((rc = transport_sendPacketBuffer(ctx->gateway_address, ctx->gateway_port, scratch_buffer, len)) != 0)
  {
    printf("Failed to send Disconnect packet to the Gateway, return code %d\r\n", rc);
    return rc;
  }

  printf("Disconnected.\r\n");

  // 2. Close the transport
  if ((rc = transport_close()) != 0)
  {
    printf("Failed to close transport socket, return code %d\r\n", rc);
    return rc;
  }

  return 0;
}

/*
 * 1. Initialize IoT Hub Client context
 * 2. Connect device
 * 3. Send sample telemetry messages
 * 4. Disconnect device
 */
int main(int argc, char** argv)
{
  int rc;
  unsigned short topicid;

  iothub_client_context iothub_ctx;

  if ((rc = init_client_context(&iothub_ctx)) != 0)
  {
    return rc;
  }

  if ((rc = connect_device(&iothub_ctx)) != 0)
  {
    return rc;
  }

  if ((rc = send_sample_telemetry_messages(&iothub_ctx)) != 0)
  {
    return rc;
  }

  if ((rc = disconnect_device(&iothub_ctx)) != 0)
  {
    return rc;
  }

  return 0;
}
