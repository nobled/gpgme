/* engine-gpgsm.c - GpgSM engine.
   Copyright (C) 2000 Werner Koch (dd9jn)
   Copyright (C) 2001, 2002, 2003, 2004, 2005, 2007, 2009 g10 Code GmbH
 
   This file is part of GPGME.

   GPGME is free software; you can redistribute it and/or modify it
   under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of
   the License, or (at your option) any later version.
   
   GPGME is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.
   
   You should have received a copy of the GNU Lesser General Public
   License along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
   02111-1307, USA.  */

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <assert.h>
#include <unistd.h>
#include <locale.h>
#include <fcntl.h> /* FIXME */
#include <errno.h>

#include "gpgme.h"
#include "util.h"
#include "ops.h"
#include "wait.h"
#include "priv-io.h"
#include "sema.h"

#include "assuan.h"
#include "status-table.h"
#include "debug.h"

#include "engine-backend.h"


typedef struct
{
  int fd;	/* FD we talk about.  */
  int server_fd;/* Server FD for this connection.  */
  int dir;	/* Inbound/Outbound, maybe given implicit?  */
  void *data;	/* Handler-specific data.  */
  void *tag;	/* ID from the user for gpgme_remove_io_callback.  */
  char server_fd_str[15]; /* Same as SERVER_FD but as a string.  We
                             need this because _gpgme_io_fd2str can't
                             be used on a closed descriptor.  */
} iocb_data_t;


struct engine_gpgsm
{
  assuan_context_t assuan_ctx;

  int lc_ctype_set;
  int lc_messages_set;

  iocb_data_t status_cb;

  /* Input, output etc are from the servers perspective.  */
  iocb_data_t input_cb;
  gpgme_data_t input_helper_data;  /* Input helper data object.  */
  void *input_helper_memory;       /* Input helper memory block.  */

  iocb_data_t output_cb;

  iocb_data_t message_cb;

  struct
  {
    engine_status_handler_t fnc;
    void *fnc_value;
  } status;

  struct
  {
    engine_colon_line_handler_t fnc;
    void *fnc_value;
    struct
    {
      char *line;
      int linesize;
      int linelen;
    } attic;
    int any; /* any data line seen */
  } colon; 

  gpgme_data_t inline_data;  /* Used to collect D lines.  */

  struct gpgme_io_cbs io_cbs;
};

typedef struct engine_gpgsm *engine_gpgsm_t;


static void gpgsm_io_event (void *engine, 
                            gpgme_event_io_t type, void *type_data);



static char *
gpgsm_get_version (const char *file_name)
{
  return _gpgme_get_program_version (file_name ? file_name
				     : _gpgme_get_gpgsm_path ());
}


static const char *
gpgsm_get_req_version (void)
{
  return NEED_GPGSM_VERSION;
}


static void
close_notify_handler (int fd, void *opaque)
{
  engine_gpgsm_t gpgsm = opaque;

  assert (fd != -1);
  if (gpgsm->status_cb.fd == fd)
    {
      if (gpgsm->status_cb.tag)
	(*gpgsm->io_cbs.remove) (gpgsm->status_cb.tag);
      gpgsm->status_cb.fd = -1;
      gpgsm->status_cb.tag = NULL;
    }
  else if (gpgsm->input_cb.fd == fd)
    {
      if (gpgsm->input_cb.tag)
	(*gpgsm->io_cbs.remove) (gpgsm->input_cb.tag);
      gpgsm->input_cb.fd = -1;
      gpgsm->input_cb.tag = NULL;
      if (gpgsm->input_helper_data)
        {
          gpgme_data_release (gpgsm->input_helper_data);
          gpgsm->input_helper_data = NULL;
        }
      if (gpgsm->input_helper_memory)
        {
          free (gpgsm->input_helper_memory);
          gpgsm->input_helper_memory = NULL;
        }
    }
  else if (gpgsm->output_cb.fd == fd)
    {
      if (gpgsm->output_cb.tag)
	(*gpgsm->io_cbs.remove) (gpgsm->output_cb.tag);
      gpgsm->output_cb.fd = -1;
      gpgsm->output_cb.tag = NULL;
    }
  else if (gpgsm->message_cb.fd == fd)
    {
      if (gpgsm->message_cb.tag)
	(*gpgsm->io_cbs.remove) (gpgsm->message_cb.tag);
      gpgsm->message_cb.fd = -1;
      gpgsm->message_cb.tag = NULL;
    }
}


/* This is the default inquiry callback.  We use it to handle the
   Pinentry notifications.  */
static gpgme_error_t
default_inq_cb (engine_gpgsm_t gpgsm, const char *line)
{
  if (!strncmp (line, "PINENTRY_LAUNCHED", 17) && (line[17]==' '||!line[17]))
    {
      _gpgme_allow_set_foreground_window ((pid_t)strtoul (line+17, NULL, 10));
    }

  return 0;
}


static gpgme_error_t
gpgsm_cancel (void *engine)
{
  engine_gpgsm_t gpgsm = engine;

  if (!gpgsm)
    return gpg_error (GPG_ERR_INV_VALUE);

  if (gpgsm->status_cb.fd != -1)
    _gpgme_io_close (gpgsm->status_cb.fd);
  if (gpgsm->input_cb.fd != -1)
    _gpgme_io_close (gpgsm->input_cb.fd);
  if (gpgsm->output_cb.fd != -1)
    _gpgme_io_close (gpgsm->output_cb.fd);
  if (gpgsm->message_cb.fd != -1)
    _gpgme_io_close (gpgsm->message_cb.fd);

  if (gpgsm->assuan_ctx)
    {
      assuan_release (gpgsm->assuan_ctx);
      gpgsm->assuan_ctx = NULL;
    }

  return 0;
}


static void
gpgsm_release (void *engine)
{
  engine_gpgsm_t gpgsm = engine;

  if (!gpgsm)
    return;

  gpgsm_cancel (engine);

  free (gpgsm->colon.attic.line);
  free (gpgsm);
}


