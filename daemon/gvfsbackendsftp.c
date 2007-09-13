/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gioerror.h>
#include <gio/gfile.h>
#include <gio/gdatainputstream.h>
#include <gio/gdataoutputstream.h>
#include <gio/gsocketinputstream.h>
#include <gio/gsocketoutputstream.h>
#include <gio/gmemoryoutputstream.h>

#include "gvfsbackendsftp.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobgetinfo.h"
#include "gvfsjobgetfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"
#include "sftp.h"
#include "pty_open.h"

typedef enum {
  SFTP_VENDOR_INVALID = 0,
  SFTP_VENDOR_OPENSSH,
  SFTP_VENDOR_SSH
} SFTPClientVendor;

struct _GVfsBackendSftp
{
  GVfsBackend parent_instance;

  SFTPClientVendor client_vendor;
  char *host;
  gboolean user_specified;
  char *user;

  GOutputStream *command_stream;
  GInputStream *reply_stream;
  
  GMountSource *mount_source; /* Only used/set during mount */
  int mount_try;
  gboolean mount_try_again;
};


G_DEFINE_TYPE (GVfsBackendSftp, g_vfs_backend_sftp, G_VFS_TYPE_BACKEND);

static SFTPClientVendor
get_sftp_client_vendor (void)
{
  char *ssh_stderr;
  char *args[3];
  gint ssh_exitcode;
  SFTPClientVendor res = SFTP_VENDOR_INVALID;
  
  args[0] = g_strdup (SSH_PROGRAM);
  args[1] = g_strdup ("-V");
  args[2] = NULL;
  if (g_spawn_sync (NULL, args, NULL,
		    G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
		    NULL, NULL,
		    NULL, &ssh_stderr,
		    &ssh_exitcode, NULL))
    {
      if (ssh_stderr == NULL)
	res = SFTP_VENDOR_INVALID;
      else if ((strstr (ssh_stderr, "OpenSSH") != NULL) ||
	       (strstr (ssh_stderr, "Sun_SSH") != NULL))
	res = SFTP_VENDOR_OPENSSH;
      else if (strstr (ssh_stderr, "SSH Secure Shell") != NULL)
	res = SFTP_VENDOR_SSH;
      else
	res = SFTP_VENDOR_INVALID;
    }
  
  g_free (ssh_stderr);
  g_free (args[0]);
  g_free (args[1]);
  
  return res;
}

static void
g_vfs_backend_sftp_finalize (GObject *object)
{
  GVfsBackendSftp *backend;

  backend = G_VFS_BACKEND_SFTP (object);

  if (G_OBJECT_CLASS (g_vfs_backend_sftp_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_sftp_parent_class)->finalize) (object);
}

static void
g_vfs_backend_sftp_init (GVfsBackendSftp *backend)
{
}

#ifdef HAVE_GRANTPT
/* We only use this on systems with unix98 ptys */
#define USE_PTY 1
#endif

static char **
setup_ssh_commandline (GVfsBackend *backend)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  guint last_arg;
  gchar **args;

  args = g_new0 (gchar *, 20); /* 20 is enought for now, bump size if code below changes */

  /* Fill in the first few args */
  last_arg = 0;
  args[last_arg++] = g_strdup (SSH_PROGRAM);

  if (op_backend->client_vendor == SFTP_VENDOR_OPENSSH)
    {
      args[last_arg++] = g_strdup ("-oForwardX11 no");
      args[last_arg++] = g_strdup ("-oForwardAgent no");
      args[last_arg++] = g_strdup ("-oClearAllForwardings yes");
      args[last_arg++] = g_strdup ("-oProtocol 2");
      args[last_arg++] = g_strdup ("-oNoHostAuthenticationForLocalhost yes");
#ifndef USE_PTY
      args[last_arg++] = g_strdup ("-oBatchMode yes");
#endif
    
    }
  else if (op_backend->client_vendor == SFTP_VENDOR_SSH)
    args[last_arg++] = g_strdup ("-x");

  /* TODO: Support port 
  if (port != 0)
    {
      args[last_arg++] = g_strdup ("-p");
      args[last_arg++] = g_strdup_printf ("%d", port);
    }
  */
    

  args[last_arg++] = g_strdup ("-l");
  args[last_arg++] = g_strdup (op_backend->user);

  args[last_arg++] = g_strdup ("-s");

  if (op_backend->client_vendor == SFTP_VENDOR_SSH)
    {
      args[last_arg++] = g_strdup ("sftp");
      args[last_arg++] = g_strdup (op_backend->host);
    }
  else
    {
      args[last_arg++] = g_strdup (op_backend->host);
      args[last_arg++] = g_strdup ("sftp");
    }

  args[last_arg++] = NULL;

  return args;
}

