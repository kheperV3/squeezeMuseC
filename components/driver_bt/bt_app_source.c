#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "bt_app_core.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_console.h"
#include "esp_pthread.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/timers.h"
#include "argtable3/argtable3.h"
#include "platform_config.h"
#include "trace.h"

static const char * TAG = "bt_app_source";

extern int32_t 	output_bt_data(uint8_t *data, int32_t len);
extern void 	output_bt_tick(void);
extern char*	output_state_str(void);
extern bool		output_stopped(void);

int64_t connecting_timeout = 0;

static const char *  art_a2dp_connected[]={"\n",
		"           ___  _____  _____     _____                            _           _ _ ",
		"     /\\   |__ \\|  __ \\|  __ \\   / ____|                          | |         | | |",
		"    /  \\     ) | |  | | |__) | | |     ___  _ __  _ __   ___  ___| |_ ___  __| | |",
		"   / /\\ \\   / /| |  | |  ___/  | |    / _ \\| '_ \\| '_ \\ / _ \\/ __| __/ _ \\/ _` | |",
		"  / ____ \\ / /_| |__| | |      | |___| (_) | | | | | | |  __/ (__| ||  __/ (_| |_|",
		" /_/    \\_\\____|_____/|_|       \\_____\\___/|_| |_|_| |_|\\___|\\___|\\__\\___|\\__,_(_)\n",
		"\0"};
static const char * art_a2dp_connecting[]= {"\n",
		 "           ___  _____  _____     _____                            _   _                   ",
		 "     /\\   |__ \\|  __ \\|  __ \\   / ____|                          | | (_)                  ",
		 "    /  \\     ) | |  | | |__) | | |     ___  _ __  _ __   ___  ___| |_ _ _ __   __ _       ",
		 "   / /\\ \\   / /| |  | |  ___/  | |    / _ \\| '_ \\| '_ \\ / _ \\/ __| __| | '_ \\ / _` |      ",
		 "  / ____ \\ / /_| |__| | |      | |___| (_) | | | | | | |  __/ (__| |_| | | | | (_| |_ _ _ ",
		 " /_/    \\_\\____|_____/|_|       \\_____\\___/|_| |_|_| |_|\\___|\\___|\\__|_|_| |_|\\__, (_|_|_)",
		 "                                                                               __/ |       ",
		 "                                                                              |___/        \n",
		 "\0"};

static void bt_app_av_state_connecting(uint16_t event, void *param);


#define IS_A2DP_TIMER_OVER esp_timer_get_time() >= connecting_timeout

static void filter_inquiry_scan_result(esp_bt_gap_cb_param_t *param);

/* event for handler "bt_av_hdl_stack_up */
enum {
    BT_APP_EVT_STACK_UP = 0,
};

/* A2DP global state */
enum {
    APP_AV_STATE_IDLE,
    APP_AV_STATE_DISCOVERING,
    APP_AV_STATE_DISCOVERED,
    APP_AV_STATE_UNCONNECTED,
    APP_AV_STATE_CONNECTING,
    APP_AV_STATE_CONNECTED,
    APP_AV_STATE_DISCONNECTING,
};

char * APP_AV_STATE_DESC[] = {
	    "APP_AV_STATE_IDLE",
	    "APP_AV_STATE_DISCOVERING",
	    "APP_AV_STATE_DISCOVERED",
	    "APP_AV_STATE_UNCONNECTED",
	    "APP_AV_STATE_CONNECTING",
	    "APP_AV_STATE_CONNECTED",
	    "APP_AV_STATE_DISCONNECTING"
};



/* sub states of APP_AV_STATE_CONNECTED */

enum {
    APP_AV_MEDIA_STATE_IDLE,
    APP_AV_MEDIA_STATE_STARTING,
//	APP_AV_MEDIA_STATE_BUFFERING,
    APP_AV_MEDIA_STATE_STARTED,
    APP_AV_MEDIA_STATE_STOPPING,
	APP_AV_MEDIA_STATE_WAIT_DISCONNECT
};

