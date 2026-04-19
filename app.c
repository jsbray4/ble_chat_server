/***************************************************************************//**
 * @file
 * @brief Core application logic.
 *******************************************************************************
 * # License
 * <b>Copyright 2024 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/
#include "sl_bt_api.h"
#include "sl_main_init.h"
#include "app_assert.h"
#include "app.h"
#include "colors.h"
#include <stdio.h>
#include "app_cli.h"
#include "sl_cli_handles.h"

// The advertising set handle allocated from Bluetooth stack.
static uint8_t advertising_set_handle = 0xff;

//UUID's set in peripheral Simplicity Studio bluetooth GATT configurator
//128 bit random service uuid
static const uint8_t custom_service_uuid[16] = {
    0x7c, 0x3b, 0x9a, 0xf1, 0xe4, 0x2d, 0x56, 0x8f, 
    0x3c, 0xa1, 0x4b, 0x22, 0xd7, 0x9e, 0x05, 0x6a
};
//128 bit random characteristic uuid
static const uint8_t custom_char_uuid[16] = {
    0xa2, 0x1b, 0x4f, 0xd3, 0x7c, 0xe8, 0x9a, 0x42,
    0xbc, 0x6d, 0x11, 0x33, 0x55, 0x77, 0x99, 0xee
};

volatile conn_state_t conn_state = init;
volatile uint8_t connection_handle = CONNECTION_HANDLE_INVALID;
volatile uint32_t service_handle = SERVICE_HANDLE_INVALID;
volatile uint16_t characteristic_handle = CHARACTERISTIC_HANDLE_INVALID;

static void menu_handler(uint8_t rx_byte);
static void message_handler(uint8_t rx_byte);
void print_menu(void);
static uint8_t find_service_in_advertisement(uint8_t *data, uint8_t len);


// Application Init.
void app_init(void)
{
  //initialize cli
  app_cli_init(&sl_cli_inst_handle);
}

//running state logic
void app_process_action(void)
{

  if (conn_state == running) {
    if (connection_handle != CONNECTION_HANDLE_INVALID) { //ensure connection valid

      //get connection rssi
      int8_t rssi;
      char color_buf[20];
      sl_status_t sc = sl_bt_connection_get_median_rssi(connection_handle, &rssi);
      if (sc != SL_STATUS_OK) {
        return;
      }

      //print rssi to terminal
      //color based on connection strength
      if(rssi >= -65) {
        snprintf(color_buf, sizeof(color_buf), COLOR_GREEN);
      }
      else if(rssi >= -75) {
        snprintf(color_buf, sizeof(color_buf), COLOR_YELLOW);
      }
      else {
        snprintf(color_buf, sizeof(color_buf), COLOR_RED);
      }

      //appends it to the end of previous line
      printf("%s\x1B[1A\t\t\t\t\t\t\t\t[RSSI] %d dBm%s\r\n", color_buf, rssi, COLOR_EXIT);
    }
  }
}

/**************************************************************************//**
 * Bluetooth stack event handler.
 * This overrides the default weak implementation.
 *
 * @param[in] evt Event coming from the Bluetooth stack.
 *****************************************************************************/