static gboolean
spawn_ssh (GVfsBackend *backend,
	   char *args[],
	   pid_t *pid,
	   int *tty_fd,
	   int *stdin_fd,
	   int *stdout_fd,
	   int *stderr_fd,
	   GError **error)
{
#ifdef USE_PTY
  *tty_fd = pty_open(pid, PTY_REAP_CHILD, NULL,
		     args[0], args, NULL,
		     300, 300, 
		     stdin_fd, stdout_fd, stderr_fd);
  if (*tty_fd == -1)
    {
      g_set_error (error,
		   G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Unable to spawn ssh program"));
      return FALSE;
    }
#else
  GError *my_error;
  GPid gpid;
  
  *tty_fd = -1;

  my_error = NULL;
  if (!g_spawn_async_with_pipes (NULL, args, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
				 &gpid,
				 stdin_fd, stdout_fd, stderr_fd, &my_error))
    {
      g_set_error (error,
		   G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Unable to spawn ssh program: %s"), my_error->msg);
      g_error_free (my_error);
      return FALSE;
    }
  *pid = gpid;
#endif
  
  return TRUE;
}

static GDataOutputStream *
new_command_stream (void)
{
  GOutputStream *mem_stream;
  GDataOutputStream *data_stream;

  mem_stream = g_memory_output_stream_new (NULL);
  data_stream = g_data_output_stream_new (mem_stream);
  g_object_unref (mem_stream);

  g_data_output_stream_put_int32 (data_stream, 0, NULL, NULL);

  return data_stream;
}

static gboolean
send_command (GOutputStream *out_stream,
	      GDataOutputStream *command_stream,
	      GCancellable *cancellable,
	      GError **error)
{
  GOutputStream *mem_stream;
  GByteArray *array;
  guint32 *len_ptr;
  gsize bytes_written;
  gboolean res;
  
  mem_stream = g_filter_output_stream_get_base_stream (G_FILTER_OUTPUT_STREAM (command_stream));
  array = g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (mem_stream));

  len_ptr = (guint32 *)array->data;
  *len_ptr = GUINT32_TO_BE (array->len - 4);

  res = g_output_stream_write_all (out_stream,
				   array->data, array->len,
				   &bytes_written,
				   cancellable, error);

  if (error == NULL && !res)
    g_warning ("Ignored send_command error\n");
  return res;
}

static gboolean
wait_for_reply (GVfsBackend *backend, int stdout_fd, GError **error)
{
  fd_set ifds;
  struct timeval tv;
  int ret;
  
  FD_ZERO (&ifds);
  FD_SET (stdout_fd, &ifds);
  
  tv.tv_sec = 20;
  tv.tv_usec = 0;
      
  ret = select (stdout_fd+1, &ifds, NULL, NULL, &tv);

  if (ret <= 0)
    {
      g_set_error (error,
		   G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
		   _("Timed out when logging in"));
      return FALSE;
    }
  return TRUE;
}

static GByteArray *
read_reply_sync (GVfsBackend *backend, GError **error)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  guint32 len;
  gsize bytes_read;
  GByteArray *array;
  
  if (!g_input_stream_read_all (op_backend->reply_stream,
				&len, 4,
				&bytes_read, NULL, error))
    return NULL;

  
  len = GUINT32_FROM_BE (len);
  
  array = g_byte_array_sized_new (len);

  if (!g_input_stream_read_all (op_backend->reply_stream,
				array->data, len,
				&bytes_read, NULL, error))
    {
      g_byte_array_free (array, TRUE);
      return NULL;
    }
  
  return array;
}

