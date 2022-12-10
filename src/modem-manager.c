#include <stdio.h>
#include <errno.h>
#include <termios.h>
#include <gpiod.h>
#include <libusb.h>
#include <gio/gio.h>
#include "modem-manager.h"
#include "common.h"

#define PCAT_MODEM_MANAGER_POWER_WAIT_TIME 50
#define PCAT_MODEM_MANAGER_POWER_READY_TIME 30
#define PCAT_MODEM_MANAGER_RESET_ON_TIME 3
#define PCAT_MODEM_MANAGER_RESET_WAIT_TIME 30

typedef enum
{
    PCAT_MODEM_MANAGER_STATE_NONE,
    PCAT_MODEM_MANAGER_STATE_READY
}PCatModemManagerState;

typedef struct _PCatModemManagerUSBData
{
    PCatModemManagerDeviceType device_type;
    guint16 id_vendor;
    guint16 id_product;
    const gchar *external_control_exec;
    gboolean external_control_exec_is_daemon;
}PCatModemManagerUSBData;

typedef struct _PCatModemManagerData
{
    gboolean initialized;
    gboolean work_flag;
    GMutex mutex;
    PCatModemManagerState state;
    GThread *modem_work_thread;
    GHashTable *modem_mode_table;
    PCatModemManagerMode modem_mode;
    gboolean modem_rfkill_state;
    gint modem_signal_strength;
    PCatModemManagerSIMState sim_state;
    gchar *isp_name;
    gchar *isp_plmn;

    libusb_context *usb_ctx;

    struct gpiod_chip *gpio_modem_power_chip;
    struct gpiod_chip *gpio_modem_rf_kill_chip;
    struct gpiod_chip *gpio_modem_reset_chip;
    struct gpiod_line *gpio_modem_power_line;
    struct gpiod_line *gpio_modem_rf_kill_line;
    struct gpiod_line *gpio_modem_reset_line;

    GSubprocess *external_control_exec_process;
    GInputStream *external_control_exec_stdout_stream;
    GSource *external_control_exec_stdout_read_source;
    GString *external_control_exec_stdout_buffer;

    FILE *external_control_exec_stdout_log_file;
    PCatModemManagerDeviceType device_type;
    gboolean modem_have_5g_connected;
    gint64 modem_5g_connection_timestamp;

    guint scanning_timeout_id;
}PCatModemManagerData;

static PCatModemManagerUSBData g_pcat_modem_manager_supported_dev_list[] =
{
    {
        .device_type = PCAT_MODEM_MANAGER_DEVICE_5G,
        .id_vendor = 0x2C7C,
        .id_product = 0x900,
        .external_control_exec = "quectel-cm",
        .external_control_exec_is_daemon = FALSE
    },
    {
        .device_type = PCAT_MODEM_MANAGER_DEVICE_GENERAL,
        .id_vendor = 0x2C7C,
        .id_product = 0,
        //.external_control_exec = "quectel-cm",
        .external_control_exec = NULL,
        .external_control_exec_is_daemon = FALSE
    }
};

static PCatModemManagerData g_pcat_modem_manager_data = {0};

