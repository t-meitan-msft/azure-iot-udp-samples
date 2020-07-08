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

// DO NOT MODIFY: MQTTSN Gateway Port Environment Variable Name
#define ENV_MQTTSN_GATEWAY_PORT "MQTTSN_GATEWAY_PORT"

// #define MQTT_QOS 0 // if not defined, default qos is 1
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

static char topicname[128];
static char device_id[64];
static char iot_hub_hostname[128];
static unsigned char scratch_buffer[128];
static char gateway_address[16];
static char gateway_port[8];

typedef struct iothub_client_context_tag
{
  char* host;
  int port;
  unsigned short telemetry_topic_id;
  az_iot_hub_client client;
  short packetid;
} iothub_client_context;

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

static az_result read_configuration_and_init_client(
    az_iot_hub_client* client,
    char** host,
    int* port)
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

  az_span gateway_address_span = AZ_SPAN_FROM_BUFFER(gateway_address);
  AZ_RETURN_IF_FAILED(read_configuration_entry(
      ENV_MQTTSN_GATEWAY_ADDRESS,
      ENV_MQTTSN_GATEWAY_ADDRESS,
      DEFAULT_GATEWAY_ADDRESS,
      false,
      gateway_address_span,
      &gateway_address_span));

  az_span gateway_port_span = AZ_SPAN_FROM_BUFFER(gateway_port);
  AZ_RETURN_IF_FAILED(read_configuration_entry(
      ENV_MQTTSN_GATEWAY_PORT,
      ENV_MQTTSN_GATEWAY_PORT,
      DEFAULT_GATEWAY_PORT,
      false,
      gateway_port_span,
      &gateway_port_span));

  // Initialize the hub client with the hub host endpoint and the default connection options
  AZ_RETURN_IF_FAILED(az_iot_hub_client_init(
      client,
      az_span_slice(iot_hub_hostname_span, 0, (int32_t)strlen(iot_hub_hostname)),
      az_span_slice(device_id_span, 0, (int32_t)strlen(device_id)),
      NULL));

  // Save gateway address to context
  az_span_to_str(
      gateway_address,
      sizeof(gateway_address),
      az_span_slice(gateway_address_span, 0, (int32_t)strlen(gateway_address)));
  *host = gateway_address;

  // Save gateway port to context
  AZ_RETURN_IF_FAILED(
      az_span_atou32(az_span_slice(gateway_port_span, 0, (int32_t)strlen(gateway_port)), port));

  return AZ_OK;
}

static int init_client_context(iothub_client_context* ctx)
{
  int rc;
  ctx->packetid = 0;

  // Read in the necessary environment variables and initialize the az_iot_hub_client
  if (az_failed(rc = read_configuration_and_init_client(&ctx->client, &ctx->host, &ctx->port)))
  {
    printf("Failed to read configuration from environment variables, return code %d\r\n", rc);
    return rc;
  }

  return 0;
}

static int exit_sample()
{
  int rc;
  if ((rc = transport_close()) != 0)
  {
    printf("Failed to close transport socket, return code %d\r\n", rc);
    return rc;
  }
  return 0;
}

static int init_transport_and_get_topicname(iothub_client_context* ctx)
{
  int rc;

  // Create a unicast UDP socket
  if ((rc = transport_open()) < 0)
  {
    return rc;
  }

  printf("Connecting to host '%s', port '%d'\r\n", ctx->host, ctx->port);

  // Get topic name that the IoT Hub is subscribed to
  if (az_failed(
          rc = az_iot_hub_client_telemetry_get_publish_topic(
              &ctx->client, NULL, topicname, sizeof(topicname), NULL)))
  {
    printf("Failed to get publish topic, return code %d\r\n", rc);
    return rc;
  }

  return 0;
}

int get_connection_timeout(int attemptNumber)
{ // First 10 attempts try within 3 seconds, next 10 attempts retry after every 1 minute
  // after 20 attempts, retry every 10 minutes
  return (attemptNumber < 10) ? 3 : (attemptNumber < 20) ? 60 : 600;
}

static int send_connect(iothub_client_context* ctx, MQTTSNPacket_connectData* options)
{
  int rc;
  int len;

  // CONNECT to MQTTSN Gateway
  if ((len = MQTTSNSerialize_connect(scratch_buffer, sizeof(scratch_buffer), options)) == 0)
  {
    printf("Failed to serialize Connect packet, return code %d\r\n", len);
    return len;
  }

  if ((rc = transport_sendPacketBuffer(ctx->host, ctx->port, scratch_buffer, len)) != 0)
  {
    printf("Failed to send Connect packet to the Gateway, return code %d\r\n", rc);
    return rc;
  }

  return 0;
}

