/*
 * Copyright (C) 2015 Alberts Muktupāvels
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
#include <math.h>
#include <gdk/gdk.h>
#include "gf-label-window.h"

#define HIDE_TIMEOUT 1500

struct _GfLabelWindow
{
  GfPopupWindow  parent;

  GdkRectangle   monitor;
  gint           size;

  guint          hide_timeout_id;

  GtkWidget     *label;
};

G_DEFINE_TYPE (GfLabelWindow, gf_label_window, GF_TYPE_POPUP_WINDOW)

static void
fade_finished_cb (GfPopupWindow *window)
{
  gtk_widget_destroy (GTK_WIDGET (window));
}

static void
gf_label_window_realize (GtkWidget *widget)
{
  cairo_region_t *region;

  GTK_WIDGET_CLASS (gf_label_window_parent_class)->realize (widget);

  region = cairo_region_create ();
  gtk_widget_input_shape_combine_region (widget, region);
  cairo_region_destroy (region);
}

static void
gf_label_window_get_preferred_width (GtkWidget *widget,
                                     gint      *minimum_width,
                                     gint      *natural_width)
{
  GfLabelWindow *window;

  window = GF_LABEL_WINDOW (widget);

  GTK_WIDGET_CLASS (gf_label_window_parent_class)->get_preferred_width (widget,
                                                                         minimum_width,
                                                                         natural_width);

  /* The label is a square of fixed size, it must not grow with its text. */
  *minimum_width = *natural_width = window->size;
}

static void
gf_label_window_get_preferred_height (GtkWidget *widget,
                                      gint      *minimum_height,
                                      gint      *natural_height)
{
  GfLabelWindow *window;

  window = GF_LABEL_WINDOW (widget);

  GTK_WIDGET_CLASS (gf_label_window_parent_class)->get_preferred_height (widget,
                                                                          minimum_height,
                                                                          natural_height);

  *minimum_height = *natural_height = window->size;
}

static void
gf_label_window_get_preferred_width_for_height (GtkWidget *widget,
                                                gint       height,
                                                gint      *minimum_width,
                                                gint      *natural_width)
{
  gf_label_window_get_preferred_width (widget, minimum_width, natural_width);
}

static void
gf_label_window_get_preferred_height_for_width (GtkWidget *widget,
                                                gint       width,
                                                gint      *minimum_height,
                                                gint      *natural_height)
{
  gf_label_window_get_preferred_height (widget, minimum_height, natural_height);
}

static void
fit_label (GfLabelWindow *window)
{
  GtkLabel *label;
  gint available;
  GtkRequisition natural;
  gdouble scale;
  PangoAttrList *attrs;

  label = GTK_LABEL (window->label);
  available = window->size - 2 * gtk_container_get_border_width (GTK_CONTAINER (window));
  if (available <= 0)
    return;

  gtk_widget_get_preferred_size (window->label, NULL, &natural);
  if (natural.width <= available && natural.height <= available)
    return;

  /* Shrink text that does not fit so that all numbers stay visible. */
  scale = MIN ((gdouble) available / MAX (natural.width, 1),
               (gdouble) available / MAX (natural.height, 1));

  attrs = pango_attr_list_new ();
  pango_attr_list_insert (attrs, pango_attr_scale_new (scale));
  gtk_label_set_attributes (label, attrs);
  pango_attr_list_unref (attrs);
}

static void
gf_label_window_class_init (GfLabelWindowClass *window_class)
{
  GtkWidgetClass *widget_class;

  widget_class = GTK_WIDGET_CLASS (window_class);

  widget_class->realize = gf_label_window_realize;
  widget_class->get_preferred_width = gf_label_window_get_preferred_width;
  widget_class->get_preferred_height = gf_label_window_get_preferred_height;
  widget_class->get_preferred_width_for_height = gf_label_window_get_preferred_width_for_height;
  widget_class->get_preferred_height_for_width = gf_label_window_get_preferred_height_for_width;
}

static void
gf_label_window_init (GfLabelWindow *window)
{
  GtkWidget *box;

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_container_set_border_width (GTK_CONTAINER (window), 20);
  gtk_container_add (GTK_CONTAINER (window), box);
  gtk_widget_show (box);

  window->label = gtk_label_new ("");
  gtk_widget_set_halign (window->label, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (window->label, GTK_ALIGN_CENTER);
  gtk_box_pack_start (GTK_BOX (box), window->label, TRUE, FALSE, 0);
  gtk_widget_show (window->label);

  g_signal_connect (window, "fade-finished",
                    G_CALLBACK (fade_finished_cb), NULL);

  gtk_widget_set_name (GTK_WIDGET (window), "gf-label-window");
}

GfLabelWindow *
gf_label_window_new (const GdkRectangle *workarea,
                     const gchar        *label)
{
  GfLabelWindow *window;
  gint width;
  gint height;
  gint size;

  window = g_object_new (GF_TYPE_LABEL_WINDOW,
                         "type", GTK_WINDOW_POPUP,
                         NULL);

  window->monitor = *workarea;

  width = window->monitor.width;
  height = window->monitor.height;
  size = 60 * MAX (1, MIN (width / 640.0, height / 480.0));

  window->size = size;
  gtk_window_resize (GTK_WINDOW (window), size, size);

  gtk_label_set_text (GTK_LABEL (window->label), label);
  fit_label (window);

  return window;
}

void
gf_label_window_show (GfLabelWindow *window)
{
  gint width;
  gint height;
  GdkRectangle rect;
  GtkTextDirection dir;
  gint x;
  gint y;

  gtk_window_get_size (GTK_WINDOW (window), &width, &height);

  rect = window->monitor;
  dir = gtk_widget_get_direction (GTK_WIDGET (window));

  if (dir == GTK_TEXT_DIR_NONE)
    dir = gtk_widget_get_default_direction ();

  if (dir == GTK_TEXT_DIR_RTL)
    x = rect.x + (rect.width - width - 20);
  else
    x = rect.x + 20;
  y = rect.y + 20;

  gtk_window_move (GTK_WINDOW (window), x, y);
  gtk_widget_show (GTK_WIDGET (window));

  gf_popup_window_fade_cancel (GF_POPUP_WINDOW (window));
}

void
gf_label_window_hide (GfLabelWindow *window)
{
  gf_popup_window_fade_start (GF_POPUP_WINDOW (window));
}
