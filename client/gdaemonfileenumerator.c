#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gdaemonfileenumerator.h>
#include <gio/gfileinfo.h>
#include <gvfsdaemondbus.h>
#include <gvfsdaemonprotocol.h>

#define OBJ_PATH_PREFIX "/org/gtk/vfs/client/enumerator/"

/* atomic */
volatile gint path_counter = 1;

G_LOCK_DEFINE_STATIC(infos);

struct _GDaemonFileEnumerator
{
  GFileEnumerator parent;

  gint id;
  DBusConnection *sync_connection;

  /* protected by infos lock */
  GList *infos;
  gboolean done;
  
};

G_DEFINE_TYPE (GDaemonFileEnumerator, g_daemon_file_enumerator, G_TYPE_FILE_ENUMERATOR);

static GFileInfo *       g_daemon_file_enumerator_next_file   (GFileEnumerator  *enumerator,
							       GCancellable     *cancellable,
							       GError          **error);
static gboolean          g_daemon_file_enumerator_stop        (GFileEnumerator  *enumerator,
							       GCancellable     *cancellable,
							       GError          **error);
static DBusHandlerResult g_daemon_file_enumerator_dbus_filter (DBusConnection   *connection,
							       DBusMessage      *message,
							       void             *user_data);

static void
g_daemon_file_enumerator_finalize (GObject *object)
{
  GDaemonFileEnumerator *daemon;
  char *path;

  daemon = G_DAEMON_FILE_ENUMERATOR (object);

  path = g_daemon_file_enumerator_get_object_path (daemon);
  _g_dbus_unregister_vfs_filter (path);
  g_free (path);

  g_list_foreach (daemon->infos, (GFunc)g_object_unref, NULL);
  g_list_free (daemon->infos);

  if (daemon->sync_connection)
    dbus_connection_unref (daemon->sync_connection);
  
  if (G_OBJECT_CLASS (g_daemon_file_enumerator_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_file_enumerator_parent_class)->finalize) (object);
}


static void
g_daemon_file_enumerator_class_init (GDaemonFileEnumeratorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS (klass);
  
  gobject_class->finalize = g_daemon_file_enumerator_finalize;

  enumerator_class->next_file = g_daemon_file_enumerator_next_file;
  enumerator_class->stop = g_daemon_file_enumerator_stop;
}

static void
g_daemon_file_enumerator_init (GDaemonFileEnumerator *daemon)
{
  char *path;
  
  daemon->id = g_atomic_int_exchange_and_add (&path_counter, 1);

  path = g_daemon_file_enumerator_get_object_path (daemon);
  _g_dbus_register_vfs_filter (path, g_daemon_file_enumerator_dbus_filter,
			       G_OBJECT (daemon));
  g_free (path);
}

GDaemonFileEnumerator *
g_daemon_file_enumerator_new (void)
{
  GDaemonFileEnumerator *daemon;

  daemon = g_object_new (G_TYPE_DAEMON_FILE_ENUMERATOR, NULL);
  
  return daemon;
}

static DBusHandlerResult
g_daemon_file_enumerator_dbus_filter (DBusConnection     *connection,
				      DBusMessage        *message,
				      void               *user_data)
{
  GDaemonFileEnumerator *enumerator = user_data;
  const char *member;
  DBusMessageIter iter, array_iter;
  GList *infos;
  GFileInfo *info;
  
  member = dbus_message_get_member (message);

  if (strcmp (member, G_VFS_DBUS_ENUMERATOR_DONE) == 0)
    {
      G_LOCK (infos);
      enumerator->done = TRUE;
      G_UNLOCK (infos);
    }
  else if (strcmp (member, G_VFS_DBUS_ENUMERATOR_GOT_INFO) == 0)
    {
      infos = NULL;
      
      dbus_message_iter_init (message, &iter);
      if (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_ARRAY &&
	  dbus_message_iter_get_element_type (&iter) == DBUS_TYPE_STRUCT)
	{
	  dbus_message_iter_recurse (&iter, &array_iter);

	  while (dbus_message_iter_get_arg_type (&array_iter) == DBUS_TYPE_STRUCT)
	    {
	      info = _g_dbus_get_file_info (&array_iter, NULL);
	      if (info)
		g_assert (G_IS_FILE_INFO (info));

	      if (info)
		infos = g_list_prepend (infos, info);

	      dbus_message_iter_next (&iter);
	    }
	}

      infos = g_list_reverse (infos);
      
      G_LOCK (infos);
      enumerator->infos = g_list_concat (enumerator->infos, infos);
      G_UNLOCK (infos);
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

char  *
g_daemon_file_enumerator_get_object_path (GDaemonFileEnumerator *enumerator)
{
  return g_strdup_printf (OBJ_PATH_PREFIX"%d", enumerator->id);
}

void
g_daemon_file_enumerator_set_sync_connection (GDaemonFileEnumerator *enumerator,
					      DBusConnection        *connection)
{
  enumerator->sync_connection = dbus_connection_ref (connection);
}

static GFileInfo *
g_daemon_file_enumerator_next_file (GFileEnumerator *enumerator,
				    GCancellable     *cancellable,
				    GError **error)
{
  GDaemonFileEnumerator *daemon = G_DAEMON_FILE_ENUMERATOR (enumerator);
  GFileInfo *info;
  gboolean done;
  
  info = NULL;
  done = FALSE;
  while (1)
    {
      G_LOCK (infos);
      if (daemon->infos)
	{
	  done = TRUE;
	  info = daemon->infos->data;
	  if (info)
	    g_assert (G_IS_FILE_INFO (info));
	  daemon->infos = g_list_delete_link (daemon->infos, daemon->infos);
	}
      else if (daemon->done)
	done = TRUE;
      G_UNLOCK (infos);

      if (info)
	g_assert (G_IS_FILE_INFO (info));
      
      if (done)
	break;
  
      if (!dbus_connection_read_write_dispatch (daemon->sync_connection, -1))
	  break;
    }

  return info;
}

static gboolean
g_daemon_file_enumerator_stop (GFileEnumerator *enumerator,
			      GCancellable     *cancellable,
			      GError          **error)
{
  /*GDaemonFileEnumerator *daemon = G_DAEMON_FILE_ENUMERATOR (enumerator); */

  return TRUE;
}