#define BT_APP_HEART_BEAT_EVT                (0xff00)

/// handler for bluetooth stack enabled events
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);

/// callback function for A2DP source
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

/// callback function for A2DP source audio data stream
static void a2d_app_heart_beat(void *arg);

/// A2DP application state machine
static void bt_app_av_sm_hdlr(uint16_t event, void *param);

/* A2DP application state machine handler for each state */
static void bt_app_av_state_unconnected(uint16_t event, void *param);
static void bt_app_av_state_connecting(uint16_t event, void *param);
static void bt_app_av_state_connected(uint16_t event, void *param);
static void bt_app_av_state_disconnecting(uint16_t event, void *param);

static esp_bd_addr_t s_peer_bda = {0};
static uint8_t s_peer_bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
static int s_a2d_state = APP_AV_STATE_IDLE;
static int s_media_state = APP_AV_MEDIA_STATE_IDLE;
static uint32_t s_pkt_cnt = 0;
static TimerHandle_t s_tmr;

static struct {
	int control_delay;
	int connect_timeout_delay;
	char * sink_name;
} squeezelite_conf;	

void hal_bluetooth_init(const char * options)
{
	struct {
		struct arg_str *sink_name;
		struct arg_int *control_delay;
		struct arg_int *connect_timeout_delay;
		struct arg_end *end;
	} squeezelite_args;
	
	ESP_LOGD(TAG,"Initializing Bluetooth HAL");

	squeezelite_args.sink_name = arg_str1("n", "name", "<sink name>", "the name of the bluetooth to connect to");
	squeezelite_args.control_delay = arg_int0("d", "delay", "<control delay>", "the delay between each pass at the A2DP control loop");
	squeezelite_args.connect_timeout_delay = arg_int0("t","timeout", "<timeout>", "the timeout duration for connecting to the A2DP sink");
	squeezelite_args.end = arg_end(2);

	ESP_LOGD(TAG,"Copying parameters");
	char * opts = strdup(options);
	char **argv = malloc(sizeof(char**)*15);

	size_t argv_size=15;

	// change parms so ' appear as " for parsing the options
	for (char* p = opts; (p = strchr(p, '\'')); ++p) *p = '"';
	ESP_LOGD(TAG,"Splitting arg line: %s", opts);

	argv_size = esp_console_split_argv(opts, argv, argv_size);
	ESP_LOGD(TAG,"Parsing parameters");
	int nerrors = arg_parse(argv_size , argv, (void **) &squeezelite_args);
	if (nerrors != 0) {
		ESP_LOGD(TAG,"Parsing Errors");
		arg_print_errors(stdout, squeezelite_args.end, "BT");
		arg_print_glossary_gnu(stdout, (void **) &squeezelite_args);
		free(opts);
		free(argv);
		return;
	}
	if(squeezelite_args.sink_name->count == 0)
	{
		squeezelite_conf.sink_name = config_alloc_get_default(NVS_TYPE_STR, "a2dp_sink_name", CONFIG_A2DP_SINK_NAME, 0);
    	if(squeezelite_conf.sink_name  == NULL){
    		ESP_LOGW(TAG,"Unable to retrieve the a2dp sink name from nvs");
    		squeezelite_conf.sink_name = strdup(CONFIG_A2DP_SINK_NAME);
    	}
	} else {
		squeezelite_conf.sink_name=strdup(squeezelite_args.sink_name->sval[0]);
	}
	if(squeezelite_args.connect_timeout_delay->count == 0)
	{
		ESP_LOGD(TAG,"Using default connect timeout");
		squeezelite_conf.connect_timeout_delay=CONFIG_A2DP_CONNECT_TIMEOUT_MS;
	} else {
		squeezelite_conf.connect_timeout_delay=squeezelite_args.connect_timeout_delay->ival[0];
	}
	if(squeezelite_args.control_delay->count == 0)
	{
		ESP_LOGD(TAG,"Using default control delay");
		squeezelite_conf.control_delay=CONFIG_A2DP_CONTROL_DELAY_MS;
	} else {
		squeezelite_conf.control_delay=squeezelite_args.control_delay->ival[0];
	}	
	ESP_LOGD(TAG,"Freeing options");
	free(argv);
	free(opts);

	/*
	 * Bluetooth audio source init Start
	 */
	//running_test = false;
	ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

	if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
		ESP_LOGE(TAG,"%s initialize controller failed\n", __func__);
		return;
	}

	if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
		ESP_LOGE(TAG,"%s enable controller failed\n", __func__);
		return;
	}

	if (esp_bluedroid_init() != ESP_OK) {
		ESP_LOGE(TAG,"%s initialize bluedroid failed\n", __func__);
		return;
	}

	if (esp_bluedroid_enable() != ESP_OK) {
		ESP_LOGE(TAG,"%s enable bluedroid failed\n", __func__);
		return;
	}
   /* create application task */
	bt_app_task_start_up();

	/* Bluetooth device name, connection mode and profile set up */
	bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_EVT_STACK_UP, NULL, 0, NULL);

	#if (CONFIG_BT_SSP_ENABLED == true)
	/* Set default parameters for Secure Simple Pairing */
	esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
	esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
	esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
	#endif

	/*
	 * Set default parameters for Legacy Pairing
	 * Use variable pin, input pin code when pairing
	 */
	esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
	esp_bt_pin_code_t pin_code;
	esp_bt_gap_set_pin(pin_type, 0, pin_code);

}

