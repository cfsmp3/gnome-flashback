/*
 * Copyright (C) 2015 Alberts Muktupāvels
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
#include "gf-screencast.h"

#include <errno.h>
#include <string.h>
#include <glib/gstdio.h>
#include <gdk/gdkx.h>
#include <gst/gst.h>
#include <gtk/gtk.h>

#include "dbus/gf-screencast-gen.h"

/* The encoder GNOME Shell used before it moved to PipeWire. */
#define DEFAULT_PIPELINE \
  "vp8enc min_quantizer=13 max_quantizer=13 cpu-used=5 deadline=1000000 " \
  "threads=%u ! queue ! webmmux"

#define DEFAULT_FRAMERATE 30

#define START_TIMEOUT_SECONDS 10
/* A slow pipeline may need a while to encode what it has queued. */
#define STOP_TIMEOUT_SECONDS 30
#define SHUTDOWN_TIMEOUT_SECONDS 5

#define FIRST_DATA_MESSAGE "gf-screencast-first-data"

typedef struct
{
  GfScreencast          *self;

  gchar                 *sender;
  guint                  name_watch_id;

  gchar                 *filename;
  GPtrArray             *created_directories;

  GstElement            *pipeline;
  GstElement            *sink;
  guint                  bus_watch_id;

  /* Start timeout until the first data is written, then stop timeout. */
  guint                  timeout_id;

  GDBusMethodInvocation *start_invocation;
  gboolean               started;

  gboolean               stopping;
  gboolean               eos_sent;
  GSList                *stop_invocations;
} GfRecording;

struct _GfScreencast
{
  GObject          parent;

  gint             bus_name_id;
  GfScreencastGen *screencast_gen;

  GSettings       *lockdown;

  /* Recordings keyed by the unique bus name of their callers. */
  GHashTable      *recordings;
};

G_DEFINE_TYPE (GfScreencast, gf_screencast, G_TYPE_OBJECT)

/*
 * Xlib only updates its screen size when it is asked to, so query the
 * root window to get the size the screen has right now.
 */
static void
get_screen_size (gint *width,
                 gint *height)
{
  Display *xdisplay;
  Window root;
  gint x;
  gint y;
  guint root_width;
  guint root_height;
  guint border_width;
  guint depth;

  xdisplay = gdk_x11_get_default_xdisplay ();

  if (!XGetGeometry (xdisplay, DefaultRootWindow (xdisplay), &root, &x, &y,
                     &root_width, &root_height, &border_width, &depth))
    {
      root_width = WidthOfScreen (DefaultScreenOfDisplay (xdisplay));
      root_height = HeightOfScreen (DefaultScreenOfDisplay (xdisplay));
    }

  *width = root_width;
  *height = root_height;
}

/*
 * Expands %d (date), %t (time) and %% in the file template. Other escapes
 * are dropped, a trailing % is kept.
 */
static gchar *
expand_template (const gchar *file_template)
{
  GDateTime *now;
  GString *result;
  const gchar *p;

  now = g_date_time_new_now_local ();
  result = g_string_new (NULL);

  p = file_template;

  while (*p != '\0')
    {
      gchar *value;

      if (*p != '%')
        {
          g_string_append_c (result, *p);
          p++;
          continue;
        }

      switch (p[1])
        {
          case '\0':
            g_string_append_c (result, '%');
            break;

          case 'd':
            value = g_date_time_format (now, "%Y-%m-%d");
            g_string_append (result, value);
            g_free (value);
            break;

          case 't':
            value = g_date_time_format (now, "%H-%M-%S");
            g_string_append (result, value);
            g_free (value);
            break;

          case '%':
            g_string_append_c (result, '%');
            break;

          default:
            break;
        }

      if (p[1] == '\0')
        break;

      /* Skip the whole escaped character, which may be multibyte. */
      p = g_utf8_next_char (p + 1);
    }

  g_date_time_unref (now);

  return g_string_free (result, FALSE);
}

static gchar *
get_filename (const gchar *file_template,
              gboolean     add_extension)
{
  gchar *expanded;
  const gchar *directory;
  gchar *filename;

  expanded = expand_template (file_template);

  if (add_extension)
    {
      gchar *tmp;

      if (g_str_has_suffix (expanded, ".webm"))
        expanded[strlen (expanded) - strlen (".webm")] = '\0';

      tmp = g_strconcat (expanded, ".webm", NULL);
      g_free (expanded);
      expanded = tmp;
    }

  directory = g_get_user_special_dir (G_USER_DIRECTORY_VIDEOS);

  if (directory == NULL || !g_file_test (directory, G_FILE_TEST_IS_DIR))
    directory = g_get_home_dir ();

  filename = g_canonicalize_filename (expanded, directory);
  g_free (expanded);

  return filename;
}