static gboolean
handle_login (GVfsBackend *backend,
	      GMountSource *mount_source,
	      int tty_fd, int stdout_fd, int stderr_fd,
	      GError **error)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  GInputStream *prompt_stream;
  GOutputStream *reply_stream;
  fd_set ifds;
  struct timeval tv;
  int ret;
  int prompt_fd;
  char buffer[1024];
  gsize len;
  gboolean aborted = FALSE;
  gboolean ret_val;
  char *new_password = NULL;
  gsize bytes_written;
  
  if (op_backend->client_vendor == SFTP_VENDOR_SSH) 
    prompt_fd = stderr_fd;
  else
    prompt_fd = tty_fd;

  prompt_stream = g_socket_input_stream_new (prompt_fd, FALSE);
  reply_stream = g_socket_output_stream_new (tty_fd, FALSE);

  ret_val = TRUE;
  while (1)
    {
      FD_ZERO (&ifds);
      FD_SET (stdout_fd, &ifds);
      FD_SET (prompt_fd, &ifds);
      
      tv.tv_sec = 20;
      tv.tv_usec = 0;
      
      ret = select (MAX (stdout_fd, prompt_fd)+1, &ifds, NULL, NULL, &tv);
  
      if (ret <= 0)
	{
	  g_set_error (error,
		       G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
		       _("Timed out when logging in"));
	  ret_val = FALSE;
	  break;
	}
      
      if (FD_ISSET (stdout_fd, &ifds))
	break; /* Got reply to initial INIT request */
  
      g_assert (FD_ISSET (prompt_fd, &ifds));
  

      len = g_input_stream_read (prompt_stream,
				 buffer, sizeof (buffer) - 1,
				 NULL, error);

      if (len == -1)
	{
	  ret_val = FALSE;
	  break;
	}
      
      buffer[len] = 0;

      /*
       * If the input URI contains a username
       *     if the input URI contains a password, we attempt one login and return GNOME_VFS_ERROR_ACCESS_DENIED on failure.
       *     if the input URI contains no password, we query the user until he provides a correct one, or he cancels.
       *
       * If the input URI contains no username
       *     (a) the user is queried for a user name and a password, with the default login being his
       *     local login name.
       *
       *     (b) if the user decides to change his remote login name, we go to tty_retry because we need a
       *     new SSH session, attempting one login with his provided credentials, and if that fails proceed
       *     with (a), but use his desired remote login name as default.
       *
       * The "password" variable is only used for the very first login attempt,
       * or for the first re-login attempt when the user decided to change his name.
       * Otherwise, we "new_password" and "new_user_name" is used, as output variable
       * for user and keyring input.
       */
      if (g_str_has_suffix (buffer, "password: ") ||
	  g_str_has_suffix (buffer, "Password: ") ||
	  g_str_has_suffix (buffer, "Password:")  ||
	  g_str_has_prefix (buffer, "Enter passphrase for key"))
	{
	  if (!g_mount_source_ask_password (mount_source,
					    g_str_has_prefix (buffer, "Enter passphrase for key") ?
					    _("Enter passphrase for key")
					    :
					    _("Enter password"),
					    op_backend->user,
					    NULL,
					    G_PASSWORD_FLAGS_NEED_PASSWORD,
					    &aborted,
					    &new_password,
					    NULL,
					    NULL) ||
	      aborted)
	    {
	      g_set_error (error,
			   G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			   _("Password dialog cancelled"));
	      ret_val = FALSE;
	      break;
	    }
	  
	  if (!g_output_stream_write_all (reply_stream,
					  new_password, strlen (new_password),
					  &bytes_written,
					  NULL, NULL) ||
	      !g_output_stream_write_all (reply_stream,
					  "\n", 1,
					  &bytes_written,
					  NULL, NULL))
	    {
	      g_set_error (error,
			   G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
			   _("Can't send password"));
	      ret_val = FALSE;
	      break;
	    }
	}
      else if (g_str_has_prefix (buffer, "The authenticity of host '") ||
	       strstr (buffer, "Key fingerprint:") != NULL)
	{
	  /* TODO: Handle these messages */
	}
    }

  g_object_unref (prompt_stream);
  g_object_unref (reply_stream);
  return ret_val;
}