static inline gboolean pcat_modem_manager_modem_power_init(
    PCatModemManagerData *mm_data, PCatManagerMainConfigData *main_config_data)
{
    guint i;
    gint ret;

    g_message("Start Modem power initialization.");

    mm_data->modem_rfkill_state = FALSE;

    if(main_config_data->hw_gpio_modem_power_chip==NULL)
    {
        g_warning("Modem power GPIO chip not configured!");

        return FALSE;
    }

    if(main_config_data->hw_gpio_modem_reset_chip==NULL)
    {
        g_warning("Modem reset GPIO chip not configured!");

        return FALSE;
    }

    if(mm_data->gpio_modem_power_chip==NULL)
    {
        mm_data->gpio_modem_power_chip = gpiod_chip_open_by_name(
            main_config_data->hw_gpio_modem_power_chip);
        if(mm_data->gpio_modem_power_chip==NULL)
        {
            g_warning("Failed to open Modem power GPIO chip!");

            return FALSE;
        }
    }

    if(mm_data->gpio_modem_rf_kill_chip==NULL &&
        main_config_data->hw_gpio_modem_rf_kill_chip!=NULL &&
        *(main_config_data->hw_gpio_modem_rf_kill_chip)!='\0')
    {
        mm_data->gpio_modem_rf_kill_chip = gpiod_chip_open_by_name(
            main_config_data->hw_gpio_modem_rf_kill_chip);
        if(mm_data->gpio_modem_rf_kill_chip==NULL)
        {
            g_warning("Failed to open Modem RF Kill GPIO chip!");
        }
    }

    if(mm_data->gpio_modem_reset_chip==NULL)
    {
        mm_data->gpio_modem_reset_chip = gpiod_chip_open_by_name(
            main_config_data->hw_gpio_modem_reset_chip);
        if(mm_data->gpio_modem_reset_chip==NULL)
        {
            g_warning("Failed to open Modem reset GPIO chip!");

            return FALSE;
        }
    }

    if(mm_data->gpio_modem_power_line==NULL)
    {
        mm_data->gpio_modem_power_line = gpiod_chip_get_line(
            mm_data->gpio_modem_power_chip,
            main_config_data->hw_gpio_modem_power_line);
        if(mm_data->gpio_modem_power_line==NULL)
        {
            g_warning("Failed to open Modem power GPIO line!");

            return FALSE;
        }
    }
    if(!gpiod_line_is_requested(mm_data->gpio_modem_power_line))
    {
        ret = gpiod_line_request_output(mm_data->gpio_modem_power_line,
            "gpio-modem-power",
            main_config_data->hw_gpio_modem_power_active_low ? 1 : 0);
        if(ret!=0)
        {
            g_warning("Failed to request output on Modem power GPIO!");
        }
    }
    else
    {
        gpiod_line_set_value(mm_data->gpio_modem_power_line,
            main_config_data->hw_gpio_modem_power_active_low ? 1 : 0);
    }

    if(mm_data->gpio_modem_rf_kill_line==NULL &&
        mm_data->gpio_modem_rf_kill_chip!=NULL)
    {
        mm_data->gpio_modem_rf_kill_line = gpiod_chip_get_line(
            mm_data->gpio_modem_rf_kill_chip,
            main_config_data->hw_gpio_modem_rf_kill_line);
        if(mm_data->gpio_modem_rf_kill_line==NULL)
        {
            g_warning("Failed to open Modem RF kill GPIO line!");
        }
    }

    if(mm_data->gpio_modem_rf_kill_line!=NULL)
    {
        if(!gpiod_line_is_requested(mm_data->gpio_modem_rf_kill_line))
        {
            ret = gpiod_line_request_output(mm_data->gpio_modem_rf_kill_line,
                "gpio-modem-rf-kill",
                main_config_data->hw_gpio_modem_rf_kill_active_low ? 0 : 1);
            if(ret!=0)
            {
                g_warning("Failed to request output on Modem RF kill GPIO!");
            }
        }
        else
        {
            gpiod_line_set_value(mm_data->gpio_modem_rf_kill_line,
                main_config_data->hw_gpio_modem_rf_kill_active_low ? 0 : 1);
        }
    }

    if(mm_data->gpio_modem_reset_line==NULL)
    {
        mm_data->gpio_modem_reset_line = gpiod_chip_get_line(
            mm_data->gpio_modem_reset_chip,
            main_config_data->hw_gpio_modem_reset_line);
        if(mm_data->gpio_modem_reset_line==NULL)
        {
            g_warning("Failed to open Modem reset GPIO line!");

            return FALSE;
        }
    }
    if(!gpiod_line_is_requested(mm_data->gpio_modem_reset_line))
    {
        ret = gpiod_line_request_output(mm_data->gpio_modem_reset_line,
            "gpio-modem-reset",
            main_config_data->hw_gpio_modem_reset_active_low ? 1 : 0);
        if(ret!=0)
        {
            g_warning("Failed to request output on Modem reset GPIO!");
        }
    }
    else
    {
        gpiod_line_set_value(mm_data->gpio_modem_reset_line,
            main_config_data->hw_gpio_modem_reset_active_low ? 1 : 0);
    }

    for(i=0;i<PCAT_MODEM_MANAGER_POWER_WAIT_TIME && mm_data->work_flag;i++)
    {
        g_usleep(100000);
    }
    if(!mm_data->work_flag)
    {
        return FALSE;
    }

    gpiod_line_set_value(mm_data->gpio_modem_power_line,
        main_config_data->hw_gpio_modem_power_active_low ? 0 : 1);
    if(mm_data->gpio_modem_rf_kill_line!=NULL)
    {
        gpiod_line_set_value(mm_data->gpio_modem_rf_kill_line,
            main_config_data->hw_gpio_modem_rf_kill_active_low ? 1 : 0);
    }
    gpiod_line_set_value(mm_data->gpio_modem_reset_line,
        main_config_data->hw_gpio_modem_reset_active_low ? 1 : 0);

    for(i=0;i<PCAT_MODEM_MANAGER_POWER_READY_TIME && mm_data->work_flag;i++)
    {
        g_usleep(100000);
    }
    if(!mm_data->work_flag)
    {
        return FALSE;
    }

    g_message("Modem power on successfully.");

    gpiod_line_set_value(mm_data->gpio_modem_reset_line,
        main_config_data->hw_gpio_modem_reset_active_low ? 0 : 1);

    for(i=0;i<PCAT_MODEM_MANAGER_RESET_ON_TIME && mm_data->work_flag;i++)
    {
        g_usleep(100000);
    }
    if(!mm_data->work_flag)
    {
        return FALSE;
    }

    gpiod_line_set_value(mm_data->gpio_modem_reset_line,
        main_config_data->hw_gpio_modem_reset_active_low ? 1 : 0);

    for(i=0;i<PCAT_MODEM_MANAGER_RESET_WAIT_TIME && mm_data->work_flag;i++)
    {
        g_usleep(100000);
    }
    if(!mm_data->work_flag)
    {
        return FALSE;
    }

    g_message("Modem power initialization completed.");

    return TRUE;
}