static gpgme_error_t
gpgsm_new (void **engine, const char *file_name, const char *home_dir)
{
  gpgme_error_t err = 0;
  engine_gpgsm_t gpgsm;
  const char *argv[5];
  int argc;
#if !USE_DESCRIPTOR_PASSING
  int fds[2];
  int child_fds[4];
#endif
  char *dft_display = NULL;
  char dft_ttyname[64];
  char *dft_ttytype = NULL;
  char *optstr;

  gpgsm = calloc (1, sizeof *gpgsm);
  if (!gpgsm)
    return gpg_error_from_errno (errno);

  gpgsm->status_cb.fd = -1;
  gpgsm->status_cb.dir = 1;
  gpgsm->status_cb.tag = 0;
  gpgsm->status_cb.data = gpgsm;

  gpgsm->input_cb.fd = -1;
  gpgsm->input_cb.dir = 0;
  gpgsm->input_cb.tag = 0;
  gpgsm->input_cb.server_fd = -1;
  *gpgsm->input_cb.server_fd_str = 0;
  gpgsm->output_cb.fd = -1;
  gpgsm->output_cb.dir = 1;
  gpgsm->output_cb.tag = 0;
  gpgsm->output_cb.server_fd = -1;
  *gpgsm->output_cb.server_fd_str = 0;
  gpgsm->message_cb.fd = -1;
  gpgsm->message_cb.dir = 0;
  gpgsm->message_cb.tag = 0;
  gpgsm->message_cb.server_fd = -1;
  *gpgsm->message_cb.server_fd_str = 0;

  gpgsm->status.fnc = 0;
  gpgsm->colon.fnc = 0;
  gpgsm->colon.attic.line = 0;
  gpgsm->colon.attic.linesize = 0;
  gpgsm->colon.attic.linelen = 0;
  gpgsm->colon.any = 0;

  gpgsm->inline_data = NULL;

  gpgsm->io_cbs.add = NULL;
  gpgsm->io_cbs.add_priv = NULL;
  gpgsm->io_cbs.remove = NULL;
  gpgsm->io_cbs.event = NULL;
  gpgsm->io_cbs.event_priv = NULL;

#if !USE_DESCRIPTOR_PASSING
  if (_gpgme_io_pipe (fds, 0) < 0)
    {
      err = gpg_error_from_errno (errno);
      goto leave;
    }
  gpgsm->input_cb.fd = fds[1];
  gpgsm->input_cb.server_fd = fds[0];

  if (_gpgme_io_pipe (fds, 1) < 0)
    {
      err = gpg_error_from_errno (errno);
      goto leave;
    }
  gpgsm->output_cb.fd = fds[0];
  gpgsm->output_cb.server_fd = fds[1];

  if (_gpgme_io_pipe (fds, 0) < 0)
    {
      err = gpg_error_from_errno (errno);
      goto leave;
    }
  gpgsm->message_cb.fd = fds[1];
  gpgsm->message_cb.server_fd = fds[0];

  child_fds[0] = gpgsm->input_cb.server_fd;
  child_fds[1] = gpgsm->output_cb.server_fd;
  child_fds[2] = gpgsm->message_cb.server_fd;
  child_fds[3] = -1;
#endif

  argc = 0;
  argv[argc++] = "gpgsm";
  if (home_dir)
    {
      argv[argc++] = "--homedir";
      argv[argc++] = home_dir;
    }
  argv[argc++] = "--server";
  argv[argc++] = NULL;

  err = assuan_new_ext (&gpgsm->assuan_ctx, GPG_ERR_SOURCE_GPGME,
			&_gpgme_assuan_malloc_hooks, _gpgme_assuan_log_cb,
			NULL);
  if (err)
    goto leave;
  assuan_ctx_set_system_hooks (gpgsm->assuan_ctx, &_gpgme_assuan_system_hooks);

#if USE_DESCRIPTOR_PASSING
  err = assuan_pipe_connect_ext
    (gpgsm->assuan_ctx, file_name ? file_name : _gpgme_get_gpgsm_path (),
     argv, NULL, NULL, NULL, 1);
#else
  err = assuan_pipe_connect
    (gpgsm->assuan_ctx, file_name ? file_name : _gpgme_get_gpgsm_path (),
     argv, child_fds);

  /* On Windows, handles are inserted in the spawned process with
     DuplicateHandle, and child_fds contains the server-local names
     for the inserted handles when assuan_pipe_connect returns.  */
  if (!err)
    {
      /* Note: We don't use _gpgme_io_fd2str here.  On W32 the
	 returned handles are real W32 system handles, not whatever
	 GPGME uses internally (which may be a system handle, a C
	 library handle or a GLib/Qt channel.  Confusing, yes, but
	 remember these are server-local names, so they are not part
	 of GPGME at all.  */
      snprintf (gpgsm->input_cb.server_fd_str,
		sizeof gpgsm->input_cb.server_fd_str, "%d", child_fds[0]);
      snprintf (gpgsm->output_cb.server_fd_str,
		sizeof gpgsm->output_cb.server_fd_str, "%d", child_fds[1]);
      snprintf (gpgsm->message_cb.server_fd_str,
		sizeof gpgsm->message_cb.server_fd_str, "%d", child_fds[2]);
    }
#endif
  if (err)
    goto leave;

  /* assuan_pipe_connect in this case uses _gpgme_io_spawn which
     closes the child fds for us.  */
  gpgsm->input_cb.server_fd = -1;
  gpgsm->output_cb.server_fd = -1;
  gpgsm->message_cb.server_fd = -1;

  err = _gpgme_getenv ("DISPLAY", &dft_display);
  if (err)
    goto leave;
  if (dft_display)
    {
      if (asprintf (&optstr, "OPTION display=%s", dft_display) < 0)
        {
	  free (dft_display);
	  err = gpg_error_from_errno (errno);
	  goto leave;
	}
      free (dft_display);

      err = assuan_transact (gpgsm->assuan_ctx, optstr, NULL, NULL, NULL,
			     NULL, NULL, NULL);
      free (optstr);
      if (err)
	goto leave;
    }

  if (isatty (1))
    {
      int rc;

      rc = ttyname_r (1, dft_ttyname, sizeof (dft_ttyname));
      if (rc)
	{
	  err = gpg_error_from_errno (rc);
	  goto leave;
	}
      else
	{
	  if (asprintf (&optstr, "OPTION ttyname=%s", dft_ttyname) < 0)
	    {
	      err = gpg_error_from_errno (errno);
	      goto leave;
	    }
	  err = assuan_transact (gpgsm->assuan_ctx, optstr, NULL, NULL, NULL,
				 NULL, NULL, NULL);
	  free (optstr);
	  if (err)
	    goto leave;

	  err = _gpgme_getenv ("TERM", &dft_ttytype);
	  if (err)
	    goto leave;
	  if (dft_ttytype)
	    {
	      if (asprintf (&optstr, "OPTION ttytype=%s", dft_ttytype) < 0)
		{
		  free (dft_ttytype);
		  err = gpg_error_from_errno (errno);
		  goto leave;
		}
	      free (dft_ttytype);

	      err = assuan_transact (gpgsm->assuan_ctx, optstr, NULL, NULL,
				     NULL, NULL, NULL, NULL);
	      free (optstr);
	      if (err)
		goto leave;
	    }
	}
    }

  /* Ask gpgsm to enable the audit log support.  */
  if (!err)
    {
      err = assuan_transact (gpgsm->assuan_ctx, "OPTION enable-audit-log=1",
                             NULL, NULL, NULL, NULL, NULL, NULL);
      if (gpg_err_code (err) == GPG_ERR_UNKNOWN_OPTION)
        err = 0; /* This is an optional feature of gpgsm.  */
    }


#ifdef HAVE_W32_SYSTEM
  /* Under Windows we need to use AllowSetForegroundWindow.  Tell
     gpgsm to tell us when it needs it.  */
  if (!err)
    {
      err = assuan_transact (gpgsm->assuan_ctx, "OPTION allow-pinentry-notify",
                             NULL, NULL, NULL, NULL, NULL, NULL);
      if (gpg_err_code (err) == GPG_ERR_UNKNOWN_OPTION)
        err = 0; /* This is a new feature of gpgsm.  */
    }
#endif /*HAVE_W32_SYSTEM*/

#if !USE_DESCRIPTOR_PASSING
  if (!err
      && (_gpgme_io_set_close_notify (gpgsm->input_cb.fd,
				      close_notify_handler, gpgsm)
	  || _gpgme_io_set_close_notify (gpgsm->output_cb.fd,
					 close_notify_handler, gpgsm)
	  || _gpgme_io_set_close_notify (gpgsm->message_cb.fd,
					 close_notify_handler, gpgsm)))
    {
      err = gpg_error (GPG_ERR_GENERAL);
      goto leave;
    }
#endif

 leave:
  /* Close the server ends of the pipes (because of this, we must use
     the stored server_fd_str in the function start).  Our ends are
     closed in gpgsm_release().  */
#if !USE_DESCRIPTOR_PASSING
  if (gpgsm->input_cb.server_fd != -1)
    _gpgme_io_close (gpgsm->input_cb.server_fd);
  if (gpgsm->output_cb.server_fd != -1)
    _gpgme_io_close (gpgsm->output_cb.server_fd);
  if (gpgsm->message_cb.server_fd != -1)
    _gpgme_io_close (gpgsm->message_cb.server_fd);
#endif

  if (err)
    gpgsm_release (gpgsm);
  else
    *engine = gpgsm;

  return err;
}


