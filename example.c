/*
* Copyright (c) 2014, Texas Instruments Incorporated - http://www.ti.com/
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
* 1. Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in the
*    documentation and/or other materials provided with the distribution.
* 3. Neither the name of the copyright holder nor the names of its
*    contributors may be used to endorse or promote products derived
*    from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
* COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
* HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
* STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
* OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/*---------------------------------------------------------------------------*/
/** \addtogroup cc2538-examples
* @{
*
* \defgroup cc2538-mqtt-demo CC2538DK MQTT Demo Project
*
* Demonstrates MQTT functionality. Works with IBM Quickstart as well as
* mosquitto.
* @{
*
* \file
* An MQTT example for the cc2538dk platform
*/
/*---------------------------------------------------------------------------*/
#include "contiki-conf.h"
#include "rpl/rpl-private.h"
#include "mqtt-service.h"
#include "net/rpl/rpl.h"
#include "net/ip/uip.h"
#include "net/ipv6/uip-icmp6.h"
#include "net/ipv6/sicslowpan.h"
#include "sys/etimer.h"
#include "sys/ctimer.h"
#include "lib/sensors.h"
#include "dev/button-sensor.h"
#include "dev/leds.h"
#include "dev/sys-ctrl.h"
#include "pwm.h"

#include <string.h>
/*---------------------------------------------------------------------------*/
/*
* IBM server: messaging.quickstart.internetofthings.ibmcloud.com
* (184.172.124.189) mapped in an NAT64 (prefix 64:ff9b::/96) IPv6 address
* Note: If not able to connect; lookup the IP address again as it may change.
*
* Alternatively, publish to a local MQTT broker (e.g. mosquitto) running on
* the node that hosts your border router
*/
#define MQTT_DEMO_BROKER_IP_ADDR "aaaa::1"
#ifdef MQTT_DEMO_BROKER_IP_ADDR
static const char *broker_ip = MQTT_DEMO_BROKER_IP_ADDR;
#define DEFAULT_ORG_ID              "mqtt-demo"
#else
static const char *broker_ip = "aaaa::1";
#define DEFAULT_ORG_ID              "AWGES-BOARD2"
#endif
/*---------------------------------------------------------------------------*/
/*
* A timeout used when waiting for something to happen (e.g. to connect or to
* disconnect)
*/
#define STATE_MACHINE_PERIODIC     (CLOCK_SECOND >> 1)
/*---------------------------------------------------------------------------*/
/* Provide visible feedback via LEDS during various states */
/* When connecting to broker */
#define CONNECTING_LED_DURATION    (CLOCK_SECOND >> 2)

/* Each time we try to publish */
#define PUBLISH_LED_ON_DURATION    (CLOCK_SECOND)
/*---------------------------------------------------------------------------*/
/* Connections and reconnections */
#define RETRY_FOREVER              0xFF
#define RECONNECT_INTERVAL         (CLOCK_SECOND * 2)