void hal_bluetooth_stop(void) {
	/* this still does not work, can't figure out how to stop properly this BT stack */
	bt_app_task_shut_down();
	ESP_LOGI(TAG, "bt_app_task shutdown successfully");	
	if (esp_bluedroid_disable() != ESP_OK) return;
    ESP_LOGI(TAG, "esp_bluedroid_disable called successfully");
    if (esp_bluedroid_deinit() != ESP_OK) return;
    ESP_LOGI(TAG, "esp_bluedroid_deinit called successfully");
    if (esp_bt_controller_disable() != ESP_OK) return;
    ESP_LOGI(TAG, "esp_bt_controller_disable called successfully");
    if (esp_bt_controller_deinit() != ESP_OK) return;
	ESP_LOGI(TAG, "bt stopped successfully");
}	

static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    bt_app_work_dispatch(bt_app_av_sm_hdlr, event, param, sizeof(esp_a2d_cb_param_t), NULL);
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{

    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        filter_inquiry_scan_result(param);
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED)
        {
            if (s_a2d_state == APP_AV_STATE_DISCOVERED)
            {
            	ESP_LOGI(TAG,"Discovery completed.  Ready to start connecting to %s. ",s_peer_bdname);
            	s_a2d_state = APP_AV_STATE_UNCONNECTED;
            }
            else
            {
                // not discovered, continue to discover
                ESP_LOGI(TAG,"Device discovery failed, continue to discover...");
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
            }
        }
        else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(TAG,"Discovery started.");
        }
        else
        {
        	ESP_LOGD(TAG,"This shouldn't happen.  Discovery has only 2 states (for now).");
        }
        break;
    }
    case ESP_BT_GAP_RMT_SRVCS_EVT:
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_BT_GAP_RMT_SRVCS_EVT));
    	break;
    case ESP_BT_GAP_RMT_SRVC_REC_EVT:
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_BT_GAP_RMT_SRVC_REC_EVT));
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
    	if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG,"authentication success: %s", param->auth_cmpl.device_name);
            //esp_log_buffer_hex(param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        } else {
            ESP_LOGE(TAG,"authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT: {
    	ESP_LOGI(TAG,"ESP_BT_GAP_PIN_REQ_EVT min_16_digit:%d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            ESP_LOGI(TAG,"Input pin code: 0000 0000 0000 0000");
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            ESP_LOGI(TAG,"Input pin code: 1234");
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '1';
            pin_code[1] = '2';
            pin_code[2] = '3';
            pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }

#if (CONFIG_BT_SSP_ENABLED == true)
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG,"ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %d", param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG,"ESP_BT_GAP_KEY_NOTIF_EVT passkey:%d", param->key_notif.passkey);
        break;
        ESP_LOGI(TAG,"ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
#endif

    default: {
        ESP_LOGI(TAG,"event: %d", event);
        break;
    }
    }
    return;
}