static gpgme_error_t
gpgsm_set_locale (void *engine, int category, const char *value)
{
  engine_gpgsm_t gpgsm = engine;
  gpgme_error_t err;
  char *optstr;
  char *catstr;

  /* FIXME: If value is NULL, we need to reset the option to default.
     But we can't do this.  So we error out here.  GPGSM needs support
     for this.  */
  if (category == LC_CTYPE)
    {
      catstr = "lc-ctype";
      if (!value && gpgsm->lc_ctype_set)
	return gpg_error (GPG_ERR_INV_VALUE);
      if (value)
	gpgsm->lc_ctype_set = 1;
    }
#ifdef LC_MESSAGES
  else if (category == LC_MESSAGES)
    {
      catstr = "lc-messages";
      if (!value && gpgsm->lc_messages_set)
	return gpg_error (GPG_ERR_INV_VALUE);
      if (value)
	gpgsm->lc_messages_set = 1;
    }
#endif /* LC_MESSAGES */
  else
    return gpg_error (GPG_ERR_INV_VALUE);

  /* FIXME: Reset value to default.  */
  if (!value) 
    return 0;

  if (asprintf (&optstr, "OPTION %s=%s", catstr, value) < 0)
    err = gpg_error_from_errno (errno);
  else
    {
      err = assuan_transact (gpgsm->assuan_ctx, optstr, NULL, NULL,
			     NULL, NULL, NULL, NULL);
      free (optstr);
    }

  return err;
}


/* Forward declaration.  */
static gpgme_status_code_t parse_status (const char *name);

static gpgme_error_t
gpgsm_assuan_simple_command (assuan_context_t ctx, char *cmd,
			     engine_status_handler_t status_fnc,
			     void *status_fnc_value)
{
  gpg_error_t err;
  char *line;
  size_t linelen;

  err = assuan_write_line (ctx, cmd);
  if (err)
    return err;

  do
    {
      err = assuan_read_line (ctx, &line, &linelen);
      if (err)
	return err;

      if (*line == '#' || !linelen)
	continue;

      if (linelen >= 2
	  && line[0] == 'O' && line[1] == 'K'
	  && (line[2] == '\0' || line[2] == ' '))
	return 0;
      else if (linelen >= 4
	  && line[0] == 'E' && line[1] == 'R' && line[2] == 'R'
	  && line[3] == ' ')
	err = atoi (&line[4]);
      else if (linelen >= 2
	       && line[0] == 'S' && line[1] == ' ')
	{
	  char *rest;
	  gpgme_status_code_t r;

	  rest = strchr (line + 2, ' ');
	  if (!rest)
	    rest = line + linelen; /* set to an empty string */
	  else
	    *(rest++) = 0;

	  r = parse_status (line + 2);

	  if (r >= 0 && status_fnc)
	    err = status_fnc (status_fnc_value, r, rest);
	  else
	    err = gpg_error (GPG_ERR_GENERAL);
	}
      else
	err = gpg_error (GPG_ERR_GENERAL);
    }
  while (!err);

  return err;
}


typedef enum { INPUT_FD, OUTPUT_FD, MESSAGE_FD } fd_type_t;

static void
gpgsm_clear_fd (engine_gpgsm_t gpgsm, fd_type_t fd_type)
{
#if !USE_DESCRIPTOR_PASSING
  switch (fd_type)
    {
    case INPUT_FD:
      _gpgme_io_close (gpgsm->input_cb.fd);
      break;
    case OUTPUT_FD:
      _gpgme_io_close (gpgsm->output_cb.fd);
      break;
    case MESSAGE_FD:
      _gpgme_io_close (gpgsm->message_cb.fd);
      break;
    }
#endif
}

#define COMMANDLINELEN 40
static gpgme_error_t
gpgsm_set_fd (engine_gpgsm_t gpgsm, fd_type_t fd_type, const char *opt)
{
  gpg_error_t err = 0;
  char line[COMMANDLINELEN];
  char *which;
  iocb_data_t *iocb_data;
  int dir;

  switch (fd_type)
    {
    case INPUT_FD:
      which = "INPUT";
      iocb_data = &gpgsm->input_cb;
      break;

    case OUTPUT_FD:
      which = "OUTPUT";
      iocb_data = &gpgsm->output_cb;
      break;

    case MESSAGE_FD:
      which = "MESSAGE";
      iocb_data = &gpgsm->message_cb;
      break;

    default:
      return gpg_error (GPG_ERR_INV_VALUE);
    }

  dir = iocb_data->dir;

#if USE_DESCRIPTOR_PASSING
  /* We try to short-cut the communication by giving GPGSM direct
     access to the file descriptor, rather than using a pipe.  */
  iocb_data->server_fd = _gpgme_data_get_fd (iocb_data->data);
  if (iocb_data->server_fd < 0)
    {
      int fds[2];

      if (_gpgme_io_pipe (fds, 0) < 0)
	return gpg_error_from_errno (errno);

      iocb_data->fd = dir ? fds[0] : fds[1];
      iocb_data->server_fd = dir ? fds[1] : fds[0];

      if (_gpgme_io_set_close_notify (iocb_data->fd,
				      close_notify_handler, gpgsm))
	{
	  err = gpg_error (GPG_ERR_GENERAL);
	  goto leave_set_fd;
	}
    }

  err = assuan_sendfd (gpgsm->assuan_ctx, iocb_data->server_fd);
  if (err)
    goto leave_set_fd;

  _gpgme_io_close (iocb_data->server_fd);
  iocb_data->server_fd = -1;

  if (opt)
    snprintf (line, COMMANDLINELEN, "%s FD %s", which, opt);
  else
    snprintf (line, COMMANDLINELEN, "%s FD", which);
#else
  if (opt)
    snprintf (line, COMMANDLINELEN, "%s FD=%s %s", 
              which, iocb_data->server_fd_str, opt);
  else
    snprintf (line, COMMANDLINELEN, "%s FD=%s", 
              which, iocb_data->server_fd_str);
#endif

  err = gpgsm_assuan_simple_command (gpgsm->assuan_ctx, line, NULL, NULL);

#if USE_DESCRIPTOR_PASSING
 leave_set_fd:
  if (err)
    {
      _gpgme_io_close (iocb_data->fd);
      iocb_data->fd = -1;
      if (iocb_data->server_fd != -1)
        {
          _gpgme_io_close (iocb_data->server_fd);
          iocb_data->server_fd = -1;
        }
    }
#endif

  return err;
}


static const char *
map_data_enc (gpgme_data_t d)
{
  switch (gpgme_data_get_encoding (d))
    {
    case GPGME_DATA_ENCODING_NONE:
      break;
    case GPGME_DATA_ENCODING_BINARY:
      return "--binary";
    case GPGME_DATA_ENCODING_BASE64:
      return "--base64";
    case GPGME_DATA_ENCODING_ARMOR:
      return "--armor";
    default:
      break;
    }
  return NULL;
}


static int
status_cmp (const void *ap, const void *bp)
{
  const struct status_table_s *a = ap;
  const struct status_table_s *b = bp;

  return strcmp (a->name, b->name);
}


static gpgme_status_code_t
parse_status (const char *name)
{
  struct status_table_s t, *r;
  t.name = name;
  r = bsearch (&t, status_table, DIM(status_table) - 1,
	       sizeof t, status_cmp);
  return r ? r->code : -1;
}


