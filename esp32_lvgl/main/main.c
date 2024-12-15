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
bool display_driver_state = false;
bool weather_update = false;

int64_t weather_update_timer;

struct weather_params weather_data = {
    .timestamp = 1734281446, // Epoch time
    .Temperature = 30.99,
    .Temperature_Max = 30.99,
    .Temperature_Min = 30.99,
    .Humidity = 33,
    .Pressure = 1012,
    .visiblity = 3,
    .date_time = "15/12/2024 - 04:50 PM" // Default string
};

lv_obj_t *main_scr; //main screen
lv_obj_t *data_scr; //display screen


static lv_style_t header_style_label;
static lv_style_t text_style_label;
static lv_style_t footer_text_style;
static lv_style_t led_style;

/*********************
 *      DEFINES
 *********************/
#define SENSOR_THREAD "sensor_thread"
#define LV_TICK_PERIOD_MS 1

#define USECS_TO_SEC 1000000
#define SWITCH_DELAY_MS 5000

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
    xTaskCreatePinnedToCore(sensor_task, "sensor_task", 1024 * 8, NULL, 0, NULL, 0); // 8KB
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
    // #ifndef CONFIG_LV_TFT_DISPLAY_MONOCHROME
    lv_color_t *buf2 = heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf2 != NULL);
    // #else
    //     static lv_color_t *buf2 = NULL;
    // #endif

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

    /* Create and start a periodic timer interrupt to call lv_tick_inc */
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &lv_tick_task,
        .name = "periodic_gui"};
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, LV_TICK_PERIOD_MS * 1000));

    set_font_style();

    display_driver_state = true;

    /* Create the demo application */
    // create_demo_application();

    set_main_screen();
    lv_scr_load(main_scr);

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

    // /* Create a style for the label */
    // static lv_style_t style_label;
    // lv_style_init(&style_label);

    /* Set properties for the style */
    // lv_style_set_text_font(&style_label, LV_STATE_DEFAULT, &lv_font_montserrat_20); // Use a larger font size

    // /*Modify the Label's text*/
    // lv_label_set_text(label1, "LT EMBEDDED LAB");

    // /* Align the Label to the center
    //  * NULL means align on parent (which is the screen now)
    //  * 0, 0 at the end means an x, y offset after alignment*/
    // lv_obj_align(label1, NULL, LV_ALIGN_IN_TOP_MID, 0, 20);

    // LV_IMG_DECLARE(img_cogwheel_argb);

    // lv_obj_t *img1 = lv_img_create(lv_scr_act(), NULL);
    // lv_img_set_src(img1, &img_cogwheel_argb);
    // lv_obj_align(img1, NULL, LV_ALIGN_CENTER, 0, -20);

    // vTaskDelay(1000);

    // lv_obj_t *scr1 = lv_obj_create(NULL, NULL);
    // lv_obj_t *label2 = lv_label_create(scr1, NULL);
    // lv_obj_t *temp_label = lv_label_create(scr1, NULL);

    // lv_obj_t *led1 = lv_led_create(scr1, NULL);

    // char temp_data[100];

    // sprintf(temp_data, "Temperature: %.2f C", 24.0);

    // /* Apply the style to the label */
    // lv_obj_add_style(label2, LV_LABEL_PART_MAIN, &style_label);

    // /*Modify the Label's text*/
    // lv_label_set_text(label2, "Live Weather Update");
    // lv_label_set_text(temp_label, temp_data);

    // /* Align the Label to the center
    //  * NULL means align on parent (which is the screen now)
    //  * 0, 0 at the end means an x, y offset after alignment*/
    // lv_obj_align(label2, NULL, LV_ALIGN_IN_TOP_MID, -5, 20);
    // lv_obj_align(temp_label, NULL, LV_ALIGN_IN_LEFT_MID, 20, -40);
    // lv_obj_align(led1, NULL, LV_ALIGN_IN_TOP_RIGHT, 0, 0);

    // lv_led_set_bright(led1, LV_LED_BRIGHT_MAX);
    // lv_led_on(led1);

    // lv_scr_load_anim(scr1, LV_SCR_LOAD_ANIM_OVER_LEFT, 1000, 500, true);
}

