#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "math.h"

#include "module.h"

#include "adc.h"

const static char *TAG = "ADC";

#define AVG_SAMPLES (10)
#define AVG_PERIOD (4) // seconds
#define SCALE_VOLTAGE(raw_mv) ((raw_mv) * 2)

//static int learned_charge_voltages[];
//int learning_voltages[];

static uint16_t full_bat = 4120;
static uint16_t empty_bat = 3080;

static int battery_voltage;

uint16_t get_battery_voltage(void)
{
    return battery_voltage;
}

#ifdef LIPO_BATTERY
uint8_t battery_percent(uint16_t battery_voltage)
{
    int percent=0;
    if (battery_voltage <= empty_bat)
        return 0; 
    if (battery_voltage >= full_bat)
        return 100; 


    double x = (double)(battery_voltage - empty_bat) / (double)(full_bat - empty_bat);


    const double a = 15.0;
    const double b = 3.73;

    double y = 1.0 / (1.0 + exp(-a * (x - b)));

    uint8_t percent = (uint8_t)lround(y * 100.0);

    ESP_LOGI(TAG, "(Conv) Battery voltage: %d mV, percentage: %d%%", battery_voltage, percent);

    return percent;
}

#elif //Fallback to default
uint8_t battery_percent(uint16_t battery_voltage)
{

    if (battery_voltage > full_bat)
        battery_voltage = full_bat;
    else if (battery_voltage < empty_bat)
        battery_voltage = empty_bat;

    uint8_t percent = (battery_voltage - empty_bat) * 100 / (full_bat - empty_bat);

    ESP_LOGI(TAG, "(Conv) Battery voltage: %d mV, percentage: %d%%", battery_voltage, percent);

    return percent;
}
#endif

static void adc_task(void *arg)
{
    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_21, 0);

    adc_unit_t unit;
    adc_channel_t channel;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(BATTERY_ADC, &unit, &channel));

    adc_oneshot_unit_handle_t adc_handle;
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = unit,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, channel, &config));

    adc_cali_handle_t cali_handle;

    adc_cali_curve_fitting_config_t cali_config = {
        .atten = config.atten,
        .bitwidth = ADC_BITWIDTH_12,
        .unit_id = unit,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle));

    TickType_t last_wake_time = xTaskGetTickCount();

    for (;;)
    {
        int averaged_result = 0;
        for (int i = 0; i < AVG_SAMPLES; i++)
        {
            int raw_output, voltage;
            ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, channel, &raw_output));
            ESP_ERROR_CHECK(adc_cali_raw_to_voltage(cali_handle, raw_output, &voltage));
            ESP_LOGI(TAG, "ADC raw: %d, voltage: %d mV", raw_output, voltage);

         //   if (battery_voltage <= 1000)
         //       battery_voltage = SCALE_VOLTAGE(voltage);

            averaged_result += voltage;

            vTaskDelay(pdMS_TO_TICKS(AVG_PERIOD * 1000 / AVG_SAMPLES));
        }
       
        averaged_result /= AVG_SAMPLES;

        ESP_LOGI(TAG, "ADC voltage: %d mV (%d mV scaled)", averaged_result, SCALE_VOLTAGE(averaged_result));
        battery_voltage = SCALE_VOLTAGE(averaged_result);

        if (battery_voltage < 3060)
        {
            esp_deep_sleep_start();
        }

        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(60000));
    }
    adc_oneshot_del_unit(adc_handle);
}

//TODO: Charging curve learning
static void learn_charging_curve(void){
    return; //XXX: Temp
    TickType_t last_wake_time = xTaskGetTickCount();
 //   if (learning_voltages)
   // {
        /* code */
   // }
    

    vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(61000));
}

// TODO: Apply Charging curve 
void batt_curve_init(){
   // if (learned_charge_voltages == NULL)
  //  {
  //      ESP_LOGW(TAG, "Battery charge curve missing, learn when possible");
  //  }
    
}

void adc_init(void)
{
    xTaskCreate(adc_task, "adc", 3072, NULL, 5, NULL);
}
