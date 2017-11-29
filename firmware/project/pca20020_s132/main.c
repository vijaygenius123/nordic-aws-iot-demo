/*
  Copyright (c) 2010 - 2017, Nordic Semiconductor ASA
  All rights reserved.

  Redistribution and use in source and binary forms, with or without modification,
  are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
     list of conditions and the following disclaimer.

  2. Redistributions in binary form, except as embedded into a Nordic
     Semiconductor ASA integrated circuit in a product or a software update for
     such product, must reproduce the above copyright notice, this list of
     conditions and the following disclaimer in the documentation and/or other
     materials provided with the distribution.

  3. Neither the name of Nordic Semiconductor ASA nor the names of its
     contributors may be used to endorse or promote products derived from this
     software without specific prior written permission.

  4. This software, with or without modification, must only be used with a
     Nordic Semiconductor ASA integrated circuit.

  5. Any software provided in binary form under this license must not be reverse
     engineered, decompiled, modified and/or disassembled.

  THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
  OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
  OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @file
 *
 * @brief    Thingy application main file.
 *
 * This file contains the source code for the Thingy application that uses the Weather Station service.
 */

#include <stdint.h>
#include <float.h>
#include <string.h>
#include "sdk_config.h"
#include "nordic_common.h"
#include "nrf.h"
#include "ble_hci.h"
#include "ble_advdata.h"
#include "ble_advertising.h"
#include "ble_conn_params.h"
#include "softdevice_handler.h"
#include "app_timer_appsh.h"
#include "app_scheduler.h"
#include "app_button.h"
#include "app_util_platform.h"
#include "app_trace.h"
#include "SEGGER_RTT.h"
#include "m_environment.h"
#include "drv_hts221.h"
#include "m_motion.h"
#include "drv_ext_light.h"
#include "drv_ext_gpio.h"
#include "nrf_delay.h"
#include "twi_manager.h"
#include "pca20020.h"
#include "app_error.h"
#include "support_func.h"
#include "mqtt.h"
#include "lwip/init.h"
#include "lwip/inet6.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "ipv6_medium.h"
#include "iot_errors.h"

#define GLOBAL_DEBUG

/*******************************************************************/
/**************** BEGIN MQTT bridge (AWS) settings *****************/
/*******************************************************************/

static const uint8_t                        m_broker_addr[IPV6_ADDR_SIZE] =							/**< IPv6 addr from bt0 iface. */
{
  0xFE, 0x80, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0xba, 0x8a, 0x60, 0xff,
  0xfe, 0xc5, 0xac, 0x28,
};


#define APP_MQTT_CONNECT_DELAY              APP_TIMER_TICKS(3000, APP_TIMER_PRESCALER)               /**< MQTT Connection delay. */
#define APP_MQTT_BROKER_PORT                8883                                                    /**< Port number of MQTT Broker. */
#define APP_MQTT_THING_NAME                 "Nordic"                                                /**< AWS IoT thing name. */
#define APP_MQTT_SHADOW_TOPIC               "aws/things/Nordic/shadow/update"                       /**< AWS IoT thing shadow topic path. */
#define APP_MQTT_DATA_FORMAT                "{"                                                \
                                                "\"state\":{"                                  \
                                                    "\"reported\": {"                          \
                                                      "\"temperature\": %d,"                   \
                                                      "\"humidity\": %d,"                      \
                                                      "\"pressure\": %d,"                      \
                                                      "\"marker\": %s"                         \
                                                    "}"                                        \
                                                "}"                                            \
                                             "}"

#define MQTT_PUBLISH_INTERVAL_MS            1500                                                   /**< Interval between publishing data. */

static const uint8_t identity[] = {0x43, 0x6c, 0x69, 0x65, 0x6e, 0x74, 0x5f, 0x69, 0x64, 0x65, 0x6e, 0x74, 0x69, 0x74, 0x79};
static const uint8_t shared_secret[] = {0x73, 0x65, 0x63, 0x72, 0x65, 0x74, 0x50, 0x53, 0x4b};

static nrf_tls_preshared_key_t m_preshared_key = {              
    .p_identity     = &identity[0],
    .p_secret_key   = &shared_secret[0],
    .identity_len   = 15,
    .secret_key_len = 9
};

