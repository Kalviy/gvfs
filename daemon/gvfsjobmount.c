/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsjobmount.h"
#include "gdbusutils.h"
#include "gvfsdaemonprotocol.h"

G_DEFINE_TYPE (GVfsJobMount, g_vfs_job_mount, G_VFS_TYPE_JOB)

static void     run        (GVfsJob *job);
static gboolean try        (GVfsJob *job);
static void     send_reply (GVfsJob *job);

static void
g_vfs_job_mount_finalize (GObject *object)
{
  GVfsJobMount *job;

  job = G_VFS_JOB_MOUNT (object);

  g_mount_spec_unref (job->mount_spec);
  g_object_unref (job->mount_source);
  g_object_unref (job->backend);
  
  if (G_OBJECT_CLASS (g_vfs_job_mount_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_mount_parent_class)->finalize) (object);
}

static void
g_vfs_job_mount_class_init (GVfsJobMountClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_mount_finalize;
  job_class->run = run;
  job_class->try = try;
  job_class->send_reply = send_reply;
}

static void
g_vfs_job_mount_init (GVfsJobMount *job)
{
}

GVfsJob *
g_vfs_job_mount_new (GMountSpec *spec,
		     GMountSource *source,
		     gboolean is_automount,
		     DBusMessage *request,
		     GVfsBackend *backend)
{
  GVfsJobMount *job;

  job = g_object_new (G_VFS_TYPE_JOB_MOUNT,
		      NULL);

  job->mount_spec = g_mount_spec_ref (spec);
  job->mount_source = g_object_ref (source);
  job->is_automount = is_automount;
  /* Ref the backend so we're sure its alive
     during the whole job request. */
  job->backend = g_object_ref (backend);
  if (request)
    job->request = dbus_message_ref (request);
  
  return G_VFS_JOB (job);
}

static void
run (GVfsJob *job)
{
  GVfsJobMount *op_job = G_VFS_JOB_MOUNT (job);
  GVfsBackend *backend = op_job->backend;
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (backend);

  if (class->mount == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported by backend"));
      return;
    }
  
  class->mount (backend,
		op_job,
		op_job->mount_spec,
		op_job->mount_source,
		op_job->is_automount);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobMount *op_job = G_VFS_JOB_MOUNT (job);
  GVfsBackend *backend = op_job->backend;
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (backend);
  gboolean result;

  if (class->try_mount == NULL)
    return FALSE;

  result = class->try_mount (backend,
			     op_job,
			     op_job->mount_spec,
			     op_job->mount_source,
			     op_job->is_automount);
  return result;
}

static void
mount_failed (GVfsJobMount *op_job, GError *error)
{
  DBusConnection *conn;
  DBusMessage *reply;
  GVfsBackend *backend;

  if (op_job->request)
    {
      reply = _dbus_message_new_from_gerror (op_job->request, error);
      
      /* Queues reply (threadsafely), actually sends it in mainloop */
      conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);
      if (conn)
	{
	  dbus_connection_send (conn, reply, NULL);
	  dbus_message_unref (reply);
	  dbus_connection_unref (conn);
	}
    }
  else
    g_debug ("Mount failed: %s\n", error->message);

  backend = g_object_ref (op_job->backend);
  g_vfs_job_emit_finished (G_VFS_JOB (op_job));
  
  /* Remove failed backend from daemon */
  g_vfs_job_source_closed  (G_VFS_JOB_SOURCE (backend));
  g_object_unref (backend);
}

static void
register_mount_callback (DBusMessage *mount_reply,
			 GError *error,
			 gpointer user_data)
{
  GVfsJobMount *op_job = G_VFS_JOB_MOUNT (user_data);
  DBusConnection *conn;
  DBusMessage *reply;

  g_debug ("register_mount_callback, mount_reply: %p, error: %p\n", mount_reply, error);
  
  if (mount_reply == NULL)
    mount_failed (op_job, error);
  else
    {
      if (op_job->request)
	{
	  reply = dbus_message_new_method_return (op_job->request);
	  /* Queues reply (threadsafely), actually sends it in mainloop */
	  conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);
	  if (conn)
	    {
	      dbus_connection_send (conn, reply, NULL);
	      dbus_message_unref (reply);
	      dbus_connection_unref (conn);
	    }
	}

      g_vfs_job_emit_finished (G_VFS_JOB (op_job));
    }
}

/* Might be called on an i/o thread */
static void
send_reply (GVfsJob *job)
{
  GVfsJobMount *op_job = G_VFS_JOB_MOUNT (job);

  g_debug ("send_reply, failed: %d\n", job->failed);
  
  if (job->failed)
    mount_failed (op_job, job->error);
  else
    g_vfs_backend_register_mount (op_job->backend,
				  register_mount_callback,
				  job);
}