static gpgme_error_t
status_handler (void *opaque, int fd)
{
  struct io_cb_data *data = (struct io_cb_data *) opaque;
  engine_gpgsm_t gpgsm = (engine_gpgsm_t) data->handler_value;
  gpgme_error_t err = 0;
  char *line;
  size_t linelen;

  do
    {
      err = assuan_read_line (gpgsm->assuan_ctx, &line, &linelen);
      if (err)
	{
	  /* Try our best to terminate the connection friendly.  */
	  /*	  assuan_write_line (gpgsm->assuan_ctx, "BYE"); */
          TRACE3 (DEBUG_CTX, "gpgme:status_handler", gpgsm,
		  "fd 0x%x: error from assuan (%d) getting status line : %s",
                  fd, err, gpg_strerror (err));
	}
      else if (linelen >= 3
	       && line[0] == 'E' && line[1] == 'R' && line[2] == 'R'
	       && (line[3] == '\0' || line[3] == ' '))
	{
	  if (line[3] == ' ')
	    err = atoi (&line[4]);
	  if (! err)
	    err = gpg_error (GPG_ERR_GENERAL);
          TRACE2 (DEBUG_CTX, "gpgme:status_handler", gpgsm,
		  "fd 0x%x: ERR line - mapped to: %s",
                  fd, err ? gpg_strerror (err) : "ok");
	  /* Try our best to terminate the connection friendly.  */
	  /*	  assuan_write_line (gpgsm->assuan_ctx, "BYE"); */
	}
      else if (linelen >= 2
	       && line[0] == 'O' && line[1] == 'K'
	       && (line[2] == '\0' || line[2] == ' '))
	{
	  if (gpgsm->status.fnc)
	    err = gpgsm->status.fnc (gpgsm->status.fnc_value,
				     GPGME_STATUS_EOF, "");
	  
	  if (!err && gpgsm->colon.fnc && gpgsm->colon.any)
            {
              /* We must tell a colon function about the EOF. We do
                 this only when we have seen any data lines.  Note
                 that this inlined use of colon data lines will
                 eventually be changed into using a regular data
                 channel. */
              gpgsm->colon.any = 0;
              err = gpgsm->colon.fnc (gpgsm->colon.fnc_value, NULL);
            }
          TRACE2 (DEBUG_CTX, "gpgme:status_handler", gpgsm,
		  "fd 0x%x: OK line - final status: %s",
                  fd, err ? gpg_strerror (err) : "ok");
	  _gpgme_io_close (gpgsm->status_cb.fd);
	  return err;
	}
      else if (linelen > 2
	       && line[0] == 'D' && line[1] == ' '
	       && gpgsm->colon.fnc)
        {
	  /* We are using the colon handler even for plain inline data
             - strange name for that function but for historic reasons
             we keep it.  */
          /* FIXME We can't use this for binary data because we
             assume this is a string.  For the current usage of colon
             output it is correct.  */
          char *src = line + 2;
	  char *end = line + linelen;
	  char *dst;
          char **aline = &gpgsm->colon.attic.line;
	  int *alinelen = &gpgsm->colon.attic.linelen;

	  if (gpgsm->colon.attic.linesize < *alinelen + linelen + 1)
	    {
	      char *newline = realloc (*aline, *alinelen + linelen + 1);
	      if (!newline)
		err = gpg_error_from_errno (errno);
	      else
		{
		  *aline = newline;
		  gpgsm->colon.attic.linesize += linelen + 1;
		}
	    }
	  if (!err)
	    {
	      dst = *aline + *alinelen;

	      while (!err && src < end)
		{
		  if (*src == '%' && src + 2 < end)
		    {
		      /* Handle escaped characters.  */
		      ++src;
		      *dst = _gpgme_hextobyte (src);
		      (*alinelen)++;
		      src += 2;
		    }
		  else
		    {
		      *dst = *src++;
		      (*alinelen)++;
		    }
		  
		  if (*dst == '\n')
		    {
		      /* Terminate the pending line, pass it to the colon
			 handler and reset it.  */
		      
		      gpgsm->colon.any = 1;
		      if (*alinelen > 1 && *(dst - 1) == '\r')
			dst--;
		      *dst = '\0';

		      /* FIXME How should we handle the return code?  */
		      err = gpgsm->colon.fnc (gpgsm->colon.fnc_value, *aline);
		      if (!err)
			{
			  dst = *aline;
			  *alinelen = 0;
			}
		    }
		  else
		    dst++;
		}
	    }
          TRACE2 (DEBUG_CTX, "gpgme:status_handler", gpgsm,
		  "fd 0x%x: D line; final status: %s",
                  fd, err? gpg_strerror (err):"ok");
        }
      else if (linelen > 2
	       && line[0] == 'D' && line[1] == ' '
	       && gpgsm->inline_data)
        {
          char *src = line + 2;
	  char *end = line + linelen;
	  char *dst = src;
          ssize_t nwritten;

          linelen = 0;
          while (src < end)
            {
              if (*src == '%' && src + 2 < end)
                {
                  /* Handle escaped characters.  */
                  ++src;
                  *dst++ = _gpgme_hextobyte (src);
                  src += 2;
                }
              else
                *dst++ = *src++;
              
              linelen++;
            }
          
          src = line + 2;
          while (linelen > 0)
            {
              nwritten = gpgme_data_write (gpgsm->inline_data, src, linelen);
              if (!nwritten || (nwritten < 0 && errno != EINTR)
                  || nwritten > linelen)
                {
                  err = gpg_error_from_errno (errno);
                  break;
                }
              src += nwritten;
              linelen -= nwritten;
            }

          TRACE2 (DEBUG_CTX, "gpgme:status_handler", gpgsm,
		  "fd 0x%x: D inlinedata; final status: %s",
                  fd, err? gpg_strerror (err):"ok");
        }
      else if (linelen > 2
	       && line[0] == 'S' && line[1] == ' ')
	{
	  char *rest;
	  gpgme_status_code_t r;
	  
	  rest = strchr (line + 2, ' ');
	  if (!rest)
	    rest = line + linelen; /* set to an empty string */
	  else
	    *(rest++) = 0;

	  r = parse_status (line + 2);

	  if (r >= 0)
	    {
	      if (gpgsm->status.fnc)
		err = gpgsm->status.fnc (gpgsm->status.fnc_value, r, rest);
	    }
	  else
	    fprintf (stderr, "[UNKNOWN STATUS]%s %s", line + 2, rest);
          TRACE3 (DEBUG_CTX, "gpgme:status_handler", gpgsm,
		  "fd 0x%x: S line (%s) - final status: %s",
                  fd, line+2, err? gpg_strerror (err):"ok");
	}
      else if (linelen >= 7
               && line[0] == 'I' && line[1] == 'N' && line[2] == 'Q'
               && line[3] == 'U' && line[4] == 'I' && line[5] == 'R'
               && line[6] == 'E' 
               && (line[7] == '\0' || line[7] == ' '))
        {
          char *keyword = line+7;

          while (*keyword == ' ')
            keyword++;;
          default_inq_cb (gpgsm, keyword);
          assuan_write_line (gpgsm->assuan_ctx, "END");
        }

    }
  while (!err && assuan_pending_line (gpgsm->assuan_ctx));
	  
  return err;
}


static gpgme_error_t
add_io_cb (engine_gpgsm_t gpgsm, iocb_data_t *iocbd, gpgme_io_cb_t handler)
{
  gpgme_error_t err;

  TRACE_BEG2 (DEBUG_ENGINE, "engine-gpgsm:add_io_cb", gpgsm,
              "fd %d, dir %d", iocbd->fd, iocbd->dir);
  err = (*gpgsm->io_cbs.add) (gpgsm->io_cbs.add_priv,
			      iocbd->fd, iocbd->dir,
			      handler, iocbd->data, &iocbd->tag);
  if (err)
    return TRACE_ERR (err);
  if (!iocbd->dir)
    /* FIXME Kludge around poll() problem.  */
    err = _gpgme_io_set_nonblocking (iocbd->fd);
  return TRACE_ERR (err);
}