static nrf_tls_key_settings_t m_tls_keys = {                                                          /**< TLS keys for connection to secure MQTT bridge */
    .p_psk = &m_preshared_key
};

/*****************************************************************/
/**************** END MQTT bridge (AWS) settings *****************/
/*****************************************************************/

#define SCHED_MAX_EVENT_DATA_SIZE   MAX(APP_TIMER_SCHED_EVT_SIZE, BLE_STACK_HANDLER_SCHED_EVT_SIZE) /**< Maximum size of scheduler events. */
#define SCHED_QUEUE_SIZE            60  /**< Maximum number of events in the scheduler queue. */

#define BUTTON_1       11	// thingy
#define BUTTON_PULL    NRF_GPIO_PIN_PULLUP
#define BUTTONS_LIST { BUTTON_1 }
#define BSP_BUTTON_0   BUTTON_1
#define BSP_BUTTON_0_MASK (1<<BSP_BUTTON_0)

#define RX_PIN_NUMBER  8
#define TX_PIN_NUMBER  6
#define CTS_PIN_NUMBER 7
#define RTS_PIN_NUMBER 5
#define HWFC           true

static const nrf_drv_twi_t     m_twi_sensors = NRF_DRV_TWI_INSTANCE(TWI_SENSOR_INSTANCE);
float tmp, prs = 0.0;
uint16_t hmd = 5;

typedef enum
{
    LEDS_INACTIVE = 0,
    LEDS_CONNECTABLE_MODE,
    LEDS_IPV6_IF_DOWN,
    LEDS_IPV6_IF_UP,
    LEDS_CONNECTED_TO_BROKER,
    LEDS_PUBLISHING
} display_state_t;

#define APP_TIMER_OP_QUEUE_SIZE             5

#define LWIP_SYS_TICK_MS                    10                                                      /**< Interval for timer used as trigger to send. */
#define LED_BLINK_INTERVAL_MS               300                                                     /**< LED blinking interval. */

#define DEAD_BEEF                           0xDEADBEEF                                              /**< Value used as error code on stack dump, can be used to identify stack location on stack unwind. */
#define MAX_LENGTH_FILENAME                 128                                                     /**< Max length of filename to copy for the debug error handler. */

#define APPL_LOG                            app_trace_log                                           /**< Macro for logging application messages on UART, in case ENABLE_DEBUG_LOG_SUPPORT is not defined, no logging occurs. */
#define APPL_DUMP                           app_trace_dump                                          /**< Macro for dumping application data on UART, in case ENABLE_DEBUG_LOG_SUPPORT is not defined, no logging occurs. */

#define APP_DATA_INITIAL_TEMPERATURE        20                                                      /**< Initial simulated temperature. */
#define APP_DATA_MAXIMUM_TEMPERATURE        25                                                      /**< Maximum simulated temperature. */
#define APP_DATA_MAX_SIZE                   2000                                                    /**< Maximum size of data buffer size. */


APP_TIMER_DEF(m_iot_timer_tick_src_id);                                                             /**< App timer instance used to update the IoT timer wall clock. */
APP_TIMER_DEF(m_mqtt_conn_timer_id);                                                                /**< Timer for delaying MQTT Connection. */
eui64_t                                     eui64_local_iid;                                        /**< Local EUI64 value that is used as the IID for*/
static ipv6_medium_instance_t               m_ipv6_medium;
static display_state_t                      m_display_state    = LEDS_INACTIVE;                     /**< Board LED display state. */
static bool                                 m_interface_state  = false;                             /**< Actual status of interface. */
static bool                                 m_connection_state = false;                             /**< MQTT Connection state. */
static bool                                 m_do_publication   = false;                             /**< Indicates if MQTT publications are enabled. */

static mqtt_client_t                        m_app_mqtt_id;                                          /**< MQTT Client instance reference provided by the MQTT module. */
static const char                           m_device_id[]      = APP_MQTT_THING_NAME;               /**< Unique MQTT client identifier. */
static const char                           m_user[]           = APP_MQTT_THING_NAME;               /**< MQTT user name. */
static uint16_t                             m_temperature      = APP_DATA_INITIAL_TEMPERATURE;      /**< Actual simulated temperature. */
static uint16_t                             m_humidity;
static char                                 m_data_body[APP_DATA_MAX_SIZE];                         /**< Buffer used for publishing data. */
static uint16_t                             m_message_counter = 1;                                  /**< Message counter used to generated message ids for MQTT messages. */
static bool									m_marker		   = false;								/**< Indicates if marker was set (button was pushed). */
static void 								app_mqtt_publish_loop_start();

