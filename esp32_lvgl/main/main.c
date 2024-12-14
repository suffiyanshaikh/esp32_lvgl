/* LVGL Example project
 *
 * Basic project to test LVGL on ESP32 based projects.
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include <esp_http_server.h>
#include "esp_event.h"
#include <time.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include <cJSON.h>

/* Littlevgl specific */
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#include "lvgl_helpers.h"
#include "main_def.h"

/*********************
 *      variables
 *********************/
esp_netif_t *netif;

uint8_t wifi_retry_num = 0;
bool wifi_restart_start;
bool wifi_connection = false;

char wifi_ssid[32] = "AIRCON";       // stored to nvs
char wifi_password[32] = "LT123456"; // stored

char *response_data = NULL;
size_t response_len = 0;
bool all_chunks_received = false;

/*********************
 *      DEFINES
 *********************/
#define SENSOR_THREAD "sensor_thread"
#define LV_TICK_PERIOD_MS 1

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void lv_tick_task(void *arg);
static void guiTask(void *pvParameter);
static void create_demo_application(void);
static void connect_wifi();
void wifi_run_reconnection();
void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void sensor_task(void *pvParameters);

#define LV_ATTRIBUTE_IMAGE_LT_LOGO

/**********************
 *   APPLICATION MAIN
 **********************/
void app_main()
{

    esp_err_t main_thread_err = nvs_flash_init();
    if (main_thread_err == ESP_ERR_NVS_NO_FREE_PAGES || main_thread_err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // If NVS flash is corrupted, erase and initialize again
        ESP_ERROR_CHECK(nvs_flash_erase());
        main_thread_err = nvs_flash_init();
    }

    /* If you want to use a task to create the graphic, you NEED to create a Pinned task
     * Otherwise there can be problem such as memory corruption and so on.
     * NOTE: When not using Wi-Fi nor Bluetooth you can pin the guiTask to core 0 */
    xTaskCreatePinnedToCore(guiTask, "gui", 4096 * 2, NULL, 0, NULL, 1);
    xTaskCreatePinnedToCore(sensor_task, "sensor_task", 1024 * 8, NULL, 0, NULL, 0); // 4KB
}

/* Creates a semaphore to handle concurrent call to lvgl stuff
 * If you wish to call *any* lvgl function from other threads/tasks
 * you should lock on the very same semaphore! */
SemaphoreHandle_t xGuiSemaphore;

static void guiTask(void *pvParameter)
{

    (void)pvParameter;
    xGuiSemaphore = xSemaphoreCreateMutex();

    lv_init();

    /* Initialize SPI or I2C bus used by the drivers */
    lvgl_driver_init();

    lv_color_t *buf1 = heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1 != NULL);

    /* Use double buffered when not working with monochrome displays */
#ifndef CONFIG_LV_TFT_DISPLAY_MONOCHROME
    lv_color_t *buf2 = heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf2 != NULL);
#else
    static lv_color_t *buf2 = NULL;
#endif

    static lv_disp_buf_t disp_buf;

    uint32_t size_in_px = DISP_BUF_SIZE;

#if defined CONFIG_LV_TFT_DISPLAY_CONTROLLER_IL3820 || defined CONFIG_LV_TFT_DISPLAY_CONTROLLER_JD79653A || defined CONFIG_LV_TFT_DISPLAY_CONTROLLER_UC8151D || defined CONFIG_LV_TFT_DISPLAY_CONTROLLER_SSD1306

    /* Actual size in pixels, not bytes. */
    size_in_px *= 8;
#endif

    /* Initialize the working buffer depending on the selected display.
     * NOTE: buf2 == NULL when using monochrome displays. */
    lv_disp_buf_init(&disp_buf, buf1, buf2, size_in_px);

    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = disp_driver_flush;

#if defined CONFIG_DISPLAY_ORIENTATION_PORTRAIT || defined CONFIG_DISPLAY_ORIENTATION_PORTRAIT_INVERTED
    disp_drv.rotated = 1;
#endif

    /* When using a monochrome display we need to register the callbacks:
     * - rounder_cb
     * - set_px_cb */
#ifdef CONFIG_LV_TFT_DISPLAY_MONOCHROME
    disp_drv.rounder_cb = disp_driver_rounder;
    disp_drv.set_px_cb = disp_driver_set_px;
