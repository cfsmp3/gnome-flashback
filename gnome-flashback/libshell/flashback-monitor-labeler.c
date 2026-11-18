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
#include <gio/gio.h>
#include "flashback-monitor-labeler.h"
#include "gf-label-window.h"
#include "gf-shell-monitors.h"

struct _FlashbackMonitorLabeler
{
  GObject           parent;

  GfMonitorManager *monitor_manager;

  gulong            gdk_monitors_changed_id;

  guint             watch_id;
  gchar            *client;

  guint             hide_id;
  guint             update_id;

  /* The most recent request from the client, NULL while hidden. */
  GVariant         *params;

  GList            *windows;
};

G_DEFINE_TYPE (FlashbackMonitorLabeler, flashback_monitor_labeler, G_TYPE_OBJECT)

static void
window_destroy_cb (GtkWidget               *widget,
                   FlashbackMonitorLabeler *labeler)
{
  labeler->windows = g_list_remove (labeler->windows, widget);
}

static void
destroy_windows (FlashbackMonitorLabeler *labeler)
{
  GList *windows;
  GList *l;

  windows = labeler->windows;
  labeler->windows = NULL;

  for (l = windows; l != NULL; l = l->next)
    {
      g_signal_handlers_disconnect_by_func (l->data, window_destroy_cb, labeler);
      gtk_widget_destroy (GTK_WIDGET (l->data));
    }

  g_list_free (windows);
}

static gint
compare_numbers (gconstpointer a,
                 gconstpointer b)
{
  gint32 number_a;
  gint32 number_b;

  number_a = *(const gint32 *) a;
  number_b = *(const gint32 *) b;

  return (number_a > number_b) - (number_a < number_b);
}

static gchar *
get_label (GfLogicalMonitor *logical_monitor,
           GVariant         *params)
{
  GArray *numbers;
  GVariantIter iter;
  const gchar *connector;
  GVariant *value;
  GString *string;
  guint i;

  numbers = g_array_new (FALSE, FALSE, sizeof (gint32));

  g_variant_iter_init (&iter, params);
  while (g_variant_iter_next (&iter, "{&sv}", &connector, &value))
    {
      if (g_variant_is_of_type (value, G_VARIANT_TYPE_INT32) &&
          gf_shell_monitors_has_connector (logical_monitor, connector))
        {
          gint32 number;

          number = g_variant_get_int32 (value);
          g_array_append_val (numbers, number);
        }

      g_variant_unref (value);
    }

  if (numbers->len == 0)
    {
      g_array_free (numbers, TRUE);
      return NULL;
    }

  g_array_sort (numbers, compare_numbers);

  string = g_string_new (NULL);
  for (i = 0; i < numbers->len; i++)
    {
      if (i > 0)
        g_string_append_c (string, ' ');

      g_string_append_printf (string, "%d", g_array_index (numbers, gint32, i));
    }

  g_array_free (numbers, TRUE);

  return g_string_free (string, FALSE);
}

static void
update_labels (FlashbackMonitorLabeler *labeler)
{
  GList *l;

  destroy_windows (labeler);

  if (labeler->params == NULL || labeler->monitor_manager == NULL)
    return;

  for (l = gf_monitor_manager_get_logical_monitors (labeler->monitor_manager);
       l != NULL;
       l = l->next)
    {
      GfLogicalMonitor *logical_monitor;
      gchar *label;
      GdkRectangle workarea;
      GfLabelWindow *window;

      logical_monitor = l->data;
      label = get_label (logical_monitor, labeler->params);

      if (label == NULL)
        continue;

      gf_shell_monitors_get_workarea (logical_monitor, &workarea);

      window = gf_label_window_new (&workarea, label);
      g_free (label);

      g_signal_connect (window, "destroy",
                        G_CALLBACK (window_destroy_cb), labeler);

      labeler->windows = g_list_prepend (labeler->windows, window);

      gf_label_window_show (window);
    }
}

static gboolean
update_cb (gpointer user_data)
{
  FlashbackMonitorLabeler *labeler;

  labeler = FLASHBACK_MONITOR_LABELER (user_data);
  labeler->update_id = 0;

  update_labels (labeler);

  return G_SOURCE_REMOVE;
}

static void
queue_update (FlashbackMonitorLabeler *labeler)
{
  if (labeler->params == NULL || labeler->update_id != 0)
    return;

  labeler->update_id = g_idle_add (update_cb, labeler);
  g_source_set_name_by_id (labeler->update_id,
                           "[gnome-flashback] update_cb");
}

static void
monitors_changed_cb (GfMonitorManager        *monitor_manager,
                     FlashbackMonitorLabeler *labeler)
{
  queue_update (labeler);
}

static void
gdk_monitors_changed_cb (GdkScreen               *screen,
                         FlashbackMonitorLabeler *labeler)
{
  /* GDK may learn about a new layout after the monitor manager, the work
   * area is taken from GDK so update labels again.
   */
  queue_update (labeler);
}

static void
real_hide (FlashbackMonitorLabeler *labeler)
{
  GList *l;

  g_clear_pointer (&labeler->params, g_variant_unref);

  if (labeler->update_id != 0)
    {
      g_source_remove (labeler->update_id);
      labeler->update_id = 0;
    }

  /* Windows destroy themselves when fade finishes. */
  for (l = labeler->windows; l != NULL; l = l->next)
    gf_label_window_hide (l->data);
}