/*
* Number of times to try reconnecting to the broker.
* Can be a limited number (e.g. 3, 10 etc) or can be set to RETRY_FOREVER
*/
#define RECONNECT_ATTEMPTS         RETRY_FOREVER
#define CONNECTION_STABLE_TIME     (CLOCK_SECOND * 5)
static struct timer connection_life;
static uint8_t connect_attempt;
/*---------------------------------------------------------------------------*/
/* Various states */
static uint8_t state;
#define STATE_INIT            0
#define STATE_REGISTERED      1
#define STATE_CONNECTING      2
#define STATE_CONNECTED       3
#define STATE_PUBLISHING      4
#define STATE_DISCONNECTED    5
#define STATE_NEWCONFIG       6
#define STATE_CONFIG_ERROR 0xFE
#define STATE_ERROR        0xFF
/*---------------------------------------------------------------------------*/
#define CONFIG_ORG_ID_LEN        32
#define CONFIG_TYPE_ID_LEN       32
#define CONFIG_AUTH_TOKEN_LEN    32
#define CONFIG_EVENT_TYPE_ID_LEN 32
#define CONFIG_CMD_TYPE_LEN       8
#define CONFIG_IP_ADDR_STR_LEN   64
/*---------------------------------------------------------------------------*/
#define RSSI_MEASURE_INTERVAL_MAX 86400 /* secs: 1 day */
#define RSSI_MEASURE_INTERVAL_MIN     5 /* secs */
#define PUBLISH_INTERVAL_MAX      86400 /* secs: 1 day */
#define PUBLISH_INTERVAL_MIN          5 /* secs */
/*---------------------------------------------------------------------------*/
/* A timeout used when waiting to connect to a network */
#define NET_CONNECT_PERIODIC        (CLOCK_SECOND >> 2)
#define NO_NET_LED_DURATION         (NET_CONNECT_PERIODIC >> 1)
/*---------------------------------------------------------------------------*/
/* Default configuration values */
#define DEFAULT_TYPE_ID             "YuriID"
#define DEFAULT_AUTH_TOKEN          "AUTHZ"
#define DEFAULT_EVENT_TYPE_ID       "status"
#define DEFAULT_SUBSCRIBE_CMD_TYPE  "+"
#define DEFAULT_BROKER_PORT         1883
#define DEFAULT_PUBLISH_INTERVAL    (1 * CLOCK_SECOND)
#define DEFAULT_KEEP_ALIVE_TIMER    60
#define DEFAULT_RSSI_MEAS_INTERVAL  (CLOCK_SECOND * 1)
/*---------------------------------------------------------------------------*/
/* Take a sensor reading on button press */
#define PUBLISH_TRIGGER &button_sensor

