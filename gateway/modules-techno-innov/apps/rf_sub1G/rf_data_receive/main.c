/****************************************************************************
 *   apps/rf_sub1G/simple/main.c
 *
 * sub1G_module support code - USB version
 *
 * Copyright 2013-2014 Nathael Pajani <nathael.pajani@ed3l.fr>
 *
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *************************************************************************** */

#include "core/system.h"
#include "core/systick.h"
#include "core/pio.h"
#include "lib/stdio.h"
#include "drivers/serial.h"
#include "drivers/gpio.h"
#include "drivers/ssp.h"
#include "drivers/i2c.h"
#include "drivers/adc.h"
#include "string.h"
#include "drivers/timers.h"
#include "extdrv/cc1101.h"
#include "extdrv/status_led.h"
#include "extdrv/tmp101_temp_sensor.h"
#include "lib/protocols/dtplug/slave.h"


#define MODULE_VERSION	0x03
#define MODULE_NAME "RF Sub1G - USB"

#define RF_868MHz  1
#define RF_915MHz  0
#if ((RF_868MHz) + (RF_915MHz) != 1)
#error Either RF_868MHz or RF_915MHz MUST be defined.
#endif

#define DEBUG 1 
#define BUFF_LEN 60
#define RF_BUFF_LEN  64

#define SELECTED_FREQ  FREQ_SEL_48MHz
#define DEVICE_ADDRESS  0x12/* Addresses 0x00 and 0xFF are broadcast */
#define NEIGHBOR_ADDRESS 0x17 /* Address of the associated device */


/***************************************************************************** */
/* Pins configuration */
/* pins blocks are passed to set_pins() for pins configuration.
 * Unused pin blocks can be removed safely with the corresponding set_pins() call
 * All pins blocks may be safelly merged in a single block for single set_pins() call..
 */
const struct pio_config common_pins[] = {
	/* UART 0 */
	{ LPC_UART0_RX_PIO_0_1,  LPC_IO_DIGITAL },
	{ LPC_UART0_TX_PIO_0_2,  LPC_IO_DIGITAL },
	/* SPI */
	{ LPC_SSP0_SCLK_PIO_0_14, LPC_IO_DIGITAL },
	{ LPC_SSP0_MOSI_PIO_0_17, LPC_IO_DIGITAL },
	{ LPC_SSP0_MISO_PIO_0_16, LPC_IO_DIGITAL },
	/* I2C 0 */
	{ LPC_I2C0_SCL_PIO_0_10, (LPC_IO_DIGITAL | LPC_IO_OPEN_DRAIN_ENABLE) },
	{ LPC_I2C0_SDA_PIO_0_11, (LPC_IO_DIGITAL | LPC_IO_OPEN_DRAIN_ENABLE) },
	ARRAY_LAST_PIO,
};

const struct pio cc1101_cs_pin = LPC_GPIO_0_15;
const struct pio cc1101_miso_pin = LPC_SSP0_MISO_PIO_0_16;
const struct pio cc1101_gdo0 = LPC_GPIO_0_6;
const struct pio cc1101_gdo2 = LPC_GPIO_0_7;

const struct pio status_led_green = LPC_GPIO_0_28;
const struct pio status_led_red = LPC_GPIO_0_29;

const struct pio button = LPC_GPIO_0_12; /* ISP button */

static struct dtplug_protocol_handle uart_handle;

// Message
struct message 
{
	uint32_t temp;
	uint16_t hum;
	uint32_t lum;
	uint32_t ordre;
};
typedef struct message message;

/***************************************************************************** */
void system_init()
{
	/* Stop the watchdog */
	startup_watchdog_disable(); /* Do it right now, before it gets a chance to break in */
	system_set_default_power_state();
	clock_config(SELECTED_FREQ);
	set_pins(common_pins);
	gpio_on();
	/* System tick timer MUST be configured and running in order to use the sleeping
	 * functions */
	systick_timer_on(1); /* 1ms */
	systick_start();
}

/* itoa:  convert n to characters in s */
 void itoa(int n, char s[])
 {
     int i, sign;
 
     if ((sign = n) < 0)  /* record sign */
         n = -n;          /* make n positive */
     i = 0;
     do {       /* generate digits in reverse order */
         s[i++] = n % 10 + '0';   /* get next digit */
     } while ((n /= 10) > 0);     /* delete it */
     if (sign < 0)
         s[i++] = '-';
     s[i] = '\0';
     reverse(s);
 }

 /* reverse:  reverse string s in place */
 void reverse(char s[])
 {
     int i, j;
     char c;
 
     for (i = 0, j = strlen(s)-1; i<j; i++, j--) {
         c = s[i];
         s[i] = s[j];
         s[j] = c;
     }
 }

