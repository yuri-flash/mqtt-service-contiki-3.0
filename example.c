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
#include "pwm_dimmer.h"
#include <string.h>

#include "dev/cc2538-sensors.h"
/*---------------------------------------------------------------------------*/
/*
* IBM server: messaging.quickstart.internetofthings.ibmcloud.com
* (184.172.124.189) mapped in an NAT64 (prefix 64:ff9b::/96) IPv6 address
* Note: If not able to connect; lookup the IP address again as it may change.
*
* Alternatively, publish to a local MQTT broker (e.g. mosquitto) running on
* the node that hosts your border router
*/
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
// static struct timer connection_life;
// static uint8_t connect_attempt;
/*---------------------------------------------------------------------------*/
/* Various states */
// static uint8_t state;
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

uint8_t new_duty_cilce  = 30;
/*---------------------------------------------------------------------------*/
/* Maximum TCP segment size for outgoing segments of our socket */
#define MAX_TCP_SEGMENT_SIZE    32
/*---------------------------------------------------------------------------*/
#define STATUS_LED LEDS_GREEN
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
PROCESS(mqtt_demo_process, "MQTT AWGES-TEST");
PROCESS(cc2538_pwm_test, "cc2538-pwm");

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
		"lights", "lights/3206", NULL
	};

	PROCESS_BEGIN();
	
/**************************************************/

	pwm_init();
	process_start(&cc2538_pwm_test,NULL);
	
/*************************************************/

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
		static char message2[128];

		mqtt_publish("lights/3206", "Hello AWGES DEV: 3206 MAC: 32-06", 0, 1);
		
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
			PRINTF("PROCESS_WAIT_EVENT_UNTIL: %d.\n\r",((mqtt_event_data_t*)data)->type);

			if (ev == mqtt_event)
			{
				if (mqtt_event_is_receive_data(data))
				{
					static char topic[64];
					static char message[64];
					static char write_command[4];
					static const char topic_out[64];
					static const char message_out[64];

					strncpy(topic, mqtt_event_get_topic(data), mqtt_event_get_topic_length(data));
					topic[mqtt_event_get_topic_length(data)] = '\0';

					strncpy(message, mqtt_event_get_data(data), mqtt_event_get_data_length(data));
					message[mqtt_event_get_data_length(data)] = '\0';
					
					PRINTF("mqtt_client: Data received: %s, %s.\n\r", topic, message);
					
					PRINTF("message: %s\n\n\r",message);
						
					if (message[0] == 'R'){
						int command = (int)atoi(message+1);
						
						switch((int)atoi(message+1)){
							case 1:
								sprintf(topic_out, "lights/3206/dimmer");
								sprintf(message_out, "%d%", new_duty_cilce);
								PRINTF("comendo de leitura recebida: %d\n\r", atoi(message+1));
								break;
							case 2:
								sprintf(topic_out, "lights/3206/dimmer");
								sprintf(message_out, "%d458mA", new_duty_cilce);
								break;
							case 3:
							
								break;
							case 4:
							
								break;
							case 5:
							
								break;
							default:
								PRINTF("comendo de leitura recebida: %d", atoi(message+1));
						}
					}
					else if(message[0] == 'W')
					{
						memcpy(write_command,(message+1),4);
						write_command[4] = '\0';
						PRINTF("write commmand : %s\n\n\r",write_command);
						
						switch((int)atoi(write_command)){//6
							case 1:
								PRINTF("valor do dimmer: %s",message+6);
								new_duty_cilce = (uint8_t)atoi(message+6);
								//process_start(&cc2538_pwm_test,NULL);
								process_post_synch(&cc2538_pwm_test,ev, data);
								break;
							case 2:
							
								break;
							default:
								PRINTF("comendo de leitura recebida: %d", atoi(message+1));
						}
					}	

					PRINTF("%s","I'm alive!");
					etimer_set(&et,CLOCK_SECOND * 0.5);
					PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
					mqtt_publish(topic_out, message_out, 0, 1);

				}
				else if (mqtt_event_is_receive_data_continuation(data))
				{
					const char* message = mqtt_event_get_data(data);
					PRINTF("mqtt_client:                %s.\n\r", message);
				}
				else if(mqtt_event_is_disconnected(data))
				{ 
					PRINTF("processo 1\n\r");
					uip_ip6addr(&server_address,
						0xaaaa, 0, 0, 0, 0, 0, 0, 0x1);
					mqtt_reconfigure(&server_address, UIP_HTONS(1883),
						1);
					mqtt_connect(&server_address, UIP_HTONS(1883),
						1, &connect_info);
				}
				else if (mqtt_event_is_connected(data))
				{
					PRINTF("processo 2\n\r");
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
		
		PRINTF("mqtt service connect failed\n\r");
		PROCESS_END();
		
	}
}

PROCESS_THREAD(cc2538_pwm_test, ev, data)
{

	PROCESS_BEGIN();
	
	while(1)
	{

		etimer_set(&et_pwm,CLOCK_SECOND* 0.1);
						PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et_pwm));
						PRINTF("testing duty: %d \n\r", duty);
						
		if (new_duty_cilce > 99) 
			new_duty_cilce = 99;
		
		PRINTF("new duty cicle: %d\n\r", new_duty_cilce);
		
		while(new_duty_cilce != duty)
		{
			PRINTF("duty cicle: %d\n\r", duty);
			if (new_duty_cilce > duty) 
				duty++;
			else 
				duty--;
			
				if(pwm_enable(pwm_num.freq, duty,
							  pwm_num.timer, pwm_num.ab) == PWM_SUCCESS) {
					pwm_en = 1;
					//PRINTF("%s (%u) configuration OK\n\r", gpt_name(pwm_num.timer),
					//       pwm_num.ab);

				}

				if((pwm_en) && (pwm_start(pwm_num.timer, pwm_num.ab,
								   pwm_num.port, pwm_num.pin) != PWM_SUCCESS)) {
					pwm_en = 0;
					PRINTF("%s (%u) failed to start \n\r", gpt_name(pwm_num.timer),
						   pwm_num.ab);

				}
				etimer_set(&et_pwm,CLOCK_SECOND* 0.02);
				PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et_pwm));
				PRINTF("testing duty: %d \n\r", duty);
			
		}
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et_pwm));
	}
    PROCESS_END();
}