static void
remove_directories (GPtrArray *directories)
{
  guint i;

  for (i = directories->len; i > 0; i--)
    g_rmdir (g_ptr_array_index (directories, i - 1));

  g_ptr_array_set_size (directories, 0);
}

/*
 * Creates the missing parent directories of @filename and adds the ones
 * it created to @created, so that a failed start can remove them again.
 */
static gboolean
create_parent_directories (const gchar  *filename,
                           GPtrArray    *created,
                           GError      **error)
{
  gchar *directory;
  GSList *missing;
  GSList *l;
  gboolean result;

  directory = g_path_get_dirname (filename);
  missing = NULL;

  while (!g_file_test (directory, G_FILE_TEST_EXISTS))
    {
      gchar *parent;

      parent = g_path_get_dirname (directory);

      if (g_strcmp0 (parent, directory) == 0)
        {
          g_free (parent);
          break;
        }

      missing = g_slist_prepend (missing, directory);
      directory = parent;
    }

  g_free (directory);
  result = TRUE;

  for (l = missing; l != NULL; l = l->next)
    {
      int saved_errno;

      if (g_mkdir (l->data, 0755) == 0)
        {
          g_ptr_array_add (created, g_strdup (l->data));
          continue;
        }

      saved_errno = errno;

      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "Failed to create directory “%s”: %s",
                   (const gchar *) l->data, g_strerror (saved_errno));

      result = FALSE;
      break;
    }

  g_slist_free_full (missing, g_free);

  if (result)
    {
      directory = g_path_get_dirname (filename);

      if (!g_file_test (directory, G_FILE_TEST_IS_DIR))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                       "“%s” is not a directory", directory);

          result = FALSE;
        }

      g_free (directory);
    }

  if (!result)
    remove_directories (created);

  return result;
}

static GstElement *
add_element (GstElement   *pipeline,
             const gchar  *factory,
             GError      **error)
{
  GstElement *element;

  element = gst_element_factory_make (factory, NULL);

  if (element == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "GStreamer element “%s” is not available", factory);

      return NULL;
    }

  gst_bin_add (GST_BIN (pipeline), element);

  return element;
}

static GstElement *
add_linked_element (GstElement   *pipeline,
                    GstElement   *previous,
                    const gchar  *factory,
                    GError      **error)
{
  GstElement *element;

  element = add_element (pipeline, factory, error);

  if (element == NULL)
    return NULL;

  if (!gst_element_link (previous, element))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to link GStreamer element “%s”", factory);

      return NULL;
    }

  return element;
}

static GstPadProbeReturn
first_data_cb (GstPad          *pad,
               GstPadProbeInfo *info,
               gpointer         user_data)
{
  GstElement *pipeline;
  GstStructure *structure;
  GstMessage *message;

  pipeline = GST_ELEMENT (user_data);

  structure = gst_structure_new_empty (FIRST_DATA_MESSAGE);
  message = gst_message_new_application (GST_OBJECT (pipeline), structure);

  gst_element_post_message (pipeline, message);

  return GST_PAD_PROBE_REMOVE;
}

static GstPadProbeReturn
first_frame_cb (GstPad          *pad,
                GstPadProbeInfo *info,
                gpointer         user_data)
{
  /* From the second frame on, fill gaps with duplicates. */
  g_object_set (user_data, "drop-only", FALSE, NULL);

  return GST_PAD_PROBE_REMOVE;
}