static inline void pcat_modem_manager_external_control_exec_line_parser(
    PCatModemManagerData *mm_data, const guint8 *buffer, gssize size)
{
    gsize i, j;
    GString *str = mm_data->external_control_exec_stdout_buffer;
    gsize used_size = 0;
    const gchar *start = str->str;
    gchar **fields, **values;
    GHashTable *table;
    const gchar *cmd, *smode, *value_raw_str;
    gint signal_raw;
    gint signal_value;
    gint sim_state;
    gint isp_name_is_ucs2 = 0;
    PCatModemManagerMode modem_mode;
    gboolean downgrade_from_5g = FALSE;

    if(mm_data->external_control_exec_stdout_log_file!=NULL)
    {
        fwrite(buffer, size, 1,
            mm_data->external_control_exec_stdout_log_file);
        fflush(mm_data->external_control_exec_stdout_log_file);
    }

    g_string_append_len(str, (const gchar *)buffer, size);

    if(str->len > 1048576)
    {
        str->len = 0;
    }

    for(i=0;i<str->len;i++)
    {
        if(str->str[i]=='\n')
        {
            str->str[i] = '\0';

            fields = g_strsplit(start, ",", -1);

            if(fields!=NULL)
            {
                table = g_hash_table_new_full(g_str_hash, g_str_equal,
                    g_free, g_free);

                for(j=0;fields[j]!=NULL;j++)
                {
                    values = g_strsplit(fields[j], "=", 2);
                    if(values!=NULL)
                    {
                        if(values[0]!=NULL && values[1]!=NULL)
                        {
                            g_hash_table_replace(table, g_strdup(values[0]),
                                g_strdup(values[1]));
                        }

                        g_strfreev(values);
                    }
                }
                g_strfreev(fields);

                cmd = g_hash_table_lookup(table, "CMD");

                if(g_strcmp0(cmd, "SIGNALINFO")==0)
                {
                    signal_value = 0;

                    smode = g_hash_table_lookup(table, "MODE");
                    modem_mode = GPOINTER_TO_UINT(
                        g_hash_table_lookup(mm_data->modem_mode_table, smode));

                    if(modem_mode==PCAT_MODEM_MANAGER_MODE_5G &&
                       mm_data->modem_mode < PCAT_MODEM_MANAGER_MODE_5G)
                    {
                        downgrade_from_5g = TRUE;
                    }

                    mm_data->modem_mode = modem_mode;

                    if(mm_data->modem_mode==PCAT_MODEM_MANAGER_MODE_5G)
                    {
                        mm_data->modem_have_5g_connected = TRUE;
                        mm_data->modem_5g_connection_timestamp =
                            g_get_monotonic_time();
                    }
                    else
                    {
                        if(mm_data->modem_have_5g_connected &&
                            downgrade_from_5g)
                        {
                            mm_data->modem_5g_connection_timestamp =
                                g_get_monotonic_time();
                        }
                    }

                    G_STMT_START
                    {
                        value_raw_str = g_hash_table_lookup(table, "RSSI");
                        if(value_raw_str!=NULL)
                        {
                            if(sscanf(value_raw_str, "%d", &signal_raw)>0)
                            {
                                if(signal_raw >= -65)
                                {
                                    signal_value = 100;
                                }
                                else if(signal_raw >= -85)
                                {
                                    signal_value = (signal_raw + 85) * 5;
                                }

                                break;
                            }
                        }

                        value_raw_str = g_hash_table_lookup(table, "RSRQ");
                        if(value_raw_str!=NULL)
                        {
                            if(sscanf(value_raw_str, "%d", &signal_raw)>0)
                            {
                                if(signal_raw >= -10)
                                {
                                    signal_value = 100;
                                }
                                else if(signal_raw >= -20)
                                {
                                    signal_value = (signal_raw + 20) * 10;
                                }

                                break;
                            }
                        }

                        value_raw_str = g_hash_table_lookup(table, "RSRP");
                        if(value_raw_str!=NULL)
                        {
                            if(sscanf(value_raw_str, "%d", &signal_raw)>0)
                            {
                                if(signal_raw >= -80)
                                {
                                    signal_value = 100;
                                }
                                else if(signal_raw >= -100)
                                {
                                    signal_value = (signal_raw + 100) * 5;
                                }

                                break;
                            }
                        }

                        value_raw_str = g_hash_table_lookup(table, "RSCP");
                        if(value_raw_str!=NULL)
                        {
                            if(sscanf(value_raw_str, "%d", &signal_raw)>0)
                            {
                                if(signal_raw >= -60)
                                {
                                    signal_value = 100;
                                }
                                else if(signal_raw >= -100)
                                {
                                    signal_value = (signal_raw + 100) * 5 / 2;
                                }

                                break;
                            }
                        }
                    }
                    G_STMT_END;

                    mm_data->modem_signal_strength = signal_value;
                    g_message("Modem signal strength: %d", signal_value);
                }
                else if(g_strcmp0(cmd, "SIMSTATUS")==0)
                {
                    value_raw_str = g_hash_table_lookup(table, "STATE");

                    if(value_raw_str!=NULL)
                    {
                        if(sscanf(value_raw_str, "%d", &sim_state) > 0)
                        {
                            mm_data->sim_state = sim_state;

                            g_message("SIM card state changed to %d.",
                                sim_state);
                        }
                    }
                }
                else if(g_strcmp0(cmd, "ISPINFO")==0)
                {
                    value_raw_str = g_hash_table_lookup(table, "ALPHABET");
                    if(value_raw_str!=NULL)
                    {
                        sscanf(value_raw_str, "%d", &isp_name_is_ucs2);
                    }

                    value_raw_str = g_hash_table_lookup(table, "FNN");
                    if(value_raw_str!=NULL)
                    {
                        if(mm_data->isp_name!=NULL)
                        {
                            g_free(mm_data->isp_name);
                        }
                        mm_data->isp_name = g_strdup(value_raw_str);
                    }

                    value_raw_str = g_hash_table_lookup(table, "RPLMN");
                    if(value_raw_str!=NULL)
                    {
                        if(mm_data->isp_plmn!=NULL)
                        {
                            g_free(mm_data->isp_plmn);
                        }
                        mm_data->isp_plmn = g_strdup(value_raw_str);
                    }
                }

                g_hash_table_unref(table);
            }
            start = str->str + i + 1;
            used_size = i + 1;
        }
    }

    if(used_size > 0)
    {
        g_string_erase(str, 0, used_size);
    }
}