static void lv_tick_task(void *arg)
{
    (void)arg;

    lv_tick_inc(LV_TICK_PERIOD_MS);
}

void sensor_task(void *pvParameters)
{
    int weather_update_secs;
    bool main_screen_on = true;


    ESP_LOGI(SENSOR_THREAD, "Sensor thread up\n");

    connect_wifi();

        vTaskDelay(pdMS_TO_TICKS(2000));

    if (wifi_connection == true)
    {
        openweather_api_http();
    }

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(SENSOR_THREAD, "Sensor thread live\n");

       
        weather_update_secs = (esp_timer_get_time() - weather_update_timer) / USECS_TO_SEC;

        printf("weather_update_secs : %d", weather_update_secs);

        if (wifi_connection && weather_update_secs > 300)
        {
            weather_update_timer = esp_timer_get_time();
            openweather_api_http();
        }

        if (main_screen_on && display_driver_state)
        {
            ESP_LOGI("SCREEN_TASK", "Switching to Screen 1");
            set_main_screen();
            lv_scr_load_anim(main_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 1000, 0, true);
        }
        else
        {
            ESP_LOGI("SCREEN_TASK", "Switching to Screen 2");
            set_data_screen();

            lv_scr_load_anim(data_scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 1000, 0, true);
        }

        main_screen_on =! main_screen_on;
        vTaskDelay(pdMS_TO_TICKS(SWITCH_DELAY_MS));

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

    esp_wifi_set_config(WIFI_IF_STA, &wifi_configuration);
    esp_wifi_start();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_connect();

    vTaskDelay(pdMS_TO_TICKS(1000));
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

    if (wifi_retry_num < 100)
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

    weather_data.timestamp = cJSON_GetObjectItemCaseSensitive(root, "dt")->valueint;

    weather_data.timestamp = weather_data.timestamp + (5 * 3600 + 30 * 60); // UTC = IST + 5:30
    convertUTCToLocalTimeString(weather_data.timestamp, weather_data.date_time, sizeof(weather_data.date_time));

    weather_data.Temperature = cJSON_GetObjectItemCaseSensitive(obj, "temp")->valuedouble;
    weather_data.Temperature = KELVIN_TO_CELSIUS(weather_data.Temperature);

    weather_data.Temperature_Min = cJSON_GetObjectItemCaseSensitive(obj, "temp_min")->valuedouble;
    weather_data.Temperature_Min = KELVIN_TO_CELSIUS(weather_data.Temperature_Min);

    weather_data.Temperature_Max = cJSON_GetObjectItemCaseSensitive(obj, "temp_max")->valuedouble;
    weather_data.Temperature_Max = KELVIN_TO_CELSIUS(weather_data.Temperature_Max);

    weather_data.Pressure = cJSON_GetObjectItemCaseSensitive(obj, "pressure")->valueint;

    weather_data.Humidity = cJSON_GetObjectItemCaseSensitive(obj, "humidity")->valueint;

    weather_data.visiblity = cJSON_GetObjectItemCaseSensitive(root, "visibility")->valueint;
    weather_data.visiblity = weather_data.visiblity / 1000;

    printf("Timestamp: %ld\n", weather_data.timestamp);
    printf("Time: %s\n", weather_data.date_time);
    printf("Temperature: %0.3f °C\nTemperature_Max: %0.3f °C\nTemperature_Min: %0.3f °C\n", weather_data.Temperature, weather_data.Temperature_Max, weather_data.Temperature_Min);
    printf("Humidity: %d %%\n", weather_data.Humidity);
    printf("Pressure: %d hPa\n", weather_data.Pressure);
    printf("Visibility: %d Km\n", weather_data.visiblity);

    cJSON_Delete(root);
    free(response_data);
}