static GstElement *
create_pipeline (const gchar   *filename,
                 gboolean       area,
                 gint           x,
                 gint           y,
                 gint           width,
                 gint           height,
                 gint           screen_width,
                 gint           screen_height,
                 gboolean       draw_cursor,
                 gint           framerate,
                 const gchar   *encoder_description,
                 GstElement   **sink_out,
                 GError       **error)
{
  GstElement *pipeline;
  GstElement *last;
  GstCaps *caps;
  gchar *description;
  GstElement *encoder;
  GstElement *sink;
  GstPad *pad;

  pipeline = gst_pipeline_new ("screencast");

  last = add_element (pipeline, "ximagesrc", error);
  if (last == NULL)
    goto error;

  g_object_set (last,
                "display-name", gdk_display_get_name (gdk_display_get_default ()),
                "show-pointer", draw_cursor,
                "use-damage", FALSE,
                NULL);

  if (area)
    {
      gint crop_right;
      gint crop_bottom;

      /*
       * ximagesrc reads an end coordinate of 0 as the edge of the screen,
       * so capture a single column or row at the origin two pixels wide
       * and crop the extra one off.
       */
      crop_right = (x == 0 && width == 1 && screen_width > 1) ? 1 : 0;
      crop_bottom = (y == 0 && height == 1 && screen_height > 1) ? 1 : 0;

      g_object_set (last,
                    "startx", (guint) x,
                    "starty", (guint) y,
                    "endx", (guint) (x + width - 1 + crop_right),
                    "endy", (guint) (y + height - 1 + crop_bottom),
                    NULL);

      if (crop_right != 0 || crop_bottom != 0)
        {
          last = add_linked_element (pipeline, last, "videocrop", error);
          if (last == NULL)
            goto error;

          g_object_set (last,
                        "right", crop_right,
                        "bottom", crop_bottom,
                        NULL);
        }
    }

  last = add_linked_element (pipeline, last, "capsfilter", error);
  if (last == NULL)
    goto error;

  caps = gst_caps_new_simple ("video/x-raw",
                              "framerate", GST_TYPE_FRACTION, framerate, 1,
                              NULL);

  g_object_set (last, "caps", caps, NULL);
  gst_caps_unref (caps);

  /* Keep capturing at a steady rate while the encoder is busy. */
  last = add_linked_element (pipeline, last, "queue", error);
  if (last == NULL)
    goto error;

  /*
   * Captures are timestamped when they are taken, which jitters. Give the
   * stream a constant rate, so that no two frames end up with the same
   * timestamp in the container.
   */
  last = add_linked_element (pipeline, last, "videorate", error);
  if (last == NULL)
    goto error;

  /*
   * videorate holds each frame until the next one arrives, which at a low
   * framerate would delay the first frame, and so the start reply, by a
   * whole frame interval. Let the first frame through right away.
   */
  g_object_set (last, "drop-only", TRUE, NULL);

  pad = gst_element_get_static_pad (last, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER,
                     first_frame_cb, last, NULL);
  gst_object_unref (pad);

  last = add_linked_element (pipeline, last, "videoconvert", error);
  if (last == NULL)
    goto error;

  if (encoder_description != NULL)
    {
      description = g_strdup (encoder_description);
    }
  else
    {
      guint threads;

      threads = CLAMP (g_get_num_processors () - 1, 1, 64);
      description = g_strdup_printf (DEFAULT_PIPELINE, threads);
    }

  encoder = gst_parse_bin_from_description_full (description,
                                                 TRUE,
                                                 NULL,
                                                 GST_PARSE_FLAG_FATAL_ERRORS,
                                                 error);

  g_free (description);

  if (encoder == NULL)
    goto error;

  gst_bin_add (GST_BIN (pipeline), encoder);

  if (!gst_element_link (last, encoder))
    {
      /* For example, webmmux takes no video smaller than 16x16. */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "The pipeline does not accept %dx%d raw video",
                   area ? width : screen_width,
                   area ? height : screen_height);

      goto error;
    }

  sink = add_element (pipeline, "filesink", error);
  if (sink == NULL)
    goto error;

  g_object_set (sink, "location", filename, NULL);

  if (!gst_element_link (encoder, sink))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "The pipeline does not produce output");

      goto error;
    }

  /*
   * The start is only confirmed once data reaches the file: by then the
   * whole pipeline, a caller-supplied encoder included, has accepted a
   * frame.
   */
  pad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (pad,
                     GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST,
                     first_data_cb, pipeline, NULL);
  gst_object_unref (pad);

  *sink_out = sink;

  return pipeline;

error:
  gst_object_unref (pipeline);

  return NULL;
}

static gpointer
teardown_thread (gpointer data)
{
  GstElement *pipeline;

  pipeline = GST_ELEMENT (data);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  return NULL;
}