static gpgme_error_t
start (engine_gpgsm_t gpgsm, const char *command)
{
  gpgme_error_t err;
  int fdlist[5];
  int nfds;

  /* We need to know the fd used by assuan for reads.  We do this by
     using the assumption that the first returned fd from
     assuan_get_active_fds() is always this one.  */
  nfds = assuan_get_active_fds (gpgsm->assuan_ctx, 0 /* read fds */,
                                fdlist, DIM (fdlist));
  if (nfds < 1)
    return gpg_error (GPG_ERR_GENERAL);	/* FIXME */

  /* We "duplicate" the file descriptor, so we can close it here (we
     can't close fdlist[0], as that is closed by libassuan, and
     closing it here might cause libassuan to close some unrelated FD
     later).  Alternatively, we could special case status_fd and
     register/unregister it manually as needed, but this increases
     code duplication and is more complicated as we can not use the
     close notifications etc.  A third alternative would be to let
     Assuan know that we closed the FD, but that complicates the
     Assuan interface.  */

  gpgsm->status_cb.fd = _gpgme_io_dup (fdlist[0]);
  if (gpgsm->status_cb.fd < 0)
    return gpg_error_from_syserror ();

  if (_gpgme_io_set_close_notify (gpgsm->status_cb.fd,
				  close_notify_handler, gpgsm))
    {
      _gpgme_io_close (gpgsm->status_cb.fd);
      gpgsm->status_cb.fd = -1;
      return gpg_error (GPG_ERR_GENERAL);
    }

  err = add_io_cb (gpgsm, &gpgsm->status_cb, status_handler);
  if (!err && gpgsm->input_cb.fd != -1)
    err = add_io_cb (gpgsm, &gpgsm->input_cb, _gpgme_data_outbound_handler);
  if (!err && gpgsm->output_cb.fd != -1)
    err = add_io_cb (gpgsm, &gpgsm->output_cb, _gpgme_data_inbound_handler);
  if (!err && gpgsm->message_cb.fd != -1)
    err = add_io_cb (gpgsm, &gpgsm->message_cb, _gpgme_data_outbound_handler);

  if (!err)
    err = assuan_write_line (gpgsm->assuan_ctx, command);

  if (!err)
    gpgsm_io_event (gpgsm, GPGME_EVENT_START, NULL);

  return err;
}


#if USE_DESCRIPTOR_PASSING
static gpgme_error_t
gpgsm_reset (void *engine)
{
  engine_gpgsm_t gpgsm = engine;

  /* We must send a reset because we need to reset the list of
     signers.  Note that RESET does not reset OPTION commands. */
  return gpgsm_assuan_simple_command (gpgsm->assuan_ctx, "RESET", NULL, NULL);
}
#endif


static gpgme_error_t
gpgsm_decrypt (void *engine, gpgme_data_t ciph, gpgme_data_t plain)
{
  engine_gpgsm_t gpgsm = engine;
  gpgme_error_t err;

  if (!gpgsm)
    return gpg_error (GPG_ERR_INV_VALUE);

  gpgsm->input_cb.data = ciph;
  err = gpgsm_set_fd (gpgsm, INPUT_FD, map_data_enc (gpgsm->input_cb.data));
  if (err)
    return gpg_error (GPG_ERR_GENERAL);	/* FIXME */
  gpgsm->output_cb.data = plain;
  err = gpgsm_set_fd (gpgsm, OUTPUT_FD, 0);
  if (err)
    return gpg_error (GPG_ERR_GENERAL);	/* FIXME */
  gpgsm_clear_fd (gpgsm, MESSAGE_FD);
  gpgsm->inline_data = NULL;

  err = start (engine, "DECRYPT");
  return err;
}


static gpgme_error_t
gpgsm_delete (void *engine, gpgme_key_t key, int allow_secret)
{
  engine_gpgsm_t gpgsm = engine;
  gpgme_error_t err;
  char *fpr = key->subkeys ? key->subkeys->fpr : NULL;
  char *linep = fpr;
  char *line;
  int length = 8;	/* "DELKEYS " */

  if (!fpr)
    return gpg_error (GPG_ERR_INV_VALUE);

  while (*linep)
    {
      length++;
      if (*linep == '%' || *linep == ' ' || *linep == '+')
	length += 2;
      linep++;
    }
  length++;

  line = malloc (length);
  if (!line)
    return gpg_error_from_errno (errno);

  strcpy (line, "DELKEYS ");
  linep = &line[8];

  while (*fpr)
    {
      switch (*fpr)
	{
	case '%':
	  *(linep++) = '%';
	  *(linep++) = '2';
	  *(linep++) = '5';
	  break;
	case ' ':
	  *(linep++) = '%';
	  *(linep++) = '2';
	  *(linep++) = '0';
	  break;
	case '+':
	  *(linep++) = '%';
	  *(linep++) = '2';
	  *(linep++) = 'B';
	  break;
	default:
	  *(linep++) = *fpr;
	  break;
	}
      fpr++;
    }
  *linep = '\0';

  gpgsm_clear_fd (gpgsm, OUTPUT_FD);
  gpgsm_clear_fd (gpgsm, INPUT_FD);
  gpgsm_clear_fd (gpgsm, MESSAGE_FD);
  gpgsm->inline_data = NULL;

  err = start (gpgsm, line);
  free (line);

  return err;
}


static gpgme_error_t
set_recipients (engine_gpgsm_t gpgsm, gpgme_key_t recp[])
{
  gpgme_error_t err = 0;
  assuan_context_t ctx = gpgsm->assuan_ctx;
  char *line;
  int linelen;
  int invalid_recipients = 0;
  int i = 0;

  linelen = 10 + 40 + 1;	/* "RECIPIENT " + guess + '\0'.  */
  line = malloc (10 + 40 + 1);
  if (!line)
    return gpg_error_from_errno (errno);
  strcpy (line, "RECIPIENT ");
  while (!err && recp[i])
    {
      char *fpr;
      int newlen;

      if (!recp[i]->subkeys || !recp[i]->subkeys->fpr)
	{
	  invalid_recipients++;
	  continue;
	}
      fpr = recp[i]->subkeys->fpr;

      newlen = 11 + strlen (fpr);
      if (linelen < newlen)
	{
	  char *newline = realloc (line, newlen);
	  if (! newline)
	    {
	      int saved_errno = errno;
	      free (line);
	      return gpg_error_from_errno (saved_errno);
	    }
	  line = newline;
	  linelen = newlen;
	}
      strcpy (&line[10], fpr);

      err = gpgsm_assuan_simple_command (ctx, line, gpgsm->status.fnc,
					 gpgsm->status.fnc_value);
      /* FIXME: This requires more work.  */
      if (gpg_err_code (err) == GPG_ERR_NO_PUBKEY)
	invalid_recipients++;
      else if (err)
	{
	  free (line);
	  return err;
	}
      i++;
    }
  free (line);
  return gpg_error (invalid_recipients
		    ? GPG_ERR_UNUSABLE_PUBKEY : GPG_ERR_NO_ERROR);
}


