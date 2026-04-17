#ifdef SL_COMPONENT_CATALOG_PRESENT
#include "sl_component_catalog.h"
#endif
#include "sl_system_init.h"
#if defined(SL_CATALOG_POWER_MANAGER_PRESENT)
#include "sl_power_manager.h"
#endif
#if defined(SL_CATALOG_KERNEL_PRESENT)
#include "sl_system_kernel.h"
#else
#include "sl_system_process_action.h"
#endif // SL_CATALOG_KERNEL_PRESENT

#include "app/framework/include/af.h"
#include "em_gpio.h"
#include "em_emu.h"
#include "em_cmu.h"
#include "em_rmu.h"
#include "em_wdog.h"

#include "app.h"
#include "base_components/button.h"
#include "base_components/led.h"
#include "base_components/network_indicator.h"
#include "base_components/relay.h"
#include "device_config/config_nv.h"
#include "device_config/config_parser.h"
#include "hal/gpio.h"
#include "hal/timer.h"
#include "hal/zigbee.h"
#include "zigbee/basic_cluster.h"
#include "zigbee/consts.h"
#include "zigbee/general_commands.h"
#include "zigbee/relay_cluster.h"
#include "zigbee/switch_cluster.h"

// TODO: make configurable via ZCL
#define POLLING_INTERVAL_MS_SHORT    100
#define POLLING_INTERVAL_MS_LONG     1000

void check_reset_cause(void) {
    // Calling the function we found in your map file
    uint32_t cause = RMU_ResetCauseGet();

    if (cause & EMU_RSTCAUSE_WDOG0) {
        printf("\r\n[CRITICAL] !! WATCHDOG RESET DETECTED !!\r\n");
    } else if (cause & EMU_RSTCAUSE_DVDDBOD) {
        printf("DEBUG: Booted after Brown-out (DVDD BOD).\r\n");
    } else if (cause & EMU_RSTCAUSE_SYSREQ) {
        printf("DEBUG: Booted after Software Reset.\r\n");
    } else if (cause & EMU_RSTCAUSE_POR) {
        printf("DEBUG: Power-On Reset (Cold Boot).\r\n");
    } else {
        printf("DEBUG: Booted (Cause: 0x%08lX)\r\n", cause);
    }
}

void init_watchdog(void) {
  // Use WDOG0 specifically for the EFR32MG21
  CMU_ClockEnable(cmuClock_WDOG0, true);

  WDOG_Init_TypeDef init = WDOG_INIT_DEFAULT;
  
  // 128k = ~4s | 256k = ~8s. 
  // Recommended: Use 256k for Zigbee to prevent resets during heavy network scans.
  init.perSel = wdogPeriod_256k; 
  
  init.enable = true;          
  init.em2Run = true;           // Watchdog continues in sleep
  init.debugRun = false;        // Standard: stops when debugger pauses CPU
  
  WDOGn_Init(WDOG0, &init);
}

void drop_old_ota_image_if_any() {
    // Drop old OTA image if any exists
    // Allows to re-download FORCE image multiple times
    uint32_t currentOffset =
        sl_zigbee_af_ota_storage_driver_retrieve_last_stored_offset_cb();

    if (currentOffset > 0) {
        printf("Dropping old OTA image, current offset: %lu\n", currentOffset);
        WDOGn_Feed(WDOG0);
        sl_zigbee_af_ota_storage_clear_temp_data_cb();
        WDOGn_Feed(WDOG0);
    }
}

int main(void) {

    // Initialize Silicon Labs device, system, service(s) and protocol stack(s).
    // Note that if the kernel is present, processing task(s) will be created by
    // this call.
    sl_system_init();
    check_reset_cause();

    // Give power a chance to stabilize after a brownout
    sl_sleeptimer_delay_millisecond(500);

    // Force NVM3 to reconcile its journal
    Ecode_t nvm_status = nvm3_initDefault();
    if (nvm_status != ECODE_NVM3_OK) {
        printf("NVM3 Error: 0x%08lX. Attempting repair...\r\n", nvm_status);
        // This is often needed if a brownout happened during a write
        WDOGn_Feed(WDOG0);
        nvm3_repack(nvm3_defaultHandle);
        WDOGn_Feed(WDOG0);
    }

    app_init();

    drop_old_ota_image_if_any();

    // Init watchdog
    init_watchdog();

    // Switch should never "long poll", as it should always be somewhat reactive
    // to ZCL commands.
    sl_zigbee_af_set_short_poll_interval_ms_cb(POLLING_INTERVAL_MS_SHORT);
    sl_zigbee_af_set_long_poll_interval_ms_cb(POLLING_INTERVAL_MS_LONG);

#if defined(SL_CATALOG_KERNEL_PRESENT)
    // Start the kernel. Task(s) created in app_init() will start running.
    sl_system_kernel_start();
#else // SL_CATALOG_KERNEL_PRESENT
    while (1) {
        // Do not remove this call: Silicon Labs components process action routine
        // must be called from the super loop.
        sl_system_process_action();

        // Application process.
        app_task();

        // reset watchdog
        WDOGn_Feed(WDOG0);

        // Let the CPU go to sleep if the system allow it.
#if defined(SL_CATALOG_POWER_MANAGER_PRESENT)
        sl_power_manager_sleep();
#endif // SL_CATALOG_POWER_MANAGER_PRESENT
    }
#endif // SL_CATALOG_KERNEL_PRESENT

    return 0;
}