static void a2d_app_heart_beat(void *arg)
{
    bt_app_work_dispatch(bt_app_av_sm_hdlr, BT_APP_HEART_BEAT_EVT, NULL, 0, NULL);
}

static void bt_app_av_sm_hdlr(uint16_t event, void *param)
{
    switch (s_a2d_state) {
    case APP_AV_STATE_DISCOVERING:
    	ESP_LOGV(TAG,"state %s, evt 0x%x, output state: %s", APP_AV_STATE_DESC[s_a2d_state], event, output_state_str());
    	break;
    case APP_AV_STATE_DISCOVERED:
    	ESP_LOGV(TAG,"state %s, evt 0x%x, output state: %s", APP_AV_STATE_DESC[s_a2d_state], event, output_state_str());
        break;
    case APP_AV_STATE_UNCONNECTED:
        bt_app_av_state_unconnected(event, param);
        break;
    case APP_AV_STATE_CONNECTING:
        bt_app_av_state_connecting(event, param);
        break;
    case APP_AV_STATE_CONNECTED:
        bt_app_av_state_connected(event, param);
        break;
    case APP_AV_STATE_DISCONNECTING:
        bt_app_av_state_disconnecting(event, param);
        break;
    default:
        ESP_LOGE(TAG,"%s invalid state %d", __func__, s_a2d_state);
        break;
    }
}

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}
static bool get_name_from_eir(uint8_t *eir, uint8_t *bdname, uint8_t *bdname_len)
{
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        return false;
    }

    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }

    return false;
}

static void filter_inquiry_scan_result(esp_bt_gap_cb_param_t *param)
{
    char bda_str[18];
    uint32_t cod = 0;
    int32_t rssi = -129; /* invalid value */
    uint8_t *eir = NULL;
    uint8_t nameLen = 0;
    esp_bt_gap_dev_prop_t *p;
    if(s_a2d_state != APP_AV_STATE_DISCOVERING)
    {
    	// Ignore messages that might have been queued already
    	// when we've discovered the target device.
    	return;
    }
    memset(s_peer_bdname, 0x00,sizeof(s_peer_bdname));

    ESP_LOGI(TAG,"\n=======================\nScanned device: %s", bda2str(param->disc_res.bda, bda_str, 18));
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        p = param->disc_res.prop + i;
        switch (p->type) {
        case ESP_BT_GAP_DEV_PROP_COD:
            cod = *(uint32_t *)(p->val);
            ESP_LOGI(TAG,"-- Class of Device: 0x%x", cod);
            break;
        case ESP_BT_GAP_DEV_PROP_RSSI:
            rssi = *(int8_t *)(p->val);
            ESP_LOGI(TAG,"-- RSSI: %d", rssi);
            break;
        case ESP_BT_GAP_DEV_PROP_EIR:
            eir = (uint8_t *)(p->val);
            ESP_LOGI(TAG,"-- EIR: %u", *eir);
            break;
        case ESP_BT_GAP_DEV_PROP_BDNAME:
            nameLen = (p->len > ESP_BT_GAP_MAX_BDNAME_LEN) ? ESP_BT_GAP_MAX_BDNAME_LEN : (uint8_t)p->len;
            memcpy(s_peer_bdname, (uint8_t *)(p->val), nameLen);
            s_peer_bdname[nameLen] = '\0';
            ESP_LOGI(TAG,"-- Name: %s", s_peer_bdname);
            break;
        default:
            break;
        }
    }
    if (!esp_bt_gap_is_valid_cod(cod)){
    /* search for device with MAJOR service class as "rendering" in COD */
    	ESP_LOGI(TAG,"--Invalid class of device. Skipping.\n");
    	return;
    }
    else if (!(esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING))
    {
    	ESP_LOGI(TAG,"--Not a rendering device. Skipping.\n");
    	return;
    }


    /* search for device named "ESP_SPEAKER" in its extended inqury response */
    if (eir) {
    	ESP_LOGI(TAG,"--Getting details from eir.\n");
        get_name_from_eir(eir, s_peer_bdname, NULL);
        ESP_LOGI(TAG,"--Device name is %s\n",s_peer_bdname);
    }

    if (strcmp((char *)s_peer_bdname, squeezelite_conf.sink_name) == 0) {
    	ESP_LOGI(TAG,"Found a target device! address %s, name %s", bda_str, s_peer_bdname);
    	ESP_LOGI(TAG,"=======================\n");
        if(esp_bt_gap_cancel_discovery()!=ESP_ERR_INVALID_STATE)
        {
        	ESP_LOGI(TAG,"Cancel device discovery ...");
			memcpy(s_peer_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
        	s_a2d_state = APP_AV_STATE_DISCOVERED;
        }
        else
        {
        	ESP_LOGE(TAG,"Cancel device discovery failed...");
        }
    }
    else
    {
    	ESP_LOGI(TAG,"Not the device we are looking for (%s). Continuing scan", squeezelite_conf.sink_name);
    }
}