static gpgme_error_t
gpgsm_encrypt (void *engine, gpgme_key_t recp[], gpgme_encrypt_flags_t flags,
	       gpgme_data_t plain, gpgme_data_t ciph, int use_armor)
{
  engine_gpgsm_t gpgsm = engine;
  gpgme_error_t err;

  if (!gpgsm)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!recp)
    return gpg_error (GPG_ERR_NOT_IMPLEMENTED);

  if (flags & GPGME_ENCRYPT_NO_ENCRYPT_TO)
    {
      err = gpgsm_assuan_simple_command (gpgsm->assuan_ctx,
					 "OPTION no-encrypt-to", NULL, NULL);
      if (err)
	return err;
    }

  gpgsm->input_cb.data = plain;
  err = gpgsm_set_fd (gpgsm, INPUT_FD, map_data_enc (gpgsm->input_cb.data));
  if (err)
    return err;
  gpgsm->output_cb.data = ciph;
  err = gpgsm_set_fd (gpgsm, OUTPUT_FD, use_armor ? "--armor"
		      : map_data_enc (gpgsm->output_cb.data));
  if (err)
    return err;
  gpgsm_clear_fd (gpgsm, MESSAGE_FD);
  gpgsm->inline_data = NULL;

  err = set_recipients (gpgsm, recp);

  if (!err)
    err = start (gpgsm, "ENCRYPT");

  return err;
}


static gpgme_error_t
gpgsm_export (void *engine, const char *pattern, gpgme_export_mode_t mode,
	      gpgme_data_t keydata, int use_armor)
{
  engine_gpgsm_t gpgsm = engine;
  gpgme_error_t err = 0;
  char *cmd;

  if (!gpgsm)
    return gpg_error (GPG_ERR_INV_VALUE);
  
  if (mode)
    return gpg_error (GPG_ERR_NOT_SUPPORTED);

  if (!pattern)
    pattern = "";

  cmd = malloc (7 + strlen (pattern) + 1);
  if (!cmd)
    return gpg_error_from_errno (errno);
  strcpy (cmd, "EXPORT ");
  strcpy (&cmd[7], pattern);

  gpgsm->output_cb.data = keydata;
  err = gpgsm_set_fd (gpgsm, OUTPUT_FD, use_armor ? "--armor"
		      : map_data_enc (gpgsm->output_cb.data));
  if (err)
    return err;
  gpgsm_clear_fd (gpgsm, INPUT_FD);
  gpgsm_clear_fd (gpgsm, MESSAGE_FD);
  gpgsm->inline_data = NULL;

  err = start (gpgsm, cmd);
  free (cmd);
  return err;
}


static gpgme_error_t
gpgsm_export_ext (void *engine, const char *pattern[], gpgme_export_mode_t mode,
		  gpgme_data_t keydata, int use_armor)
{
  engine_gpgsm_t gpgsm = engine;
  gpgme_error_t err = 0;
  char *line;
  /* Length is "EXPORT " + p + '\0'.  */
  int length = 7 + 1;
  char *linep;

  if (!gpgsm)
    return gpg_error (GPG_ERR_INV_VALUE);

  if (mode)
    return gpg_error (GPG_ERR_NOT_SUPPORTED);

  if (pattern && *pattern)
    {
      const char **pat = pattern;

      while (*pat)
	{
	  const char *patlet = *pat;

	  while (*patlet)
	    {
	      length++;
	      if (*patlet == '%' || *patlet == ' ' || *patlet == '+')
		length += 2;
	      patlet++;
	    }
	  pat++;
	  length++;
	}
    }
  line = malloc (length);
  if (!line)
    return gpg_error_from_errno (errno);

  strcpy (line, "EXPORT ");
  linep = &line[7];

  if (pattern && *pattern)
    {
      while (*pattern)
	{
	  const char *patlet = *pattern;

	  while (*patlet)
	    {
	      switch (*patlet)
		{
		case '%':
		  *(linep++) = '%';
		  *(linep++) = '2';
		  *(linep++) = '5';
		  break;
		case ' ':
		  *(linep++) = '%';
		  *(linep++) = '2';
		  *(linep++) = '0';
		  break;
		case '+':
		  *(linep++) = '%';
		  *(linep++) = '2';
		  *(linep++) = 'B';
		  break;
		default:
		  *(linep++) = *patlet;
		  break;
		}
	      patlet++;
	    }
	  pattern++;
          if (*pattern)
            *linep++ = ' ';
	}
    }
  *linep = '\0';

  gpgsm->output_cb.data = keydata;
  err = gpgsm_set_fd (gpgsm, OUTPUT_FD, use_armor ? "--armor"
		      : map_data_enc (gpgsm->output_cb.data));
  if (err)
    return err;
  gpgsm_clear_fd (gpgsm, INPUT_FD);
  gpgsm_clear_fd (gpgsm, MESSAGE_FD);
  gpgsm->inline_data = NULL;

  err = start (gpgsm, line);
  free (line);
  return err;
}


static gpgme_error_t
gpgsm_genkey (void *engine, gpgme_data_t help_data, int use_armor,
	      gpgme_data_t pubkey, gpgme_data_t seckey)
{
  engine_gpgsm_t gpgsm = engine;
  gpgme_error_t err;

  if (!gpgsm || !pubkey || seckey)
    return gpg_error (GPG_ERR_INV_VALUE);

  gpgsm->input_cb.data = help_data;
  err = gpgsm_set_fd (gpgsm, INPUT_FD, map_data_enc (gpgsm->input_cb.data));
  if (err)
    return err;
  gpgsm->output_cb.data = pubkey;
  err = gpgsm_set_fd (gpgsm, OUTPUT_FD, use_armor ? "--armor"
		      : map_data_enc (gpgsm->output_cb.data));
  if (err)
    return err;
  gpgsm_clear_fd (gpgsm, MESSAGE_FD);
  gpgsm->inline_data = NULL;

  err = start (gpgsm, "GENKEY");
  return err;
}


static gpgme_error_t
gpgsm_import (void *engine, gpgme_data_t keydata, gpgme_key_t *keyarray)
{
  engine_gpgsm_t gpgsm = engine;
  gpgme_error_t err;
  gpgme_data_encoding_t dataenc;
  int idx;

  if (!gpgsm)
    return gpg_error (GPG_ERR_INV_VALUE);

  if (keydata && keyarray)
    return gpg_error (GPG_ERR_INV_VALUE); /* Only one is allowed.  */

  dataenc = gpgme_data_get_encoding (keydata);

  if (keyarray)
    {
      size_t buflen;
      char *buffer, *p;

      /* Fist check whether the engine already features the
         --re-import option.  */
      err = gpgsm_assuan_simple_command 
        (gpgsm->assuan_ctx, 
         "GETINFO cmd_has_option IMPORT re-import", NULL, NULL);
      if (err)
	return gpg_error (GPG_ERR_NOT_SUPPORTED);

      /* Create an internal data object with a list of all
         fingerprints.  The data object and its memory (to avoid an
         extra copy by gpgme_data_new_from_mem) are stored in two
         variables which are released by the close_notify_handler.  */
      for (idx=0, buflen=0; keyarray[idx]; idx++)
        {
          if (keyarray[idx]->protocol == GPGME_PROTOCOL_CMS
              && keyarray[idx]->subkeys
              && keyarray[idx]->subkeys->fpr 
              && *keyarray[idx]->subkeys->fpr)
            buflen += strlen (keyarray[idx]->subkeys->fpr) + 1;
        }
      /* Allocate a bufer with extra space for the trailing Nul
         introduced by the use of stpcpy.  */
      buffer = malloc (buflen+1);
      if (!buffer)
        return gpg_error_from_syserror ();
      for (idx=0, p = buffer; keyarray[idx]; idx++)
        {
          if (keyarray[idx]->protocol == GPGME_PROTOCOL_CMS
              && keyarray[idx]->subkeys
              && keyarray[idx]->subkeys->fpr 
              && *keyarray[idx]->subkeys->fpr)
            p = stpcpy (stpcpy (p, keyarray[idx]->subkeys->fpr), "\n");
        }
      
      err = gpgme_data_new_from_mem (&gpgsm->input_helper_data,
                                     buffer, buflen, 0);
      if (err)
        {
          free (buffer);
          return err;
        }
      gpgsm->input_helper_memory = buffer;

      gpgsm->input_cb.data = gpgsm->input_helper_data;
      err = gpgsm_set_fd (gpgsm, INPUT_FD, map_data_enc (gpgsm->input_cb.data));
      if (err)
        {
          gpgme_data_release (gpgsm->input_helper_data);
          gpgsm->input_helper_data = NULL;
          free (gpgsm->input_helper_memory);
          gpgsm->input_helper_memory = NULL;
          return err;
        }
      gpgsm_clear_fd (gpgsm, OUTPUT_FD);
      gpgsm_clear_fd (gpgsm, MESSAGE_FD);
      gpgsm->inline_data = NULL;

      return start (gpgsm, "IMPORT --re-import");
    }
  else if (dataenc == GPGME_DATA_ENCODING_URL
           || dataenc == GPGME_DATA_ENCODING_URL0
           || dataenc == GPGME_DATA_ENCODING_URLESC)
    {
      return gpg_error (GPG_ERR_NOT_IMPLEMENTED);
    }
  else
    {
      gpgsm->input_cb.data = keydata;
      err = gpgsm_set_fd (gpgsm, INPUT_FD, map_data_enc (gpgsm->input_cb.data));
      if (err)
        return err;
      gpgsm_clear_fd (gpgsm, OUTPUT_FD);
      gpgsm_clear_fd (gpgsm, MESSAGE_FD);
      gpgsm->inline_data = NULL;

      return start (gpgsm, "IMPORT");
    }
}