static void
untrack_client (FlashbackMonitorLabeler *labeler)
{
  if (labeler->watch_id > 0)
    {
      g_bus_unwatch_name (labeler->watch_id);
      labeler->watch_id = 0;
    }

  if (labeler->hide_id > 0)
    {
      g_source_remove (labeler->hide_id);
      labeler->hide_id = 0;
    }

  g_clear_pointer (&labeler->client, g_free);
}

static void
name_vanished_handler (GDBusConnection *connection,
                       const gchar     *name,
                       gpointer         user_data)
{
  FlashbackMonitorLabeler *labeler;

  labeler = FLASHBACK_MONITOR_LABELER (user_data);

  untrack_client (labeler);
  real_hide (labeler);
}

static gboolean
track_client (FlashbackMonitorLabeler *labeler,
              const gchar             *client)
{
  if (labeler->client != NULL)
    return g_strcmp0 (labeler->client, client) == 0;

  labeler->client = g_strdup (client);
  labeler->watch_id = g_bus_watch_name (G_BUS_TYPE_SESSION,
                                        client,
                                        G_BUS_NAME_WATCHER_FLAGS_NONE,
                                        NULL,
                                        (GBusNameVanishedCallback) name_vanished_handler,
                                        labeler,
                                        NULL);

  return TRUE;
}

static gboolean
hide_cb (gpointer user_data)
{
  FlashbackMonitorLabeler *labeler;

  labeler = FLASHBACK_MONITOR_LABELER (user_data);
  labeler->hide_id = 0;

  untrack_client (labeler);
  real_hide (labeler);

  return G_SOURCE_REMOVE;
}

static void
flashback_monitor_labeler_dispose (GObject *object)
{
  FlashbackMonitorLabeler *labeler;

  labeler = FLASHBACK_MONITOR_LABELER (object);

  untrack_client (labeler);

  g_clear_pointer (&labeler->params, g_variant_unref);

  if (labeler->update_id != 0)
    {
      g_source_remove (labeler->update_id);
      labeler->update_id = 0;
    }

  flashback_monitor_labeler_set_monitor_manager (labeler, NULL);

  if (labeler->gdk_monitors_changed_id != 0)
    {
      g_signal_handler_disconnect (gdk_screen_get_default (),
                                   labeler->gdk_monitors_changed_id);
      labeler->gdk_monitors_changed_id = 0;
    }

  destroy_windows (labeler);

  G_OBJECT_CLASS (flashback_monitor_labeler_parent_class)->dispose (object);
}

static void
flashback_monitor_labeler_class_init (FlashbackMonitorLabelerClass *labeler_class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (labeler_class);

  object_class->dispose = flashback_monitor_labeler_dispose;
}

static void
flashback_monitor_labeler_init (FlashbackMonitorLabeler *labeler)
{
  labeler->gdk_monitors_changed_id =
    g_signal_connect (gdk_screen_get_default (), "monitors-changed",
                      G_CALLBACK (gdk_monitors_changed_cb), labeler);
}

FlashbackMonitorLabeler *
flashback_monitor_labeler_new (void)
{
  return g_object_new (FLASHBACK_TYPE_MONITOR_LABELER, NULL);
}

void
flashback_monitor_labeler_set_monitor_manager (FlashbackMonitorLabeler *labeler,
                                               GfMonitorManager        *monitor_manager)
{
  if (labeler->monitor_manager == monitor_manager)
    return;

  if (labeler->monitor_manager != NULL)
    {
      g_signal_handlers_disconnect_by_func (labeler->monitor_manager,
                                            monitors_changed_cb,
                                            labeler);
    }

  labeler->monitor_manager = monitor_manager;

  if (monitor_manager != NULL)
    {
      g_signal_connect (monitor_manager, "monitors-changed",
                        G_CALLBACK (monitors_changed_cb), labeler);
    }

  queue_update (labeler);
}

void
flashback_monitor_labeler_show (FlashbackMonitorLabeler *labeler,
                                const gchar             *sender,
                                GVariant                *params)
{
  if (!track_client (labeler, sender))
    return;

  if (labeler->hide_id > 0)
    {
      g_source_remove (labeler->hide_id);
      labeler->hide_id = 0;
    }

  if (labeler->update_id != 0)
    {
      g_source_remove (labeler->update_id);
      labeler->update_id = 0;
    }

  /* Every request replaces the labels of the previous one. */
  g_clear_pointer (&labeler->params, g_variant_unref);
  labeler->params = g_variant_ref_sink (params);

  update_labels (labeler);
}

void
flashback_monitor_labeler_hide (FlashbackMonitorLabeler *labeler,
                                const gchar             *sender)
{
  if (labeler->client == NULL || g_strcmp0 (labeler->client, sender) != 0)
    return;

  if (labeler->hide_id > 0)
    g_source_remove (labeler->hide_id);

  labeler->hide_id = g_timeout_add (100, hide_cb, labeler);
  g_source_set_name_by_id (labeler->hide_id, "[gnome-flashback] hide_cb");
}