static void bt_av_hdl_stack_evt(uint16_t event, void *p_param)
{

    switch (event) {
    case BT_APP_EVT_STACK_UP: {
    	ESP_LOGI(TAG,"BT Stack going up.");
        /* set up device name */


        char * a2dp_dev_name = 	config_alloc_get_default(NVS_TYPE_STR, "a2dp_dev_name", CONFIG_A2DP_DEV_NAME, 0);
    	if(a2dp_dev_name  == NULL){
    		ESP_LOGW(TAG,"Unable to retrieve the a2dp device name from nvs");
    		esp_bt_dev_set_device_name(CONFIG_A2DP_DEV_NAME);
    	}
    	else {
    		esp_bt_dev_set_device_name(a2dp_dev_name);
    		free(a2dp_dev_name);
    	}

        ESP_LOGI(TAG,"Preparing to connect");

        /* register GAP callback function */
        esp_bt_gap_register_callback(bt_app_gap_cb);

        /* initialize A2DP source */
        esp_a2d_register_callback(&bt_app_a2d_cb);
        esp_a2d_source_register_data_callback(&output_bt_data);
        esp_a2d_source_init();

        /* set discoverable and connectable mode */
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

        /* start device discovery */
        ESP_LOGI(TAG,"Starting device discovery...");
        s_a2d_state = APP_AV_STATE_DISCOVERING;
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);

        /* create and start heart beat timer */
        do {
            int tmr_id = 0;
            uint16_t ctr_delay_ms=CONFIG_A2DP_CONTROL_DELAY_MS;
            char * value = config_alloc_get_default(NVS_TYPE_STR, "a2dp_ctrld", STR(CONFIG_A2DP_CONNECT_TIMEOUT_MS), 0);
			if(value!=NULL){
				ESP_LOGI(TAG,  "A2DP ctrl delay: %s", value);
				ctr_delay_ms=atoi(value);
			}
			FREE_AND_NULL(value);

            s_tmr = xTimerCreate("connTmr", ( ctr_delay_ms/ portTICK_RATE_MS),
                               pdTRUE, (void *)tmr_id, a2d_app_heart_beat);
            xTimerStart(s_tmr, portMAX_DELAY);
        } while (0);
        break;
    }
    default:
    	ESP_LOGE(TAG,"%s unhandled evt %d", __func__, event);
        break;
    }
}