/**@brief Function to handle interface up event. */
void nrf_driver_interface_up(void)
{
    APPL_LOG ("[APPL]: IPv6 Interface Up.\r\n");
#ifdef COMMISSIONING_ENABLED
    commissioning_joining_mode_timer_ctrl(JOINING_MODE_TIMER_STOP_RESET);
#endif // COMMISSIONING_ENABLED

    sys_check_timeouts();

    // Set flag indicating interface state.
    m_interface_state = true;
    m_display_state = LEDS_IPV6_IF_UP;

    // try connect after stack connnected.
    app_mqtt_publish_loop_start();

}


/**@brief Function to handle interface down event. */
void nrf_driver_interface_down(void)
{
    APPL_LOG ("[APPL]: IPv6 Interface Down.\r\n");
#ifdef COMMISSIONING_ENABLED
    commissioning_joining_mode_timer_ctrl(JOINING_MODE_TIMER_START);
#endif // COMMISSIONING_ENABLED

    // Clear flag indicating interface state.
    m_interface_state = false;
    m_display_state = LEDS_IPV6_IF_DOWN;

    // A system reset is issued, becuase a defect 
    // in lwIP prevents recovery -- see SDK release notes for details.
    NVIC_SystemReset();
}

/**@brief Function for initializing IP stack.
 *
 * @details Initialize the IP Stack and its driver.
 */
