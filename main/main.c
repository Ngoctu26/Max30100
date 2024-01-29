/**
 * @file example.c
 * 
 * @author
 * Angelo Elias Dalzotto (150633@upf.br)
 * GEPID - Grupo de Pesquisa em Cultura Digital (http://gepid.upf.br/)
 * Universidade de Passo Fundo (http://www.upf.br/)
 * 
 * @copyright
 * Copyright (c) 2017 Raivis Strogonovs (https://morf.lv)
 * 
 * @brief This is an example file to the MAX30100 library for the ESP32.
 * It initializes the IDF-SDK I2C driver and then initializes the sensor.
 * It crates a task to update the readings at a rate of 100Hz and prints
 * the bpm and oxigen saturation results.
*/
/**
 * Pin assignment:
 * - i2c:
 *    GPIO12: SDA
 *    GPIO14: SDL
 * - no need to add external pull-up resistors.
 */

#include <stdio.h>
#include "driver/i2c.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "freertos/queue.h"
#include "max30100.h"
#include <freertos/semphr.h>
#include "lcd_i2c.h"
#include "driver/gpio.h"
#include "esp_sleep.h"

#define SSID "Trần Ngọc Tú"
#define PASS "123123124"

#define WIFI_TAG        "Esp_Wifi"
#define TAG_HTTP        "Esp_HTTP"
#define TAG                "LCD"
#define DEVICE_TOKEN    "B37SU6c36_x-k5hHIn2ElDDcuSXd_5NR"
#define SERVER_URL      "http://blynk.cloud"
#define BPM_PIN            "V3"
#define SPO2_PIN            "V4"
#define PIN_ARR         {PIN1, PIN2}
#define HTTP_SEND_CYCLIC_MS 100
#define GET_DATA_CYCLIC     100
#define I2C_SDA 21
#define I2C_SCL 22
#define I2C_FRQ 100000
#define I2C_PORT I2C_NUM_0

#define WIFI_CONNECTED_BIT BIT0
#define SLAVE_ADDRESS_LCD 0x27
#define I2C_MASTER_NUM  I2C_NUM_0
#define BUTTON_PIN GPIO_NUM_0
#define GPIO_IN_PIN (1ULL << BUTTON_PIN)
#define LED GPIO_NUM_2
#define GPIO_OUT_PIN (1ULL << LED)

#define EMERGENGY_BPM_HIGH 100
#define EMERGENGY_BPM_LOW  50
#define EMERGENGY_SPO2_LOW  90
#define TIME_TO_GO_SLEEP    2000 // go to deep sleep after 10s 

max30100_config_t max30100 = {};
static EventGroupHandle_t wifi_event_group;
QueueHandle_t qdata_main;
SemaphoreHandle_t Sem_send_http;
SemaphoreHandle_t Sem_I2C;
QueueHandle_t qBtSt = NULL;
QueueHandle_t qClick = NULL;
QueueHandle_t qdata_http = NULL;
typedef struct data_main
{
    float BPM;
    float SPO2;
}data_main_t;

typedef enum led_st
{
    LED_NORMAL = 0,
    LED_EMERGENCY,
}led_st_t;

volatile led_st_t led_status;
volatile uint8_t button_flag = 0;
volatile uint8_t button_continue = 1;
static void IRAM_ATTR gpio_ISR_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t) arg;
	xQueueSendFromISR(qClick, &gpio_num, NULL);
}

void gpio_init(){
	// config input
	gpio_config_t gpiocf = {};
	gpiocf.pin_bit_mask = GPIO_IN_PIN;
	gpiocf.mode = GPIO_MODE_INPUT;
	gpiocf.pull_up_en = 1;
	gpiocf.intr_type = GPIO_INTR_NEGEDGE;
	gpiocf.pull_down_en = 0;
	gpio_config(&gpiocf);
	// config output
	gpio_config_t gpiocfO ={
			.pin_bit_mask = GPIO_OUT_PIN,
			.mode = GPIO_MODE_OUTPUT,
			.pull_down_en =0,
			.pull_up_en = 0,
			.intr_type =  GPIO_INTR_DISABLE
	};
	gpio_config(&gpiocfO);
	gpio_set_level(LED, 0);
	gpio_install_isr_service(0);
	gpio_isr_handler_add(BUTTON_PIN,gpio_ISR_handler,(void *)BUTTON_PIN);
}

void button_status_task(void * param){
	int gpio_num;
	TickType_t l_time = 0;
	uint32_t cnt  = 0;
	while(1){
		if(xQueueReceive(qClick,&gpio_num,50) ){
			if(gpio_num == BUTTON_PIN && gpio_get_level(gpio_num) == 0){
                if(button_flag == 1)
                {
                    button_flag = 0;
                }
                button_continue = 0;
				printf("Button Pass\r\n");
			}
		}
	}
}

