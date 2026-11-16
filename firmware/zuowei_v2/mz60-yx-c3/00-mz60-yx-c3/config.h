#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define BOOT_BUTTON_GPIO        GPIO_NUM_9

#define CODEC_TX_GPIO           GPIO_NUM_12
#define CODEC_RX_GPIO           GPIO_NUM_13

#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

#define BATTERY_CHARG_GPIO      GPIO_NUM_6
#define BATTERY_ADC_GPIO        GPIO_NUM_4
#define BATTERY_VOLTAGE_MAX     680
#define BATTERY_VOLTAGE_MIN     557

#define POWER_KEEP_GPIO         GPIO_NUM_10
#define POWER_KEEP_LEVEL        1
#define POWER_SLEEP_TIME        -1
#define POWER_SHUTDOWN_TIME     -1
// #define POWER_SLEEP_TIME        (30*1000)
// #define POWER_SHUTDOWN_TIME     (1800*1000)

#define CONFIG_POWER_ON_SOUNDS
#define CONFIG_NET_STA_SOUNDS

#define RGB_DI_GPIO                GPIO_NUM_8

#endif // _BOARD_CONFIG_H_
