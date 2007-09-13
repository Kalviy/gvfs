#ifndef __G_DAEMON_VFS_H__
#define __G_DAEMON_VFS_H__

#include <gio/gvfs.h>
#include <dbus/dbus.h>
#include "gmountspec.h"
#include "gvfsuriutils.h"

G_BEGIN_DECLS

typedef struct _GDaemonVfs       GDaemonVfs;
typedef struct _GDaemonVfsClass  GDaemonVfsClass;

typedef struct {
  volatile int ref_count;
  char *dbus_id;
  char *object_path;
  GMountSpec *spec;
  char *prefered_filename_encoding; /* NULL -> UTF8 */
} GMountRef;

typedef void (*GMountRefLookupCallback) (GMountRef *mount_ref,
					 gpointer data,
					 GError *error);

GType   g_daemon_vfs_get_type  (GTypeModule *module);

GDaemonVfs *g_daemon_vfs_new (void);

GDecodedUri *_g_daemon_vfs_get_uri_for_mountspec (GMountSpec               *spec,
						  char                     *path);
void         _g_daemon_vfs_get_mount_ref_async   (GMountSpec               *spec,
						  const char               *path,
						  GMountRefLookupCallback   callback,
						  gpointer                  user_data);
GMountRef  * _g_daemon_vfs_get_mount_ref_sync    (GMountSpec               *spec,
						  const char               *path,
						  GError                  **error);
const char * _g_mount_ref_resolve_path           (GMountRef                *ref,
						  const char               *path);
void         _g_mount_ref_unref                  (GMountRef                *ref);


G_END_DECLS

#endif /* __G_DAEMON_VFS_H__ */
