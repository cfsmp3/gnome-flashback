/*
 * Copyright (C) 2015-2019 Alberts Muktupāvels
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include "flashback-osd.h"
#include "gf-osd-window.h"
#include "gf-shell-monitors.h"

struct _FlashbackOsd
{
  GObject           parent;

  GfMonitorManager *monitor_manager;

  GfOsdWindow     **windows;
  gint              n_monitors;

  gulong            monitors_changed_id;
};

G_DEFINE_TYPE (FlashbackOsd, flashback_osd, G_TYPE_OBJECT)

static void
destroy_windows (FlashbackOsd *osd)
{
  gint i;

  if (osd->windows == NULL)
    return;

  for (i = 0; i < osd->n_monitors; i++)
    gtk_widget_destroy (GTK_WIDGET (osd->windows[i]));

  g_clear_pointer (&osd->windows, g_free);
  osd->n_monitors = 0;
}

static void
update_windows (FlashbackOsd *osd)
{
  GList *logical_monitors;
  GList *l;
  gint i;

  destroy_windows (osd);

  if (osd->monitor_manager == NULL)
    return;

  logical_monitors = gf_monitor_manager_get_logical_monitors (osd->monitor_manager);

  /* One window for each logical monitor, in the order of the monitor
   * manager's list. Mirrored monitors share a window.
   */
  osd->n_monitors = g_list_length (logical_monitors);
  osd->windows = g_new0 (GfOsdWindow *, osd->n_monitors);

  for (l = logical_monitors, i = 0; l != NULL; l = l->next, i++)
    {
      GdkRectangle workarea;

      gf_shell_monitors_get_workarea (l->data, &workarea);
      osd->windows[i] = gf_osd_window_new (&workarea);
    }
}

static void
monitors_changed (GdkScreen *screen,
                  gpointer   user_data)
{
  update_windows (FLASHBACK_OSD (user_data));
}

static void
monitor_manager_monitors_changed_cb (GfMonitorManager *monitor_manager,
                                     FlashbackOsd     *osd)
{
  update_windows (osd);
}

static void
flashback_osd_dispose (GObject *object)
{
  FlashbackOsd *osd;

  osd = FLASHBACK_OSD (object);

  if (osd->monitor_manager != NULL)
    {
      g_signal_handlers_disconnect_by_func (osd->monitor_manager,
                                            monitor_manager_monitors_changed_cb,
                                            osd);

      osd->monitor_manager = NULL;
    }

  G_OBJECT_CLASS (flashback_osd_parent_class)->dispose (object);
}

static void
flashabck_osd_finalize (GObject *object)
{
  GdkScreen *screen;
  FlashbackOsd *osd;

  screen = gdk_screen_get_default ();
  osd = FLASHBACK_OSD (object);

  g_signal_handler_disconnect (screen, osd->monitors_changed_id);

  destroy_windows (osd);

  G_OBJECT_CLASS (flashback_osd_parent_class)->finalize (object);
}

static void
flashback_osd_class_init (FlashbackOsdClass *osd_class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (osd_class);

  object_class->dispose = flashback_osd_dispose;
  object_class->finalize = flashabck_osd_finalize;
}

static void
flashback_osd_init (FlashbackOsd *osd)
{
  GdkScreen *screen;

  screen = gdk_screen_get_default ();

  osd->monitors_changed_id =
    g_signal_connect (screen, "monitors-changed",
                      G_CALLBACK (monitors_changed), osd);

  update_windows (osd);
}

FlashbackOsd *
flashback_osd_new (void)
{
  return g_object_new (FLASHBACK_TYPE_OSD, NULL);
}

void
flashback_osd_set_monitor_manager (FlashbackOsd     *osd,
                                   GfMonitorManager *monitor_manager)
{
  if (osd->monitor_manager == monitor_manager)
    return;

  if (osd->monitor_manager != NULL)
    {
      g_signal_handlers_disconnect_by_func (osd->monitor_manager,
                                            monitor_manager_monitors_changed_cb,
                                            osd);
    }

  osd->monitor_manager = monitor_manager;

  if (monitor_manager != NULL)
    {
      g_signal_connect (monitor_manager, "monitors-changed",
                        G_CALLBACK (monitor_manager_monitors_changed_cb),
                        osd);
    }

  update_windows (osd);
}

void
flashback_osd_show (FlashbackOsd *osd,
                    GVariant     *params)
{
  GVariantDict dict;
  const gchar *icon_name;
  const gchar *label;
  GIcon *icon;
  gdouble level;
  gdouble max_level;
  const gchar *connector;
  GfLogicalMonitor *logical_monitor;
  gint monitor;
  gint i;

  g_variant_dict_init (&dict, params);

  if (!g_variant_dict_lookup (&dict, "icon", "&s", &icon_name))
    icon_name = NULL;

  if (!g_variant_dict_lookup (&dict, "label", "&s", &label))
    label = NULL;

  if (!g_variant_dict_lookup (&dict, "level", "d", &level))
    level = -1;

  if (!g_variant_dict_lookup (&dict, "max_level", "d", &max_level))
    max_level = 1.0;

  if (!g_variant_dict_lookup (&dict, "connector", "&s", &connector))
    connector = NULL;

  logical_monitor = gf_shell_monitors_get_logical_monitor (osd->monitor_manager,
                                                           connector);

  /* The windows follow the list of logical monitors, which is only
   * replaced together with the windows on "monitors-changed".
   */
  monitor = -1;
  if (logical_monitor != NULL)
    {
      GList *logical_monitors;

      logical_monitors = gf_monitor_manager_get_logical_monitors (osd->monitor_manager);
      monitor = g_list_index (logical_monitors, logical_monitor);
    }

  icon = NULL;
  if (icon_name)
    icon = g_icon_new_for_string (icon_name, NULL);

  if (monitor != -1)
    {
      for (i = 0; i < osd->n_monitors; i++)
        {
          if (i == monitor)
            {
              gf_osd_window_set_icon (osd->windows[i], icon);
              gf_osd_window_set_label (osd->windows[i], label);
              gf_osd_window_set_level (osd->windows[i], level);
              gf_osd_window_set_max_level (osd->windows[i], max_level);
              gf_osd_window_show (osd->windows[i]);
            }
          else
            {
              gf_osd_window_hide (osd->windows[i]);
            }
        }
    }
  else
    {
      for (i = 0; i < osd->n_monitors; i++)
        {
          gf_osd_window_set_icon (osd->windows[i], icon);
          gf_osd_window_set_label (osd->windows[i], label);
          gf_osd_window_set_level (osd->windows[i], level);
          gf_osd_window_set_max_level (osd->windows[i], max_level);
          gf_osd_window_show (osd->windows[i]);
        }
    }

  if (icon)
    g_object_unref (icon);
}