static gboolean pcat_modem_manager_external_control_exec_stdout_watch_func(
    GObject *object, gpointer user_data)
{
    PCatModemManagerData *mm_data = (PCatModemManagerData *)user_data;
    gssize rsize;
    guint8 buffer[4096];
    GError *error = NULL;

    while((rsize=g_pollable_input_stream_read_nonblocking(
        G_POLLABLE_INPUT_STREAM(object), buffer, 4096, NULL, &error))>0)
    {
        pcat_modem_manager_external_control_exec_line_parser(mm_data,
            buffer, rsize);
    }

    if(error!=NULL)
    {
        g_clear_error(&error);
    }

    return TRUE;
}

static void pcat_modem_manager_external_control_exec_wait_func(
    GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    PCatModemManagerData *mm_data = (PCatModemManagerData *)user_data;

    if(g_subprocess_wait_check_finish(G_SUBPROCESS(source_object), res,
        &error))
    {
        g_message("External control process exits normally.");
    }
    else
    {
        g_warning("External control process exits with error: %s",
            error->message!=NULL ? error->message : "Unknown");
        g_clear_error(&error);
    }

    if(mm_data->external_control_exec_stdout_read_source!=NULL)
    {
        g_source_destroy(mm_data->external_control_exec_stdout_read_source);
        mm_data->external_control_exec_stdout_read_source = NULL;
    }

    mm_data->external_control_exec_stdout_stream = NULL;

    g_object_unref(mm_data->external_control_exec_process);
    mm_data->external_control_exec_process = NULL;
}