static void
complete_stop_invocations (GfRecording  *recording,
                           gboolean      stopped,
                           const GError *error)
{
  GSList *invocations;
  GSList *l;

  invocations = g_steal_pointer (&recording->stop_invocations);

  for (l = invocations; l != NULL; l = l->next)
    {
      GDBusMethodInvocation *invocation;

      invocation = l->data;

      if (error != NULL)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 G_IO_ERROR,
                                                 G_IO_ERROR_FAILED,
                                                 "Recording failed: %s",
                                                 error->message);
        }
      else
        {
          gf_screencast_gen_complete_stop_screencast (recording->self->screencast_gen,
                                                      invocation,
                                                      stopped);
        }
    }

  g_slist_free_full (invocations, g_object_unref);
}

static void
recording_free (gpointer data)
{
  GfRecording *recording;

  recording = data;

  if (recording->name_watch_id != 0)
    g_bus_unwatch_name (recording->name_watch_id);

  if (recording->bus_watch_id != 0)
    g_source_remove (recording->bus_watch_id);

  if (recording->timeout_id != 0)
    g_source_remove (recording->timeout_id);

  if (recording->pipeline != NULL)
    {
      gst_element_set_state (recording->pipeline, GST_STATE_NULL);
      gst_object_unref (recording->pipeline);
    }

  if (recording->start_invocation != NULL)
    {
      g_dbus_method_invocation_return_error_literal (recording->start_invocation,
                                                     G_IO_ERROR,
                                                     G_IO_ERROR_FAILED,
                                                     "Screencast service is "
                                                     "shutting down");
    }

  complete_stop_invocations (recording, FALSE, NULL);

  g_clear_pointer (&recording->created_directories, g_ptr_array_unref);

  g_free (recording->filename);
  g_free (recording->sender);
  g_free (recording);
}

/*
 * Stops the pipeline of a recording that never wrote any data and removes
 * whatever the start created: the partial file and the directories.
 */
static void
recording_cleanup (GfRecording *recording)
{
  GstState sink_state;

  gst_element_get_state (recording->sink, &sink_state, NULL, 0);
  gst_element_set_state (recording->pipeline, GST_STATE_NULL);

  /* The sink opens, and so truncates, the file when it starts. */
  if (sink_state != GST_STATE_NULL)
    g_unlink (recording->filename);

  remove_directories (recording->created_directories);
}

static void
recording_start_failed (GfRecording *recording,
                        const gchar *reason)
{
  GDBusMethodInvocation *invocation;

  recording_cleanup (recording);

  invocation = g_steal_pointer (&recording->start_invocation);

  if (invocation != NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_FAILED,
                                             "Failed to start recording "
                                             "to “%s”: %s",
                                             recording->filename,
                                             reason);
    }

  /* The caller never had a recording to stop. */
  complete_stop_invocations (recording, FALSE, NULL);

  g_hash_table_remove (recording->self->recordings, recording->sender);
}

static gboolean
stop_timeout_cb (gpointer user_data)
{
  GfRecording *recording;
  GError *error;

  recording = user_data;
  recording->timeout_id = 0;

  g_warning ("Screencast to “%s” did not finish in time", recording->filename);

  error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                               "The recording did not finish in time");

  complete_stop_invocations (recording, FALSE, error);
  g_error_free (error);

  /* Stopping waits for streaming threads, which may be stuck. */
  g_thread_unref (g_thread_new ("gf-screencast-teardown",
                                teardown_thread,
                                g_steal_pointer (&recording->pipeline)));

  g_hash_table_remove (recording->self->recordings, recording->sender);

  return G_SOURCE_REMOVE;
}

static void
recording_send_eos (GfRecording *recording)
{
  if (recording->eos_sent)
    return;

  recording->eos_sent = TRUE;

  /* The file is complete once the end of stream reaches the sink. */
  gst_element_send_event (recording->pipeline, gst_event_new_eos ());

  recording->timeout_id = g_timeout_add_seconds (STOP_TIMEOUT_SECONDS,
                                                 stop_timeout_cb,
                                                 recording);
}

static void
recording_started (GfRecording *recording)
{
  GDBusMethodInvocation *invocation;

  if (recording->started)
    return;

  recording->started = TRUE;

  g_clear_handle_id (&recording->timeout_id, g_source_remove);
  g_clear_pointer (&recording->created_directories, g_ptr_array_unref);

  invocation = g_steal_pointer (&recording->start_invocation);

  if (invocation != NULL)
    {
      gf_screencast_gen_complete_screencast (recording->self->screencast_gen,
                                             invocation,
                                             TRUE,
                                             recording->filename);
    }

  /* Asked to stop, or the caller left, before any data was written. */
  if (recording->stopping)
    recording_send_eos (recording);
}

