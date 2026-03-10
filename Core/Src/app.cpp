#include "app.h"

extern "C" {
#include "backend.h"
}

#include "device_config.h"
#include "device_igniter.hpp"
#include "device_dpt.hpp"
#include "mku_cfg_flash.h"
#include "stm32h5xx_hal.h"
#include "stm32h5xx_hal_flash.h"
#include "stm32h5xx_hal_flash_ex.h"

/* Конфиг MKUCfg — секция .mku_cfg в STM32H523RETX_FLASH.ld (область FLASH_CFG),
 * адрес и размер — mku_cfg_flash.h (_mku_cfg_start, _mku_cfg_end) */
#define FLASH_CFG_SECTOR     (31u)         /* последний сектор Bank 2 (0x0807E000); в каждом банке 32 сектора 0–31 */
#define MKU_CFG_HEADER_MAGIC 0x4D4B5543u   /* 'MKUC' */
#include <string.h>
#include "main.h"

/* Конфигурация платы — единая структура MKUCfg из device_config */
static MKUCfg g_cfg;
static MKUCfg g_saved_cfg;

/* Виртуальные устройства: Dev=1 Igniter (Devices[0]), Dev=2 DPT (Devices[1]) */
static VDeviceIgniter g_igniter(1);
static VDeviceDPT    g_dpt(2);

/* Кольцевой буфер принятых CAN-пакетов (как в MCU_TC) */
#define APP_CAN_RX_RING_SIZE  64

typedef struct {
    uint32_t id;
    uint8_t  data[8];
    uint8_t bus;
} AppCanRxEntry;

static AppCanRxEntry     can_rx_ring[APP_CAN_RX_RING_SIZE];
static volatile uint8_t  can_rx_head = 0;
static volatile uint8_t  can_rx_tail = 0;

/* Флаги активности шин CAN: 1 - были пакеты за последние 2 секунды, 0 - тишина */
volatile uint8_t CAN1_Active = 0;
volatile uint8_t CAN2_Active = 0;
static uint32_t can1_last_rx_tick = 0;
static uint32_t can2_last_rx_tick = 0;

/* callback статуса: отправляем его через CAN по протоколу backend */
static void VDeviceSetStatus(uint8_t DNum, uint8_t Code, const uint8_t *Parameters)
{
	//if(DNum == 2) return;
    uint8_t data[7] = {0};
    for (uint8_t i = 0; i < 7; i++) {
        data[i] = Parameters[i];
    }
    SendMessage(DNum, Code, data, 0, BUS_CAN12);
}

/* Заглушка сохранения конфига виртуальных устройств */
static void SaveCfg(void)
{
    /* пока ничего не делаем, конфиг хранится в g_cfg */
}


void SetHAdr(uint8_t h_adr) {
	g_cfg.UId.devId.h_adr = h_adr;
	extern uint8_t nDevs;
	extern Device BoardDevicesList[];
	for(uint8_t i = 0; i < nDevs; i++) {
		BoardDevicesList[i].h_adr = g_cfg.UId.devId.h_adr;
	}
	SaveConfig();
}

/* -------- C-интерфейс, который ожидает backend и main.c -------- */

