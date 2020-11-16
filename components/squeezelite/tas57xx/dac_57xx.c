/* 
 *  Squeezelite for esp32
 *
 *  (c) Sebastien 2019
 *      Philippe G. 2019, philippe_44@outlook.com
 *
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 *
 */
 
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "adac.h"

#define TAS575x 0x98
#define TAS578x	0x90

static const char TAG[] = "TAS575x/8x";

static bool init(char *config, int i2c_port_num, i2s_config_t *i2s_config);
static void deinit(void);
static void speaker(bool active);
static void headset(bool active);
static bool volume(unsigned left, unsigned right);
static void power(adac_power_e mode);

const struct adac_s dac_tas57xx = { "TAS57xx", init, deinit, power, speaker, headset, volume };

struct tas57xx_cmd_s {
	uint8_t reg;
	uint8_t value;
};

static const struct tas57xx_cmd_s tas57xx_init_sequence[] = {
    { 0x00, 0x00 },		// select page 0
    { 0x02, 0x10 },		// standby
    { 0x0d, 0x10 },		// use SCK for PLL
	{ 0x25, 0x08 },		// ignore SCK halt 
	{ 0x08, 0x10 },		// Mute control enable (from TAS5780)
	{ 0x54, 0x02 },		// Mute output control (from TAS5780)
	{ 0x02, 0x00 },		// restart
	{ 0xff, 0xff }		// end of table
};

// matching orders
typedef enum { TAS57_ACTIVE = 0, TAS57_STANDBY, TAS57_DOWN, TAS57_ANALOGUE_OFF, TAS57_ANALOGUE_ON, TAS57_VOLUME } dac_cmd_e;

static const struct tas57xx_cmd_s tas57xx_cmd[] = {
	{ 0x02, 0x00 },	// TAS57_ACTIVE
	{ 0x02, 0x10 },	// TAS57_STANDBY
	{ 0x02, 0x01 },	// TAS57_DOWN
	{ 0x56, 0x10 },	// TAS57_ANALOGUE_OFF
	{ 0x56, 0x00 },	// TAS57_ANALOGUE_ON
};

static uint8_t tas57_addr;
static int i2c_port;

static void dac_cmd(dac_cmd_e cmd, ...);
static int tas57_detect(void);

/****************************************************************************************
 * init
 */
static bool init(char *config, int i2c_port_num, i2s_config_t *i2s_config) {	 
	char *p;	
	i2c_port = i2c_port_num;
		
	// configure i2c
	i2c_config_t i2c_config = {
			.mode = I2C_MODE_MASTER,
			.sda_io_num = -1,
			.sda_pullup_en = GPIO_PULLUP_ENABLE,
			.scl_io_num = -1,
			.scl_pullup_en = GPIO_PULLUP_ENABLE,
			.master.clk_speed = 250000,
		};

	if ((p = strcasestr(config, "sda")) != NULL) i2c_config.sda_io_num = atoi(strchr(p, '=') + 1);
	if ((p = strcasestr(config, "scl")) != NULL) i2c_config.scl_io_num = atoi(strchr(p, '=') + 1);
	
	i2c_param_config(i2c_port, &i2c_config);
	i2c_driver_install(i2c_port, I2C_MODE_MASTER, false, false, false);
		
	// find which TAS we are using (if any)
	tas57_addr = tas57_detect();
	
	if (!tas57_addr) {
		ESP_LOGW(TAG, "No TAS57xx detected");
		i2c_driver_delete(i2c_port);
		return false;
	}

	i2c_cmd_handle_t i2c_cmd = i2c_cmd_link_create();
	
	for (int i = 0; tas57xx_init_sequence[i].reg != 0xff; i++) {
		i2c_master_start(i2c_cmd);
		i2c_master_write_byte(i2c_cmd, tas57_addr | I2C_MASTER_WRITE, I2C_MASTER_NACK);
		i2c_master_write_byte(i2c_cmd, tas57xx_init_sequence[i].reg, I2C_MASTER_NACK);
		i2c_master_write_byte(i2c_cmd, tas57xx_init_sequence[i].value, I2C_MASTER_NACK);
		ESP_LOGD(TAG, "i2c write %x at %u", tas57xx_init_sequence[i].reg, tas57xx_init_sequence[i].value);
	}

	i2c_master_stop(i2c_cmd);	
	esp_err_t res = i2c_master_cmd_begin(i2c_port, i2c_cmd, 500 / portTICK_RATE_MS);
    i2c_cmd_link_delete(i2c_cmd);

	ESP_LOGI(TAG, "TAS57xx uses I2C sda:%d, scl:%d", i2c_config.sda_io_num, i2c_config.scl_io_num);
	
	if (res != ESP_OK) {
		ESP_LOGE(TAG, "could not intialize TAS57xx %d", res);
		return false;
	}	
	
	return true;
}	