static void
recording_finished (GfRecording  *recording,
                    const GError *error)
{
  gst_element_set_state (recording->pipeline, GST_STATE_NULL);

  complete_stop_invocations (recording, TRUE, error);

  g_hash_table_remove (recording->self->recordings, recording->sender);
}

static gboolean
bus_watch_cb (GstBus     *bus,
              GstMessage *message,
              gpointer    user_data)
{
  GfRecording *recording;
  GError *error;
  gchar *debug;

  recording = user_data;

  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_APPLICATION)
    {
      if (gst_message_has_name (message, FIRST_DATA_MESSAGE))
        recording_started (recording);
    }
  else if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_EOS)
    {
      recording->bus_watch_id = 0;

      if (recording->started)
        recording_finished (recording, NULL);
      else
        recording_start_failed (recording, "No data was recorded");

      return G_SOURCE_REMOVE;
    }
  else if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR)
    {
      gst_message_parse_error (message, &error, &debug);

      g_warning ("Screencast to “%s” failed: %s (%s)",
                 recording->filename, error->message,
                 debug ? debug : "no details");

      recording->bus_watch_id = 0;

      if (recording->started)
        recording_finished (recording, error);
      else
        recording_start_failed (recording, error->message);

      g_error_free (error);
      g_free (debug);

      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
start_timeout_cb (gpointer user_data)
{
  GfRecording *recording;

  recording = user_data;
  recording->timeout_id = 0;

  recording_start_failed (recording, "No data was recorded in time");

  return G_SOURCE_REMOVE;
}

static void
recording_stop (GfRecording           *recording,
                GDBusMethodInvocation *invocation)
{
  if (invocation != NULL)
    {
      recording->stop_invocations = g_slist_prepend (recording->stop_invocations,
                                                     g_object_ref (invocation));
    }

  if (recording->stopping)
    return;

  recording->stopping = TRUE;

  /* Ending the stream before any data would leave an empty file. */
  if (recording->started)
    recording_send_eos (recording);
}

/* Finishes a recording without a main loop, used when shutting down. */
static void
recording_stop_sync (GfRecording *recording)
{
  GstBus *bus;
  GstMessage *message;

  if (!recording->started)
    {
      recording_cleanup (recording);
      return;
    }

  gst_element_send_event (recording->pipeline, gst_event_new_eos ());

  bus = gst_element_get_bus (recording->pipeline);
  message = gst_bus_timed_pop_filtered (bus,
                                        SHUTDOWN_TIMEOUT_SECONDS * GST_SECOND,
                                        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

  complete_stop_invocations (recording,
                             message != NULL &&
                             GST_MESSAGE_TYPE (message) == GST_MESSAGE_EOS,
                             NULL);

  g_clear_pointer (&message, gst_message_unref);
  gst_object_unref (bus);
}

static void
name_vanished_cb (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  GfScreencast *self;
  GfRecording *recording;

  self = GF_SCREENCAST (user_data);
  recording = g_hash_table_lookup (self->recordings, name);

  if (recording != NULL)
    recording_stop (recording, NULL);
}

static gboolean
is_filename_in_use (GfScreencast *self,
                    const gchar  *filename)
{
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, self->recordings);

  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      GfRecording *recording;

      recording = value;

      if (g_strcmp0 (recording->filename, filename) == 0)
        return TRUE;
    }

  return FALSE;
}

