#ifndef MAIN_DEF_H
#define MAIN_DEF_H

#include <stdio.h>
#include <time.h>


/*********************
 *      DEFINES
 *********************/
#define KELVIN_TO_CELSIUS(k) ((k) - 273.15)


/*********************
 *      variables
 *********************/

// API key from OpenWeatherMap
char open_weather_map_api_key[] = "1488d3fe9e946724785b07a62a92b786";

char city[] = "Mumbai";
char country_code[] = "IN";
char open_weather_map_url[200];

struct  weather_params
{

time_t timestamp;
float Temperature;
float Temperature_Max;
float Temperature_Min;
int Humidity;
int Pressure;
int visiblity;
char date_time[200];

};


/**********************
 *  STATIC PROTOTYPES
 **********************/
void openweather_api_http();
esp_err_t _http_event_handler(esp_http_client_event_t *evt);
void get_temp_pressure_humidity(const char *json_string);
void convertUTCToLocalTimeString(time_t utc_timestamp, char *buffer, size_t buffer_size);
void set_font_style();
void set_main_screen();
void set_data_screen();


#endif