void sl_bt_on_event(sl_bt_msg_t *evt)
{
  sl_status_t sc;
  uint16_t addr_value;
  switch (SL_BT_MSG_ID(evt->header)) {
    // -------------------------------
    // This event indicates the device has started and the radio is ready.
    // Do not call any stack command before receiving this boot event!
     case sl_bt_evt_system_boot_id:
      //configure security flags
      uint8_t flags = SL_BT_SM_CONFIGURATION_MITM_REQUIRED | SL_BT_SM_CONFIGURATION_SC_ONLY | SL_BT_SM_CONFIGURATION_BONDING_REQUEST_REQUIRED | SL_BT_SM_CONFIGURATION_PREFER_MITM;
      sc = sl_bt_sm_configure(flags, sl_bt_sm_io_capability_keyboardonly); //configurable on central, fixed on peripheral
      app_assert_status(sc);

      //configure bonding
      sc = sl_bt_sm_store_bonding_configuration(4, 2);
      app_assert_status(sc);
      sc = sl_bt_sm_set_bondable_mode(0);
      app_assert_status(sc);

      print_menu();
      break;

    // -------------------------------
    // This event is generated when an advertisement packet or a scan response
    // is received from a responder
    case sl_bt_evt_scanner_legacy_advertisement_report_id:
      //parse advertisement packets
      if (evt->data.evt_scanner_legacy_advertisement_report.event_flags
          == (SL_BT_SCANNER_EVENT_FLAG_CONNECTABLE | SL_BT_SCANNER_EVENT_FLAG_SCANNABLE)) {
        //if chat app service uuid found in advertisement
        if (find_service_in_advertisement(&(evt->data.evt_scanner_legacy_advertisement_report.data.data[0]),
                                          evt->data.evt_scanner_legacy_advertisement_report.data.len) != 0) {

          //stop scanner
          sc = sl_bt_scanner_stop();
          app_assert_status(sc);

          //connect to device
          sc = sl_bt_connection_open(evt->data.evt_scanner_legacy_advertisement_report.address,
                                      evt->data.evt_scanner_legacy_advertisement_report.address_type,
                                      sl_bt_gap_phy_1m,
                                      NULL);
          app_assert_status(sc);

          conn_state = opening;
        }
      }
      break;

    // -------------------------------
    // This event is generated when a new connection is established
    case sl_bt_evt_connection_opened_id:
      //store connection handle
      printf("Connection opened\r\n");
      connection_handle = evt->data.evt_connection_opened.connection;

      //initiate pairing; skips if previously bonded
      sc = sl_bt_sm_increase_security(evt->data.evt_connection_opened.connection);
      app_assert_status(sc);
      break;

    // -------------------------------
    // This event is generated when a new service is discovered
    case sl_bt_evt_gatt_service_id:
      //store service handle
      printf("Service discovered\r\n"); 
      service_handle = evt->data.evt_gatt_service.service;
      break;

    // -------------------------------
    // This event is generated when a new characteristic is discovered
    case sl_bt_evt_gatt_characteristic_id:
      //store characteristic handle
      printf("Characteristic discovered\r\n");
      characteristic_handle = evt->data.evt_gatt_characteristic.characteristic;
      break;

// -------------------------------
    // This event is generated for various procedure completions, e.g. when a
    // write procedure is completed, or service discovery is completed
    case sl_bt_evt_gatt_procedure_completed_id:
      //if service discovery finished
      if (conn_state == discover_services && service_handle != SERVICE_HANDLE_INVALID) {
        //discover message stream characteristic on peripheral
        sc = sl_bt_gatt_discover_characteristics_by_uuid(evt->data.evt_gatt_procedure_completed.connection,
                                                         service_handle,
                                                         sizeof(custom_char_uuid),
                                                         (const uint8_t*)custom_char_uuid);
        app_assert_status(sc);
        conn_state = discover_characteristics;
        break;
      }
      //if characteristic discovery finished
      if (conn_state == discover_characteristics && characteristic_handle != CHARACTERISTIC_HANDLE_INVALID) {
        // stop discovering
        sl_bt_scanner_stop();

        //enable notifications on message stream characteristic
        sc = sl_bt_gatt_set_characteristic_notification(evt->data.evt_gatt_procedure_completed.connection,
                                                        characteristic_handle,
                                                        sl_bt_gatt_notification);
        app_assert_status(sc);

        conn_state = running;
        break;
      }
      break;

// -------------------------------
    // This event is generated when a notification is received   
    case sl_bt_evt_gatt_characteristic_value_id:
      //echo value to terminal
      fwrite(evt->data.evt_gatt_characteristic_value.value.data, sizeof(uint8_t), evt->data.evt_gatt_characteristic_value.value.len, stdout);
    break;

    // -------------------------------
    // This event is generated when a connection is dropped
    case sl_bt_evt_connection_closed_id:
      //reset handles + app state
      printf("Connection terminated\r\n");
      connection_handle = CONNECTION_HANDLE_INVALID;
      service_handle = SERVICE_HANDLE_INVALID;
      characteristic_handle = CHARACTERISTIC_HANDLE_INVALID;
      print_menu();
      conn_state = init;
      break;

    // This event indicated connection parameters have changed
    case sl_bt_evt_connection_parameters_id:
      //print connection parameters
      printf("Connection parameters updated:\r\n");
      printf("\tConnection Handle: %u\r\n", evt->data.evt_connection_parameters.connection);
      printf("\tInterval: %u * 1.25ms\r\n", evt->data.evt_connection_parameters.interval);
      printf("\tLatency: %u\r\n", evt->data.evt_connection_parameters.latency);
      printf("\tTimeout: %u * 10ms\r\n", evt->data.evt_connection_parameters.timeout);
      printf("\tSecurity Mode: %u\r\n", evt->data.evt_connection_parameters.security_mode);

      //triggers in all scenarios
      if(evt->data.evt_connection_parameters.security_mode == 0)
        printf("Waiting on pairing and/or bonding\r\n");
      //connection authenticated
      if(evt->data.evt_connection_parameters.security_mode == 3)
      {
        //triggered if bond already existed
        if(conn_state == opening)
          printf("Previous bond used to reconnect\r\n");
        
        //initiate service discovery
        sc = sl_bt_gatt_discover_primary_services_by_uuid(connection_handle,
                                                  sizeof(custom_service_uuid),
                                                  (const uint8_t*)custom_service_uuid);
        
        if (sc == SL_STATUS_INVALID_HANDLE) {
          // Failed to open connection, restart scanning
          sc = sl_bt_scanner_start(sl_bt_gap_phy_1m, sl_bt_scanner_discover_generic);
          app_assert_status(sc);
          conn_state = scanning;
          break;
        } 
        else {
          app_assert_status(sc);
          conn_state = discover_services;
        }
      }
      break;

    //passkey pairing, triggered if central set to keyboard only
    case sl_bt_evt_sm_passkey_request_id:
        printf("Passkey confirmation requested. Please enter the passkey displayed on the peripheral device using '/verify {xxxxxx}'.\r\n");
        conn_state = passkey;
        break;
    
    //numeric comparison pairing, triggered if central set to keyboard/display
    case sl_bt_evt_sm_confirm_passkey_id:
        printf("Numeric comparison confirmation requested. Please confirm the passkey %06u matches using '/verify {0,1}'.\r\n", evt->data.evt_sm_confirm_passkey.passkey);
        conn_state = numeric;
        break;

    //triggered when bonding complete
    case sl_bt_evt_sm_bonded_id:
        printf("Pairing process completed\r\n");
        break;

    //triggered when bonding/pairing process fails
    //peripheral handles closing the connection
    case sl_bt_evt_sm_bonding_failed_id:
      printf("Bonding/pairing process failed\r\n");
      break;

        // -------------------------------
    // Default event handler.
    default:
      break;
}
}