#endif

    disp_drv.buffer = &disp_buf;
    lv_disp_drv_register(&disp_drv);

    /* Register an input device when enabled on the menuconfig */
#if CONFIG_LV_TOUCH_CONTROLLER != TOUCH_CONTROLLER_NONE
    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.read_cb = touch_driver_read;
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    lv_indev_drv_register(&indev_drv);
#endif

    /* Create and start a periodic timer interrupt to call lv_tick_inc */
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &lv_tick_task,
        .name = "periodic_gui"};
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, LV_TICK_PERIOD_MS * 1000));

    /* Create the demo application */
    create_demo_application();

    while (1)
    {
        /* Delay 1 tick (assumes FreeRTOS tick is 10ms */
        vTaskDelay(pdMS_TO_TICKS(10));

        /* Try to take the semaphore, call lvgl related function on success */
        if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY))
        {
            lv_task_handler();
            xSemaphoreGive(xGuiSemaphore);
        }
    }

    /* A task should NEVER return */
    free(buf1);
#ifndef CONFIG_LV_TFT_DISPLAY_MONOCHROME
    free(buf2);
#endif
    vTaskDelete(NULL);
}

static void create_demo_application(void)
{
    /* When using a monochrome display we only show "Hello World" centered on the
     * screen */
    // #if defined CONFIG_LV_TFT_DISPLAY_MONOCHROME || defined CONFIG_LV_TFT_DISPLAY_CONTROLLER_ST7735S

    /* use a pretty small demo for monochrome displays */
    /* Get the current screen  */

    /* Change the active screen's background color to white */
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);

    /*Create a Label on the currently active screen*/
    lv_obj_t *label1 = lv_label_create(scr, NULL);

    /* Create a style for the label */
    static lv_style_t style_label;
    lv_style_init(&style_label);

    /* Set properties for the style */
    lv_style_set_text_font(&style_label, LV_STATE_DEFAULT, &lv_font_montserrat_20); // Use a larger font size

    /* Apply the style to the label */
    lv_obj_add_style(label1, LV_LABEL_PART_MAIN, &style_label);

    /*Modify the Label's text*/
    lv_label_set_text(label1, "LT EMBEDDED LAB");

    /* Align the Label to the center
     * NULL means align on parent (which is the screen now)
     * 0, 0 at the end means an x, y offset after alignment*/
    lv_obj_align(label1, NULL, LV_ALIGN_IN_TOP_MID, 0, 20);

    LV_IMG_DECLARE(img_cogwheel_argb);

    lv_obj_t *img1 = lv_img_create(lv_scr_act(), NULL);
    lv_img_set_src(img1, &img_cogwheel_argb);
    lv_obj_align(img1, NULL, LV_ALIGN_CENTER, 0, -20);

    lv_scr_load(scr);

    vTaskDelay(1000);

    lv_obj_t *scr1 = lv_obj_create(NULL, NULL);
    lv_obj_t *label2 = lv_label_create(scr1, NULL);
    lv_obj_t *temp_label = lv_label_create(scr1, NULL);

    lv_obj_t *led1 = lv_led_create(scr1, NULL);

    char temp_data[100];

    sprintf(temp_data, "Temperature: %.2f C", 24.0);

    /* Apply the style to the label */
    lv_obj_add_style(label2, LV_LABEL_PART_MAIN, &style_label);

    /*Modify the Label's text*/
    lv_label_set_text(label2, "Live Weather Update");
    lv_label_set_text(temp_label, temp_data);

    /* Align the Label to the center
     * NULL means align on parent (which is the screen now)
     * 0, 0 at the end means an x, y offset after alignment*/
    lv_obj_align(label2, NULL, LV_ALIGN_IN_TOP_MID, -5, 20);
    lv_obj_align(temp_label, NULL, LV_ALIGN_IN_LEFT_MID, 20, -40);
    lv_obj_align(led1, NULL, LV_ALIGN_IN_TOP_RIGHT, 0, 0);

    lv_led_set_bright(led1, LV_LED_BRIGHT_MAX);
    lv_led_on(led1);

    lv_scr_load_anim(scr1, LV_SCR_LOAD_ANIM_OVER_LEFT, 1000, 500, true);
}

static void lv_tick_task(void *arg)
{
    (void)arg;

    lv_tick_inc(LV_TICK_PERIOD_MS);
}

