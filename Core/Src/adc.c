#include "main.h"
#include "app.h"

/* Буфер DMA для трёх каналов АЦП */
uint16_t MCU_IGN03_ADC_VAL[MCU_IGN03_NUM_ADC_CHANNEL];

/* Последнее отфильтрованное значение ch2 (спичка) — для чтения из App_Timer1ms */
static volatile uint16_t igniter_adc_filtered = 0;

uint16_t ADC_GetIgniterFiltered(void) {
	return igniter_adc_filtered;
}

/* Фильтрация по скользящему среднему (аналог PPKY) */
static uint16_t adc_filter_val[MCU_IGN03_NUM_ADC_CHANNEL];
static uint16_t adc_sma_buf[MCU_IGN03_NUM_ADC_CHANNEL][MCU_IGN03_FILTERSIZE];
static uint32_t adc_sma_sum[MCU_IGN03_NUM_ADC_CHANNEL];
static uint8_t  adc_sma_fill_index[MCU_IGN03_NUM_ADC_CHANNEL];
static uint8_t  adc_sma_index[MCU_IGN03_NUM_ADC_CHANNEL];

static uint16_t SmaProcess(uint8_t num, uint16_t val)
{
    uint16_t old_val = 0;

    if (adc_sma_fill_index[num] == MCU_IGN03_FILTERSIZE) {
        old_val = adc_sma_buf[num][adc_sma_index[num]];
        adc_sma_sum[num] -= old_val;
    } else {
        adc_sma_fill_index[num]++;
    }

    adc_sma_buf[num][adc_sma_index[num]] = val;
    adc_sma_sum[num] += val;

    adc_sma_index[num]++;
    if (adc_sma_index[num] >= MCU_IGN03_FILTERSIZE) {
        adc_sma_index[num] = 0;
    }

    uint32_t result = adc_sma_sum[num] / adc_sma_fill_index[num];
    return (uint16_t)result;
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc != &hadc1) {
        return;
    }

    /* MCU_IGN03_ADC_VAL содержит три канала:
     * 0,1 – линия ДПТ (для VDeviceDPT)
     * 2   – контроль -5В спички (через инвертер, 4.7кОм), значения в мВ
     */
    for (uint8_t i = 0; i < MCU_IGN03_NUM_ADC_CHANNEL; i++) {
        adc_filter_val[i] = SmaProcess(i, MCU_IGN03_ADC_VAL[i]);
    }
    igniter_adc_filtered = adc_filter_val[2];

    App_SetDPTAdcValues(adc_filter_val[0], adc_filter_val[1]);
}