// Parse advertisements looking for advertised chat app service
static uint8_t find_service_in_advertisement(uint8_t *data, uint8_t len)
{
  uint8_t ad_field_length;
  uint8_t ad_field_type;
  uint8_t i = 0;

  while (i < len) {
    ad_field_length = data[i];
    ad_field_type = data[i + 1];

    // Check for 128-bit UUIDs (custom services)
    if (ad_field_type == 0x06 || ad_field_type == 0x07) {

      // Number of UUIDs in this field
      uint8_t uuid_count = (ad_field_length - 1) / 16;

      for (uint8_t j = 0; j < uuid_count; j++) {
        if (memcmp(&data[i + 2 + (j * 16)], custom_service_uuid, 16) == 0) {
          return 1;
        }
      }
    }

    // Move to next AD structure
    i += ad_field_length + 1;
  }

  return 0;
}

void print_menu(void)
{
    printf("\r\n=== BLE Chat App Commands ===\r\n");

    //commands
    printf("/scan       - Start scanning for devices\r\n");
    printf("/security   - Configure security settings (passkey/numeric)\r\n");
    printf("/erase      - Erase all bonds\r\n");
    printf("/bondable   - Enable or disable bondable mode (0 or 1)\r\n");
    printf("/dc         - Disconnect from current device\r\n");
    printf("/chat       - Send a chat message to the connected device\r\n");
    printf("/menu       - Show this menu\r\n");
    printf("\r\n");
}