/* Payload length of ICMPv6 echo requests used to measure RSSI with def rt */
#define ECHO_REQ_PAYLOAD_LEN   20
/*---------------------------------------------------------------------------*/
PROCESS_NAME(mqtt_demo_process);
AUTOSTART_PROCESSES(&mqtt_demo_process);
/*---------------------------------------------------------------------------*/
/**
* \brief Data structure declaration for the MQTT client configuration
*/
typedef struct mqtt_client_config {
	char org_id[CONFIG_ORG_ID_LEN];
	char type_id[CONFIG_TYPE_ID_LEN];
	char auth_token[CONFIG_AUTH_TOKEN_LEN];
	char event_type_id[CONFIG_EVENT_TYPE_ID_LEN];
	char broker_ip[CONFIG_IP_ADDR_STR_LEN];
	char cmd_type[CONFIG_CMD_TYPE_LEN];
	clock_time_t pub_interval;
	int def_rt_ping_interval;
	uint16_t broker_port;
} mqtt_client_config_t;
/*---------------------------------------------------------------------------*/
/* Maximum TCP segment size for outgoing segments of our socket */
#define MAX_TCP_SEGMENT_SIZE    32
/*---------------------------------------------------------------------------*/
#define STATUS_LED LEDS_GREEN
/*---------------------------------------------------------------------------*/
/*
* Buffers for Client ID and Topic.
* Make sure they are large enough to hold the entire respective string
*
* d:quickstart:status:EUI64 is 32 bytes long
* iot-2/evt/status/fmt/json is 25 bytes
* We also need space for the null termination
*/
#define BUFFER_SIZE 64
static char client_id[BUFFER_SIZE];
static char pub_topic[BUFFER_SIZE];
static char sub_topic[BUFFER_SIZE];
/*---------------------------------------------------------------------------*/
/*
* The main MQTT buffers.
* We will need to increase if we start publishing more data.
*/
#define APP_BUFFER_SIZE 512
static struct mqtt_connection conn;
static char app_buffer[APP_BUFFER_SIZE];
/*---------------------------------------------------------------------------*/
#define QUICKSTART "quickstart"
/*---------------------------------------------------------------------------*/
static struct mqtt_message *msg_ptr = 0;
static struct etimer publish_periodic_timer;
static struct ctimer ct;
static char *buf_ptr;
static uint16_t seq_nr_value = 0;
/*---------------------------------------------------------------------------*/
/* Parent RSSI functionality */
static struct uip_icmp6_echo_reply_notification echo_reply_notification;
static struct etimer echo_request_timer;
static int def_rt_rssi = 0;
/*---------------------------------------------------------------------------*/
static mqtt_client_config_t conf;
/*---------------------------------------------------------------------------*/
PROCESS(mqtt_demo_process, "MQTT AWGES-TEST");
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(mqtt_demo_process, ev, data)
{

	static struct etimer et;
	static uip_ip6addr_t server_address;

	// Allocate buffer space for the MQTT client
	static uint8_t in_buffer[64];
	static uint8_t out_buffer[64];

	// Setup an mqtt_connect_info_t structure to describe
	// how the connection should behave
	static mqtt_connect_info_t connect_info =
	{
		.client_id = "AWGES01",
		.username = NULL,
		.password = NULL,
		.will_topic = NULL,
		.will_message = NULL,
		.keepalive_timeout = 60,
		.keepalive = 40,
		.retry_timeout = 5,
		.will_qos = 0,
		.will_retain = 0,
		.clean_session = 1
	};

	// The list of topics that we want to subscribe to
	static const char* topics[] =
	{
		"awges", "yuriar", NULL
	};

	PROCESS_BEGIN();

	// Set the server address
	uip_ip6addr(&server_address,
	0xaaaa, 0, 0, 0, 0, 0, 0, 0x1);

	// Initialise the MQTT client
	mqtt_init(in_buffer, sizeof(in_buffer),
	out_buffer, sizeof(out_buffer));

	// Ask the client to connect to the server
	// and wait for it to complete.
	mqtt_connect(&server_address, UIP_HTONS(1883),
	1, &connect_info);
	PROCESS_WAIT_EVENT_UNTIL(ev == mqtt_event);

	if(mqtt_connected())
	{
		static int i;

		for(i = 0; topics[i] != NULL; ++i)
		{
			// Ask the client to subscribe to the topic
			// and wait for it to complete
			mqtt_subscribe(topics[i],0);
			PROCESS_WAIT_EVENT_UNTIL(ev == mqtt_event);
		}

		// Loop waiting for events from the MQTT client
		while(1)
		{
			PROCESS_WAIT_EVENT_UNTIL(ev == mqtt_event || ev == PROCESS_EVENT_TIMER);
			printf("PROCESS_WAIT_EVENT_UNTIL: %d.\n\r",((mqtt_event_data_t*)data)->type);

			if (ev == mqtt_event)
			{
				if (mqtt_event_is_receive_data(data))
				{
					static char topic[128];
					static char message[128];

					strncpy(topic, mqtt_event_get_topic(data), mqtt_event_get_topic_length(data));
					topic[mqtt_event_get_topic_length(data)] = 0;

					strncpy(message, mqtt_event_get_data(data), mqtt_event_get_data_length(data));
					message[mqtt_event_get_data_length(data)] = 0;

					printf("mqtt_client: Data received: %s, %s.\n\r", topic, message);
					
							//watchdog_start();
							//while (1);
				}
				else if (mqtt_event_is_receive_data_continuation(data))
				{
					const char* message = mqtt_event_get_data(data);
					printf("mqtt_client:                %s.\n\r", message);
				}
				else if(mqtt_event_is_disconnected(data))
				{ 
					printf("processo 1\n\r");
					uip_ip6addr(&server_address,
						0xaaaa, 0, 0, 0, 0, 0, 0, 0x1);
					mqtt_reconfigure(&server_address, UIP_HTONS(1883),
						1);
					mqtt_connect(&server_address, UIP_HTONS(1883),
						1, &connect_info);
				}
				else if (mqtt_event_is_connected(data))
				{
					printf("processo 2\n\r");
					while (!mqtt_connected());
					for(i = 0; topics[i] != NULL; ++i)
					{
						// Ask the client to subscribe to the topic
						// and wait for it to complete
						mqtt_subscribe(topics[i],0);
						PROCESS_WAIT_EVENT_UNTIL(ev == mqtt_event);
					}
				}	
			
			}
		}
		
		printf("mqtt service connect failed\n\r");
		PROCESS_END();
		
	}
}
