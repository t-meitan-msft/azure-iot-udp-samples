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
	
	char *host = "127.0.0.1";
	int port = 10000; // unicast port is 10000
	MQTTSNPacket_connectData options = MQTTSNPacket_connectData_initializer;
	unsigned short topicid;	

	// init IoT Hub client 
	rc = read_configuration_and_init_client();

	// get topic name that the IoT Hub is subscribed to
	rc = az_iot_hub_client_telemetry_get_publish_topic(
              &client, NULL, topicname, sizeof(topicname), NULL);

	mysock = transport_open();
	if(mysock < 0)
		return mysock;

	if (argc > 1)
		host = argv[1];

	if (argc > 2)
		port = atoi(argv[2]);

	printf("Sending to hostname %s port %d\n", host, port);

	options.clientID.cstring = device_id;
	
	// connect to MQTTSN Gateway
	len = MQTTSNSerialize_connect(buf, buflen, &options);
	rc = transport_sendPacketBuffer(host, port, buf, len);

	/* wait for connack */
	if (MQTTSNPacket_read(buf, buflen, transport_getdata) == MQTTSN_CONNACK)
	{
		int connack_rc = -1;

		if (MQTTSNDeserialize_connack(&connack_rc, buf, buflen) != 1 || connack_rc != 0)
		{
			printf("Unable to connect, return code %d\n", connack_rc);
			goto exit;
		}
		else 
			printf("connected rc %d\n", connack_rc);
	}
	else
	{
		printf("could not connect to gateway\n");
		goto exit;
	}

	/* register topic name */
	printf("Registering\n");
	int packetid = 1;
	topicstr.cstring = topicname;
	topicstr.lenstring.len = strlen(topicname);
	len = MQTTSNSerialize_register(buf, buflen, 0, packetid, &topicstr);
	rc = transport_sendPacketBuffer(host, port, buf, len);

	if (MQTTSNPacket_read(buf, buflen, transport_getdata) == MQTTSN_REGACK) 	/* wait for regack */
	{
		unsigned short submsgid;
		unsigned char returncode;

		rc = MQTTSNDeserialize_regack(&topicid, &submsgid, &returncode, buf, buflen);
		if (returncode != 0)
		{
			printf("return code %d\n", returncode);
			goto exit;
		}
		else
			printf("regack topic id %d\n", topicid);
	}
	else
		goto exit;

	for (int i = 0; i < 5; i++)
    {          
		/* publish with obtained id */
		printf("Publishing\n");
		topic.type = MQTTSN_TOPIC_TYPE_NORMAL;
		topic.data.id = topicid;

		unsigned char payload[250];
		int payloadlen = sprintf((char*)payload,
	"{\"d\":{\"myName\":\"IoT mbed\",\"accelX\":%0.4f,\"accelY\":%0.4f,\"accelZ\":%0.4f,\"temp\":%0.4f}}",
		(rand() % 10) * 2.0, (rand() % 10) * 2.0, (rand() % 10) * 2.0, (rand() % 10) + 18.0); 
		len = MQTTSNSerialize_publish(buf, buflen, 0, 0, retained, 0, topic, payload, payloadlen);
		rc = transport_sendPacketBuffer(host, port, buf, len);

		printf("rc %d from send packet for publish length %d\n", rc, len);

		// [BUG] doesnt work?
		// sleep_seconds(1); // Publish a message every second
	}

	len = MQTTSNSerialize_disconnect(buf, buflen, 0);
	rc = transport_sendPacketBuffer(host, port, buf, len);

exit:
	transport_close();

	return 0;
}