// A simple atoi() function 
int atoi(char* str) 
{ 
    int res = 0; // Initialize result 
  
    // Iterate through all characters of input string and 
    // update result 
    for (int i = 0; str[i] != '\0'; ++i) 
        res = res * 10 + str[i] - '0'; 
  
    // return result. 
    return res; 
} 

int decrypt(int val) {
	char string[20];
	int i = 0;
	itoa(val, string);
	while(string[i] != '\0')
	{

		int bob = string[i] - '0';
		int digit = (bob - 2) % 10;
		if(digit < 0)
			digit = digit + 10;
		string[i] = digit + '0';
		bob = string[i] - '0';

		i++;
	}
	
	return atoi(string);
}

/* Define our fault handler. This one is not mandatory, the dummy fault handler
 * will be used when it's not overridden here.
 * Note : The default one does a simple infinite loop. If the watchdog is deactivated
 * the system will hang.
 */
void fault_info(const char* name, uint32_t len)
{
	uprintf(UART0, name);
	while (1);
}

static volatile int check_rx = 0;
void rf_rx_calback(uint32_t gpio)
{
	check_rx = 1;
}

static uint8_t rf_specific_settings[] = {
	CC1101_REGS(gdo_config[2]), 0x07, /* GDO_0 - Assert on CRC OK | Disable temp sensor */
	CC1101_REGS(gdo_config[0]), 0x2E, /* GDO_2 - FIXME : do something usefull with it for tests */
	CC1101_REGS(pkt_ctrl[0]), 0x0F, /* Accept all sync, CRC err auto flush, Append, Addr check and Bcast */
#if (RF_915MHz == 1)
	/* FIXME : Add here a define protected list of settings for 915MHz configuration */
#endif
};

/* RF config */
void rf_config(void)
{
	config_gpio(&cc1101_gdo0, LPC_IO_MODE_PULL_UP, GPIO_DIR_IN, 0);
	cc1101_init(0, &cc1101_cs_pin, &cc1101_miso_pin); /* ssp_num, cs_pin, miso_pin */
	/* Set default config */
	cc1101_config();
	/* And change application specific settings */
	cc1101_update_config(rf_specific_settings, sizeof(rf_specific_settings));
	set_gpio_callback(rf_rx_calback, &cc1101_gdo0, EDGE_RISING);
    cc1101_set_address(DEVICE_ADDRESS);
#ifdef DEBUG
	uprintf(UART0, "CC1101 RF link init done.\n\r");
#endif
}

void handle_rf_rx_data(void)
{
	uint8_t data[RF_BUFF_LEN];
	int8_t ret = 0;
	uint8_t status = 0;

	/* Check for received packet (and get it if any) */
	ret = cc1101_receive_packet(data, RF_BUFF_LEN, &status);
	/* Go back to RX mode */
	cc1101_enter_rx_mode();
	message msg_data;
	memcpy(&msg_data,&data[2],sizeof(message));
#ifdef DEBUG
/*
	uprintf(UART0, "RF: ret:%d, st: %d.\n\r", ret, status);
    uprintf(UART0, "RF: data lenght: %d.\n\r", data[0]);
    uprintf(UART0, "RF: destination: %x.\n\r", data[1]);
*/
	/* JSON PRINT*/
	uprintf(UART0, "{ \"LUMINOSITY\": %d, \"TEMPERATURE\": %d.%d, \"HUMIDITY\": %d.%d}\n\r",  
					decrypt(msg_data.lum),
					decrypt(msg_data.temp) / 10, decrypt(msg_data.temp) % 10,
					decrypt(msg_data.hum) / 10, decrypt(msg_data.hum) % 10);
    //uprintf(UART0, "RF: message: %c.\n\r", data[2]);
#endif
}

int validDisplayingConfiguration(char* order){
	return (strcmp(order, "LTH") == 0 || strcmp(order, "LHT") == 0 || strcmp(order, "HLT") == 0 
	|| strcmp(order, "HTL") == 0 || strcmp(order, "TLH") == 0 || strcmp(order, "THL") == 0);     
}