void sensor_task(void *pvParameters)
{
    ESP_LOGI(SENSOR_THREAD, "Sensor thread up\n");

    connect_wifi();

    vTaskDelay(2000);

    if (wifi_connection == true)
    {
        openweather_api_http();
    }

    while (1)
    {
        vTaskDelay(1000);
        ESP_LOGI(SENSOR_THREAD, "Sensor thread live\n");
    }
}

static void connect_wifi()
{

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wifi_configuration = {};

    wifi_configuration.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    strncpy((char *)wifi_configuration.sta.ssid, wifi_ssid, strlen(wifi_ssid));
    strncpy((char *)wifi_configuration.sta.password, wifi_password, strlen(wifi_password));

    wifi_configuration.sta.scan_method = WIFI_FAST_SCAN;

    // memset(wifi.connected_wifi, 0, sizeof(wifi.connected_wifi));
    // strncpy(wifi.connected_wifi, wifi_ssid, strlen(wifi_ssid));

    esp_wifi_set_config(WIFI_IF_STA, &wifi_configuration);
    esp_wifi_start();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_connect();

    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(SENSOR_THREAD, "WiFi initialization finished. SSID:%s  password:%s\n", wifi_ssid, wifi_password);
}

void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(SENSOR_THREAD, "WIFI CONNECTING to ssid %s\n", wifi_ssid);
    }
    else if (event_id == WIFI_EVENT_STA_CONNECTED)
    {

        wifi_retry_num = 0; // Reset retry number on successful connection
        // wifi_restart_start = false; // reset restart timer
        // wifi_restart_retry = 0;
        wifi_connection = true;
    }
    else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
    {

        ESP_LOGI(SENSOR_THREAD, "WiFi lost connection\n");
        wifi_run_reconnection();
        wifi_connection = false;
    }
    else if (event_id == IP_EVENT_STA_GOT_IP)
    {

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(SENSOR_THREAD, "GOT IP Address: " IPSTR "\n", IP2STR(&event->ip_info.ip));
    }
}

void wifi_run_reconnection()
{

    // start counting wifi down_time
    // if (!wifi_restart_start)
    // {
    //     wifi_restart_timer = (esp_timer_get_time() / 1000);
    //     wifi_restart_start = true;
    // }

    if (wifi_retry_num < 3)
    {

        esp_wifi_connect();
        wifi_retry_num++;
        ESP_LOGI(SENSOR_THREAD, "Retrying to Connect...\n");
    }
}

void get_temp_pressure_humidity(const char *json_string)
{

    cJSON *root = cJSON_Parse(json_string);
    cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "main");

    float temp = cJSON_GetObjectItemCaseSensitive(obj, "temp")->valuedouble;
    int pressure = cJSON_GetObjectItemCaseSensitive(obj, "pressure")->valueint;
    int humidity = cJSON_GetObjectItemCaseSensitive(obj, "humidity")->valueint;
    printf("Temperature: %0.00f°F\nPressure: %d hPa\nHumidity: %d%%\n", temp, pressure, humidity);

    cJSON_Delete(root);
    free(response_data);
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_DATA:
        // Resize the buffer to fit the new chunk of data
        response_data = realloc(response_data, response_len + evt->data_len);
        memcpy(response_data + response_len, evt->data, evt->data_len);
        response_len += evt->data_len;
        break;
    case HTTP_EVENT_ON_FINISH:
        all_chunks_received = true;
        ESP_LOGI("OpenWeatherAPI", "Received data: %s", response_data);
        get_temp_pressure_humidity(response_data);
        break;
    default:
        break;
    }
    return ESP_OK;
}

void openweather_api_http()
{

    char open_weather_map_url[200];
    snprintf(open_weather_map_url,
             sizeof(open_weather_map_url),
             "%s%s%s%s%s%s",
             "http://api.openweathermap.org/data/2.5/weather?q=",
             city,
             ",",
             country_code,
             "&APPID=",
             open_weather_map_api_key);

    esp_http_client_config_t config = {
        .url = open_weather_map_url,
        .method = HTTP_METHOD_GET,
        .event_handler = _http_event_handler,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200)
        {
            ESP_LOGI(SENSOR_THREAD, "Message sent Successfully");
        }
        else
        {
            ESP_LOGI(SENSOR_THREAD, "Message sent Failed");
        }
    }
    else
    {
        ESP_LOGI(SENSOR_THREAD, "Message sent Failed");
    }
    esp_http_client_cleanup(client);
}