static void
do_mount (GVfsBackend *backend,
	  GVfsJobMount *job,
	  GMountSpec *mount_spec,
	  GMountSource *mount_source,
	  gboolean is_automount)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  gchar **args; /* Enough for now, extend if you add more args */
  pid_t pid;
  int tty_fd, stdout_fd, stdin_fd, stderr_fd;
  GError *error;
  GOutputStream *so;
  GDataOutputStream *ds;
  gboolean res;
  GByteArray *reply;
  GMountSpec *sftp_mount_spec;

  args = setup_ssh_commandline (backend);

  error = NULL;
  if (!spawn_ssh (backend,
		  args, &pid,
		  &tty_fd, &stdin_fd, &stdout_fd, &stderr_fd,
		  &error))
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      g_strfreev (args);
      return;
    }
  
  g_strfreev (args);

  so = g_socket_output_stream_new (stdin_fd, TRUE);

  ds = new_command_stream ();
  g_data_output_stream_put_byte (ds,
				 SSH_FXP_INIT, NULL, NULL);
  g_data_output_stream_put_int32 (ds,
				  SSH2_FILEXFER_VERSION, NULL, NULL);
  send_command (so,ds, NULL, NULL);
  g_object_unref (ds);

  if (tty_fd == -1)
    res = wait_for_reply (backend, stdout_fd, &error);
  else
    res = handle_login (backend, mount_source, tty_fd, stdout_fd, stderr_fd, &error);
  if (!res)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  op_backend->command_stream = g_socket_output_stream_new (stdout_fd, TRUE);
  op_backend->reply_stream = g_socket_input_stream_new (stdout_fd, TRUE);

  reply = read_reply_sync (backend, &error);
  if (reply == NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  /* TODO: On errors above, check stderr_fd output for errors */

  sftp_mount_spec = g_mount_spec_new ("sftp");
  if (op_backend->user_specified)
    g_mount_spec_set (sftp_mount_spec, "user", op_backend->user);
  g_mount_spec_set (sftp_mount_spec, "host", op_backend->host);

  g_vfs_backend_set_mount_spec (backend, sftp_mount_spec);
  g_mount_spec_unref (sftp_mount_spec);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_mount (GVfsBackend *backend,
	   GVfsJobMount *job,
	   GMountSpec *mount_spec,
	   GMountSource *mount_source,
	   gboolean is_automount)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);
  const char *user, *host;

  op_backend->client_vendor = get_sftp_client_vendor ();

  if (op_backend->client_vendor == SFTP_VENDOR_INVALID)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Unable to find supported ssh command"));
      return TRUE;
    }
  
  host = g_mount_spec_get (mount_spec, "host");

  if (host == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			_("Invalid mount spec"));
      return TRUE;
    }

  user = g_mount_spec_get (mount_spec, "user");

  op_backend->host = g_strdup (host);
  op_backend->user = g_strdup (user);
  if (op_backend->user)
    op_backend->user_specified = TRUE;
  else
    op_backend->user = g_strdup (g_get_user_name ());

  return FALSE;
}

static void 
do_open_for_read (GVfsBackend *backend,
		  GVfsJobOpenForRead *job,
		  const char *filename)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */

#if 0
  if (file == NULL)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      g_vfs_job_open_for_read_set_can_seek (job, TRUE);
      g_vfs_job_open_for_read_set_handle (job, file);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
#endif
}

static void
do_read (GVfsBackend *backend,
	 GVfsJobRead *job,
	 GVfsBackendHandle handle,
	 char *buffer,
	 gsize bytes_requested)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */

#if 0
  if (res == -1)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      g_vfs_job_read_set_size (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
	    
    }
#endif
}

static void
do_seek_on_read (GVfsBackend *backend,
		 GVfsJobSeekRead *job,
		 GVfsBackendHandle handle,
		 goffset    offset,
		 GSeekType  type)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

#if 0
  switch (type)
    {
    case G_SEEK_SET:
      whence = SEEK_SET;
      break;
    case G_SEEK_CUR:
      whence = SEEK_CUR;
      break;
    case G_SEEK_END:
      whence = SEEK_END;
      break;
    default:
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Unsupported seek type"));
      return;
    }
#endif

  /* TODO */

#if 0
  if (res == (off_t)-1)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      g_vfs_job_seek_read_set_offset (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
#endif
}

static void
do_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */

#if 0
  if (res == -1)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
#endif

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_create (GVfsBackend *backend,
	   GVfsJobOpenForWrite *job,
	   const char *filename)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */

#if 0
  if (file == NULL)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      handle = g_new0 (SftpWriteHandle, 1);
      handle->file = file;

      g_vfs_job_open_for_write_set_can_seek (job, TRUE);
      g_vfs_job_open_for_write_set_handle (job, handle);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
#endif
}

static void
do_append_to (GVfsBackend *backend,
	      GVfsJobOpenForWrite *job,
	      const char *filename)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */

#if 0
  if (file == NULL)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      handle = g_new0 (SftpWriteHandle, 1);
      handle->file = file;

      initial_offset = op_backend->sftp_context->lseek (op_backend->sftp_context, file,
						       0, SEEK_CUR);
      if (initial_offset == (off_t) -1)
	g_vfs_job_open_for_write_set_can_seek (job, FALSE);
      else
	{
	  g_vfs_job_open_for_write_set_initial_offset (job, initial_offset);
	  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
	}
      g_vfs_job_open_for_write_set_handle (job, handle);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
#endif
}

static void
do_replace (GVfsBackend *backend,
	    GVfsJobOpenForWrite *job,
	    const char *filename,
	    time_t mtime,
	    gboolean make_backup)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */
}

static void
do_write (GVfsBackend *backend,
	  GVfsJobWrite *job,
	  GVfsBackendHandle _handle,
	  char *buffer,
	  gsize buffer_size)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */

#if 0
  if (res == -1)
    g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
  else
    {
      g_vfs_job_write_set_written_size (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
#endif
}

static void
do_seek_on_write (GVfsBackend *backend,
		  GVfsJobSeekWrite *job,
		  GVfsBackendHandle _handle,
		  goffset    offset,
		  GSeekType  type)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */
}

static void
do_close_write (GVfsBackend *backend,
		GVfsJobCloseWrite *job,
		GVfsBackendHandle _handle)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */
}

static void
do_get_info (GVfsBackend *backend,
	     GVfsJobGetInfo *job,
	     const char *filename,
	     const char *attributes,
	     GFileGetInfoFlags flags)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */
}

static void
do_get_fs_info (GVfsBackend *backend,
		GVfsJobGetFsInfo *job,
		const char *filename,
		const char *attributes)
{
  /* TODO */
}

static gboolean
try_query_settable_attributes (GVfsBackend *backend,
			       GVfsJobQueryAttributes *job,
			       const char *filename)
{
  /* TODO */

  return TRUE;
}

static void
do_enumerate (GVfsBackend *backend,
	      GVfsJobEnumerate *job,
	      const char *filename,
	      const char *attributes,
	      GFileGetInfoFlags flags)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */
}

static void
do_set_display_name (GVfsBackend *backend,
		     GVfsJobSetDisplayName *job,
		     const char *filename,
		     const char *display_name)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */
}

static void
do_delete (GVfsBackend *backend,
	   GVfsJobDelete *job,
	   const char *filename)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */
}

static void
do_make_directory (GVfsBackend *backend,
		   GVfsJobMakeDirectory *job,
		   const char *filename)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */
}

static void
do_move (GVfsBackend *backend,
	 GVfsJobMove *job,
	 const char *source,
	 const char *destination,
	 GFileCopyFlags flags,
	 GFileProgressCallback progress_callback,
	 gpointer progress_callback_data)
{
  GVfsBackendSftp *op_backend = G_VFS_BACKEND_SFTP (backend);

  /* TODO */
}

static void
g_vfs_backend_sftp_class_init (GVfsBackendSftpClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_sftp_finalize;

  backend_class->mount = do_mount;
  backend_class->try_mount = try_mount;
  backend_class->open_for_read = do_open_for_read;
  backend_class->read = do_read;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->close_read = do_close_read;
  backend_class->create = do_create;
  backend_class->append_to = do_append_to;
  backend_class->replace = do_replace;
  backend_class->write = do_write;
  backend_class->seek_on_write = do_seek_on_write;
  backend_class->close_write = do_close_write;
  backend_class->get_info = do_get_info;
  backend_class->get_fs_info = do_get_fs_info;
  backend_class->enumerate = do_enumerate;
  backend_class->set_display_name = do_set_display_name;
  backend_class->delete = do_delete;
  backend_class->make_directory = do_make_directory;
  backend_class->move = do_move;
  backend_class->try_query_settable_attributes = try_query_settable_attributes;
}
