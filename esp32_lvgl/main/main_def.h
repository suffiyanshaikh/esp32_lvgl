#ifndef MAIN_DEF_H
#define MAIN_DEF_H

#include <stdio.h>

// API key from OpenWeatherMap
char open_weather_map_api_key[] = "1488d3fe9e946724785b07a62a92b786";

char city[] = "Mumbai";
char country_code[] = "IN";
char open_weather_map_url[200];

void openweather_api_http();
esp_err_t _http_event_handler(esp_http_client_event_t *evt);
void get_temp_pressure_humidity(const char *json_string);

#endif