void convertUTCToLocalTimeString(time_t utc_timestamp, char *buffer, size_t buffer_size)
{
    struct tm *local_time;

    // Convert UTC timestamp to local time
    local_time = localtime(&utc_timestamp);
    if (local_time == NULL)
    {
        printf("Error converting UTC to local time.\n");
        return;
    }

    // Format the local time into a string (e.g., "DD-MM-YYYY HH:MM:SS")
    if (strftime(buffer, buffer_size, "%d/%m/%Y - %I:%M %p", local_time) == 0)
    {
        printf("Error formatting local time to string.\n");
        return;
    }
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {

    case HTTP_EVENT_ERROR:
        ESP_LOGI(SENSOR_THREAD, "HTTP_EVENT_ERROR");
        break;

    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(SENSOR_THREAD, "HTTP_EVENT_ON_CONNECTED");
        break;

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
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(SENSOR_THREAD, "HTTP_EVENT_DISCONNECTED");

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

void set_font_style()
{

    /* Create a style for the label */

    lv_style_init(&header_style_label);
    lv_style_init(&text_style_label);
    lv_style_init(&footer_text_style);
    lv_style_init(&led_style);

    lv_style_set_text_color(&header_style_label, LV_STATE_DEFAULT, LV_COLOR_GREEN);
    lv_style_set_text_font(&header_style_label, LV_STATE_DEFAULT, &lv_font_montserrat_20); // Use a larger font size

    lv_style_set_text_color(&footer_text_style, LV_STATE_DEFAULT, LV_COLOR_NAVY);
    lv_style_set_text_font(&footer_text_style, LV_STATE_DEFAULT, &lv_font_montserrat_14);

    lv_style_set_text_color(&text_style_label, LV_STATE_DEFAULT, LV_COLOR_NAVY);
    lv_style_set_text_font(&text_style_label, LV_STATE_DEFAULT, &lv_font_montserrat_12);
}

void set_main_screen()
{

    printf("display_main_screen");

    /* Change the active screen's background color to white */
    main_scr = lv_obj_create(NULL, NULL);;

    /*Create a Label on the currently active screen*/
    lv_obj_t *label1 = lv_label_create(main_scr, NULL);
    lv_obj_t *label2 = lv_label_create(main_scr, NULL);

    // /*Create a LED and switch it OFF*/
    // lv_obj_t *led1 = lv_led_create(main_scr, NULL);
    // lv_obj_align(led1, NULL, LV_ALIGN_IN_BOTTOM_LEFT, 0, 0);
    // // lv_obj_add_style(led1, LV_LABEL_PART_MAIN, &led_style);
    // lv_led_set_bright(led1, 255);

    // lv_led_on(led1);

    /* Apply the style to the label */
    lv_obj_add_style(label1, LV_LABEL_PART_MAIN, &header_style_label);
    lv_obj_add_style(label2, LV_LABEL_PART_MAIN, &footer_text_style);

    /*Modify the Label's text*/
    lv_label_set_text(label1, "LT EMBEDDED LAB");
    lv_label_set_text(label2, "LVGL v7.11.0");

    /* Align the Label to the center
     * NULL means align on parent (which is the screen now)
     * 0, 0 at the end means an x, y offset after alignment*/
    lv_obj_align(label1, NULL, LV_ALIGN_IN_TOP_MID, 0, 20);
    lv_obj_align(label2, NULL, LV_ALIGN_IN_BOTTOM_MID, 0, -20);

    LV_IMG_DECLARE(img_cogwheel_argb);

    lv_obj_t *img1 = lv_img_create(main_scr, NULL);
    lv_img_set_src(img1, &img_cogwheel_argb);
    lv_obj_align(img1, NULL, LV_ALIGN_CENTER, 0, -20);

    // lv_scr_load(main_scr);
    // lv_scr_load_anim(main_scr, LV_SCR_LOAD_ANIM_OVER_LEFT, 1000, 500, true);

}

void set_data_screen()
{
    char display_str[500];
    printf("display_data_screen");

    /* Change the active screen's background color to white */
    data_scr = lv_obj_create(NULL, NULL);;

    /*Create a Label on the currently active screen*/
    lv_obj_t *header = lv_label_create(data_scr, NULL);
    lv_obj_t *timestamp = lv_label_create(data_scr, NULL);
    lv_obj_t *temperature = lv_label_create(data_scr, NULL);
    lv_obj_t *temperature_max = lv_label_create(data_scr, NULL);
    lv_obj_t *temperature_min = lv_label_create(data_scr, NULL);
    lv_obj_t *humidity = lv_label_create(data_scr, NULL);
    lv_obj_t *pressure = lv_label_create(data_scr, NULL);
    lv_obj_t *visiblity = lv_label_create(data_scr, NULL);
    lv_obj_t *location = lv_label_create(data_scr, NULL);
    lv_obj_t *data_source = lv_label_create(data_scr, NULL);

    lv_obj_add_style(header, LV_LABEL_PART_MAIN, &header_style_label);
    lv_label_set_text(header, "Live Weather Update");
    lv_obj_align(header, NULL, LV_ALIGN_IN_TOP_MID, -5, 10);

    sprintf(display_str, "Last Sync At: %s", weather_data.date_time);
    lv_label_set_text(timestamp, display_str);
    lv_obj_add_style(timestamp, LV_LABEL_PART_MAIN, &text_style_label);
    lv_obj_align(timestamp, NULL, LV_ALIGN_IN_LEFT_MID, 20, -60);

    memset(display_str, "0", sizeof(display_str));
    sprintf(display_str, "Temperature: %0.2f °C", weather_data.Temperature);
    lv_label_set_text(temperature, display_str);
    lv_obj_add_style(temperature, LV_LABEL_PART_MAIN, &text_style_label);
    lv_obj_align(temperature, timestamp, LV_ALIGN_IN_LEFT_MID, 0, 20);

    memset(display_str, "0", sizeof(display_str));
    sprintf(display_str, "Max Temperature: %0.2f °C", weather_data.Temperature_Max);
    lv_label_set_text(temperature_max, display_str);
    lv_obj_add_style(temperature_max, LV_LABEL_PART_MAIN, &text_style_label);
    lv_obj_align(temperature_max, temperature, LV_ALIGN_IN_LEFT_MID, 0, 20);

    memset(display_str, "0", sizeof(display_str));
    sprintf(display_str, "Mix Temperature: %0.2f °C", weather_data.Temperature_Min);
    lv_label_set_text(temperature_min, display_str);
    lv_obj_add_style(temperature_min, LV_LABEL_PART_MAIN, &text_style_label);
    lv_obj_align(temperature_min, temperature_max, LV_ALIGN_IN_LEFT_MID, 0, 20);

    memset(display_str, "0", sizeof(display_str));
    sprintf(display_str, "Humidity: %d %%", weather_data.Humidity);
    lv_label_set_text(humidity, display_str);
    lv_obj_add_style(humidity, LV_LABEL_PART_MAIN, &text_style_label);
    lv_obj_align(humidity, temperature_min, LV_ALIGN_IN_LEFT_MID, 0, 20);

    memset(display_str, "0", sizeof(display_str));
    sprintf(display_str, "Pressure: %d hPa", weather_data.Pressure);
    lv_label_set_text(pressure, display_str);
    lv_obj_add_style(pressure, LV_LABEL_PART_MAIN, &text_style_label);
    lv_obj_align(pressure, humidity, LV_ALIGN_IN_LEFT_MID, 0, 20);

    memset(display_str, "0", sizeof(display_str));
    sprintf(display_str, "Visibility: %d Km", weather_data.visiblity);
    lv_label_set_text(visiblity, display_str);
    lv_obj_add_style(visiblity, LV_LABEL_PART_MAIN, &text_style_label);
    lv_obj_align(visiblity, pressure, LV_ALIGN_IN_LEFT_MID, 0, 20);

    memset(display_str, "0", sizeof(display_str));
    sprintf(display_str, "Location:Mumbai,IN");
    lv_label_set_text(location, display_str);
    lv_obj_add_style(location, LV_LABEL_PART_MAIN, &text_style_label);
    lv_obj_align(location, visiblity, LV_ALIGN_IN_LEFT_MID, 0, 20);

    memset(display_str, "0", sizeof(display_str));
    sprintf(display_str, "Data Source:openweathermap.org");
    lv_label_set_text(data_source, display_str);
    lv_obj_add_style(data_source, LV_LABEL_PART_MAIN, &text_style_label);
    lv_obj_align(data_source, NULL, LV_ALIGN_IN_BOTTOM_LEFT, 20, -10);

    // lv_scr_load_anim(data_scr, LV_SCR_LOAD_ANIM_OVER_LEFT, 1000, 500, true);
    // lv_scr_load(data_scr);
}