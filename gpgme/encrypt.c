/* encrypt.c - Encrypt function.
   Copyright (C) 2000 Werner Koch (dd9jn)
   Copyright (C) 2001, 2002, 2003 g10 Code GmbH

   This file is part of GPGME.
 
   GPGME is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
 
   GPGME is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
 
   You should have received a copy of the GNU General Public License
   along with GPGME; if not, write to the Free Software Foundation,
   Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#if HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "gpgme.h"
#include "context.h"
#include "ops.h"


typedef struct
{
  struct _gpgme_op_encrypt_result result;

  /* A pointer to the next pointer of the last invalid recipient in
     the list.  This makes appending new invalid recipients painless
     while preserving the order.  */
  gpgme_invalid_user_id_t *lastp;
} *op_data_t;


static void
release_op_data (void *hook)
{
  op_data_t opd = (op_data_t) hook;
  gpgme_invalid_user_id_t invalid_recipient = opd->result.invalid_recipients;

  while (invalid_recipient)
    {
      gpgme_invalid_user_id_t next = invalid_recipient->next;
      free (invalid_recipient->id);
      invalid_recipient = next;
    }
}


gpgme_encrypt_result_t
gpgme_op_encrypt_result (gpgme_ctx_t ctx)
{
  op_data_t opd;
  gpgme_error_t err;

  err = _gpgme_op_data_lookup (ctx, OPDATA_ENCRYPT, (void **) &opd, -1, NULL);
  if (err || !opd)
    return NULL;

  return &opd->result;
}


gpgme_error_t
_gpgme_encrypt_status_handler (void *priv, gpgme_status_code_t code, char *args)
{
  gpgme_ctx_t ctx = (gpgme_ctx_t) priv;
  gpgme_error_t err;
  op_data_t opd;

  err = _gpgme_op_data_lookup (ctx, OPDATA_ENCRYPT, (void **) &opd,
			       -1, NULL);
  if (err)
    return err;

  switch (code)
    {
    case GPGME_STATUS_EOF:
      if (opd->result.invalid_recipients)
	return GPGME_Invalid_UserID;
      break;

    case GPGME_STATUS_INV_RECP:
      err = _gpgme_parse_inv_userid (args, opd->lastp);
      if (err)
	return err;

      opd->lastp = &(*opd->lastp)->next;
      break;

    case GPGME_STATUS_NO_RECP:
      /* Should not happen, because we require at least one recipient.  */
      return GPGME_No_UserID;

    default:
      break;
    }
  return 0;
}


gpgme_error_t
_gpgme_encrypt_sym_status_handler (void *priv, gpgme_status_code_t code,
				   char *args)
{
  return _gpgme_passphrase_status_handler (priv, code, args);
}


gpgme_error_t
_gpgme_op_encrypt_init_result (gpgme_ctx_t ctx)
{
  gpgme_error_t err;
  op_data_t opd;

  err = _gpgme_op_data_lookup (ctx, OPDATA_ENCRYPT, (void **) &opd,
			       sizeof (*opd), release_op_data);
  if (err)
    return err;
  opd->lastp = &opd->result.invalid_recipients;
  return 0;
}


static gpgme_error_t
encrypt_start (gpgme_ctx_t ctx, int synchronous, gpgme_recipients_t recp,
	       gpgme_data_t plain, gpgme_data_t cipher)
{
  gpgme_error_t err;
  int symmetric = 0;

  err = _gpgme_op_reset (ctx, synchronous);
  if (err)
    return err;

  err = _gpgme_op_encrypt_init_result (ctx);
  if (err)
    return err;

  if (!recp)
    symmetric = 1;
  else if (gpgme_recipients_count (recp) == 0)
    return GPGME_No_UserID;

  if (!plain)
    return GPGME_No_Data;
  if (!cipher)
    return GPGME_Invalid_Value;

  if (symmetric && ctx->passphrase_cb)
    {
      /* Symmetric encryption requires a passphrase.  */
      err = _gpgme_engine_set_command_handler (ctx->engine,
					       _gpgme_passphrase_command_handler,
					       ctx, NULL);
      if (err)
	return err;
    }

  _gpgme_engine_set_status_handler (ctx->engine,
				    symmetric
				    ? _gpgme_encrypt_sym_status_handler
				    : _gpgme_encrypt_status_handler,
				    ctx);

  return _gpgme_engine_op_encrypt (ctx->engine, recp, plain, cipher,
				   ctx->use_armor);
}


gpgme_error_t
gpgme_op_encrypt_start (gpgme_ctx_t ctx, gpgme_recipients_t recp, gpgme_data_t plain,
			gpgme_data_t cipher)
{
  return encrypt_start (ctx, 0, recp, plain, cipher);
}


/* Encrypt plaintext PLAIN within CTX for the recipients RECP and
   store the resulting ciphertext in CIPHER.  */
gpgme_error_t
gpgme_op_encrypt (gpgme_ctx_t ctx, gpgme_recipients_t recp,
		  gpgme_data_t plain, gpgme_data_t cipher)
{
  int err = encrypt_start (ctx, 1, recp, plain, cipher);
  if (!err)
    err = _gpgme_wait_one (ctx);
  return err;
}
