// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"

#include "esp_bt.h"

#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "driver/i2s.h"

#include "esp32-hal-bt.h"

extern "C" {
#include "bt_app_core.h"
#include "bt_app_av.h"
}

/* event for handler "bt_av_hdl_stack_up */
enum {
  BT_APP_EVT_STACK_UP = 0,
};

/* handler for bluetooth stack enabled events */
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);



void setup()
{


  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_PDM ),                                  // Only TX
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,                           //2-channels
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 6,
    .dma_buf_len = 64,                                                      //
    .use_apll = false,
    //.fixed_mclk = -1,
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);

  /*i2s_pin_config_t pin_config = {
    .bck_io_num = 26,
    .ws_io_num = 25,
    .data_out_num = 27,
    .data_in_num = -1                                                       //Not used
    };*/
  i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);
  i2s_set_pin(I2S_NUM_0, NULL);

  

 
  //ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));


 esp_err_t err;
  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
 
  
  /* if ((err = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
    ESP_LOGE(BT_AV_TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name(ret));
    return;
  }

  if ((err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
    ESP_LOGE(BT_AV_TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));
    return;
  }*/

  btStart();

  if ((err = esp_bluedroid_init()) != ESP_OK) {
    ESP_LOGE(BT_AV_TAG, "%s initialize bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
    return;
  }

  if ((err = esp_bluedroid_enable()) != ESP_OK) {
    ESP_LOGE(BT_AV_TAG, "%s enable bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
    return;
  }

  /* create application task */
  bt_app_task_start_up();

  /* Bluetooth device name, connection mode and profile set up */
  bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_EVT_STACK_UP, NULL, 0, NULL);
}


static void bt_av_hdl_stack_evt(uint16_t event, void *p_param)
{
  ESP_LOGD(BT_AV_TAG, "%s evt %d", __func__, event);
  switch (event) {
    case BT_APP_EVT_STACK_UP: {
        /* set up device name */
        const char *dev_name = "ESP_SPEAKER";
        esp_bt_dev_set_device_name(dev_name);

        /* initialize A2DP sink */
        esp_a2d_register_callback(&bt_app_a2d_cb);
        esp_a2d_sink_register_data_callback(bt_app_a2d_data_cb);
        esp_a2d_sink_init();

        /* initialize AVRCP controller */
        esp_avrc_ct_init();
        esp_avrc_ct_register_callback(bt_app_rc_ct_cb);

        /* set discoverable and connectable mode, wait to be connected */
        esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
        break;
      }
    default:
      ESP_LOGE(BT_AV_TAG, "%s unhandled evt %d", __func__, event);
      break;
  }
}

void loop() {}