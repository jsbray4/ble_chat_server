// app_cli.c
#include "sl_cli.h"
#include "sl_bt_api.h"
#include "app_assert.h"
#include "app.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// -----------------------------------------------------------------------------
// Externs / globals from your application
// -----------------------------------------------------------------------------
extern volatile conn_state_t conn_state;
extern uint8_t connection_handle;
extern uint32_t service_handle;
extern uint16_t characteristic_handle;
#define SL_CLI_CHAT_BUFFER_SIZE 50 
#define NAME                          "Jesse: " //prepended to peripheral messages
//erase current line, may not produce desired effects in all terminals, used for macos native terminal
#define REMOVE_PREV_LINE            "\x1B[2K"  

// -----------------------------------------------------------------------------
// Command Handlers
// -----------------------------------------------------------------------------
//initiate scanner
static void cmd_scan(sl_cli_command_arg_t *arguments)
{
    sl_status_t sc;
    if (conn_state == init) {
        printf("Scanning started\r\n");
        sc = sl_bt_connection_set_default_parameters(CONN_INTERVAL_MIN,
                                    CONN_INTERVAL_MAX,
                                    CONN_RESPONDER_LATENCY,
                                    CONN_TIMEOUT,
                                    CONN_MIN_CE_LENGTH,
                                    CONN_MAX_CE_LENGTH);
        app_assert_status(sc);
        sc = sl_bt_scanner_start(sl_bt_scanner_scan_phy_1m,
                            sl_bt_scanner_discover_generic);
        app_assert_status_f(sc, "Failed to start discovery #1\r\n");
        conn_state = scanning;
    } else {
        printf("Cannot start scan: wrong state\r\n");
    }
}

//set io capabilities
//since peripheral io is nonconfigurable and set to keyboard/display
//setting central to keyboard enforces passkey authentication
//and setting central to keyboard/display enforces numeric comparison authentication
static void cmd_security(sl_cli_command_arg_t *arguments)
{
    const char *mode = sl_cli_get_argument_string(arguments, 0);

    uint8_t flags = SL_BT_SM_CONFIGURATION_MITM_REQUIRED | SL_BT_SM_CONFIGURATION_SC_ONLY | SL_BT_SM_CONFIGURATION_BONDING_REQUEST_REQUIRED | SL_BT_SM_CONFIGURATION_PREFER_MITM;
    uint8_t io_capabilities;

    if (strcmp(mode, "passkey") == 0) {
        io_capabilities = sl_bt_sm_io_capability_keyboardonly;

    } else if (strcmp(mode, "numeric") == 0) {
        io_capabilities = sl_bt_sm_io_capability_keyboarddisplay;

    } else {
        printf("Invalid argument value. Use: passkey | numeric\r\n");
        return;
    }

    sl_status_t sc = sl_bt_sm_configure(flags, io_capabilities);

    if (sc != SL_STATUS_OK) {
        printf("Failed to configure security (0x%04X)\r\n", sc);
        return;
    }

    printf("Security configuration applied\r\n");
}

//set bonding mode
static void cmd_bondable(sl_cli_command_arg_t *arguments)
{
    int value = sl_cli_get_argument_int8(arguments, 0);

    if (value != 0 && value != 1) {
        printf("Invalid argument value. Use 0 (disable) or 1 (enable)\r\n");
        return;
    }

    sl_status_t sc = sl_bt_sm_set_bondable_mode(value);

    if (sc != SL_STATUS_OK) {
        printf("Failed to set bondable mode (0x%04X)\r\n", sc);
        return;
    }
    if(value)
        printf("Bonding enabled successfully\r\n");
    else
        printf("Bonding disabled successfully\r\n");
}

//erase bonds
static void cmd_erase(sl_cli_command_arg_t *arguments)
{
    sl_status_t sc = sl_bt_sm_delete_bondings();
    if(sc != SL_STATUS_OK)
    {
        printf("Bonds failed to erase\r\n");
    }
    else
        printf("Bonds successfully erased\r\n");

}

//disconnect from peripheral
static void cmd_dc(sl_cli_command_arg_t *arguments)
{
    if(connection_handle != CONNECTION_HANDLE_INVALID) {
      sl_bt_connection_close(connection_handle);
    }
    else{
      printf("Cannot disconnect: not connected\r\n");
    }
}

//send a chat
static void cmd_chat(sl_cli_command_arg_t *arguments)
{
    //clear current line
    printf(REMOVE_PREV_LINE);

    if (conn_state != running) {
        printf("Cannot send message: not connected\r\n");
        return;
    }

    char buf[SL_CLI_CHAT_BUFFER_SIZE];
    size_t pos = 0;

    //prepend NAME
    size_t name_len = strlen(NAME);
    if (name_len >= SL_CLI_CHAT_BUFFER_SIZE) return;
    memcpy(buf + pos, NAME, name_len);
    pos += name_len;

    //no argument provided
    int argc = sl_cli_get_argument_count(arguments);
    if (argc < 1) {
        printf("Incorrect number of arguments\r\n"); //removing this breaks code?
        return;
    }

    //append all CLI arguments
    for (int i = 0; i < argc; i++) {
        const char *arg = sl_cli_get_argument_string(arguments, i);
        if (!arg) continue;

        size_t arg_len = strlen(arg);
        if (pos + arg_len + 2 >= SL_CLI_CHAT_BUFFER_SIZE) {
            arg_len = SL_CLI_CHAT_BUFFER_SIZE - pos - 2; //truncate if needed
        }

        memcpy(buf + pos, arg, arg_len);
        pos += arg_len;

        //add space between arguments
        if (i < argc - 1 && pos + 1 < SL_CLI_CHAT_BUFFER_SIZE) {
            buf[pos++] = ' ';
        }
    }

    //append \r\n
    if (pos + 2 < SL_CLI_CHAT_BUFFER_SIZE) {
        buf[pos++] = '\r';
        buf[pos++] = '\n';
    }

    //write to GATT characteristic
    sl_status_t sc = sl_bt_gatt_write_characteristic_value(
        connection_handle,
        characteristic_handle,
        pos,          
        (uint8_t*)buf
    );
    if (sc != SL_STATUS_OK) {
        printf("Failed to send message (0x%02X)\r\n", sc);
        return;
    }

    //echo to central terminal
    fwrite(buf, sizeof(char), pos, stdout);
}