static int receive_connack()
{
  int rc;

  // Wait for CONNACK from the MQTTSN Gateway
  if (MQTTSNPacket_read(scratch_buffer, sizeof(scratch_buffer), transport_getdata)
      == MQTTSN_CONNACK)
  {
    if (MQTTSNDeserialize_connack(&rc, scratch_buffer, sizeof(scratch_buffer)) != 1 || rc != 0)
    {
      printf("Failed to receive Connect ACK packet, return code %d\r\n", rc);
    }
    else
    {
      printf("CONNACK\r\n");
    }

    return rc;
  }
  else
  {
    printf("Failed to receive Connect ACK packet\r\n");
    return -1;
  }

  return 0;
}

static int attempt_connect(iothub_client_context* ctx, MQTTSNPacket_connectData* options)
{
  int rc;
  int len;

  if ((rc = send_connect(ctx, options)) != 0)
  {
    return rc;
  }

  if ((rc = receive_connack()) != 0)
  {
    return rc;
  }

  return 0;
}

static int connect_device(iothub_client_context* ctx)
{
  int rc;
  int len;

  MQTTSNPacket_connectData options = MQTTSNPacket_connectData_initializer;
  options.clientID.cstring = device_id;

  while (attempt_connect(ctx, &options) != 0)
  {
    static int retryAttempt = 0;

    int timeout = get_connection_timeout(++retryAttempt);
    printf("Retry attempt number %d waiting %d\n", retryAttempt, timeout);

    sleep_seconds(timeout);
  }

  return 0;
}

static int send_register(iothub_client_context* ctx, MQTTSNString* topicstr)
{
  int rc;
  int len;

  // REGISTER topic name with the MQTTSN Gateway
  printf("Registering topic %s\r\n", topicname);
  topicstr->cstring = topicname;
  topicstr->lenstring.len = strlen(topicname);

  if ((len = MQTTSNSerialize_register(
           scratch_buffer, sizeof(scratch_buffer), 0, ctx->packetid, topicstr))
      == 0)
  {
    printf("Failed to serialize Register packet, return code %d\r\n", len);
    return len;
  }

  if ((rc = transport_sendPacketBuffer(ctx->host, ctx->port, scratch_buffer, len)) != 0)
  {
    printf("Failed to send Register packet to the Gateway, return code %d\r\n", rc);
    return rc;
  }

  return 0;
}

static int receive_regack(iothub_client_context* ctx)
{
  int len;

  // Wait for REGACK from the MQTTSN Gateway
  if (MQTTSNPacket_read(scratch_buffer, sizeof(scratch_buffer), transport_getdata) == MQTTSN_REGACK)
  {
    unsigned short submsgid;
    unsigned char returncode;

    if (MQTTSNDeserialize_regack(
            &ctx->telemetry_topic_id,
            &submsgid,
            &returncode,
            scratch_buffer,
            sizeof(scratch_buffer))
            != 1
        || returncode != 0)
    {
      printf("Failed to receive Register ACK packet, return code %d\r\n", returncode);
      return -1;
    }
    else
    {
      printf("REGACK with topic id %d \r\n", ctx->telemetry_topic_id);
    }
  }
  else
  {
    printf("Failed to register topic with the Gateway\r\n");
    return -1;
  }

  return 0;
}

static int attempt_register(iothub_client_context* ctx, MQTTSNString* topicstr)
{
  int rc;
  int len;

  if ((rc = send_register(ctx, topicstr)) != 0)
  {
    return rc;
  }

  if ((rc = receive_regack(ctx)) != 0)
  {
    return rc;
  }

  return 0;
}

static int register_topic(iothub_client_context* ctx)
{
  int rc;
  int len;
  MQTTSNString topicstr;

  while (attempt_register(ctx, &topicstr) != 0)
  {
    static int retryAttempt = 0;

    int timeout = get_connection_timeout(++retryAttempt);
    printf("Retry attempt number %d waiting %d\n", retryAttempt, timeout);

    sleep_seconds(timeout);
  }

  return 0;
}