static inline gboolean pcat_modem_manager_run_external_exec(
    PCatModemManagerData *mm_data, const PCatModemManagerUSBData *usb_data)
{
    GError *error = NULL;
    gboolean ret = TRUE;
    PCatManagerUserConfigData *uconfig_data;

    if(mm_data==NULL || usb_data==NULL ||
        usb_data->external_control_exec==NULL)
    {
        return FALSE;
    }

    uconfig_data = pcat_main_user_config_data_get();

    if(!usb_data->external_control_exec_is_daemon)
    {
        G_STMT_START
        {
            if(mm_data->external_control_exec_process!=NULL)
            {
                break;
            }

            if(usb_data->id_vendor==0x2C7C &&
                uconfig_data->modem_dial_apn!=NULL)
            {
                if(uconfig_data->modem_dial_user!=NULL &&
                    uconfig_data->modem_dial_password!=NULL &&
                    uconfig_data->modem_dial_auth!=NULL)
                {
                    if(!uconfig_data->modem_disable_ipv6)
                    {
                        mm_data->external_control_exec_process =
                            g_subprocess_new(
                            G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                            G_SUBPROCESS_FLAGS_STDERR_MERGE, &error,
                            usb_data->external_control_exec, "-4", "-6", "-s",
                            uconfig_data->modem_dial_apn,
                            uconfig_data->modem_dial_user,
                            uconfig_data->modem_dial_password,
                            uconfig_data->modem_dial_auth, NULL);
                    }
                    else
                    {
                        mm_data->external_control_exec_process =
                            g_subprocess_new(
                            G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                            G_SUBPROCESS_FLAGS_STDERR_MERGE, &error,
                            usb_data->external_control_exec, "-s",
                            uconfig_data->modem_dial_apn,
                            uconfig_data->modem_dial_user,
                            uconfig_data->modem_dial_password,
                            uconfig_data->modem_dial_auth, NULL);
                    }
                }
                else
                {
                    if(!uconfig_data->modem_disable_ipv6)
                    {
                        mm_data->external_control_exec_process =
                            g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                            G_SUBPROCESS_FLAGS_STDERR_MERGE, &error,
                            usb_data->external_control_exec, "-4", "-6", "-s",
                            uconfig_data->modem_dial_apn, NULL);
                    }
                    else
                    {
                        mm_data->external_control_exec_process =
                            g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                            G_SUBPROCESS_FLAGS_STDERR_MERGE, &error,
                            usb_data->external_control_exec, "-s",
                            uconfig_data->modem_dial_apn, NULL);
                    }
                }
            }
            else
            {
                if(!uconfig_data->modem_disable_ipv6)
                {
                    mm_data->external_control_exec_process = g_subprocess_new(
                        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                        G_SUBPROCESS_FLAGS_STDERR_MERGE, &error,
                        usb_data->external_control_exec, "-4", "-6", NULL);
                }
                else
                {
                    mm_data->external_control_exec_process = g_subprocess_new(
                        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                        G_SUBPROCESS_FLAGS_STDERR_MERGE, &error,
                        usb_data->external_control_exec, NULL);
                }
            }

            if(mm_data->external_control_exec_process==NULL)
            {
                g_warning("Failed to run external modem control "
                    "executable file %s: %s",
                    usb_data->external_control_exec,
                    error!=NULL ? error->message: "Unknown");
                g_clear_error(&error);
                ret = FALSE;

                break;
            }

            mm_data->external_control_exec_stdout_stream =
                g_subprocess_get_stdout_pipe(
                mm_data->external_control_exec_process);
            if(mm_data->external_control_exec_stdout_stream!=NULL)
            {
                mm_data->external_control_exec_stdout_read_source =
                    g_pollable_input_stream_create_source(
                    G_POLLABLE_INPUT_STREAM(
                    mm_data->external_control_exec_stdout_stream),
                    NULL);

                g_source_set_callback(
                    mm_data->external_control_exec_stdout_read_source,
                    (GSourceFunc)
                    pcat_modem_manager_external_control_exec_stdout_watch_func,
                    mm_data, NULL);
                g_source_attach(
                    mm_data->external_control_exec_stdout_read_source, NULL);
            }

            g_subprocess_wait_async(
                mm_data->external_control_exec_process, NULL,
                pcat_modem_manager_external_control_exec_wait_func,
                mm_data);
        }
        G_STMT_END;

    }
    else
    {
        /* TODO: Run external control exec as daemon. */
    }

    return ret;
}