/****************************************************************************************
 * init
 */
static void deinit(void)	{	 
	i2c_driver_delete(i2c_port);
}

/****************************************************************************************
 * change volume
 */
static bool volume(unsigned left, unsigned right) { 
	return false; 
}

/****************************************************************************************
 * power
 */
static void power(adac_power_e mode) {
	switch(mode) {
	case ADAC_STANDBY:
		dac_cmd(TAS57_STANDBY);
		break;
	case ADAC_ON:
		dac_cmd(TAS57_ACTIVE);
		break;		
	case ADAC_OFF:
		dac_cmd(TAS57_DOWN);
		break;				
	default:
		ESP_LOGW(TAG, "unknown DAC command");
		break;
	}
}

/****************************************************************************************
 * speaker
 */
static void speaker(bool active) {
	if (active) dac_cmd(TAS57_ANALOGUE_ON);
	else dac_cmd(TAS57_ANALOGUE_OFF);
} 

/****************************************************************************************
 * headset
 */
static void headset(bool active) { } 
 
/****************************************************************************************
 * DAC specific commands
 */
void dac_cmd(dac_cmd_e cmd, ...) {
	va_list args;
	esp_err_t ret = ESP_OK;
	
	va_start(args, cmd);
	i2c_cmd_handle_t i2c_cmd = i2c_cmd_link_create();

	switch(cmd) {
	case TAS57_VOLUME:
		ESP_LOGE(TAG, "DAC volume not handled yet");
		break;
	default:
		i2c_master_start(i2c_cmd);
		i2c_master_write_byte(i2c_cmd, tas57_addr | I2C_MASTER_WRITE, I2C_MASTER_NACK);
		i2c_master_write_byte(i2c_cmd, tas57xx_cmd[cmd].reg, I2C_MASTER_NACK);
		i2c_master_write_byte(i2c_cmd, tas57xx_cmd[cmd].value, I2C_MASTER_NACK);
		i2c_master_stop(i2c_cmd);	
		ret	= i2c_master_cmd_begin(i2c_port, i2c_cmd, 50 / portTICK_RATE_MS);
	}
	
    i2c_cmd_link_delete(i2c_cmd);
	
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "could not intialize TAS57xx %d", ret);
	}

	va_end(args);
}

/****************************************************************************************
 * TAS57 detection
 */
static int tas57_detect(void) {
	uint8_t data, addr[] = {TAS578x, TAS575x};
	int ret;
	
	for (int i = 0; i < sizeof(addr); i++) {
		i2c_cmd_handle_t i2c_cmd = i2c_cmd_link_create();

		i2c_master_start(i2c_cmd);
		i2c_master_write_byte(i2c_cmd, addr[i] | I2C_MASTER_WRITE, I2C_MASTER_NACK);
		i2c_master_write_byte(i2c_cmd, 00, I2C_MASTER_NACK);
		
		i2c_master_start(i2c_cmd);			
		i2c_master_write_byte(i2c_cmd, addr[i] | I2C_MASTER_READ, I2C_MASTER_NACK);
		i2c_master_read_byte(i2c_cmd, &data, I2C_MASTER_NACK);
		
		i2c_master_stop(i2c_cmd);	
		ret	= i2c_master_cmd_begin(i2c_port, i2c_cmd, 50 / portTICK_RATE_MS);
		i2c_cmd_link_delete(i2c_cmd);	
		
		if (ret == ESP_OK) {
			ESP_LOGI(TAG, "Detected TAS @0x%x", addr[i]);
			return addr[i];
		}	
	}	
	
	return 0;
}