esp_err_t i2c_master_init(i2c_port_t i2c_port){
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_SDA;
    conf.scl_io_num = I2C_SCL;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_FRQ;
    i2c_param_config(i2c_port, &conf);
    return i2c_driver_install(i2c_port, I2C_MODE_MASTER, 0, 0, 0);
}


static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(WIFI_TAG,"WiFi connecting ... \n");
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(WIFI_TAG,"WiFi connected ... \n");
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(WIFI_TAG,"WiFi lost connection ... \n");
        esp_wifi_connect();
        break;
    case IP_EVENT_STA_GOT_IP:
        ESP_LOGI(WIFI_TAG,"WiFi got IP ... \n\n");
        break;
    default:
        break;
    }
}

void wifi_connection()
{
    // 1 - Wi-Fi/LwIP Init Phase
    wifi_event_group = xEventGroupCreate();
    esp_netif_init();                    // TCP/IP initiation 					s1.1
    esp_event_loop_create_default();     // event loop 			                s1.2
    esp_netif_create_default_wifi_sta(); // WiFi station 	                    s1.3
    wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_initiation); // 					                    s1.4
    // 2 - Wi-Fi Configuration Phase
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    wifi_config_t wifi_configuration = {
        .sta = {
            .ssid = SSID,
            .password = PASS}};
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_configuration);
    // 3 - Wi-Fi Start Phase
    esp_wifi_start();
    // 4- Wi-Fi Connect Phase
    esp_wifi_connect();
}

esp_err_t client_event_get_handler(esp_http_client_event_handle_t evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG_HTTP, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG_HTTP, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG_HTTP, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG_HTTP, "HTTP_EVENT_ON_HEADER");
            printf("%.*s", evt->data_len, (char*)evt->data);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG_HTTP, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                printf("%.*s", evt->data_len, (char*)evt->data);
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG_HTTP, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG_HTTP, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}

esp_err_t rest_get(int8_t* url_buffer)
{
    esp_http_client_config_t config_get = {
        .url = (char*)url_buffer,
        .method = HTTP_METHOD_GET,
        .cert_pem = NULL,
        .event_handler = client_event_get_handler};
    
    esp_http_client_handle_t client = esp_http_client_init(&config_get);
    esp_err_t err_1 = esp_http_client_perform(client);
     if (err_1 == ESP_OK) {
        ESP_LOGI(TAG_HTTP, "HTTP GET Status = %d, content_length = %d"PRId64,
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG_HTTP, "HTTP GET request failed: %s", esp_err_to_name(err_1)); 
    }
    esp_http_client_cleanup(client);
    return err_1;
}

void get_bpm(void* param) {
    printf("MAX30100 Test\n");
    max30100_data_t result = {};

    while(true) {
        //Update sensor, saving to "result"
        if (xSemaphoreTake(Sem_I2C, portMAX_DELAY) == pdTRUE) 
        {
            ESP_ERROR_CHECK(max30100_update(&max30100, &result));
            xSemaphoreGive(Sem_I2C);
        }
        
        if(result.pulse_detected) 
        {
            printf("BEAT\n");
            printf("BPM: %f | SpO2: %f%%\n", result.heart_bpm, result.spO2);
            printf("queue send: BPM: %f | SpO2: %f%%\n", result.heart_bpm, result.spO2);
            xQueueSend(qdata_main, &result, 1000/portTICK_PERIOD_MS);

        }
            
        //Update rate: 100Hz
        vTaskDelay(10/portTICK_PERIOD_MS);
    }
}

void Task_send_http(void*arg)
{
    data_main_t result = {};
    char buffer_send[128];
    while(1)
    {
        if(xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY) && WIFI_CONNECTED_BIT)
        {
            if (xQueueReceive(qdata_http, &result, portMAX_DELAY) == pdPASS) {
                printf("queue receive: BPM: %f | SpO2: %f%%\r\n", result.BPM, result.SPO2);
                if (xSemaphoreTake(Sem_send_http, portMAX_DELAY) == pdTRUE) 
                {
                    memset(buffer_send, 0, 128);
                    sprintf(buffer_send,"%s/external/api/batch/update?token=%s&%s=%.2f&%s=%.2f",SERVER_URL,DEVICE_TOKEN,SPO2_PIN,result.SPO2,BPM_PIN,result.BPM);
                    ESP_LOGI(TAG_HTTP,"req string: %s\r\n",buffer_send);
                    rest_get((int8_t*)buffer_send);
                    xSemaphoreGive(Sem_send_http);
                }
            }
            
        }
        vTaskDelay(HTTP_SEND_CYCLIC_MS/portTICK_PERIOD_MS);
    }
}

void Task_Led_status(void *arg)
{
    while(1)
    {
        if(led_status == LED_EMERGENCY)
        {
            gpio_set_level(LED, 1);
            vTaskDelay(200/portTICK_PERIOD_MS);
            gpio_set_level(LED, 0);
            vTaskDelay(200/portTICK_PERIOD_MS);
        }
        else if(led_status == LED_NORMAL)
        {
            gpio_set_level(LED, 1);
            vTaskDelay(1000/portTICK_PERIOD_MS);
            gpio_set_level(LED, 0);
            vTaskDelay(1000/portTICK_PERIOD_MS);
        }
    }
}