static void pcat_modem_manager_scan_usb_devs(PCatModemManagerData *mm_data)
{
    libusb_device *dev;
    guint i;
    ssize_t cnt;
    struct libusb_device_descriptor desc;
    int r;
    libusb_device **devs = NULL;
    const PCatModemManagerUSBData *usb_data;
    guint uc;
    gboolean detected;
    PCatModemManagerDeviceType device_type = PCAT_MODEM_MANAGER_DEVICE_NONE;

    cnt = libusb_get_device_list(NULL, &devs);
    if(cnt < 0)
    {
        return;
    }

    for(i=0;devs[i]!=NULL;i++)
    {
        detected = FALSE;
        dev = devs[i];

        r = libusb_get_device_descriptor(dev, &desc);
        if(r < 0)
        {
            g_warning("Failed to get USB device descriptor!");

            continue;
        }

        for(uc=0;uc < sizeof(g_pcat_modem_manager_supported_dev_list) /
            sizeof(PCatModemManagerUSBData);uc++)
        {
            usb_data = &(g_pcat_modem_manager_supported_dev_list[uc]);

            if(usb_data->id_vendor==desc.idVendor &&
               (usb_data->id_product==0 ||
                usb_data->id_product==desc.idProduct))
            {
                detected = TRUE;
                device_type = usb_data->device_type;
                break;
            }
        }

        if(!detected)
        {
            continue;
        }

        switch(usb_data->device_type)
        {
            case PCAT_MODEM_MANAGER_DEVICE_5G:
            {
                break;
            }
            case PCAT_MODEM_MANAGER_DEVICE_GENERAL:
            {
                break;
            }
            default:
            {
                break;
            }
        }
        if(usb_data->external_control_exec!=NULL)
        {
            g_spawn_command_line_async("ModemManagerSwitch.sh disable",
                NULL);
            pcat_modem_manager_run_external_exec(mm_data, usb_data);
        }
        else
        {
            g_spawn_command_line_async("ModemManagerSwitch.sh enable",
                NULL);
        }
    }

    libusb_free_device_list(devs, 1);

    mm_data->device_type = device_type;
}

static gpointer pcat_modem_manager_modem_work_thread_func(
    gpointer user_data)
{
    PCatModemManagerData *mm_data = (PCatModemManagerData *)user_data;
    PCatManagerMainConfigData *main_config_data;

    main_config_data = pcat_main_config_data_get();

    while(mm_data->work_flag)
    {
        switch(mm_data->state)
        {
            case PCAT_MODEM_MANAGER_STATE_NONE:
            {
                pcat_modem_manager_modem_power_init(mm_data,
                    main_config_data);
                mm_data->state = PCAT_MODEM_MANAGER_STATE_READY;

                break;
            }

            case PCAT_MODEM_MANAGER_STATE_READY:
            {
                if(!pcat_main_is_running_on_distro())
                {
                    pcat_modem_manager_scan_usb_devs(mm_data);
                }

                g_usleep(1000000); /* WIP */

                break;
            }

            default:
            {
                break;
            }
        }
    }

    if(mm_data->external_control_exec_process!=NULL)
    {
        g_subprocess_force_exit(mm_data->external_control_exec_process);
        g_subprocess_wait(mm_data->external_control_exec_process, NULL, NULL);
        g_object_unref(mm_data->external_control_exec_process);
        mm_data->external_control_exec_process = NULL;
    }

    if(mm_data->gpio_modem_reset_line!=NULL)
    {
        gpiod_line_set_value(mm_data->gpio_modem_reset_line,
            main_config_data->hw_gpio_modem_reset_active_low ? 1 : 0);

        gpiod_line_release(mm_data->gpio_modem_reset_line);
        mm_data->gpio_modem_reset_line = NULL;
    }
    if(mm_data->gpio_modem_rf_kill_line!=NULL)
    {
        gpiod_line_set_value(mm_data->gpio_modem_rf_kill_line,
            main_config_data->hw_gpio_modem_rf_kill_active_low ? 0 : 1);

        gpiod_line_release(mm_data->gpio_modem_rf_kill_line);
        mm_data->gpio_modem_rf_kill_line = NULL;
    }
    if(mm_data->gpio_modem_power_line!=NULL)
    {
        gpiod_line_set_value(mm_data->gpio_modem_power_line,
            main_config_data->hw_gpio_modem_power_active_low ? 1 : 0);

        gpiod_line_release(mm_data->gpio_modem_power_line);
        mm_data->gpio_modem_power_line = NULL;
    }

    if(mm_data->gpio_modem_reset_chip!=NULL)
    {
        gpiod_chip_close(mm_data->gpio_modem_reset_chip);
        mm_data->gpio_modem_reset_chip = NULL;
    }
    if(mm_data->gpio_modem_rf_kill_chip!=NULL)
    {
        gpiod_chip_close(mm_data->gpio_modem_rf_kill_chip);
        mm_data->gpio_modem_rf_kill_chip = NULL;
    }
    if(mm_data->gpio_modem_power_chip!=NULL)
    {
        gpiod_chip_close(mm_data->gpio_modem_power_chip);
        mm_data->gpio_modem_power_chip = NULL;
    }

    return NULL;
}