static void ip_stack_init(void)
{
    uint32_t err_code;

    err_code = ipv6_medium_eui64_get(m_ipv6_medium.ipv6_medium_instance_id, \
                                     &eui64_local_iid);
    APP_ERROR_CHECK(err_code);

    err_code = nrf_mem_init();
    APP_ERROR_CHECK(err_code);

    //Initialize LwIP stack.
    lwip_init();

    //Initialize LwIP stack driver.
    err_code = nrf_driver_init();
    APP_ERROR_CHECK(err_code);

    err_code = mqtt_init();
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for starting connectable mode.
 */
static void connectable_mode_enter(void)
{
    uint32_t err_code = ipv6_medium_connectable_mode_enter(m_ipv6_medium.ipv6_medium_instance_id);
    APP_ERROR_CHECK(err_code);

    APPL_LOG("[APPL]: Physical layer in connectable mode.\r\n");
    m_display_state = LEDS_CONNECTABLE_MODE;
}


static void on_ipv6_medium_evt(ipv6_medium_evt_t * p_ipv6_medium_evt)
{
    switch (p_ipv6_medium_evt->ipv6_medium_evt_id)
    {
        case IPV6_MEDIUM_EVT_CONN_UP:
        {
            APPL_LOG("[APPL]: Physical layer: connected.\r\n");
            m_display_state = LEDS_IPV6_IF_DOWN;
            break;
        }
        case IPV6_MEDIUM_EVT_CONN_DOWN:
        {
            APPL_LOG("[APPL]: Physical layer: disconnected.\r\n");
            connectable_mode_enter();
            break;
        }
        default:
        {
            break;
        }
    }
}


static void on_ipv6_medium_error(ipv6_medium_error_t * p_ipv6_medium_error)
{
    // Do something.
}

/**@brief Function for starting mqtt publish loop.
 */
static void app_mqtt_publish_loop_start()
{
	APPL_LOG("[APPL]: Start MQTT publish loop \r\n");

	uint32_t err_code;

	// Check if interface has been established.
	if (m_interface_state == false)
	{
		return;
	}

	if (m_connection_state == false)
	{
		err_code = app_timer_start(m_mqtt_conn_timer_id, APP_MQTT_CONNECT_DELAY, NULL);
		APP_ERROR_CHECK(err_code);
	}
}


/**@brief Function for handling button events.
 *
 * @param[in]   pin_no         The pin number of the button pressed.
 * @param[in]   button_action  The action performed on button.
 */
static void button_event_handler(uint8_t pin_no, uint8_t button_action)
{
#ifdef COMMISSIONING_ENABLED
    if ((button_action == APP_BUTTON_PUSH) && (pin_no == ERASE_BUTTON_PIN_NO))
    {
        APPL_LOG("[APPL]: Erasing all commissioning settings from persistent storage... \r\n");
        commissioning_settings_clear();
        return;
    }
#endif // COMMISSIONING_ENABLED

    // Check if interface has been established.
    if(m_interface_state == false)
    {
        return;
    }

    if (button_action == APP_BUTTON_PUSH)
    {
        switch (pin_no)
        {
            case BSP_BUTTON_0:
            {
            	//app_mqtt_publish_loop_start();
            	m_marker = true;
                break;
            }
            default:
                break;
        }
    }
}

/**@brief Function for updating the wall clock of the IoT Timer module.
 */
static void iot_timer_tick_callback(void * p_context)
{
    UNUSED_VARIABLE(p_context);
    uint32_t err_code = iot_timer_update();
    APP_ERROR_CHECK(err_code);
}


/**@brief Timer callback used for periodic servicing of LwIP protocol timers.
 *        This trigger is also used in the example to trigger sending TCP Connection.
 *
 * @details Timer callback used for periodic servicing of LwIP protocol timers.
 *
 */
static void app_lwip_time_tick(iot_timer_time_in_ms_t wall_clock_value)
{
    UNUSED_PARAMETER(wall_clock_value);
    sys_check_timeouts();
    UNUSED_VARIABLE(mqtt_live());
}

void app_error_fault_handler(uint32_t id, uint32_t pc, uint32_t info)
{
    error_info_t * err_info = (error_info_t*)info;
    (void)SEGGER_RTT_printf(0, RTT_CTRL_TEXT_BRIGHT_RED"ASSERT: id = %d, pc = %d, file = %s, line number: %d, error code = %d \r\n"RTT_CTRL_RESET, \
    id, pc, err_info->p_file_name, err_info->line_num, err_info->err_code);

    //(void)m_ui_led_set_event(M_UI_ERROR);
    nrf_delay_ms(100);
    // On assert, the system can only recover with a reset.
    #ifndef DEBUG
        NVIC_SystemReset();
    #endif

    app_error_save_and_stop(id, pc, info);
}


/**@brief Function for assert macro callback.
 *
 * @details This function will be called in case of an assert in the SoftDevice.
 *
 * @warning On assert from the SoftDevice, the system can only recover on reset.
 *
 * @param[in] line_num    Line number of the failing ASSERT call.
 * @param[in] p_file_name File name of the failing ASSERT call.
 */
void assert_nrf_callback(uint16_t line_num, const uint8_t * p_file_name)
{
    app_error_handler(DEAD_BEEF, line_num, p_file_name);
}

/**@brief Function for traversing all GPIOs for lowest power consumption.
 *
 * @param[in] vdd_on    If true, keep VDD turned on.
 */
static void configure_gpio_for_sleep(vdd_state_t vdd_on)
{
    ret_code_t err_code;

    sx_gpio_cfg_t  ioext_sys_off_pin_cfg[SX_IOEXT_NUM_PINS] = IOEXT_SYSTEM_OFF_PIN_CFG;

    nrf_gpio_cfg_t   nrf_sys_off_pin_cfg[NRF_NUM_GPIO_PINS] = NRF_SYSTEM_OFF_PIN_CFG;

    #if    defined(THINGY_HW_v0_7_0)
        // VDD always on.
    #elif  defined(THINGY_HW_v0_8_0)
        // VDD always on.
    #elif  defined(THINGY_HW_v0_9_0)
        // VDD always on.
    #else
        if (vdd_on)
        {
            nrf_sys_off_pin_cfg[VDD_PWR_CTRL] = (nrf_gpio_cfg_t)NRF_PIN_OUTPUT_SET;
        }
    #endif

    err_code = drv_ext_gpio_reset();
    APP_ERROR_CHECK(err_code);

    /* Set all IO extender pins in system off state. IO extender will be powered down as well,
    Hence, this config will not be retained when VDD is turned off. */
    for (uint8_t i = 0; i < SX_IOEXT_NUM_PINS; i++)
    {
        err_code = drv_ext_gpio_cfg(i,
                         ioext_sys_off_pin_cfg[i].dir,
                         ioext_sys_off_pin_cfg[i].input_buf,
                         ioext_sys_off_pin_cfg[i].pull_config,
                         ioext_sys_off_pin_cfg[i].drive_type,
                         ioext_sys_off_pin_cfg[i].slew_rate);
        APP_ERROR_CHECK(err_code);

        switch(ioext_sys_off_pin_cfg[i].state)
        {
            case PIN_CLEAR:
                err_code = drv_ext_gpio_pin_clear(i);
                APP_ERROR_CHECK(err_code);
                break;

            case PIN_SET:
                err_code = drv_ext_gpio_pin_set(i);
                APP_ERROR_CHECK(err_code);
                break;

            case PIN_NO_OUTPUT:
                err_code = NRF_SUCCESS;     // Do nothing.
                APP_ERROR_CHECK(err_code);
                break;

            default:
                err_code = NRF_ERROR_NOT_SUPPORTED;
        }
    }

    // Set all nRF pins in desired state for power off.
    for(uint8_t i = 0; i < NRF_NUM_GPIO_PINS; i++)
    {
        nrf_gpio_cfg(i,
                     nrf_sys_off_pin_cfg[i].dir,
                     nrf_sys_off_pin_cfg[i].input,
                     nrf_sys_off_pin_cfg[i].pull,
                     nrf_sys_off_pin_cfg[i].drive,
                     nrf_sys_off_pin_cfg[i].sense);

        switch(nrf_sys_off_pin_cfg[i].state)
        {
            case PIN_CLEAR:
                nrf_gpio_pin_clear(i);
                break;

            case PIN_SET:
                nrf_gpio_pin_set(i);
                break;

            case PIN_NO_OUTPUT:
                // Do nothing.
                break;

            default:
                APP_ERROR_CHECK(NRF_ERROR_NOT_SUPPORTED);
        }
    }
}

/**@brief Function for placing the application in low power state while waiting for events.
 */
#define FPU_EXCEPTION_MASK 0x0000009F
static void power_manage(void)
{
    __set_FPSCR(__get_FPSCR()  & ~(FPU_EXCEPTION_MASK));
    (void) __get_FPSCR();
    NVIC_ClearPendingIRQ(FPU_IRQn);

    uint32_t err_code = sd_app_evt_wait();
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for initializing the Thingy.
 */
static void thingy_init(void)
{
    uint32_t                 err_code;
    m_environment_init_t     env_params;
    m_motion_init_t          motion_params;

    /**@brief Initialize the TWI manager. */
    err_code = twi_manager_init(APP_IRQ_PRIORITY_THREAD);
    APP_ERROR_CHECK(err_code);

    /**@brief Initialize environment module. */
    env_params.p_twi_instance = &m_twi_sensors;
    err_code = m_environment_init(&env_params);
    APP_ERROR_CHECK(err_code);

}


static void board_init(void)
{
    uint32_t            err_code;
    drv_ext_gpio_init_t ext_gpio_init;

    #if defined(THINGY_HW_v0_7_0)
        #error   "HW version v0.7.0 not supported."
    #elif defined(THINGY_HW_v0_8_0)
        (void)SEGGER_RTT_WriteString(0, RTT_CTRL_TEXT_BRIGHT_YELLOW"FW compiled for depricated Thingy HW v0.8.0"RTT_CTRL_RESET"\n");
    #elif defined(THINGY_HW_v0_9_0)
        (void)SEGGER_RTT_WriteString(0, RTT_CTRL_TEXT_BRIGHT_YELLOW"FW compiled for depricated Thingy HW v0.9.0"RTT_CTRL_RESET"\n");
    #endif

    /**@brief Enable power on VDD. */
    nrf_gpio_cfg_output(VDD_PWR_CTRL);
    nrf_gpio_pin_set(VDD_PWR_CTRL);

    nrf_delay_ms(75);

    static const nrf_drv_twi_config_t twi_config =
    {
        .scl                = TWI_SCL,
        .sda                = TWI_SDA,
        .frequency          = NRF_TWI_FREQ_400K,
        .interrupt_priority = APP_IRQ_PRIORITY_LOW
    };

    static const drv_sx1509_cfg_t sx1509_cfg =
    {
        .twi_addr       = SX1509_ADDR,
        .p_twi_instance = &m_twi_sensors,
        .p_twi_cfg      = &twi_config
    };

    ext_gpio_init.p_cfg = &sx1509_cfg;

    err_code = drv_ext_gpio_init(&ext_gpio_init, true);
    APP_ERROR_CHECK(err_code);

    configure_gpio_for_sleep(VDD_ON);    // Set all pins to their default state, with the exception of VDD_PWR_CTRL.

    err_code = drv_ext_gpio_cfg_output(SX_LIGHTWELL_R);
    APP_ERROR_CHECK(err_code);
    err_code = drv_ext_gpio_cfg_output(SX_LIGHTWELL_G);
    APP_ERROR_CHECK(err_code);
    err_code = drv_ext_gpio_cfg_output(SX_LIGHTWELL_B);
    APP_ERROR_CHECK(err_code);
    err_code = drv_ext_gpio_pin_set(SX_LIGHTWELL_R);
    APP_ERROR_CHECK(err_code);
    err_code = drv_ext_gpio_pin_set(SX_LIGHTWELL_G);
    APP_ERROR_CHECK(err_code);
    err_code = drv_ext_gpio_pin_set(SX_LIGHTWELL_B);
    APP_ERROR_CHECK(err_code);

    #if defined(THINGY_HW_v0_8_0)
        nrf_gpio_cfg_output(VOLUME);
        nrf_gpio_pin_clear(VOLUME);

        err_code = drv_ext_gpio_cfg_output(SX_SPK_PWR_CTRL);
        APP_ERROR_CHECK(err_code);

        err_code = drv_ext_gpio_pin_clear(SX_SPK_PWR_CTRL);
        APP_ERROR_CHECK(err_code);
    #else
        nrf_gpio_cfg_output(SPK_PWR_CTRL);
        nrf_gpio_pin_clear(SPK_PWR_CTRL);
    #endif

    nrf_delay_ms(100);
}

/**@brief Asynchronous event notification callback registered by the application with the module to receive MQTT events. */
static void app_mqtt_evt_handler(mqtt_client_t * p_client, const mqtt_evt_t * p_evt)
{
    uint32_t err_code;
    switch(p_evt->id)
    {
        case MQTT_EVT_CONNECT:
        {
            APPL_LOG("[APPL]: >> MQTT_EVT_CONNECT, result: 0x%08x\r\n", p_evt->result);

            if(p_evt->result == NRF_SUCCESS)
            {
                m_connection_state = true;
                m_do_publication   = true;
                m_display_state = LEDS_CONNECTED_TO_BROKER;
            }
            break;
        }
        case MQTT_EVT_DISCONNECT:
        {
            APPL_LOG ("[APPL]: >> MQTT_EVT_DISCONNECT\r\n");
            m_connection_state = false;
            m_display_state = LEDS_IPV6_IF_UP;

            // Make reconnection in case of tcp problem.
            if(p_evt->result == MQTT_ERR_TRANSPORT_CLOSED)
            {
                err_code = app_timer_start(m_mqtt_conn_timer_id, APP_MQTT_CONNECT_DELAY, NULL);
                APP_ERROR_CHECK(err_code);
            }
            else
            {
                // In case of normal MQTT disconnection, turn off data transfer.
                m_do_publication = false;
            }

            break;
        }
        default:
            break;
    }
}

/**@brief Timer callback used for publishing simulated temperature periodically.
 *
 */
static void app_mqtt_publish_callback(iot_timer_time_in_ms_t wall_clock_value)
{
    UNUSED_PARAMETER(wall_clock_value);
    uint32_t err_code;

    // Check if data transfer is enabled.
    if (m_do_publication == false)
    {
        return;
    }

    // Check if connection has been established.
    if (m_connection_state == false)
    {
        return;
    }

    // Reset temperature to initial state.
//    if (m_temperature == APP_DATA_MAXIMUM_TEMPERATURE)
//    {
//        m_temperature = APP_DATA_INITIAL_TEMPERATURE;
//    }

    err_code = temperature_start();
    APP_ERROR_CHECK(err_code);

    drv_humidity_temp_get(&tmp);

    m_temperature = (uint16_t)tmp;

    err_code = humidity_start();
    APP_ERROR_CHECK(err_code);

    hmd = drv_humidity_get();

    err_code = pressure_start();
    APP_ERROR_CHECK(err_code);

    drv_pressure_get(&prs);

    // Prepare data in JSON format.
    sprintf(m_data_body, APP_MQTT_DATA_FORMAT,
    		(uint16_t)tmp, hmd, (uint16_t)prs,
			(m_marker ? "true" : "false")
			);
    
    mqtt_publish_param_t param;
    
    param.message.topic.qos              = MQTT_QoS_0_AT_MOST_ONCE;
    param.message.topic.topic.p_utf_str  = (uint8_t *)APP_MQTT_SHADOW_TOPIC;
    param.message.topic.topic.utf_strlen = strlen(APP_MQTT_SHADOW_TOPIC);
    param.message.payload.p_bin_str      = (uint8_t *)m_data_body;
    param.message.payload.bin_strlen     = strlen(m_data_body);
    param.message_id                     = m_message_counter;
    param.dup_flag                       = 0;
    param.retain_flag                    = 0;

    APPL_LOG("[TEMPE]: %d\n\r", m_temperature);
    APPL_LOG("[HUMID]: %d\r\n", hmd);
    APPL_LOG("[PRESS]: %d\n\r", (uint16_t)prs);
    APPL_LOG("[MARKER]: %s\n\r", (m_marker ? "true" : "false"));

    // Publish data.
    err_code = mqtt_publish(&m_app_mqtt_id, &param);

    // Check if there is no pending transaction.
    if(err_code != (NRF_ERROR_BUSY | IOT_MQTT_ERR_BASE))
    {
        APP_ERROR_CHECK(err_code);
        m_message_counter += 2;
    }

    // Increase temperature value.
    // m_temperature++;

    // Flash marker value
	m_marker = false;

    APPL_LOG("[APPL]: mqtt_publish result 0x%08lx\r\n", err_code);
}

/**@brief Timer callback used for connecting to AWS cloud.
 *
 * @param[in]   p_context   Pointer used for passing context. No context used in this application.
 */
static void app_mqtt_connect(void * p_context)
{
    UNUSED_VARIABLE(p_context);

    uint32_t       err_code;

    mqtt_client_init(&m_app_mqtt_id);

    static mqtt_username_t username;
    username.p_utf_str  = (uint8_t *)m_user;
    username.utf_strlen = strlen(m_user);

    memcpy (m_app_mqtt_id.broker_addr.u8, m_broker_addr, IPV6_ADDR_SIZE);

    m_app_mqtt_id.broker_port             = APP_MQTT_BROKER_PORT;
    m_app_mqtt_id.evt_cb                  = app_mqtt_evt_handler;
    m_app_mqtt_id.client_id.p_utf_str     = (uint8_t *)m_device_id;
    m_app_mqtt_id.client_id.utf_strlen    = strlen(m_device_id);
    m_app_mqtt_id.p_password              = NULL;
    m_app_mqtt_id.p_user_name             = NULL/*(mqtt_username_t *)&username*/;
    m_app_mqtt_id.transport_type          = MQTT_TRANSPORT_SECURE;
    m_app_mqtt_id.p_security_settings     = &m_tls_keys;

    err_code = mqtt_connect(&m_app_mqtt_id);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for the Timer initialization.
 *
 * @details Initializes the timer module. This creates and starts application timers.
 */
static void timers_init(void)
{
    uint32_t err_code;

    // Initialize timer module.
//    APP_TIMER_APPSH_INIT(APP_TIMER_PRESCALER, APP_TIMER_OP_QUEUE_SIZE, false);	// iot
    APP_TIMER_APPSH_INIT(APP_TIMER_PRESCALER, APP_TIMER_OP_QUEUE_SIZE, true);		// thingy

    // Create timer to create delay connection to AWS.
    err_code = app_timer_create(&m_mqtt_conn_timer_id,
                                APP_TIMER_MODE_SINGLE_SHOT,
                                app_mqtt_connect);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for initializing the IoT Timer. */
static void iot_timer_init(void)
{
    uint32_t err_code;

    static const iot_timer_client_t list_of_clients[] =
    {
        {app_lwip_time_tick,          LWIP_SYS_TICK_MS},
        {app_mqtt_publish_callback, MQTT_PUBLISH_INTERVAL_MS},
#ifdef COMMISSIONING_ENABLED
        {commissioning_time_tick,     SEC_TO_MILLISEC(COMMISSIONING_TICK_INTERVAL_SEC)}
#endif // COMMISSIONING_ENABLED
    };

    // The list of IoT Timer clients is declared as a constant.
    static const iot_timer_clients_list_t iot_timer_clients =
    {
        (sizeof(list_of_clients) / sizeof(iot_timer_client_t)),
        &(list_of_clients[0]),
    };

    // Passing the list of clients to the IoT Timer module.
    err_code = iot_timer_client_list_set(&iot_timer_clients);
    APP_ERROR_CHECK(err_code);

    // Create app timer to serve as tick source.
    err_code = app_timer_create(&m_iot_timer_tick_src_id,
                                APP_TIMER_MODE_REPEATED,
                                iot_timer_tick_callback);
    APP_ERROR_CHECK(err_code);

    // Starting the app timer instance that is the tick source for the IoT Timer.
    err_code = app_timer_start(m_iot_timer_tick_src_id, \
                               APP_TIMER_TICKS(IOT_TIMER_RESOLUTION_IN_MS, APP_TIMER_PRESCALER), \
                               NULL);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for the Button initialization.
 *
 * @details Initializes all Buttons used by this application.
 */
static void button_init(void)
{
    uint32_t err_code;

    static app_button_cfg_t buttons[] =
    {
        {BSP_BUTTON_0,        false, BUTTON_PULL, button_event_handler}
    };

    #define BUTTON_DETECTION_DELAY APP_TIMER_TICKS(50, APP_TIMER_PRESCALER)

    err_code = app_button_init(buttons, sizeof(buttons) / sizeof(buttons[0]), BUTTON_DETECTION_DELAY);
    APP_ERROR_CHECK(err_code);

    err_code = app_button_enable();
    APP_ERROR_CHECK(err_code);
}

/**@brief Application main function.
 */
int main(void)
{
    uint32_t err_code;

    static ipv6_medium_init_params_t ipv6_medium_init_params;
    memset(&ipv6_medium_init_params, 0x00, sizeof(ipv6_medium_init_params));
    ipv6_medium_init_params.ipv6_medium_evt_handler    = on_ipv6_medium_evt;
    ipv6_medium_init_params.ipv6_medium_error_handler  = on_ipv6_medium_error;
    ipv6_medium_init_params.use_scheduler              = false;

    err_code = ipv6_medium_init(&ipv6_medium_init_params, \
                                IPV6_MEDIUM_ID_BLE,       \
                                &m_ipv6_medium);
    APP_ERROR_CHECK(err_code);

    eui48_t ipv6_medium_eui48;
    err_code = ipv6_medium_eui48_get(m_ipv6_medium.ipv6_medium_instance_id, \
                                     &ipv6_medium_eui48);

    ipv6_medium_eui48.identifier[EUI_48_SIZE - 1] = 0x00;

    err_code = ipv6_medium_eui48_set(m_ipv6_medium.ipv6_medium_instance_id, \
                                     &ipv6_medium_eui48);
    APP_ERROR_CHECK(err_code);

    ip_stack_init();

    //Start execution.
    connectable_mode_enter();

    board_init();
	timers_init();
	iot_timer_init();
	button_init();
	thingy_init();

	// Initialize.
    APP_SCHED_INIT(SCHED_MAX_EVENT_DATA_SIZE, SCHED_QUEUE_SIZE);

    err_code = temperature_start();
    APP_ERROR_CHECK(err_code);

    err_code = humidity_start();
    APP_ERROR_CHECK(err_code);

    err_code = pressure_start();
    APP_ERROR_CHECK(err_code);

    //Enter main loop.
    for (;;)
    {
		app_sched_execute();
    }

}