static void
start_recording (GfScreencast          *self,
                 GDBusMethodInvocation *invocation,
                 gboolean               area,
                 gint                   x,
                 gint                   y,
                 gint                   width,
                 gint                   height,
                 const gchar           *file_template,
                 GVariant              *options)
{
  const gchar *sender;
  gint screen_width;
  gint screen_height;
  gboolean draw_cursor;
  gint framerate;
  const gchar *encoder_description;
  GError *error;
  gchar *filename;
  GPtrArray *created_directories;
  GstElement *pipeline;
  GstElement *sink;
  GfRecording *recording;
  GstBus *bus;

  sender = g_dbus_method_invocation_get_sender (invocation);

  if (g_hash_table_contains (self->recordings, sender))
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_IO_ERROR,
                                                     G_IO_ERROR_EXISTS,
                                                     "A screencast is already "
                                                     "being recorded");

      return;
    }

  if (g_settings_get_boolean (self->lockdown, "disable-save-to-disk"))
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_IO_ERROR,
                                                     G_IO_ERROR_PERMISSION_DENIED,
                                                     "Saving to disk is "
                                                     "disabled");

      return;
    }

  get_screen_size (&screen_width, &screen_height);

  if (area &&
      (x < 0 || y < 0 || width < 1 || height < 1 ||
       (gint64) x + width > screen_width ||
       (gint64) y + height > screen_height))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_INVALID_ARGUMENT,
                                             "Invalid area %d,%d %dx%d for "
                                             "a %dx%d screen",
                                             x, y, width, height,
                                             screen_width, screen_height);

      return;
    }

  if (!g_variant_lookup (options, "draw-cursor", "b", &draw_cursor))
    draw_cursor = TRUE;

  if (!g_variant_lookup (options, "framerate", "i", &framerate))
    framerate = DEFAULT_FRAMERATE;

  if (!g_variant_lookup (options, "pipeline", "&s", &encoder_description))
    encoder_description = NULL;

  if (framerate < 1)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_INVALID_ARGUMENT,
                                             "Invalid framerate %d",
                                             framerate);

      return;
    }

  if (encoder_description != NULL && *encoder_description == '\0')
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_IO_ERROR,
                                                     G_IO_ERROR_INVALID_ARGUMENT,
                                                     "The pipeline is empty");

      return;
    }

  error = NULL;

  if (!gst_init_check (NULL, NULL, &error))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_FAILED,
                                             "Failed to initialize "
                                             "GStreamer: %s",
                                             error->message);

      g_error_free (error);
      return;
    }

  filename = get_filename (file_template, encoder_description == NULL);

  if (is_filename_in_use (self, filename))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_BUSY,
                                             "“%s” is already being recorded "
                                             "to", filename);

      g_free (filename);
      return;
    }

  pipeline = create_pipeline (filename,
                              area, x, y, width, height,
                              screen_width, screen_height,
                              draw_cursor, framerate, encoder_description,
                              &sink, &error);

  if (pipeline == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_FAILED,
                                             "Failed to create the "
                                             "recording pipeline: %s",
                                             error->message);

      g_error_free (error);
      g_free (filename);
      return;
    }

  created_directories = g_ptr_array_new_with_free_func (g_free);

  if (!create_parent_directories (filename, created_directories, &error))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_FAILED,
                                             "%s", error->message);

      g_error_free (error);
      g_ptr_array_unref (created_directories);
      gst_object_unref (pipeline);
      g_free (filename);
      return;
    }

  recording = g_new0 (GfRecording, 1);
  recording->self = self;
  recording->sender = g_strdup (sender);
  recording->filename = filename;
  recording->created_directories = created_directories;
  recording->pipeline = pipeline;
  recording->sink = sink;
  /* The reply, sent later, takes over the reference of the handler. */
  recording->start_invocation = invocation;

  if (gst_element_set_state (pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
      GstMessage *message;
      gchar *reason;

      bus = gst_element_get_bus (pipeline);
      message = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);
      gst_object_unref (bus);

      reason = NULL;

      if (message != NULL)
        {
          gst_message_parse_error (message, &error, NULL);
          reason = g_strdup (error->message);

          g_error_free (error);
          gst_message_unref (message);
        }

      g_hash_table_insert (self->recordings, recording->sender, recording);
      recording_start_failed (recording, reason ? reason : "Unknown error");

      g_free (reason);
      return;
    }

  bus = gst_element_get_bus (pipeline);
  recording->bus_watch_id = gst_bus_add_watch (bus, bus_watch_cb, recording);
  gst_object_unref (bus);

  recording->name_watch_id =
    g_bus_watch_name_on_connection (g_dbus_method_invocation_get_connection (invocation),
                                    sender,
                                    G_BUS_NAME_WATCHER_FLAGS_NONE,
                                    NULL,
                                    name_vanished_cb,
                                    self,
                                    NULL);

  recording->timeout_id = g_timeout_add_seconds (START_TIMEOUT_SECONDS,
                                                 start_timeout_cb,
                                                 recording);

  /* The reply is sent once data reaches the file, see recording_started. */
  g_hash_table_insert (self->recordings, recording->sender, recording);
}