extern "C" {

extern TIM_HandleTypeDef htim4;

void DefaultConfig(void)
{
    uint32_t uid0 = HAL_GetUIDw0();
    uint32_t uid1 = HAL_GetUIDw1();
    uint32_t uid2 = HAL_GetUIDw2();

    memset(&g_cfg, 0, sizeof(g_cfg));

    g_cfg.UId.UId0 = uid0;
    g_cfg.UId.UId1 = uid1;
    g_cfg.UId.UId2 = uid2;
    g_cfg.UId.UId3 = HAL_GetDEVID();
    g_cfg.UId.UId4 = 1;

    g_cfg.UId.devId.zone  = 0;
    g_cfg.UId.devId.l_adr = 0;

    uint8_t hadr = static_cast<uint8_t>(uid0 & 0xFFu);
    if (hadr == 0u) {
        hadr = static_cast<uint8_t>(uid1 & 0xFFu);
        if (hadr == 0u) {
            hadr = 1u;
        }
    }
    g_cfg.UId.devId.h_adr  = hadr;
    g_cfg.UId.devId.d_type = DEVICE_MCU_IGN_TYPE;

    /* VDtype: Devices[0]=Igniter, Devices[1]=DPT */
    g_cfg.VDtype[0] = DEVICE_IGNITER_TYPE;
    g_cfg.VDtype[1] = 0;//DEVICE_DPT_TYPE;

    /* Igniter config в Devices[0].reserv */
    DeviceIgniterConfig *ign_cfg = reinterpret_cast<DeviceIgniterConfig*>(g_cfg.Devices[0].reserv);
    ign_cfg->disable_sc_check     = 1u;
    ign_cfg->threshold_break_low  = 1000u;
    ign_cfg->threshold_break_high = 3000u;
    ign_cfg->burn_retry_count     = 2u;

    /* ДПТ config в Devices[1].reserv */
    /*
    DeviceDPTConfig *dpt_cfg = reinterpret_cast<DeviceDPTConfig*>(g_cfg.Devices[1].reserv);
    dpt_cfg->fire_threshold_ohm   = 680u;
    dpt_cfg->normal_threshold_ohm = 5380u;
    dpt_cfg->break_threshold_ohm  = 100000u;
    dpt_cfg->resistor_r1_ohm      = 10000u;
    dpt_cfg->resistor_r2_ohm      = 10000u;
    dpt_cfg->supply_voltage_mv    = 3300u;
    dpt_cfg->adc_resolution       = 4095u;
    dpt_cfg->is_limit_switch      = 0u;
*/
    /* reserv[64] — отличия платы игнитера (пока пусто) */
}

uint16_t GetConfigSize(void)
{
    return static_cast<uint16_t>(sizeof(g_cfg));
}

uint32_t GetConfigWord(uint16_t num)
{
    uint32_t byte_index = static_cast<uint32_t>(num) * 4u;
    uint16_t cfg_size   = GetConfigSize();

    if (byte_index + 4u > cfg_size) {
        return 0u;
    }

    uint8_t *p = reinterpret_cast<uint8_t *>(&g_cfg);
    uint32_t word = 0u;
    word |= (static_cast<uint32_t>(p[byte_index + 0]) << 24);
    word |= (static_cast<uint32_t>(p[byte_index + 1]) << 16);
    word |= (static_cast<uint32_t>(p[byte_index + 2]) << 8);
    word |= (static_cast<uint32_t>(p[byte_index + 3]) << 0);

    return word;
}

void SetConfigWord(uint16_t num, uint32_t word)
{
    uint32_t byte_index = static_cast<uint32_t>(num) * 4u;
    uint16_t cfg_size   = GetConfigSize();

    if (byte_index + 4u > cfg_size) {
        return;
    }

    uint8_t *p = reinterpret_cast<uint8_t *>(&g_cfg);
    p[byte_index + 0] = static_cast<uint8_t>((word >> 24) & 0xFFu);
    p[byte_index + 1] = static_cast<uint8_t>((word >> 16) & 0xFFu);
    p[byte_index + 2] = static_cast<uint8_t>((word >> 8)  & 0xFFu);
    p[byte_index + 3] = static_cast<uint8_t>((word >> 0)  & 0xFFu);
}

/* Заголовок конфига во Flash: magic (4) + size (4) = 8 байт, затем MKUCfg */
#define MKU_CFG_HEADER_SIZE  8u
#define QUADWORD_SIZE        16u   /* 128-bit для HAL_FLASH_Program */

static bool FlashReadConfig(MKUCfg *out)
{
    if (out == nullptr) {
        return false;
    }
    const uint32_t *p = reinterpret_cast<const uint32_t *>(FLASH_CFG_ADDR);
    if (p[0] != MKU_CFG_HEADER_MAGIC) {
        return false;
    }
    uint32_t sz = p[1];
    if (sz != sizeof(MKUCfg) || sz > FLASH_CFG_SIZE - MKU_CFG_HEADER_SIZE) {
        return false;
    }
    memcpy(out, p + 2, sz);
    return true;
}

void FlashWriteData(uint8_t *ConfigPtr, uint16_t ConfigSize)
{
    if (ConfigPtr == nullptr || ConfigSize != sizeof(MKUCfg) ||
        ConfigSize > FLASH_CFG_SIZE - MKU_CFG_HEADER_SIZE) {
        return;
    }

    /* Буфер: заголовок + MKUCfg, выровнен по 16 байт для quad-word программирования */
    __attribute__((aligned(16))) uint8_t buf[FLASH_CFG_SIZE_BYTES];
    uint32_t *hdr = reinterpret_cast<uint32_t *>(buf);
    hdr[0] = MKU_CFG_HEADER_MAGIC;
    hdr[1] = ConfigSize;
    memcpy(buf + MKU_CFG_HEADER_SIZE, ConfigPtr, ConfigSize);

    uint32_t total = MKU_CFG_HEADER_SIZE + ConfigSize;
    uint32_t n_quad = (total + QUADWORD_SIZE - 1u) / QUADWORD_SIZE;

    FLASH_EraseInitTypeDef erase;
    uint32_t sector_err = 0u;

#if defined(FLASH_TYPEERASE_SECTORS_NS)
    erase.TypeErase = FLASH_TYPEERASE_SECTORS_NS;
#else
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
#endif
    erase.Banks     = FLASH_BANK_2;
    erase.Sector    = FLASH_CFG_SECTOR;
    erase.NbSectors = 1u;

    HAL_StatusTypeDef st = HAL_FLASH_Unlock();
    if (st != HAL_OK) {
        return;
    }

    st = HAL_FLASHEx_Erase(&erase, &sector_err);
    if (st != HAL_OK) {
        HAL_FLASH_Lock();
        return;
    }

#if defined(FLASH_TYPEPROGRAM_QUADWORD_NS)
    uint32_t prog_type = FLASH_TYPEPROGRAM_QUADWORD_NS;
#else
    uint32_t prog_type = FLASH_TYPEPROGRAM_QUADWORD;
#endif

    for (uint32_t i = 0u; i < n_quad && st == HAL_OK; i++) {
        uint32_t addr = FLASH_CFG_ADDR + i * QUADWORD_SIZE;
        st = HAL_FLASH_Program(prog_type, addr,
                              reinterpret_cast<uint32_t>(buf + i * QUADWORD_SIZE));
    }

    HAL_FLASH_Lock();
}

void SaveConfig(void)
{
    uint16_t size = GetConfigSize();
    (void)size;

    FlashWriteData(reinterpret_cast<uint8_t *>(&g_cfg), size);
    g_saved_cfg = g_cfg;
}

void ResetMCU(void)
{
    NVIC_SystemReset();
}

uint32_t GetID(void)
{
    uint32_t id0 = HAL_GetUIDw0();
    uint32_t id1 = HAL_GetUIDw1();
    uint32_t id2 = HAL_GetUIDw2();
    return (id0 ^ id1 ^ id2);
}

void CommandCB(uint8_t Dev, uint8_t Command, uint8_t *Parameters)
{
    switch (Dev) {
    case 0:
        /* сервисные команды физической платы – пока заглушка */
        break;
    case 1:
        /* команды для Igniter */
        g_igniter.CommandCB(Command, Parameters);
        break;
    case 2:
        /* команды для ДПТ */
        g_dpt.CommandCB(Command, Parameters);
        break;
    default:
        break;
    }
}

void ListenerCommandCB(uint32_t MsgID, uint8_t *MsgData)
{
    (void)MsgID;
    (void)MsgData;
}

void App_CanOnRx(uint8_t bus)
{
    uint32_t now = HAL_GetTick();
    if (bus == 1u) {
        CAN1_Active = 1u;
        can1_last_rx_tick = now;
    } else if (bus == 2u) {
        CAN2_Active = 1u;
        can2_last_rx_tick = now;
    }
}

void App_CanRxPush(uint32_t id, const uint8_t *data, uint8_t bus)
{
    uint8_t next = static_cast<uint8_t>(can_rx_head + 1u);
    if (next >= APP_CAN_RX_RING_SIZE) {
        next = 0u;
    }

    /* При переполнении затираем самый старый пакет */
    if (next == can_rx_tail) {
        can_rx_tail++;
        if (can_rx_tail >= APP_CAN_RX_RING_SIZE) {
            can_rx_tail = 0u;
        }
    }

    can_rx_ring[can_rx_head].id = id;
    can_rx_ring[can_rx_head].bus = bus;
    memcpy(can_rx_ring[can_rx_head].data, data, 8u);
    can_rx_head = next;
}

void App_CanProcess(void)
{
    while (can_rx_head != can_rx_tail) {
        AppCanRxEntry *e = &can_rx_ring[can_rx_tail];
        can_rx_tail++;
        if (can_rx_tail >= APP_CAN_RX_RING_SIZE) {
            can_rx_tail = 0u;
        }

        ProtocolParse(e->id, e->data, e->bus);
    }
}

void App_Init(void)
{
    extern Device BoardDevicesList[];
    extern uint8_t nDevs;

    if (!FlashReadConfig(&g_cfg)) {
        DefaultConfig();
        SaveConfig();   /* сохраняем дефолт при первом запуске */
    }
    g_saved_cfg = g_cfg;
    SetConfigPtr(reinterpret_cast<uint8_t *>(&g_saved_cfg),
                 reinterpret_cast<uint8_t *>(&g_cfg));

    /* Инициализируем виртуальный Igniter (Devices[0]) */
    g_igniter.DeviceInit(&g_cfg.Devices[0]);
    g_igniter.VDeviceSetStatus = VDeviceSetStatus;
    g_igniter.VDeviceSaveCfg   = SaveCfg;
    g_igniter.Init();


    /* Инициализируем виртуальный ДПТ (Devices[1]) */
    g_dpt.DeviceInit(&g_cfg.Devices[1]);
    g_dpt.VDeviceSetStatus = VDeviceSetStatus;
    g_dpt.VDeviceSaveCfg   = SaveCfg;
    g_dpt.Init();

    /* Регистрируем устройства в backend:
     * Dev 0 – физическая плата (тип DEVICE_MCU_IGN_TYPE)
     * Dev 1 – Igniter (тип DEVICE_IGNITER_TYPE)
     * Dev 2 – DPT (тип DEVICE_DPT_TYPE)
     */
    nDevs = 1; /* Dev 0 – плата */

    if (nDevs <= 0) {
        nDevs = 1;
    }
    BoardDevicesList[0].zone  = g_cfg.UId.devId.zone;
    BoardDevicesList[0].h_adr = g_cfg.UId.devId.h_adr;
    BoardDevicesList[0].l_adr = g_cfg.UId.devId.l_adr;
    BoardDevicesList[0].d_type = DEVICE_MCU_IGN_TYPE;

    if (nDevs < MAX_DEVS) {
        BoardDevicesList[nDevs].zone  = g_cfg.UId.devId.zone;
        BoardDevicesList[nDevs].h_adr = g_cfg.UId.devId.h_adr;
        BoardDevicesList[nDevs].l_adr = 1;
        BoardDevicesList[nDevs].d_type = DEVICE_IGNITER_TYPE;
        nDevs++;
    }
/*
    if (nDevs < MAX_DEVS) {
        BoardDevicesList[nDevs].zone  = g_cfg.UId.devId.zone;
        BoardDevicesList[nDevs].h_adr = g_cfg.UId.devId.h_adr;
        BoardDevicesList[nDevs].l_adr = 2;
        BoardDevicesList[nDevs].d_type = DEVICE_DPT_TYPE;
        nDevs++;
    }
*/
    App_SetIgniterLineState(0);

    HAL_GPIO_WritePin(LINE1_EN_GPIO_Port, LINE1_EN_Pin, GPIO_PIN_SET);
}

void App_Timer1ms(void)
{
    static uint16_t led_cnt = 0u;
    static uint16_t status_cnt = 0u;

    /* Статус активности МКУ раз в секунду (Dev 0 — плата) */
    if (status_cnt < 1000u) {
        status_cnt++;
    } else {
        status_cnt = 0u;
        uint32_t tick = HAL_GetTick();
        uint8_t status_data[7] = {
            (uint8_t)(tick & 0xFFu),
            (uint8_t)((tick >> 8) & 0xFFu),
            (uint8_t)((tick >> 16) & 0xFFu),
            (uint8_t)((tick >> 24) & 0xFFu),
            (uint8_t)(CAN1_Active | (CAN2_Active << 1)),
            0u,
            0u
        };
        SendMessage(0, 0, status_data, SEND_NOW, BUS_CAN12);
    }

    if (led_cnt < 1000u) {
        led_cnt++;
    } else {
        led_cnt = 0u;
        HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
    }

    /* Обновляем флаги активности CAN: если 2 секунды тишина — считаем шину неактивной */
    App_UpdateCanActivity();

    /* Обновление состояния линии спички по ADC ch2 (только когда ШИМ выключен) */
    if (!g_igniter.IsPwmActive()) {
        uint16_t raw = ADC_GetIgniterFiltered();
        uint16_t mv = (uint16_t)((uint32_t)raw * 3300u / 4095u);
        g_igniter.UpdateLineFromAdcMv(mv);
    }
    g_igniter.Timer1ms();
    //g_dpt.Timer1ms();

    App_CanProcess();
    BackendProcess();

    /* Обновляем ШИМ воспламенителя (как в старом MCU_igniter):
     * период и частота заданы в TIM4 (CubeMX),
     * виртуальное устройство выдаёт только скважность 0..99.
     */
    uint16_t pwm = App_GetIgniterPwm();
    if (pwm > 0) {
        HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_3);
        __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_3, pwm);
    } else {
        HAL_TIM_PWM_Stop(&htim4, TIM_CHANNEL_3);
    }
}

void App_UpdateCanActivity(void)
{
    uint32_t now = HAL_GetTick();

    if (can1_last_rx_tick != 0u) {
        if ((now - can1_last_rx_tick) >= 3000u) {
            CAN1_Active = 0u;
            can1_last_rx_tick = 0u;
        }
    }

    if (can2_last_rx_tick != 0u) {
        if ((now - can2_last_rx_tick) >= 3000u) {
            CAN2_Active = 0u;
            can2_last_rx_tick = 0u;
        }
    }
}

void App_SetDPTAdcValues(uint16_t ch1, uint16_t ch2)
{
    g_dpt.SetAdcValues(ch1, ch2);
}

void App_SetIgniterLineState(uint8_t state)
{
    if (state > 2) {
        state = 0;
    }
    g_igniter.SetLineState(static_cast<DeviceIgniterLineState>(state));
}

uint16_t App_GetIgniterPwm(void)
{
    return g_igniter.GetPwm();
}

} /* extern "C" */

