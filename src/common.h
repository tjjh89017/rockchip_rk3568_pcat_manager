#ifndef HAVE_PCAT_COMMON_H
#define HAVE_PCAT_COMMON_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
    PCAT_MANAGER_POWER_SCHEDULE_ENABLE_YEAR = (1 << 0),
    PCAT_MANAGER_POWER_SCHEDULE_ENABLE_MONTH = (1 << 1),
    PCAT_MANAGER_POWER_SCHEDULE_ENABLE_DAY = (1 << 2),
    PCAT_MANAGER_POWER_SCHEDULE_ENABLE_HOUR = (1 << 3),
    PCAT_MANAGER_POWER_SCHEDULE_ENABLE_MINUTE = (1 << 4),
    PCAT_MANAGER_POWER_SCHEDULE_ENABLE_DOW = (1 << 5) /* Day of week */
}PCatManagerTimeEnableBits;

typedef struct _PCatManagerMainConfigData
{
    gboolean valid;

    gchar *hw_gpio_modem_power_chip;
    guint hw_gpio_modem_power_line;
    gboolean hw_gpio_modem_power_active_low;
    gchar *hw_gpio_modem_rf_kill_chip;
    guint hw_gpio_modem_rf_kill_line;
    gboolean hw_gpio_modem_rf_kill_active_low;
    gchar *hw_gpio_modem_reset_chip;
    guint hw_gpio_modem_reset_line;
    gboolean hw_gpio_modem_reset_active_low;

    gchar *pm_serial_device;
    guint pm_serial_baud;

    gboolean debug_modem_external_exec_stdout_log;
}PCatManagerMainConfigData;

typedef struct _PCatManagerPowerScheduleData
{
    gboolean enabled;
    gboolean action;
    guint8 enable_bits;
    gint16 year;
    guint8 month;
    guint8 day;
    guint8 hour;
    guint8 minute;
    guint8 second;
    guint8 dow_bits;
}PCatManagerPowerScheduleData;

typedef struct _PCatManagerMainUserConfigData
{
    gboolean valid;
    gboolean dirty;

    GPtrArray *power_schedule_data;
}PCatManagerMainUserConfigData;

PCatManagerMainConfigData *pcat_manager_main_config_data_get();
PCatManagerMainUserConfigData *pcat_manager_main_user_config_data_get();
void pcat_manager_main_request_shutdown();

G_END_DECLS

#endif