static gpgme_error_t
gpgsm_keylist (void *engine, const char *pattern, int secret_only,
	       gpgme_keylist_mode_t mode)
{
  engine_gpgsm_t gpgsm = engine;
  char *line;
  gpgme_error_t err;
  int list_mode = 0;

  if (mode & GPGME_KEYLIST_MODE_LOCAL)
    list_mode |= 1;
  if (mode & GPGME_KEYLIST_MODE_EXTERN)
    list_mode |= 2;

  if (!pattern)
    pattern = "";

  /* Always send list-mode option because RESET does not reset it.  */
  if (asprintf (&line, "OPTION list-mode=%d", (list_mode & 3)) < 0)
    return gpg_error_from_errno (errno);
  err = gpgsm_assuan_simple_command (gpgsm->assuan_ctx, line, NULL, NULL);
  free (line);
  if (err)
    return err;


  /* Always send key validation because RESET does not reset it.  */

  /* Use the validation mode if requested.  We don't check for an error
     yet because this is a pretty fresh gpgsm features. */
  gpgsm_assuan_simple_command (gpgsm->assuan_ctx, 
                               (mode & GPGME_KEYLIST_MODE_VALIDATE)?
                               "OPTION with-validation=1":
                               "OPTION with-validation=0" ,
                               NULL, NULL);
  /* Include the ephemeral keys if requested.  We don't check for an error
     yet because this is a pretty fresh gpgsm features. */
  gpgsm_assuan_simple_command (gpgsm->assuan_ctx, 
                               (mode & GPGME_KEYLIST_MODE_EPHEMERAL)?
                               "OPTION with-ephemeral-keys=1":
                               "OPTION with-ephemeral-keys=0" ,
                               NULL, NULL);


  /* Length is "LISTSECRETKEYS " + p + '\0'.  */
  line = malloc (15 + strlen (pattern) + 1);
  if (!line)
    return gpg_error_from_errno (errno);
  if (secret_only)
    {
      strcpy (line, "LISTSECRETKEYS ");
      strcpy (&line[15], pattern);
    }
  else
    {
      strcpy (line, "LISTKEYS ");
      strcpy (&line[9], pattern);
    }

  gpgsm_clear_fd (gpgsm, INPUT_FD);
  gpgsm_clear_fd (gpgsm, OUTPUT_FD);
  gpgsm_clear_fd (gpgsm, MESSAGE_FD);
  gpgsm->inline_data = NULL;

  err = start (gpgsm, line);
  free (line);
  return err;
}


static gpgme_error_t
gpgsm_keylist_ext (void *engine, const char *pattern[], int secret_only,
		   int reserved, gpgme_keylist_mode_t mode)
{
  engine_gpgsm_t gpgsm = engine;
  char *line;
  gpgme_error_t err;
  /* Length is "LISTSECRETKEYS " + p + '\0'.  */
  int length = 15 + 1;
  char *linep;
  int any_pattern = 0;
  int list_mode = 0;

  if (reserved)
    return gpg_error (GPG_ERR_INV_VALUE);

  if (mode & GPGME_KEYLIST_MODE_LOCAL)
    list_mode |= 1;
  if (mode & GPGME_KEYLIST_MODE_EXTERN)
    list_mode |= 2;

  /* Always send list-mode option because RESET does not reset it.  */
  if (asprintf (&line, "OPTION list-mode=%d", (list_mode & 3)) < 0)
    return gpg_error_from_errno (errno);
  err = gpgsm_assuan_simple_command (gpgsm->assuan_ctx, line, NULL, NULL);
  free (line);
  if (err)
    return err;

  /* Always send key validation because RESET does not reset it.  */
  /* Use the validation mode if required.  We don't check for an error
     yet because this is a pretty fresh gpgsm features. */
  gpgsm_assuan_simple_command (gpgsm->assuan_ctx, 
                               (mode & GPGME_KEYLIST_MODE_VALIDATE)?
                               "OPTION with-validation=1":
                               "OPTION with-validation=0" ,
                               NULL, NULL);


  if (pattern && *pattern)
    {
      const char **pat = pattern;

      while (*pat)
	{
	  const char *patlet = *pat;

	  while (*patlet)
	    {
	      length++;
	      if (*patlet == '%' || *patlet == ' ' || *patlet == '+')
		length += 2;
	      patlet++;
	    }
	  pat++;
	  length++;
	}
    }
  line = malloc (length);
  if (!line)
    return gpg_error_from_errno (errno);
  if (secret_only)
    {
      strcpy (line, "LISTSECRETKEYS ");
      linep = &line[15];
    }
  else
    {
      strcpy (line, "LISTKEYS ");
      linep = &line[9];
    }

  if (pattern && *pattern)
    {
      while (*pattern)
	{
	  const char *patlet = *pattern;

	  while (*patlet)
	    {
	      switch (*patlet)
		{
		case '%':
		  *(linep++) = '%';
		  *(linep++) = '2';
		  *(linep++) = '5';
		  break;
		case ' ':
		  *(linep++) = '%';
		  *(linep++) = '2';
		  *(linep++) = '0';
		  break;
		case '+':
		  *(linep++) = '%';
		  *(linep++) = '2';
		  *(linep++) = 'B';
		  break;
		default:
		  *(linep++) = *patlet;
		  break;
		}
	      patlet++;
	    }
          any_pattern = 1;
          *linep++ = ' ';
	  pattern++;
	}
    }
  if (any_pattern)
    linep--;
  *linep = '\0';

  gpgsm_clear_fd (gpgsm, INPUT_FD);
  gpgsm_clear_fd (gpgsm, OUTPUT_FD);
  gpgsm_clear_fd (gpgsm, MESSAGE_FD);
  gpgsm->inline_data = NULL;

  err = start (gpgsm, line);
  free (line);
  return err;
}


