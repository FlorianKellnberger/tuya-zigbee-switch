#include "config_nv.h"
#include "hal/nvm.h"
#include "hal/printf_selector.h"
#include "nvm_items.h"
#include <string.h>

#ifdef HAL_SILABS
#include "silabs_config.h"
#endif

#ifndef STRINGIFY
#define _STRINGIFY(x)    #x
#define STRINGIFY(x)     _STRINGIFY(x)
#endif

#ifndef DEFAULT_CONFIG
const char default_config_data[] = "unknown;TS0012-CUSTOM;";
#else
const char default_config_data[] = STRINGIFY(DEFAULT_CONFIG);
#endif

device_config_str_t device_config_str;

void device_config_write_to_nv() {
    // We write the ENTIRE struct because the first 2 bytes ARE the ZCL length
    // Total bytes to write: 2 (for size) + 128 (for data) = 130 bytes
    uint16_t total_size = sizeof(device_config_str_t); 

    hal_nvm_status_t st = hal_nvm_write(NV_ITEM_DEVICE_CONFIG, 
                                        total_size, 
                                        (uint8_t *)&device_config_str);

    if (st != HAL_NVM_SUCCESS) {
        printf("NV Write Failed: 0x%lx\r\n", (unsigned long)st);
    }
}

void device_config_read_from_nv() {
    // Read the full 130 bytes back into the struct
    hal_nvm_status_t st = hal_nvm_read(NV_ITEM_DEVICE_CONFIG, 
                                       sizeof(device_config_str_t), 
                                       (uint8_t *)&device_config_str);
    
    if (st != HAL_NVM_SUCCESS) {
        // Fallback to defaults if NVM is empty or failed
        device_config_str.size = strlen((char*)default_config_data);
        memcpy(device_config_str.data, default_config_data, device_config_str.size);
    }
}