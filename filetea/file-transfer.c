#include "file-transfer.h"

#define BLOCK_SIZE 0x0FFF

static void     file_transfer_on_target_closed (EvdHttpConnection *conn,
                                                gpointer           user_data);
static void     file_transfer_on_source_closed (EvdHttpConnection *conn,
                                                gpointer           user_data);

static void     file_transfer_read             (FileTransfer *self);
static void     file_transfer_flush_target     (FileTransfer *self);
static void     file_transfer_complete         (FileTransfer *self);

FileTransfer *
file_transfer_new (FileSource          *source,
                   EvdHttpConnection   *target_conn,
                   gboolean             download,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  FileTransfer *self;
  gchar *st;
  gchar *uuid;

  g_return_val_if_fail (EVD_IS_HTTP_CONNECTION (target_conn), NULL);
  g_return_val_if_fail (source != NULL, NULL);

  self = g_slice_new0 (FileTransfer);
  self->ref_count = 1;
  self->report_interval = 1;

  self->download = download;

  self->result = g_simple_async_result_new (NULL,
                                            callback,
                                            self,
                                            file_transfer_new);

  self->target_conn = target_conn;
  g_object_ref (target_conn);
  g_signal_connect (target_conn,
                    "close",
                    G_CALLBACK (file_transfer_on_target_closed),
                    self);

  self->source = file_source_ref (source);

  uuid = evd_uuid_new ();
  st = g_strdup_printf ("%s%s",
                        source->file_name,
                        uuid);
  g_free (uuid);

  self->id = g_compute_checksum_for_string (G_CHECKSUM_SHA1, st, -1);
  g_free (st);

  return self;
}

static void
file_transfer_free (FileTransfer *self)
{
  g_return_if_fail (self != NULL);

  g_free (self->id);

  g_object_unref (self->target_conn);

  if (self->source_conn != NULL)
    g_object_unref (self->source_conn);

  if (self->buf != NULL)
    g_slice_free1 (BLOCK_SIZE, self->buf);

  file_source_unref (self->source);

  g_slice_free (FileTransfer, self);
}

FileTransfer *
file_transfer_ref (FileTransfer *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  g_return_val_if_fail (self->ref_count > 0, NULL);

  g_atomic_int_exchange_and_add (&self->ref_count, 1);

  return self;
}

void
file_transfer_unref (FileTransfer *self)
{
  gint old_ref;

  g_return_if_fail (self != NULL);
  g_return_if_fail (self->ref_count > 0);

  old_ref = g_atomic_int_get (&self->ref_count);
  if (old_ref > 1)
    g_atomic_int_compare_and_exchange (&self->ref_count, old_ref, old_ref - 1);
  else
    file_transfer_free (self);
}

static void
file_transfer_on_target_closed (EvdHttpConnection *conn, gpointer user_data)
{
  FileTransfer *self = user_data;

  g_simple_async_result_set_error (self->result,
                                   G_IO_ERROR,
                                   G_IO_ERROR_CLOSED,
                                   "Target connection dropped");

  if (self->source_conn != NULL)
    g_signal_handlers_disconnect_by_func (self->source_conn,
                                          file_transfer_on_source_closed,
                                          self);

  file_transfer_complete (self);
}

static void
file_transfer_on_source_closed (EvdHttpConnection *conn, gpointer user_data)
{
  FileTransfer *self = user_data;

  g_simple_async_result_set_error (self->result,
                                   G_IO_ERROR,
                                   G_IO_ERROR_CLOSED,
                                   "Source connection dropped");

  g_signal_handlers_disconnect_by_func (self->target_conn,
                                        file_transfer_on_target_closed,
                                        self);

  file_transfer_complete (self);
}

void
file_transfer_set_source_conn (FileTransfer      *self,
                               EvdHttpConnection *conn)
{
  self->source_conn = conn;
  g_object_ref (conn);

  g_signal_connect (self->source_conn,
                    "close",
                    G_CALLBACK (file_transfer_on_source_closed),
                    self);
}

static void
file_transfer_on_target_can_write (EvdConnection *target_conn, gpointer user_data)
{
  FileTransfer *self = user_data;

  evd_connection_unlock_close (EVD_CONNECTION (self->source_conn));
  file_transfer_read (self);
}

void
file_transfer_start (FileTransfer *self)
{
  SoupMessageHeaders *headers;
  GError *error = NULL;

  g_return_if_fail (self->source_conn != NULL);

  headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_RESPONSE);

  if (self->download)
    {
      gchar *st;

      st = g_strdup_printf ("attachment; filename=%s", self->source->file_name);
      soup_message_headers_replace (headers, "Content-disposition", st);
      g_free (st);
    }

  soup_message_headers_set_content_length (headers, self->source->file_size);
  soup_message_headers_set_content_type (headers, self->source->file_type, NULL);

  if (! evd_http_connection_write_response_headers (self->target_conn,
                                                    SOUP_HTTP_1_1,
                                                    SOUP_STATUS_OK,
                                                    NULL,
                                                    headers,
                                                    &error))
    {
      g_debug ("error sending transfer headers: %s", error->message);
      g_error_free (error);
    }
  else
    {
      if (self->buf == NULL)
        self->buf = g_slice_alloc (BLOCK_SIZE);

      g_signal_connect (self->target_conn,
                        "write",
                        G_CALLBACK (file_transfer_on_target_can_write),
                        self);

      file_transfer_read (self);
    }

  soup_message_headers_free (headers);
}

