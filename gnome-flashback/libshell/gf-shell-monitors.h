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

#ifndef GF_SHELL_MONITORS_H
#define GF_SHELL_MONITORS_H

#include <gdk/gdk.h>

#include "backends/gf-logical-monitor-private.h"

G_BEGIN_DECLS

GfLogicalMonitor *gf_shell_monitors_get_logical_monitor (GfMonitorManager *monitor_manager,
                                                         const gchar      *connector);

gboolean          gf_shell_monitors_has_connector       (GfLogicalMonitor *logical_monitor,
                                                         const gchar      *connector);

void              gf_shell_monitors_get_workarea        (GfLogicalMonitor *logical_monitor,
                                                         GdkRectangle     *workarea);

G_END_DECLS

#endif