static int send_publish(iothub_client_context* ctx)
{
  int rc;
  int len;
  int retained = 0;
  MQTTSN_topicid topic;
  int qos;

#ifdef MQTT_QOS
  qos = MQTT_QOS;
#else
  qos = 1;
#endif

  topic.type = MQTTSN_TOPIC_TYPE_NORMAL;
  topic.data.id = ctx->telemetry_topic_id;

  // PUBLISH
  if ((len = MQTTSNSerialize_publish(
           scratch_buffer,
           sizeof(scratch_buffer),
           0,
           qos,
           retained,
           ctx->packetid,
           topic,
           TELEMETRY_PAYLOAD,
           sizeof(TELEMETRY_PAYLOAD)))
      == 0)
  {
    printf("Failed to serialize Publish packet, return code %d\r\n", len);
    return len;
  }

  if ((rc = transport_sendPacketBuffer(ctx->host, ctx->port, scratch_buffer, len)) != 0)
  {
    printf(
        "Failed to publish telemetry message with packet id %d, return code %d\r\n",
        ctx->packetid,
        rc);
    return rc;
  }

  printf("Published telemetry payload of length %d\r\n", len);

  return 0;
}

#ifdef ENABLE_PUBACK
static int receive_puback()
{
  // Wait for PUBACK
  if (MQTTSNPacket_read(scratch_buffer, sizeof(scratch_buffer), transport_getdata) == MQTTSN_PUBACK)
  {
    unsigned short packet_id, topic_id;
    unsigned char returncode;

    if (MQTTSNDeserialize_puback(
            &topic_id, &packet_id, &returncode, scratch_buffer, sizeof(scratch_buffer))
            != 1
        || returncode != MQTTSN_RC_ACCEPTED)
    {
      printf(
          "Failed to receive Publish ACK packet ID %hu, return code %d\r\n", packet_id, returncode);
      return -1;
    }
    else
    {
      printf("PUBACK received, packet ID %hu\r\n", packet_id);
    }
  }
  else
  {
    printf("Failed to Acknowledge Publish packet\r\n");
    return -1;
  }

  return 0;
}
#endif

static int attempt_publish(iothub_client_context* ctx)
{
  int rc;
  int len;

  // Publish 5 messages
  for (int i = 0; i < NUMBER_OF_MESSAGES; ++i)
  {
    printf("Sending Message %d\r\n", i + 1);

    ctx->packetid = ctx->packetid + 1;

    if ((rc = send_publish(ctx)) != 0)
    {
      return rc;
    }

#ifdef ENABLE_PUBACK
    if ((rc = receive_puback()) != 0)
    {
      return rc;
    }
#endif

    sleep_seconds(TELEMETRY_SEND_INTERVAL); // Publish a message every second
  }

  return 0;
}

static int send_telemetry(iothub_client_context* ctx)
{
  int rc;
  int len;

  while (attempt_publish(ctx) != 0)
  {
    static int retryAttempt = 0;

    int timeout = get_connection_timeout(++retryAttempt);
    printf("Retry attempt number %d waiting %d\n", retryAttempt, timeout);

    sleep_seconds(timeout);
  }

  return 0;
}

static int disconnect_device(iothub_client_context* ctx)
{
  int rc;
  int len;

  // DISCONNECT the client
  printf("Disconnecting\r\n");

  if ((len = MQTTSNSerialize_disconnect(scratch_buffer, sizeof(scratch_buffer), 0)) == 0)
  {
    printf("Failed to serialize Disconnect packet, return code %d\r\n", len);
    return len;
  }

  if ((rc = transport_sendPacketBuffer(ctx->host, ctx->port, scratch_buffer, len)) != 0)
  {
    printf("Failed to send Disconnect packet to the Gateway, return code %d\r\n", rc);
    return rc;
  }

  printf("Disconnected.\r\n");

  return 0;
}

int main(int argc, char** argv)
{
  int rc;
  unsigned short topicid;

  iothub_client_context iothub_ctx;

  if ((rc = init_client_context(&iothub_ctx)) != 0)
  {
    return rc;
  }

  if ((rc = init_transport_and_get_topicname(&iothub_ctx)) != 0)
  {
    return rc;
  }

  if ((rc = connect_device(&iothub_ctx)) != 0)
  {
    return rc;
  }

  if ((rc = register_topic(&iothub_ctx)) != 0)
  {
    return rc;
  }

  if ((rc = send_telemetry(&iothub_ctx)) != 0)
  {
    return rc;
  }

  if ((rc = disconnect_device(&iothub_ctx)) != 0)
  {
    return rc;
  }

  if ((rc = exit_sample()) != 0)
  {
    return rc;
  }

  return 0;
}