static void
file_transfer_on_read (GObject      *obj,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  FileTransfer *self = user_data;
  gssize size;
  GError *error = NULL;
  time_t current_time;

  size = g_input_stream_read_finish (G_INPUT_STREAM (obj), res, &error);
  if (size < 0)
    {
      g_debug ("ERROR reading from source: %s", error->message);
      g_error_free (error);
      return;
    }

  if (! evd_http_connection_write_content (self->target_conn,
                                           self->buf,
                                           size,
                                           &error))
    {
      g_debug ("ERROR writing to target: %s", error->message);
      g_error_free (error);
      return;
    }

  self->transferred += size;

  /* report bandwidth to source */
  if (self->report_cb != NULL)
    {
      current_time = time (NULL);
      if (current_time - self->report_last_time >= self->report_interval)
        {
          EvdStreamThrottle *throttle;
          gdouble completed;
          gdouble bandwidth;

          self->report_last_time = current_time;

          completed = ((gdouble) self->transferred / (gdouble) self->source->file_size) * 100;

          throttle = evd_connection_get_input_throttle (EVD_CONNECTION (self->source_conn));
          bandwidth = evd_stream_throttle_get_actual_bandwidth (throttle);

          self->report_cb (self, completed, bandwidth, self->report_cb_user_data);
        }
    }

  g_assert (self->transferred <= self->source->file_size);
  if (self->transferred == self->source->file_size)
    {
      /* finished reading, send HTTP response to source */
      g_signal_handlers_disconnect_by_func (self->source_conn,
                                            file_transfer_on_source_closed,
                                            self);

      if (! evd_http_connection_respond (self->source_conn,
                                         SOUP_HTTP_1_1,
                                         SOUP_STATUS_OK,
                                         NULL,
                                         NULL,
                                         NULL,
                                         0,
                                         TRUE,
                                         &error))
        {
          g_debug ("ERROR sending response to source: %s", error->message);
          g_error_free (error);
        }

      file_transfer_flush_target (self);
    }
  else
    {
      file_transfer_read (self);
    }

  file_transfer_unref (self);
}

static void
file_transfer_read (FileTransfer *self)
{
  if (evd_connection_get_max_writable (EVD_CONNECTION (self->target_conn)) > 0)
    {
      GInputStream *stream;
      gssize size;

      size = MIN (BLOCK_SIZE, self->source->file_size - self->transferred);
      if (size <= 0)
        return;

      stream = g_io_stream_get_input_stream (G_IO_STREAM (self->source_conn));

      file_transfer_ref (self);
      g_input_stream_read_async (stream,
                                 self->buf,
                                 (gsize) size,
                                 G_PRIORITY_DEFAULT,
                                 NULL,
                                 file_transfer_on_read,
                                 self);
    }
  else
    {
      evd_connection_lock_close (EVD_CONNECTION (self->source_conn));
    }
}

static void
file_transfer_on_target_flushed (GObject      *obj,
                                 GAsyncResult *res,
                                 gpointer      user_data)
{
  FileTransfer *self = user_data;
  GError *error = NULL;

  if (! g_output_stream_flush_finish (G_OUTPUT_STREAM (obj), res, &error))
    {
      g_debug ("ERROR flushing target: %s", error->message);
      g_error_free (error);
    }

  g_signal_handlers_disconnect_by_func (self->target_conn,
                                        file_transfer_on_target_can_write,
                                        self);

  file_transfer_complete (self);

  file_transfer_unref (self);
}

static void
file_transfer_flush_target (FileTransfer *self)
{
  GOutputStream *stream;

  stream = g_io_stream_get_output_stream (G_IO_STREAM (self->target_conn));

  g_signal_handlers_disconnect_by_func (self->target_conn,
                                        file_transfer_on_target_closed,
                                        self);

  file_transfer_ref (self);
  g_output_stream_flush_async (stream,
                               G_PRIORITY_DEFAULT,
                               NULL,
                               file_transfer_on_target_flushed,
                               self);
}

static void
file_transfer_complete (FileTransfer *self)
{
  if (! g_io_stream_is_closed (G_IO_STREAM (self->target_conn)))
    g_io_stream_close (G_IO_STREAM (self->target_conn), NULL, NULL);

  if (self->source_conn != NULL)
    {
      if (! g_io_stream_is_closed (G_IO_STREAM (self->source_conn)))
        g_io_stream_close (G_IO_STREAM (self->source_conn), NULL, NULL);
    }

  /* notify transfer completed */
  file_transfer_ref (self);

  g_simple_async_result_complete (self->result);
  g_object_unref (self->result);
  self->result = NULL;

  file_transfer_unref (self);
}

gboolean
file_transfer_finish (FileTransfer  *self,
                      GAsyncResult  *result,
                      GError       **error)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (g_simple_async_result_is_valid (result,
                                                        NULL,
                                                        file_transfer_new),
                        FALSE);

  return ! g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result),
                                                  error);
}