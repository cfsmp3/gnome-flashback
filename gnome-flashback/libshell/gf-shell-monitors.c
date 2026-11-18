/*
 * Copyright (C) 2026 Carlos Fernandez Sanz
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

#include "config.h"
#include "gf-shell-monitors.h"

/**
 * gf_shell_monitors_get_logical_monitor:
 * @monitor_manager: a #GfMonitorManager
 * @connector: a connector name
 *
 * Returns: (transfer none) (nullable): the logical monitor that the active
 * connector @connector belongs to in the current configuration, or %NULL if
 * the connector is unknown or not active.
 */
GfLogicalMonitor *
gf_shell_monitors_get_logical_monitor (GfMonitorManager *monitor_manager,
                                       const gchar      *connector)
{
  GList *l;

  if (monitor_manager == NULL || connector == NULL)
    return NULL;

  for (l = gf_monitor_manager_get_logical_monitors (monitor_manager);
       l != NULL;
       l = l->next)
    {
      GfLogicalMonitor *logical_monitor;

      logical_monitor = l->data;

      if (gf_shell_monitors_has_connector (logical_monitor, connector))
        return logical_monitor;
    }

  return NULL;
}

gboolean
gf_shell_monitors_has_connector (GfLogicalMonitor *logical_monitor,
                                 const gchar      *connector)
{
  GList *l;

  if (connector == NULL)
    return FALSE;

  for (l = gf_logical_monitor_get_monitors (logical_monitor);
       l != NULL;
       l = l->next)
    {
      if (g_strcmp0 (gf_monitor_get_connector (l->data), connector) == 0)
        return TRUE;
    }

  return FALSE;
}

/**
 * gf_shell_monitors_get_workarea:
 * @logical_monitor: a #GfLogicalMonitor
 * @workarea: (out): return location for the work area
 *
 * Gets the work area of @logical_monitor. The position and size always come
 * from the logical monitor layout. GDK numbers its monitors in its own order,
 * which does not follow the logical monitors, so the GDK monitor covering
 * exactly the same area is only used to take reserved space into account.
 * Until GDK has caught up with a new layout the whole logical monitor is
 * used.
 */
void
gf_shell_monitors_get_workarea (GfLogicalMonitor *logical_monitor,
                                GdkRectangle     *workarea)
{
  GfRectangle layout;
  GdkRectangle rect;
  GdkDisplay *display;
  gboolean primary;
  GdkMonitor *best_monitor;
  gint best_score;
  gint n_monitors;
  gint i;

  layout = gf_logical_monitor_get_layout (logical_monitor);

  rect.x = layout.x;
  rect.y = layout.y;
  rect.width = layout.width;
  rect.height = layout.height;

  *workarea = rect;

  primary = gf_logical_monitor_is_primary (logical_monitor) ? TRUE : FALSE;
  display = gdk_display_get_default ();
  n_monitors = gdk_display_get_n_monitors (display);

  best_monitor = NULL;
  best_score = -1;

  for (i = 0; i < n_monitors; i++)
    {
      GdkMonitor *monitor;
      GdkRectangle geometry;
      gboolean is_primary;
      gint score;

      monitor = gdk_display_get_monitor (display, i);
      gdk_monitor_get_geometry (monitor, &geometry);

      if (!gdk_rectangle_equal (&geometry, &rect))
        continue;

      /* Mirrored outputs share the geometry; prefer the one that matches
       * the primary state and one of the logical monitor's connectors.
       */
      is_primary = gdk_monitor_is_primary (monitor) ? TRUE : FALSE;

      score = 0;
      if (is_primary == primary)
        score += 2;

      if (gf_shell_monitors_has_connector (logical_monitor,
                                           gdk_monitor_get_model (monitor)))
        score += 1;

      if (score > best_score)
        {
          best_monitor = monitor;
          best_score = score;
        }
    }

  if (best_monitor != NULL)
    {
      GdkRectangle area;

      gdk_monitor_get_workarea (best_monitor, &area);

      if (gdk_rectangle_intersect (&area, &rect, &area))
        *workarea = area;
    }
}
