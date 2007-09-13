#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsjobmove.h"
#include "gdbusutils.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsjobqueryattributes.h"

G_DEFINE_TYPE (GVfsJobQueryAttributes, g_vfs_job_query_attributes, G_VFS_TYPE_JOB_DBUS);

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static DBusMessage *create_reply (GVfsJob        *job,
				  DBusConnection *connection,
				  DBusMessage    *message);

static void
g_vfs_job_query_attributes_finalize (GObject *object)
{
  GVfsJobQueryAttributes *job;

  job = G_VFS_JOB_QUERY_ATTRIBUTES (object);

  g_free (job->filename);
  if (job->list)
    g_file_attribute_info_list_free (job->list);
  
  if (G_OBJECT_CLASS (g_vfs_job_query_attributes_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_query_attributes_parent_class)->finalize) (object);
}

static void
g_vfs_job_query_attributes_class_init (GVfsJobQueryAttributesClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_query_attributes_finalize;
  job_class->run = run;
  job_class->try = try;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_query_attributes_init (GVfsJobQueryAttributes *job)
{
}

GVfsJob *
g_vfs_job_query_attributes_new (DBusConnection *connection,
				DBusMessage *message,
				GVfsBackend *backend,
				gboolean namespaces)
{
  GVfsJobQueryAttributes *job;
  DBusMessage *reply;
  DBusError derror;
  const gchar *path = NULL;
  gint path_len;
  
  dbus_error_init (&derror);
  if (!dbus_message_get_args (message, &derror, 
			      DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			      &path, &path_len,
			      0))
    {
      reply = dbus_message_new_error (message,
				      derror.name,
                                      derror.message);
      dbus_error_free (&derror);

      dbus_connection_send (connection, reply, NULL);
      return NULL;
    }

  job = g_object_new (G_VFS_TYPE_JOB_QUERY_ATTRIBUTES,
		      "message", message,
		      "connection", connection,
		      NULL);

  job->backend = backend;
  job->filename = g_strndup (path, path_len);
  job->namespaces = namespaces;

  return G_VFS_JOB (job);
}

static void
run (GVfsJob *job)
{
  GVfsJobQueryAttributes *op_job = G_VFS_JOB_QUERY_ATTRIBUTES (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  void (*cb) (GVfsBackend *backend,
	      GVfsJobQueryAttributes *job,
	      const char *filename);

  if (op_job->namespaces)
    cb = class->query_writable_namespaces;
  else
    cb = class->query_settable_attributes;

  if (cb == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported by backend"));
      return;
    }
      
  cb (op_job->backend,
      op_job,
      op_job->filename);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobQueryAttributes *op_job = G_VFS_JOB_QUERY_ATTRIBUTES (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  gboolean (*cb) (GVfsBackend *backend,
		  GVfsJobQueryAttributes *job,
		  const char *filename);

  if (op_job->namespaces)
    cb = class->try_query_writable_namespaces;
  else
    cb = class->try_query_settable_attributes;

  if (cb == NULL)
    return FALSE;
      
  return cb (op_job->backend,
	     op_job,
	     op_job->filename);
}

void
g_vfs_job_query_attributes_set_list (GVfsJobQueryAttributes *job,
				     GFileAttributeInfoList *list)
{
  job->list = g_file_attribute_info_list_dup (list);
}

/* Might be called on an i/o thread */
static DBusMessage *
create_reply (GVfsJob *job,
	      DBusConnection *connection,
	      DBusMessage *message)
{
  GVfsJobQueryAttributes *op_job = G_VFS_JOB_QUERY_ATTRIBUTES (job);
  DBusMessage *reply;
  DBusMessageIter iter;

  reply = dbus_message_new_method_return (message);

  dbus_message_iter_init_append (reply, &iter);
  _g_dbus_append_attribute_info_list (&iter, 
				      op_job->list);
  
  return reply;
}