static gboolean pcat_modem_scan_timeout_func(gpointer user_data)
{
    PCatModemManagerData *mm_data = (PCatModemManagerData *)user_data;
    const PCatManagerUserConfigData *uconfig_data;
    gint64 now;

    uconfig_data = pcat_main_user_config_data_get();
    now = g_get_monotonic_time();

    if(!uconfig_data->modem_disable_5g_fail_auto_reset)
    {
        if(mm_data->modem_have_5g_connected && !mm_data->modem_rfkill_state)
        {
            if(now > mm_data->modem_5g_connection_timestamp +
                uconfig_data->modem_5g_fail_timeout * 1e6)
            {
                pcat_modem_manager_device_rfkill_mode_set(TRUE);
                pcat_modem_manager_device_rfkill_mode_set(FALSE);

                mm_data->modem_have_5g_connected = FALSE;
            }
        }
    }

    return TRUE;
}

gboolean pcat_modem_manager_init()
{
    int errcode;
    PCatManagerMainConfigData *main_config_data;
    gchar *command[] = {"/usr/sbin/rfkill", "unblock", "wwan", NULL};

    if(g_pcat_modem_manager_data.initialized)
    {
        g_message("Modem Manager is already initialized!");

        return TRUE;
    }

    main_config_data = pcat_main_config_data_get();

    g_pcat_modem_manager_data.work_flag = TRUE;
    g_mutex_init(&(g_pcat_modem_manager_data.mutex));

    g_pcat_modem_manager_data.modem_mode_table = g_hash_table_new_full(
        g_str_hash, g_str_equal, NULL, NULL);
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "NR5G-SA", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_5G));
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "NR5G-NSA", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_5G));
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "LTE", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_LTE));
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "WCDMA", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_3G));
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "TDSCDMA", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_3G));
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "GSM", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_2G));
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "HDR", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_2G));
    g_hash_table_insert(g_pcat_modem_manager_data.modem_mode_table,
        "CDMA", GUINT_TO_POINTER(PCAT_MODEM_MANAGER_MODE_2G));

    if(main_config_data->debug_modem_external_exec_stdout_log)
    {
        g_pcat_modem_manager_data.external_control_exec_stdout_log_file =
            fopen("/tmp/pcat-modem-external-exec-stdout.log", "w+");
    }

    errcode = libusb_init(&g_pcat_modem_manager_data.usb_ctx);
    if(errcode!=0)
    {
        g_warning("Failed to initialize libusb: %s, 5G modem may not work!",
            libusb_strerror(errcode));
    }

    g_pcat_modem_manager_data.external_control_exec_stdout_buffer =
        g_string_new(NULL);

    g_pcat_modem_manager_data.modem_work_thread = g_thread_new(
        "pcat-modem-manager-work-thread",
        pcat_modem_manager_modem_work_thread_func,
        &g_pcat_modem_manager_data);

    g_spawn_async(NULL, command, NULL, G_SPAWN_DEFAULT,
        NULL, NULL, NULL, NULL);

    g_pcat_modem_manager_data.scanning_timeout_id = g_timeout_add_seconds(5,
        pcat_modem_scan_timeout_func, &g_pcat_modem_manager_data);

    g_pcat_modem_manager_data.initialized = TRUE;

    return TRUE;
}

