#ifndef EVENT_CONFIG_H
#define EVENT_CONFIG_H

// FreeRTOS configuration
#ifndef CONFIG_FREERTOS_HZ
#define CONFIG_FREERTOS_HZ 100
#endif

#ifndef IP_EVENT
#define IP_EVENT 0
#endif

#ifndef CONFIG_FREERTOS_HZ
#define CONFIG_FREERTOS_HZ 100
#endif 

#ifndef GPIO_NUM_0
#define GPIO_NUM_0 0
#endif

#ifndef GPIO_NUM_48
#define GPIO_NUM_48 48
#endif

#ifndef GPIO_NUM_11
#define GPIO_NUM_11 11
#endif

#ifndef GPIO_NUM_12
#define GPIO_NUM_12 12
#endif

#ifndef IP_EVENT_STA_GOT_IP
#define IP_EVENT_STA_GOT_IP 0
#endif

#endif // EVENT_CONFIG_H
