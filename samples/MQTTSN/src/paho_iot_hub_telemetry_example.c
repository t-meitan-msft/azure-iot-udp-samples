// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#define WARN printf
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "MQTTSNPacket.h"
#include "transport.h"
#include "azure/iot/az_iot_hub_client.h"

// DO NOT MODIFY: Device ID Environment Variable Name
#define ENV_DEVICE_ID "AZ_IOT_DEVICE_ID"

// DO NOT MODIFY: IoT Hub Hostname Environment Variable Name
#define ENV_IOT_HUB_HOSTNAME "AZ_IOT_HUB_HOSTNAME"

#define TELEMETRY_SEND_INTERVAL 1
#define NUMBER_OF_MESSAGES 5

char topicname[128];
static char device_id[64];
static char iot_hub_hostname[128];
static az_iot_hub_client client;

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
    printf("%s\n", hide_value ? "***" : env);
    az_span env_span = az_span_from_str(env);
    AZ_RETURN_IF_NOT_ENOUGH_SIZE(buffer, az_span_size(env_span));
    az_span_copy(buffer, env_span);
    *out_value = az_span_slice(buffer, 0, az_span_size(env_span));
  }
  else if (default_value != NULL)
  {
    printf("%s\n", default_value);
    az_span default_span = az_span_from_str(default_value);
    AZ_RETURN_IF_NOT_ENOUGH_SIZE(buffer, az_span_size(default_span));
    az_span_copy(buffer, default_span);
    *out_value = az_span_slice(buffer, 0, az_span_size(default_span));
  }
  else
  {
    printf("(missing) Please set the %s environment variable.\n", env_name);
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

// This sample implements QoS 0
int main(int argc, char** argv)
{
	int rc = 0;
	int mysock;
	unsigned char buf[500];
	int buflen = sizeof(buf);
	MQTTSN_topicid topic;
	MQTTSNString topicstr;
	int len = 0;
	int retained = 0;
	
	// Default
	char *host = "127.0.0.1";
	int port = 10000; // use unicast port if sending a unicast packet

	MQTTSNPacket_connectData options = MQTTSNPacket_connectData_initializer;
	unsigned short topicid;	

	// Create a unicast UDP socket
	mysock = transport_open();
	if(mysock < 0)
		return mysock;

	// Read for optional destination address and port
	if (argc > 1)
		host = argv[1];

	if (argc > 2)
		port = atoi(argv[2]);

	printf("Connecting to hostname %s port %d\n", host, port);

	// Read in the necessary environment variables and initialize the az_iot_hub_client
	if (az_failed(rc = read_configuration_and_init_client()))
	{
		printf("Failed to read configuration from environment variables, return code %d\n", rc);
		return rc;
	}

	options.clientID.cstring = device_id;

	// Get topic name that the IoT Hub is subscribed to
	if (az_failed(
		rc = az_iot_hub_client_telemetry_get_publish_topic(
			&client, NULL, topicname, sizeof(topicname), NULL)))
	{
		printf("Failed to get publish topic, return code %d\n", rc);
		return rc;
	}
	
	// Connect to MQTTSN Gateway
	len = MQTTSNSerialize_connect(buf, buflen, &options);

	if (az_failed(
		rc = transport_sendPacketBuffer(host, port, buf, len)))
	{
		printf("Failed to send Connect packet to the Gateway, return code %d\n", rc);
		return rc;
	}
	
	// Wait for connack from the MQTTSN Gateway
	if (MQTTSNPacket_read(buf, buflen, transport_getdata) == MQTTSN_CONNACK)
	{
		int connack_rc = -1;

		if (MQTTSNDeserialize_connack(&connack_rc, buf, buflen) != 1 || connack_rc != 0)
		{
			printf("Failed to receive Connect ACK packet, return code %d\nExiting...\n", connack_rc);
			goto exit;
		}
		else 
			printf("CONNACK rc %d\n", connack_rc);
	}
	else
	{
		printf("Failed to connect to the Gateway\nExiting...\n");
		goto exit;
	}

	// Register topic name with the MQTTSN Gateway
	printf("Registering topic %s\n", topicname);
	int packetid = 1;
	topicstr.cstring = topicname;
	topicstr.lenstring.len = strlen(topicname);
	len = MQTTSNSerialize_register(buf, buflen, 0, packetid, &topicstr);
	
	if (az_failed(
		rc = transport_sendPacketBuffer(host, port, buf, len)))
	{
		printf("Failed to send Register packet to the Gateway, return code %d\n", rc);
		return rc;
	}

	// Wait for regack from the MQTTSN Gateway
	if (MQTTSNPacket_read(buf, buflen, transport_getdata) == MQTTSN_REGACK) 	
	{
		unsigned short submsgid;
		unsigned char returncode;

		rc = MQTTSNDeserialize_regack(&topicid, &submsgid, &returncode, buf, buflen);
		if (returncode != 0)
		{
			printf("Failed to receive Register ACK packet, return code %d\nExiting...\n", returncode); // TODO
			goto exit;
		}
		else
			printf("REGACK topic id %d\n", topicid);
	}
	else
	{
		printf("Failed to register topic with the Gateway\nExiting...\n"); // TODO
		goto exit;	
	}
		
	// Publish 5 messages
	for (int i = 0; i < NUMBER_OF_MESSAGES; ++i)
    {          
		printf("Sending Message %d\n", i + 1);
		topic.type = MQTTSN_TOPIC_TYPE_NORMAL;
		topic.data.id = topicid;

		// Build payload
		unsigned char payload[250];
		int payloadlen = sprintf((char*)payload,
	"{\"d\":{\"myName\":\"IoT mbed\",\"accelX\":%0.4f,\"accelY\":%0.4f,\"accelZ\":%0.4f,\"temp\":%0.4f}}",
		(rand() % 10) * 2.0, (rand() % 10) * 2.0, (rand() % 10) * 2.0, (rand() % 10) + 18.0); 

		// Publish
		len = MQTTSNSerialize_publish(buf, buflen, 0, 0, retained, 0, topic, payload, payloadlen);

		if (az_failed(
			rc = transport_sendPacketBuffer(host, port, buf, len)))
		{
			printf("Failed to publish telemetry message %d, return code %d\n", i + 1, rc);
			return rc;
		}

		printf("Published rc %d for publish length %d\n", rc, len);

		// [BUG] doesnt work?
		sleep_seconds(TELEMETRY_SEND_INTERVAL); // Publish a message every second
	}

	// Disconnect the client
	printf("Disconnecting\n");
	len = MQTTSNSerialize_disconnect(buf, buflen, 0);

	if (az_failed(
		rc = transport_sendPacketBuffer(host, port, buf, len)))
	{
		printf("Failed to send Disconnect packet to the Gateway, return code %d\n", rc);
		return rc;
	}

	printf("Disconnected.\n");

exit:
	transport_close();

	return 0;
}