void pcat_modem_manager_uninit()
{
    if(!g_pcat_modem_manager_data.initialized)
    {
        return;
    }

    if(g_pcat_modem_manager_data.scanning_timeout_id > 0)
    {
        g_source_remove(g_pcat_modem_manager_data.scanning_timeout_id);
        g_pcat_modem_manager_data.scanning_timeout_id = 0;
    }

    g_pcat_modem_manager_data.work_flag = FALSE;

    if(g_pcat_modem_manager_data.modem_work_thread!=NULL)
    {
        g_thread_join(g_pcat_modem_manager_data.modem_work_thread);
        g_pcat_modem_manager_data.modem_work_thread = NULL;
    }

    g_mutex_clear(&(g_pcat_modem_manager_data.mutex));

    if(g_pcat_modem_manager_data.usb_ctx!=NULL)
    {
        libusb_exit(g_pcat_modem_manager_data.usb_ctx);
        g_pcat_modem_manager_data.usb_ctx = NULL;
    }

    if(g_pcat_modem_manager_data.external_control_exec_stdout_buffer!=NULL)
    {
        g_string_free(
            g_pcat_modem_manager_data.external_control_exec_stdout_buffer,
            TRUE);
        g_pcat_modem_manager_data.external_control_exec_stdout_buffer = NULL;
    }

    if(g_pcat_modem_manager_data.external_control_exec_stdout_log_file!=NULL)
    {
        fclose(
            g_pcat_modem_manager_data.external_control_exec_stdout_log_file);
        g_pcat_modem_manager_data.external_control_exec_stdout_log_file =
            NULL;
    }

    g_pcat_modem_manager_data.initialized = FALSE;
}

gboolean pcat_modem_manager_status_get(PCatModemManagerMode *mode,
    PCatModemManagerSIMState *sim_state, gboolean *rfkill_state,
    gint *signal_strength, gchar **isp_name, gchar **isp_plmn)
{
    if(!g_pcat_modem_manager_data.initialized)
    {
        return FALSE;
    }

    if(mode!=NULL)
    {
        *mode = g_pcat_modem_manager_data.modem_mode;
    }
    if(sim_state!=NULL)
    {
        *sim_state = g_pcat_modem_manager_data.sim_state;
    }
    if(rfkill_state!=NULL)
    {
        *rfkill_state = g_pcat_modem_manager_data.modem_rfkill_state;
    }
    if(signal_strength!=NULL)
    {
        *signal_strength = g_pcat_modem_manager_data.modem_signal_strength;
    }
    if(isp_name!=NULL)
    {
        *isp_name = g_strdup(g_pcat_modem_manager_data.isp_name);
    }
    if(isp_plmn!=NULL)
    {
        *isp_plmn = g_strdup(g_pcat_modem_manager_data.isp_plmn);
    }

    return TRUE;
}

PCatModemManagerDeviceType pcat_modem_manager_device_type_get()
{
    return g_pcat_modem_manager_data.device_type;
}

void pcat_modem_manager_device_rfkill_mode_set(gboolean state)
{
    PCatManagerMainConfigData *main_config_data;
    gint value;
    gchar *command[] = {"/usr/sbin/rfkill", "unblock", "wwan", NULL};

    if(!!g_pcat_modem_manager_data.modem_rfkill_state==!!state)
    {
        return;
    }

    g_pcat_modem_manager_data.modem_rfkill_state = state;
    main_config_data = pcat_main_config_data_get();

    if(state)
    {
        command[1] = "block";
    }
    g_spawn_async(NULL, command, NULL, G_SPAWN_DEFAULT,
        NULL, NULL, NULL, NULL);

    if(g_pcat_modem_manager_data.gpio_modem_rf_kill_line!=NULL)
    {
        if(state)
        {
            value = main_config_data->hw_gpio_modem_rf_kill_active_low ? 0 : 1;
        }
        else
        {
            value = main_config_data->hw_gpio_modem_rf_kill_active_low ? 1 : 0;
        }

        gpiod_line_set_value(
            g_pcat_modem_manager_data.gpio_modem_rf_kill_line, value);
    }
}