//print menu
static void cmd_menu(sl_cli_command_arg_t *arguments)
{
    (void)arguments;
    print_menu();
}

//verify bonding/pairing
static void cmd_verify(sl_cli_command_arg_t *arguments)
{
    sl_status_t sc;

    if (conn_state != passkey && conn_state != numeric) {
        printf("No passkey to verify\r\n");
        return;
    }

    const char *key_str = sl_cli_get_argument_string(arguments, 0);

    //passkey
    if (conn_state == passkey) {
        //must be exactly 6 digits
        if (strlen(key_str) != 6) {
            printf("Invalid passkey. Must be a 6-digit number.\r\n");
            return;
        }

        char *endptr = NULL;
        uint32_t key = strtoul(key_str, &endptr, 10);  // Convert base 10

        //check for invalid characters, read strtoul docs
        if (*endptr != '\0') {
            printf("Invalid passkey. Must be numeric.\r\n");
            return;
        }

        //enter passkey
        sc = sl_bt_sm_enter_passkey(connection_handle, key);
        if (sc != SL_STATUS_OK) {
            printf("Failed to enter passkey (0x%02X)\r\n", sc);
            return;
        }
        printf("Passkey %s entered\r\n", key_str);

    } 
    else if (conn_state == numeric) { //numeric comparison
        //expects 0 or 1
        if (strlen(key_str) != 1 || (key_str[0] != '0' && key_str[0] != '1')) {
            printf("Invalid input. Use 1 to confirm, 0 to reject.\r\n");
            return;
        }

        //confirm/deny pairing
        uint8_t confirm = (key_str[0] == '1') ? 1 : 0;
        sc = sl_bt_sm_passkey_confirm(connection_handle, confirm);
        if (sc != SL_STATUS_OK) {
            printf("Failed to confirm passkey (0x%02X)\r\n", sc);
            return;
        }
        printf("Numeric comparison %s\r\n", confirm ? "confirmed" : "rejected");
    }
}

// -----------------------------------------------------------------------------
// Command Info (metadata)
// -----------------------------------------------------------------------------
static const sl_cli_command_info_t cmd_scan_info =
    SL_CLI_COMMAND(cmd_scan, "Start scanning", "", { SL_CLI_ARG_END });

static const sl_cli_command_info_t cmd_security_info =
    SL_CLI_COMMAND(cmd_security,
                   "Set security mode",
                   "passkey | numeric",
                   { SL_CLI_ARG_STRING, SL_CLI_ARG_END });

static const sl_cli_command_info_t cmd_bondable_info =
    SL_CLI_COMMAND(cmd_bondable,
                   "Allow/reject new bondings",
                   "0 = reject, 1 = allow",
                   { SL_CLI_ARG_INT8, SL_CLI_ARG_END });

static const sl_cli_command_info_t cmd_erase_info =
    SL_CLI_COMMAND(cmd_erase, "Delete bondings", "", { SL_CLI_ARG_END });

static const sl_cli_command_info_t cmd_dc_info =
    SL_CLI_COMMAND(cmd_dc, "Disconnect connection", "", { SL_CLI_ARG_END });

static const sl_cli_command_info_t cmd_chat_info =
    SL_CLI_COMMAND(cmd_chat, "Send a message", "", { SL_CLI_ARG_WILDCARD,SL_CLI_ARG_END });

static const sl_cli_command_info_t cmd_menu_info =
    SL_CLI_COMMAND(cmd_menu, "Show command menu", "", { SL_CLI_ARG_END });

static const sl_cli_command_info_t cmd_verify_info =
    SL_CLI_COMMAND(cmd_verify, "Verify passkey or numeric comparison", "passkey (6-digit) or numeric (0/1)", { SL_CLI_ARG_STRING, SL_CLI_ARG_END });


// -----------------------------------------------------------------------------
// Command Table
// -----------------------------------------------------------------------------
static const sl_cli_command_entry_t app_cli_table[] = {
    { "/scan", &cmd_scan_info, false },
    { "/security", &cmd_security_info, false },
    { "/erase", &cmd_erase_info, false },
    { "/bondable", &cmd_bondable_info, false },
    { "/dc", &cmd_dc_info, false },
    { "/chat", &cmd_chat_info, false },
    { "/menu", &cmd_menu_info, false },
    { "/verify", &cmd_verify_info, false },
    { NULL, NULL, false } // terminator
};

// -----------------------------------------------------------------------------
// Command Group
// -----------------------------------------------------------------------------
sl_cli_command_group_t app_cli_group = {
    { NULL }, // no parent
    false,    // visible
    app_cli_table
};

// -----------------------------------------------------------------------------
// CLI Initialization Helper
// -----------------------------------------------------------------------------
void app_cli_init(sl_cli_handle_t *cli_handle)
{
    //cli initialization, call from app.c
    sl_cli_command_add_command_group(*cli_handle, &app_cli_group);
}