static gpgme_error_t
gpgsm_sign (void *engine, gpgme_data_t in, gpgme_data_t out,
	    gpgme_sig_mode_t mode, int use_armor, int use_textmode,
	    int include_certs, gpgme_ctx_t ctx /* FIXME */)
{
  engine_gpgsm_t gpgsm = engine;
  gpgme_error_t err;
  char *assuan_cmd;
  int i;
  gpgme_key_t key;

  if (!gpgsm)
    return gpg_error (GPG_ERR_INV_VALUE);

  /* FIXME: This does not work as RESET does not reset it so we can't
     revert back to default.  */
  if (include_certs != GPGME_INCLUDE_CERTS_DEFAULT)
    {
      /* FIXME: Make sure that if we run multiple operations, that we
	 can reset any previously set value in case the default is
	 requested.  */

      if (asprintf (&assuan_cmd, "OPTION include-certs %i", include_certs) < 0)
	return gpg_error_from_errno (errno);
      err = gpgsm_assuan_simple_command (gpgsm->assuan_ctx, assuan_cmd,
                                         NULL, NULL);
      free (assuan_cmd);
      if (err)
	return err;
    }

  for (i = 0; (key = gpgme_signers_enum (ctx, i)); i++)
    {
      const char *s = key->subkeys ? key->subkeys->fpr : NULL;
      if (s && strlen (s) < 80)
	{
          char buf[100];

          strcpy (stpcpy (buf, "SIGNER "), s);
          err = gpgsm_assuan_simple_command (gpgsm->assuan_ctx, buf,
                                             gpgsm->status.fnc,
                                             gpgsm->status.fnc_value);
	}
      else
        err = gpg_error (GPG_ERR_INV_VALUE);
      gpgme_key_unref (key);
      if (err) 
        return err;
    }

  gpgsm->input_cb.data = in;
  err = gpgsm_set_fd (gpgsm, INPUT_FD, map_data_enc (gpgsm->input_cb.data));
  if (err)
    return err;
  gpgsm->output_cb.data = out;
  err = gpgsm_set_fd (gpgsm, OUTPUT_FD, use_armor ? "--armor"
		      : map_data_enc (gpgsm->output_cb.data));
  if (err)
    return err;
  gpgsm_clear_fd (gpgsm, MESSAGE_FD);
  gpgsm->inline_data = NULL;

  err = start (gpgsm, mode == GPGME_SIG_MODE_DETACH
	       ? "SIGN --detached" : "SIGN");
  return err;
}


static gpgme_error_t
gpgsm_verify (void *engine, gpgme_data_t sig, gpgme_data_t signed_text,
	      gpgme_data_t plaintext)
{
  engine_gpgsm_t gpgsm = engine;
  gpgme_error_t err;

  if (!gpgsm)
    return gpg_error (GPG_ERR_INV_VALUE);

  gpgsm->input_cb.data = sig;
  err = gpgsm_set_fd (gpgsm, INPUT_FD, map_data_enc (gpgsm->input_cb.data));
  if (err)
    return err;
  if (plaintext)
    {
      /* Normal or cleartext signature.  */
      gpgsm->output_cb.data = plaintext;
      err = gpgsm_set_fd (gpgsm, OUTPUT_FD, 0);
      gpgsm_clear_fd (gpgsm, MESSAGE_FD);
    }
  else
    {
      /* Detached signature.  */
      gpgsm->message_cb.data = signed_text;
      err = gpgsm_set_fd (gpgsm, MESSAGE_FD, 0);
      gpgsm_clear_fd (gpgsm, OUTPUT_FD);
    }
  gpgsm->inline_data = NULL;

  if (!err)
    err = start (gpgsm, "VERIFY");

  return err;
}


/* Send the GETAUDITLOG command.  The result is saved to a gpgme data
   object.  */
static gpgme_error_t
gpgsm_getauditlog (void *engine, gpgme_data_t output, unsigned int flags)
{
  engine_gpgsm_t gpgsm = engine;
  gpgme_error_t err = 0;

  if (!gpgsm || !output)
    return gpg_error (GPG_ERR_INV_VALUE);

#if USE_DESCRIPTOR_PASSING
  gpgsm->output_cb.data = output;
  err = gpgsm_set_fd (gpgsm, OUTPUT_FD, 0);
  if (err)
    return err;

  gpgsm_clear_fd (gpgsm, INPUT_FD);
  gpgsm_clear_fd (gpgsm, MESSAGE_FD);
  gpgsm->inline_data = NULL;
# define CMD  "GETAUDITLOG"
#else
  gpgsm_clear_fd (gpgsm, OUTPUT_FD);
  gpgsm_clear_fd (gpgsm, INPUT_FD);
  gpgsm_clear_fd (gpgsm, MESSAGE_FD);
  gpgsm->inline_data = output;
# define CMD  "GETAUDITLOG --data"
#endif

  err = start (gpgsm, (flags & GPGME_AUDITLOG_HTML)? CMD " --html" : CMD);

  return err;
}



static void
gpgsm_set_status_handler (void *engine, engine_status_handler_t fnc,
			  void *fnc_value) 
{
  engine_gpgsm_t gpgsm = engine;

  gpgsm->status.fnc = fnc;
  gpgsm->status.fnc_value = fnc_value;
}


static gpgme_error_t
gpgsm_set_colon_line_handler (void *engine, engine_colon_line_handler_t fnc,
			      void *fnc_value) 
{
  engine_gpgsm_t gpgsm = engine;

  gpgsm->colon.fnc = fnc;
  gpgsm->colon.fnc_value = fnc_value;
  gpgsm->colon.any = 0;
  return 0;
}


static void
gpgsm_set_io_cbs (void *engine, gpgme_io_cbs_t io_cbs)
{
  engine_gpgsm_t gpgsm = engine;
  gpgsm->io_cbs = *io_cbs;
}


static void
gpgsm_io_event (void *engine, gpgme_event_io_t type, void *type_data)
{
  engine_gpgsm_t gpgsm = engine;

  TRACE3 (DEBUG_ENGINE, "gpgme:gpgsm_io_event", gpgsm,
          "event %p, type %d, type_data %p",
          gpgsm->io_cbs.event, type, type_data);
  if (gpgsm->io_cbs.event)
    (*gpgsm->io_cbs.event) (gpgsm->io_cbs.event_priv, type, type_data);
}


struct engine_ops _gpgme_engine_ops_gpgsm =
  {
    /* Static functions.  */
    _gpgme_get_gpgsm_path,
    NULL,
    gpgsm_get_version,
    gpgsm_get_req_version,
    gpgsm_new,

    /* Member functions.  */
    gpgsm_release,
#if USE_DESCRIPTOR_PASSING
    gpgsm_reset,
#else
    NULL,			/* reset */
#endif
    gpgsm_set_status_handler,
    NULL,		/* set_command_handler */
    gpgsm_set_colon_line_handler,
    gpgsm_set_locale,
    gpgsm_decrypt,
    gpgsm_delete,
    NULL,		/* edit */
    gpgsm_encrypt,
    NULL,		/* encrypt_sign */
    gpgsm_export,
    gpgsm_export_ext,
    gpgsm_genkey,
    gpgsm_import,
    gpgsm_keylist,
    gpgsm_keylist_ext,
    gpgsm_sign,
    NULL,		/* trustlist */
    gpgsm_verify,
    gpgsm_getauditlog,
    NULL,               /* opassuan_transact */
    NULL,		/* conf_load */
    NULL,		/* conf_save */
    gpgsm_set_io_cbs,
    gpgsm_io_event,
    gpgsm_cancel,
    NULL		/* cancel_op */
  };
