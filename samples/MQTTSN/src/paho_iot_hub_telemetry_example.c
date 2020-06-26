// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#define WARN printf
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "MQTTSNPacket.h"
#include "transport.h"

int main()
{
	while(1)
	{
		printf("This is a template message\n");
		sleep(100);
	}
}