void Task_monitor(void *arg)
{
    max30100_data_t result = {};
    float sum_spo2 = 0.0, sum_BPM = 0.0, mean_spo2 = 0.0, mean_bpm = 0.0;
    static int8_t count = 0;
    char buff_lcd[7];
    data_main_t data_http;
    while(1)
    {
        if (xQueueReceive(qdata_main, &result, 500/portTICK_PERIOD_MS) == pdPASS)
        {
            if (count >= 10)
            { 
                mean_bpm = sum_BPM /count;
                mean_spo2 = sum_spo2/count;
                sum_BPM = 0.0;
                sum_spo2 = 0.0;
                count = 0;
                if (xSemaphoreTake(Sem_I2C, portMAX_DELAY) == pdTRUE) 
                {
                    sprintf(buff_lcd,"%.2f",mean_bpm);
                    lcd_put_cur(0, 4);
                    lcd_send_string(buff_lcd);
                    sprintf(buff_lcd,"%.2f",mean_spo2);
                    lcd_put_cur(1, 5);
                    lcd_send_string(buff_lcd);
                    xSemaphoreGive(Sem_I2C);
                }
                
                data_http.BPM = mean_bpm;
                data_http.SPO2  = mean_spo2;
                xQueueSend(qdata_http,&data_http, 1000/portMAX_DELAY);
                if((mean_bpm > EMERGENGY_BPM_HIGH) || (mean_bpm < EMERGENGY_BPM_LOW) || (mean_spo2 < EMERGENGY_SPO2_LOW))
                {
                    led_status = LED_EMERGENCY;
                    button_flag = 1;
                }
                else
                {
                    led_status = LED_NORMAL;
                }
                
            }
            
            sum_spo2    += result.spO2;
            sum_BPM     += result.heart_bpm;
            count ++;
        }
        if((led_status != LED_NORMAL) && (button_flag == 0))
        {
            led_status = LED_NORMAL;
        }
    }
    
}

void Task_to_Sleep(void * arg)
{
    static TickType_t  tick_begin ;
    tick_begin = xTaskGetTickCount();
    esp_sleep_enable_ext0_wakeup(BUTTON_PIN,0);
    while (1)
    {
        if(button_continue == 0)
        {
            button_continue = 1;
            tick_begin = xTaskGetTickCount();
        }
        if(xTaskGetTickCount() - tick_begin > TIME_TO_GO_SLEEP)
        {
            printf("GO TO DEEP SLEEP\r\n");
            esp_deep_sleep_start();
        }
        vTaskDelay(1000/portTICK_PERIOD_MS);
    }
}

void app_main()
{
    qdata_main = xQueueCreate(20, sizeof(max30100_data_t));
    qClick = xQueueCreate(1, sizeof(uint32_t));
	qBtSt = xQueueCreate(1,sizeof(uint32_t));
    Sem_send_http = xSemaphoreCreateMutex();
    Sem_I2C = xSemaphoreCreateMutex();
    qdata_http = xQueueCreate(5,sizeof(data_main_t));

    gpio_init();
    //Init I2C_NUM_0
    ESP_ERROR_CHECK(i2c_master_init(I2C_PORT));
    //Init sensor at I2C_NUM_0
    ESP_ERROR_CHECK(max30100_init( &max30100, I2C_PORT,
                   MAX30100_DEFAULT_OPERATING_MODE,
                   MAX30100_DEFAULT_SAMPLING_RATE,
                   MAX30100_DEFAULT_LED_PULSE_WIDTH,
                   MAX30100_LED_CURRENT_20_8MA,
                   MAX30100_DEFAULT_START_RED_LED_CURRENT,
                   MAX30100_DEFAULT_MEAN_FILTER_SIZE,
                   MAX30100_DEFAULT_PULSE_BPM_SAMPLE_SIZE,
                   true, false ));
    ESP_LOGI(TAG, "I2C initialized successfully");
    //Start test task
    lcd_init();
    lcd_clear();
    lcd_put_cur(0, 0);
    lcd_send_string("BPM:");

    lcd_put_cur(1, 0);
    lcd_send_string("SPO2:     %");
    // init wifi 
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_connection();

    xTaskCreate(get_bpm, "Get BPM", 4096, NULL, 2, NULL);
    xTaskCreate(Task_send_http,"send http", 4096,NULL,3,NULL);
    xTaskCreate(button_status_task,"Button Task", 2048,NULL,1,NULL);
    xTaskCreate(Task_Led_status,"Button Task", 1024,NULL,4,NULL);
    xTaskCreate(Task_monitor,"Task monitor data",4096,NULL,1,NULL);
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, 30000/portTICK_PERIOD_MS);
    xTaskCreate(Task_to_Sleep,"Task sleep mode",2048,NULL,5,NULL);
}