static void bt_app_av_media_proc(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;
    switch (s_media_state) {
    case APP_AV_MEDIA_STATE_IDLE: {
    	if (event == BT_APP_HEART_BEAT_EVT) {
            if(!output_stopped())
            {
            	ESP_LOGI(TAG,"Output state is %s, Checking if A2DP is ready.", output_state_str());
            	esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
            }

        } else if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
        	a2d = (esp_a2d_cb_param_t *)(param);
			if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
					a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS
					) {
				ESP_LOGI(TAG,"a2dp media ready, starting playback!");
				s_media_state = APP_AV_MEDIA_STATE_STARTING;
				esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
			}
        }
        break;
    }

    case APP_AV_MEDIA_STATE_STARTING: {
    	if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            	ESP_LOGI(TAG,"a2dp media started successfully.");
                s_media_state = APP_AV_MEDIA_STATE_STARTED;
            } else {
                // not started succesfully, transfer to idle state
            	ESP_LOGI(TAG,"a2dp media start failed.");
                s_media_state = APP_AV_MEDIA_STATE_IDLE;
            }
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STARTED: {
        if (event == BT_APP_HEART_BEAT_EVT) {
        	if(output_stopped()) {
        		ESP_LOGI(TAG,"Output state is %s. Stopping a2dp media ...", output_state_str());
                s_media_state = APP_AV_MEDIA_STATE_STOPPING;
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);
            } else {
				output_bt_tick();	
        	}
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STOPPING: {
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(APP_AV_MEDIA_STATE_STOPPING));
        if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_STOP &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(TAG,"a2dp media stopped successfully...");
               	s_media_state = APP_AV_MEDIA_STATE_IDLE;
            } else {
                ESP_LOGI(TAG,"a2dp media stopping...");
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);
            }
        }
        break;
    }

    case APP_AV_MEDIA_STATE_WAIT_DISCONNECT:{
    	esp_a2d_source_disconnect(s_peer_bda);
		s_a2d_state = APP_AV_STATE_DISCONNECTING;
		ESP_LOGI(TAG,"a2dp disconnecting...");
    }
    }
}

static void bt_app_av_state_unconnected(uint16_t event, void *param)
{
	switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_A2D_CONNECTION_STATE_EVT));
    	// this could happen if connection was established
    	// right after we timed out. Pass the call down to the connecting
    	// handler.
    	esp_a2d_cb_param_t *a2d = (esp_a2d_cb_param_t *)(param);
    	if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED){
    		bt_app_av_state_connecting(event, param);
    	}

    	break;
    case ESP_A2D_AUDIO_STATE_EVT:
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_A2D_AUDIO_STATE_EVT));

    	break;
    case ESP_A2D_AUDIO_CFG_EVT:
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_A2D_AUDIO_CFG_EVT));
    	break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_A2D_MEDIA_CTRL_ACK_EVT));
    	break;
    case BT_APP_HEART_BEAT_EVT: {
        switch (esp_bluedroid_get_status()) {
		case ESP_BLUEDROID_STATUS_UNINITIALIZED:
			ESP_LOGV(TAG,"BlueDroid Status is ESP_BLUEDROID_STATUS_UNINITIALIZED.");
			break;
		case ESP_BLUEDROID_STATUS_INITIALIZED:
			ESP_LOGV(TAG,"BlueDroid Status is ESP_BLUEDROID_STATUS_INITIALIZED.");
			break;
		case ESP_BLUEDROID_STATUS_ENABLED:
			ESP_LOGV(TAG,"BlueDroid Status is ESP_BLUEDROID_STATUS_ENABLED.");
			break;
		default:
			break;
		}
		for(uint8_t l=0;art_a2dp_connecting[l][0]!='\0';l++){
			ESP_LOGI(TAG,"%s",art_a2dp_connecting[l]);
		}
		ESP_LOGI(TAG,"Device: %s", s_peer_bdname);
		int64_t connecting_timeout_offset=CONFIG_A2DP_CONNECT_TIMEOUT_MS;
		if(esp_a2d_source_connect(s_peer_bda)==ESP_OK) {
			char * value = config_alloc_get_default(NVS_TYPE_STR, "a2dp_ctmt", STR(CONFIG_A2DP_CONNECT_TIMEOUT_MS), 0);
			if(value!=NULL){
				ESP_LOGI(TAG,  "A2DP pairing timeout: %s", value);
				connecting_timeout_offset=atoi(value);
			}
			FREE_AND_NULL(value);

			connecting_timeout = esp_timer_get_time() +(connecting_timeout_offset * 1000);
			s_a2d_state = APP_AV_STATE_CONNECTING;
		}
		else {
			// there was an issue connecting... continue to discover
			ESP_LOGE(TAG,"Attempt at connecting failed, restart at discover...");
			esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
        }
        break;
    }
    default:
    	ESP_LOGE(TAG,"%s unhandled evt %d", __func__, event);
        break;
    }
}

