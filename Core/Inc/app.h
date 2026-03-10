#ifndef MCU_IGNITER_V03_APP_H
#define MCU_IGNITER_V03_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void App_Init(void);
void App_Timer1ms(void);
void App_CanRxPush(uint32_t id, const uint8_t *data, uint8_t bus);
void App_CanProcess(void);
void App_SetDPTAdcValues(uint16_t ch1, uint16_t ch2);
void App_SetIgniterLineState(uint8_t state);
uint16_t App_GetIgniterPwm(void);
void App_CanOnRx(uint8_t bus);      /* 1 = CAN1, 2 = CAN2 */
void App_UpdateCanActivity(void);
extern volatile uint8_t CAN1_Active;
extern volatile uint8_t CAN2_Active;

#ifdef __cplusplus
}
#endif

#endif /* MCU_IGNITER_V03_APP_H */