static gboolean
handle_screencast (GfScreencastGen       *screencast_gen,
                   GDBusMethodInvocation *invocation,
                   const gchar           *file_template,
                   GVariant              *options,
                   GfScreencast          *self)
{
  start_recording (self, invocation, FALSE, 0, 0, 0, 0,
                   file_template, options);

  return TRUE;
}

static gboolean
handle_screencast_area (GfScreencastGen       *screencast_gen,
                        GDBusMethodInvocation *invocation,
                        gint                   x,
                        gint                   y,
                        gint                   width,
                        gint                   height,
                        const gchar           *file_template,
                        GVariant              *options,
                        GfScreencast          *self)
{
  start_recording (self, invocation, TRUE, x, y, width, height,
                   file_template, options);

  return TRUE;
}

static gboolean
handle_stop_screencast (GfScreencastGen       *screencast_gen,
                        GDBusMethodInvocation *invocation,
                        GfScreencast          *self)
{
  const gchar *sender;
  GfRecording *recording;

  sender = g_dbus_method_invocation_get_sender (invocation);
  recording = g_hash_table_lookup (self->recordings, sender);

  if (recording == NULL)
    {
      gf_screencast_gen_complete_stop_screencast (screencast_gen,
                                                  invocation,
                                                  FALSE);

      return TRUE;
    }

  /* The reply is sent once the file is complete. */
  recording_stop (recording, invocation);

  return TRUE;
}

static void
bus_acquired_cb (GDBusConnection *connection,
                 const char      *name,
                 gpointer         user_data)
{
  GfScreencast *self;
  GDBusInterfaceSkeleton *skeleton;
  GError *error;

  self = GF_SCREENCAST (user_data);
  skeleton = G_DBUS_INTERFACE_SKELETON (self->screencast_gen);
  error = NULL;

  if (!g_dbus_interface_skeleton_export (skeleton,
                                         connection,
                                         "/org/gnome/Shell/Screencast",
                                         &error))
    {
      g_warning ("Failed to export interface: %s", error->message);
      g_error_free (error);
      return;
    }

  g_signal_connect (skeleton, "handle-screencast",
                    G_CALLBACK (handle_screencast),
                    self);

  g_signal_connect (skeleton, "handle-screencast-area",
                    G_CALLBACK (handle_screencast_area),
                    self);

  g_signal_connect (skeleton, "handle-stop-screencast",
                    G_CALLBACK (handle_stop_screencast),
                    self);
}

static void
name_acquired_cb (GDBusConnection *connection,
                  const char      *name,
                  gpointer         user_data)
{
}

static void
name_lost_cb (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
}

static void
gf_screencast_dispose (GObject *object)
{
  GfScreencast *self;

  self = GF_SCREENCAST (object);

  if (self->recordings != NULL)
    {
      GHashTableIter iter;
      gpointer value;

      g_hash_table_iter_init (&iter, self->recordings);

      while (g_hash_table_iter_next (&iter, NULL, &value))
        recording_stop_sync (value);

      g_clear_pointer (&self->recordings, g_hash_table_destroy);
    }

  if (self->bus_name_id != 0)
    {
      g_bus_unown_name (self->bus_name_id);
      self->bus_name_id = 0;
    }

  if (self->screencast_gen)
    {
      GDBusInterfaceSkeleton *skeleton;

      skeleton = G_DBUS_INTERFACE_SKELETON (self->screencast_gen);

      g_dbus_interface_skeleton_unexport (skeleton);
      g_clear_object (&self->screencast_gen);
    }

  g_clear_object (&self->lockdown);

  G_OBJECT_CLASS (gf_screencast_parent_class)->dispose (object);
}

static void
gf_screencast_class_init (GfScreencastClass *self_class)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (self_class);

  object_class->dispose = gf_screencast_dispose;
}

static void
gf_screencast_init (GfScreencast *self)
{
  self->screencast_gen = gf_screencast_gen_skeleton_new ();
  self->lockdown = g_settings_new ("org.gnome.desktop.lockdown");

  self->recordings = g_hash_table_new_full (g_str_hash,
                                            g_str_equal,
                                            NULL,
                                            recording_free);

  self->bus_name_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                      "org.gnome.Shell.Screencast",
                                      G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
                                      G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                      bus_acquired_cb,
                                      name_acquired_cb,
                                      name_lost_cb,
                                      self,
                                      NULL);
}

GfScreencast *
gf_screencast_new (void)
{
  return g_object_new (GF_TYPE_SCREENCAST, NULL);
}