static void bt_app_av_state_connecting(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_a2d_state =  APP_AV_STATE_CONNECTED;
            s_media_state = APP_AV_MEDIA_STATE_IDLE;

			ESP_LOGD(TAG,"Setting scan mode to ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE");
            esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            ESP_LOGD(TAG,"Done setting scan mode. App state is now CONNECTED and media state IDLE.");
			for(uint8_t l=0;art_a2dp_connected[l][0]!='\0';l++){
				ESP_LOGI(TAG,"%s",art_a2dp_connected[l]);
			}
        } else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_a2d_state =  APP_AV_STATE_UNCONNECTED;
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_A2D_AUDIO_STATE_EVT));
    	break;
    case ESP_A2D_AUDIO_CFG_EVT:
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_A2D_AUDIO_CFG_EVT));
    	break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_A2D_MEDIA_CTRL_ACK_EVT));
    	break;
    case BT_APP_HEART_BEAT_EVT:
    	if (IS_A2DP_TIMER_OVER)
    	{
            s_a2d_state = APP_AV_STATE_UNCONNECTED;
            ESP_LOGW(TAG,"A2DP Connect time out!  Setting state to Unconnected. ");
        }
    	ESP_LOGV(TAG,"BT_APP_HEART_BEAT_EVT");
        break;
    default:
        ESP_LOGE(TAG,"%s unhandled evt %d", __func__, event);
        break;
    }
}


static void bt_app_av_state_connected(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
    	a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG,"a2dp disconnected");
            s_a2d_state = APP_AV_STATE_UNCONNECTED;
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_A2D_AUDIO_STATE_EVT));
        a2d = (esp_a2d_cb_param_t *)(param);
        if (ESP_A2D_AUDIO_STATE_STARTED == a2d->audio_stat.state) {
            s_pkt_cnt = 0;
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT:
        // not suppposed to occur for A2DP source
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_A2D_AUDIO_CFG_EVT));
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:{
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_A2D_MEDIA_CTRL_ACK_EVT));
            bt_app_av_media_proc(event, param);
            break;
        }
    case BT_APP_HEART_BEAT_EVT: {
    	ESP_LOGV(TAG,QUOTE(BT_APP_HEART_BEAT_EVT));
        bt_app_av_media_proc(event, param);
        break;
    }
    default:
        ESP_LOGE(TAG,"%s unhandled evt %d", __func__, event);
        break;
    }
}

static void bt_app_av_state_disconnecting(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_A2D_CONNECTION_STATE_EVT));
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG,"a2dp disconnected");
            s_a2d_state =  APP_AV_STATE_UNCONNECTED;
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_A2D_AUDIO_STATE_EVT));
    	break;
    case ESP_A2D_AUDIO_CFG_EVT:
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_A2D_AUDIO_CFG_EVT));
    	break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(ESP_A2D_MEDIA_CTRL_ACK_EVT));
    	break;
    case BT_APP_HEART_BEAT_EVT:
    	ESP_LOG_DEBUG_EVENT(TAG,QUOTE(BT_APP_HEART_BEAT_EVT));
    	break;
    default:
        ESP_LOGE(TAG,"%s unhandled evt %d", __func__, event);
        break;
    }
}