void handle_uart_commands(char * command)
{
	if(validDisplayingConfiguration(command))
	{
		uint32_t ordre;
		if(strcmp(command, "LTH") == 0)
			ordre = 231;
		else if(strcmp(command, "LHT") == 0)
			ordre = 213;
		else if(strcmp(command, "HTL") == 0)
			ordre = 132;
		else if(strcmp(command, "HLT") == 0)
			ordre = 123;
		else if(strcmp(command, "THL") == 0)
			ordre = 312;
		else if(strcmp(command, "TLH") == 0)
			ordre = 321;
		send_on_rf_test(ordre);
	}
	dtplug_protocol_release_old_packet(&uart_handle);
}

void send_on_rf(uint32_t data)
{
	uprintf(UART0, "%d\r\n", data);
	uint8_t cc_tx_data[sizeof(uint32_t) +2];
	cc_tx_data[0]=sizeof(uint32_t) +1;
	cc_tx_data[1]=NEIGHBOR_ADDRESS;

	memcpy(&cc_tx_data[2], &data, sizeof(uint32_t));

	/* Send */
	if (cc1101_tx_fifo_state() != 0) {
		cc1101_flush_tx_fifo();
	}

	cc1101_send_packet(cc_tx_data, sizeof(uint32_t));
	uprintf(UART0, "Message envoye\r\n");
}

static volatile message cc_tx_msg;
void send_on_rf_test(uint32_t ordre)
{
	message data;
	uint8_t cc_tx_data[sizeof(message)+2];
	cc_tx_data[0]=sizeof(message)+1;
	cc_tx_data[1]=NEIGHBOR_ADDRESS;
	/*char sensorValue[20];
	char humidity[20];
	char luminosity[20];
	char temperature[20];
	snprintf(sensorValue, 20, "%lu", cc_tx_msg.hum);
	strcpy(humidity, cesar_crypter_int(sensorValue));
	snprintf(sensorValue, 20, "%lu", cc_tx_msg.lum);
	strcpy(luminosity, cesar_crypter_int(sensorValue));
	snprintf(sensorValue, 20, "%lu", cc_tx_msg.temp);
	strcpy(temperature, cesar_crypter_int(sensorValue));*/

	data.hum = 8;
	data.lum = 9;
	data.temp = 10;
	data.ordre = ordre;
	uprintf(UART0, "Values sent :   TEMP : %d, LUM : %d, HUM: %d , ORDRE : %d\n\r", data.temp, data.lum, data.hum, data.ordre);
	memcpy(&cc_tx_data[2], &data, sizeof(message));
	/* Send */
	if (cc1101_tx_fifo_state() != 0) {
		cc1101_flush_tx_fifo();
	}
	cc1101_send_packet(cc_tx_data, sizeof(message)+2);
}

int main(void)
{
	system_init();
	uart_on(UART0, 115200, NULL);
	//i2c_on(I2C0, I2C_CLK_100KHz, I2C_MASTER);
	ssp_master_on(0, LPC_SSP_FRAME_SPI, 8, 4*1000*1000); /* bus_num, frame_type, data_width, rate */
	status_led_config(&status_led_green, &status_led_red);
	dtplug_protocol_set_dtplug_comm_uart(0, &uart_handle);
	
	
	/* Radio */
	rf_config();

	char * command = NULL;


	uprintf(UART0, "App started\n\r");

	while (1) {
		uint8_t status = 0;

		//send_on_rf(0);
		
		handle_uart_commands("LTH");

		/* Tell we are alive :) */
		chenillard(250);

		command = dtplug_protocol_get_next_packet_ok(&uart_handle);
		if (command != NULL) {
			handle_uart_commands(command);
		}

		/* Do not leave radio in an unknown or unwated state */
		do {
			status = (cc1101_read_status() & CC1101_STATE_MASK);
		} while (status == CC1101_STATE_TX);

		if (status != CC1101_STATE_RX) {
			static uint8_t loop = 0;
			loop++;
			if (loop > 10) {
				if (cc1101_rx_fifo_state() != 0) {
					cc1101_flush_rx_fifo();
				}
				cc1101_enter_rx_mode();
				loop = 0;
			}
			
		}
		if (check_rx == 1) {
			check_rx = 0;
			handle_rf_rx_data();
		}
		
	}
	return 0;
}