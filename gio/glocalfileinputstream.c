#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include "gioerror.h"
#include "glocalfileinputstream.h"
#include "glocalfileinfo.h"


G_DEFINE_TYPE (GLocalFileInputStream, g_local_file_input_stream, G_TYPE_FILE_INPUT_STREAM);

struct _GLocalFileInputStreamPrivate {
  int fd;
};

static gssize     g_local_file_input_stream_read          (GInputStream           *stream,
							   void                   *buffer,
							   gsize                   count,
							   GCancellable           *cancellable,
							   GError                **error);
static gssize     g_local_file_input_stream_skip          (GInputStream           *stream,
							   gsize                   count,
							   GCancellable           *cancellable,
							   GError                **error);
static gboolean   g_local_file_input_stream_close         (GInputStream           *stream,
							   GCancellable           *cancellable,
							   GError                **error);
static GFileInfo *g_local_file_input_stream_get_file_info (GFileInputStream       *stream,
							   char                   *attributes,
							   GCancellable           *cancellable,
							   GError                **error);

static void
g_local_file_input_stream_finalize (GObject *object)
{
  GLocalFileInputStream *file;
  
  file = G_LOCAL_FILE_INPUT_STREAM (object);
  
  if (G_OBJECT_CLASS (g_local_file_input_stream_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_local_file_input_stream_parent_class)->finalize) (object);
}

static void
g_local_file_input_stream_class_init (GLocalFileInputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);
  GFileInputStreamClass *file_stream_class = G_FILE_INPUT_STREAM_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GLocalFileInputStreamPrivate));
  
  gobject_class->finalize = g_local_file_input_stream_finalize;

  stream_class->read = g_local_file_input_stream_read;
  stream_class->skip = g_local_file_input_stream_skip;
  stream_class->close = g_local_file_input_stream_close;
  file_stream_class->get_file_info = g_local_file_input_stream_get_file_info;
}

static void
g_local_file_input_stream_init (GLocalFileInputStream *info)
{
  info->priv = G_TYPE_INSTANCE_GET_PRIVATE (info,
					    G_TYPE_LOCAL_FILE_INPUT_STREAM,
					    GLocalFileInputStreamPrivate);
}

GFileInputStream *
g_local_file_input_stream_new (int fd)
{
  GLocalFileInputStream *stream;

  stream = g_object_new (G_TYPE_LOCAL_FILE_INPUT_STREAM, NULL);
  stream->priv->fd = fd;
  
  return G_FILE_INPUT_STREAM (stream);
}

static gssize
g_local_file_input_stream_read (GInputStream *stream,
				void         *buffer,
				gsize         count,
				GCancellable *cancellable,
				GError      **error)
{
  GLocalFileInputStream *file;
  gssize res;

  file = G_LOCAL_FILE_INPUT_STREAM (stream);

  res = -1;
  while (1)
    {
      if (g_cancellable_is_cancelled (cancellable))
	{
	  g_set_error (error,
		       G_IO_ERROR,
		       G_IO_ERROR_CANCELLED,
		       _("Operation was cancelled"));
	  break;
	}
      res = read (file->priv->fd, buffer, count);
      if (res == -1)
	{
	  if (errno == EINTR)
	    continue;
	  
	  g_set_error (error, G_IO_ERROR,
		       g_io_error_from_errno (errno),
		       _("Error reading from file: %s"),
		       g_strerror (errno));
	}
      
      break;
    }
  
  return res;
}

static gssize
g_local_file_input_stream_skip (GInputStream *stream,
				gsize         count,
				GCancellable *cancellable,
				GError      **error)
{
  off_t res, start;
  GLocalFileInputStream *file;

  file = G_LOCAL_FILE_INPUT_STREAM (stream);
  
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return -1;
    }
  
  start = lseek (file->priv->fd, 0, SEEK_CUR);
  if (start == -1)
    {
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error seeking in file: %s"),
		   g_strerror (errno));
      return -1;
    }
  
  res = lseek (file->priv->fd, count, SEEK_CUR);
  if (res == -1)
    {
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error seeking in file: %s"),
		   g_strerror (errno));
      return -1;
    }

  return res - start;
}

static gboolean
g_local_file_input_stream_close (GInputStream *stream,
				 GCancellable *cancellable,
				 GError      **error)
{
  GLocalFileInputStream *file;
  int res;

  file = G_LOCAL_FILE_INPUT_STREAM (stream);

  if (file->priv->fd == -1)
    return TRUE;

  while (1)
    {
      res = close (file->priv->fd);
      if (res == -1)
	{
	  g_set_error (error, G_IO_ERROR,
		       g_io_error_from_errno (errno),
		       _("Error closing file: %s"),
		       g_strerror (errno));
	}
      break;
    }

  return res != -1;
}

static GFileInfo *
g_local_file_input_stream_get_file_info (GFileInputStream     *stream,
					 char                 *attributes,
					 GCancellable         *cancellable,
					 GError              **error)
{
  GLocalFileInputStream *file;

  file = G_LOCAL_FILE_INPUT_STREAM (stream);

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
  
  return g_local_file_info_get_from_fd (file->priv->fd,
					attributes,
					error);
}