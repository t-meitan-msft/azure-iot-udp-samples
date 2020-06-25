// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#define WARN printf
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "MQTTSNPacket.h"
#include "transport.h"

#define AZURE_SN_GATEWAY "127.0.0.1"
#define AZURE_SN_UNICAST_PORT 10000
#define AZURE_IOT_HUB_DEVICE_ID ""
#define AZURE_TELEMETRY_TOPIC "devices/" AZURE_IOT_HUB_DEVICE_ID "/messages/events/"

unsigned char user_buffer[200];

typedef struct iothub_client_context_tag
{
	char *host;
	int port;
	unsigned short telemetry_topic_id;
	char *client_id;
	int packetid;
} iothub_client_context;

static iothub_client_context g_iothub_client;

int init(iothub_client_context *ctx, char *host, int port, char *client_id)
{
	ctx->host = host;
	ctx->port = port;
	ctx->client_id = client_id;
	ctx->telemetry_topic_id = 0;
	ctx->packetid = 0;

	return (0);
}

int register_topic(iothub_client_context *iothub_ctx,  char *topic, int topic_len, unsigned short *topicid)
{
MQTTSNString topicstr;
int rc;
unsigned short submsgid;
unsigned char returncode;
int len;

	topicstr.cstring = topic;
	topicstr.lenstring.len = topic_len;
	len = MQTTSNSerialize_register(user_buffer, sizeof(user_buffer), 0, ++(iothub_ctx->packetid), &topicstr);

	if ((rc = transport_sendPacketBuffer(iothub_ctx->host, iothub_ctx->port, user_buffer, len)) != 0)
	{
		printf("failed to send packet, return code %d\n", rc);
	}
	else if (MQTTSNPacket_read(user_buffer, sizeof(user_buffer), transport_getdata) == MQTTSN_REGACK &&
			 MQTTSNDeserialize_regack(topicid, &submsgid, &returncode, user_buffer, sizeof(user_buffer)) == 1 &&
			 returncode == 0) /* wait for regack */
	{
		printf("regack topic id %c\n", topicid);
	}
	else
	{
		printf("filed regack \n");
		rc = -1;
	}

	return (rc);
}

int connect(iothub_client_context *iothub_ctx)
{
MQTTSNPacket_connectData options = MQTTSNPacket_connectData_initializer;
int rc = 0;
int connect_ack_rc = 0;
int len;

	options.clientID.cstring = iothub_ctx -> client_id;
	options.duration = 400;
	len = MQTTSNSerialize_connect(user_buffer, sizeof(user_buffer), &options);
	rc = transport_sendPacketBuffer(iothub_ctx->host, iothub_ctx->port, user_buffer, len);

	/* wait for connack */
	if (MQTTSNPacket_read(user_buffer, sizeof(user_buffer), transport_getdata) != MQTTSN_CONNACK)
	{
		printf("could not connect to gateway\n");
		rc = -1;
	}
	else
	{
		if (MQTTSNDeserialize_connack(&connect_ack_rc, user_buffer, sizeof(user_buffer)) == 1 && connect_ack_rc == 0)
		{
			printf("connected rc %d\n", connect_ack_rc);
			rc = register_topic(iothub_ctx, AZURE_TELEMETRY_TOPIC, sizeof(AZURE_TELEMETRY_TOPIC) - 1, &(iothub_ctx->telemetry_topic_id));
		}
		else
		{
			printf("Unable to connect, return code %d\n", connect_ack_rc);
			rc = -1;
		}
	}

	return (rc);
}

int getConnTimeout(int attemptNumber)
{	// First 10 attempts try within 3 seconds, next 10 attempts retry after every 1 minute
	// after 20 attempts, retry every 10 minutes
	return (attemptNumber < 10) ? 3 : (attemptNumber < 20) ? 60 : 600;
}

void attemptConnect(iothub_client_context *iothub_ctx)
{
int retryAttempt = 0;

	while (connect(iothub_ctx) != 0)
	{
		int timeout = getConnTimeout(++retryAttempt);
		WARN("Retry attempt number %d waiting %d\n", retryAttempt, timeout);

		// if ipstack and client were on the heap we could deconstruct and goto a label where they are constructed
		//  or maybe just add the proper members to do this disconnect and call attemptConnect(...)

		sleep(timeout);
	}
}

int publish_telemetry(iothub_client_context *iothub_ctx, unsigned char *payload, int payload_len)
{
MQTTSN_topicid topic;
int len;
int rc;
unsigned char returncode;
unsigned short packet_id, topic_id;

	topic.type = MQTTSN_TOPIC_TYPE_NORMAL;
	topic.data.id = iothub_ctx->telemetry_topic_id;

	len = MQTTSNSerialize_publish(user_buffer, sizeof(user_buffer), 0, 1, 0, ++(iothub_ctx->packetid),
								  topic, payload, payload_len);
	if ((rc = transport_sendPacketBuffer(iothub_ctx->host, iothub_ctx->port, user_buffer, len)) != 0)
	{
		printf("failed to send packet, return code %d\n", rc);
	}
	/* wait for puback */
	else if (MQTTSNPacket_read(user_buffer, sizeof(user_buffer), transport_getdata) == MQTTSN_PUBACK &&
			 MQTTSNDeserialize_puback(&topic_id, &packet_id, &returncode, user_buffer, sizeof(user_buffer)) == 1 &&
			 returncode == MQTTSN_RC_ACCEPTED)
	{
		printf("puback received, msgid %d topic id %d\n", packet_id, topic_id);
	}
	else
	{
		rc = -1;
	}

	return (rc);
}

int main()
{
	int mysock = transport_open();
	if(mysock < 0)
		return mysock;

	if (init(&g_iothub_client, AZURE_SN_GATEWAY, AZURE_SN_UNICAST_PORT, AZURE_IOT_HUB_DEVICE_ID) != 0)
	{
		printf("failed to initialize \r\n");
	}
	else
	{
		attemptConnect(&g_iothub_client);

		int count = 0;
		while (1)
		{
			if (++count == 10)
			{ // Publish a message every second
				if (publish_telemetry(&g_iothub_client, "hello", sizeof("hello") - 1) != 0)
					attemptConnect(&g_iothub_client); // if we have lost the connection
				count = 0;
			}
			//sleep(1);
		}
	}
}
