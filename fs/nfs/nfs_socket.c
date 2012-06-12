/****************************************************************************
 * fs/nfs/nfs_socket.c
 *
 *   Copyright (C) 2012 Gregory Nutt. All rights reserved.
 *   Copyright (C) 2012 Jose Pablo Rojas Vargas. All rights reserved.
 *   Author: Jose Pablo Rojas Vargas <jrojas@nx-engineering.com>
 *           Gregory Nutt <gnutt@nuttx.org>
 *
 * Leveraged from OpenBSD:
 *
 *   copyright (c) 2004
 *   the regents of the university of michigan
 *   all rights reserved
 *
 * permission is granted to use, copy, create derivative works and redistribute
 * this software and such derivative works for any purpose, so long as the name
 * of the university of michigan is not used in any advertising or publicity
 * pertaining to the use or distribution of this software without specific,
 * written prior authorization.  if the above copyright notice or any other
 * identification of the university of michigan is included in any copy of any
 * portion of this software, then the disclaimer below must also be included.
 *
 * this software is provided as is, without representation from the university
 * of michigan as to its fitness for any purpose, and without warranty by the
 * university of michigan of any kind, either express or implied, including
 * without limitation the implied warranties of merchantability and fitness for
 * a particular purpose. the regents of the university of michigan shall not be
 * liable for any damages, including special, indirect, incidental, or
 * consequential damages, with respect to any claim arising out of or in
 * connection with the use of the software, even if it has been or is hereafter
 * advised of the possibility of such damages.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <sys/socket.h>
#include <queue.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <debug.h>
#include <nuttx/kmalloc.h>

#include "nfs_proto.h"
#include "nfs_mount.h"
#include "nfs.h"
#include "rpc.h"
#include "xdr_subs.h"
#include "nfs_socket.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Flag translations */

#define nfsmnt_to_rpcclnt(nf, rf, name) do \
  {                                        \
    if (nf & NFSMNT_##name)                \
      {                                    \
        rf |= RPCCLNT_##name;              \
      }                                    \
  } while(0)

/****************************************************************************
 * Private Variables
 ****************************************************************************/

static struct rpc_program nfs3_program =
{
  NFS_PROG, NFS_VER3, "NFSv3"
};

/****************************************************************************
 * Public Variables
 ****************************************************************************/

uint32_t nfs_true;
uint32_t nfs_false;
uint32_t nfs_xdrneg1;
int nfs_ticks;
struct nfsstats nfsstats;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void nfs_init(void)
{
   nfs_true = txdr_unsigned(TRUE);
   nfs_false = txdr_unsigned(FALSE);
   nfs_xdrneg1 = txdr_unsigned(-1);

   nfs_ticks = (CLOCKS_PER_SEC * NFS_TICKINTVL + 500) / 1000;
   if (nfs_ticks < 1)
    {
      nfs_ticks = 1;
    }

   rpcclnt_init();
}

int nfs_connect(struct nfsmount *nmp)
{
  struct rpcclnt *rpc;

  if (nmp == NULL)
    {
      return EFAULT;
    }

  /* Create an instance of the rpc state structure */

  rpc = (struct rpcclnt *)kzalloc(sizeof(struct rpcclnt));
  if (!rpc)
    {
      fdbg("ERROR: Failed to allocate rpc structure\n");
      return ENOMEM;
    }

  rpc->rc_prog = &nfs3_program;

  fvdbg("Connecting\n");

  /* Translate nfsmnt flags -> rpcclnt flags */

  rpc->rc_flag = 0;
  nfsmnt_to_rpcclnt(nmp->nm_flag, rpc->rc_flag, SOFT);
  nfsmnt_to_rpcclnt(nmp->nm_flag, rpc->rc_flag, INT);
  nfsmnt_to_rpcclnt(nmp->nm_flag, rpc->rc_flag, NOCONN);
  nfsmnt_to_rpcclnt(nmp->nm_flag, rpc->rc_flag, DUMBTIMR);

  rpc->rc_authtype   = RPCAUTH_NULL;      /* for now */
  rpc->rc_path       = nmp->nm_path;
  rpc->rc_name       = &nmp->nm_nam;

  rpc->rc_sotype     = nmp->nm_sotype;
  rpc->rc_soproto    = nmp->nm_soproto;
  rpc->rc_timeo      = nmp->nm_timeo;
  rpc->rc_retry      = nmp->nm_retry;

  /* V3 needs to use this */

  rpc->rc_proctlen   = 0;
  rpc->rc_proct      = NULL;

  nmp->nm_rpcclnt    = rpc;

  return rpcclnt_connect(rpc);
}

/* NFS disconnect. Clean up and unlink. */

void nfs_disconnect(struct nfsmount *nmp)
{
  rpcclnt_disconnect(nmp->nm_rpcclnt);
}

#ifdef CONFIG_NFS_TCPIP
void nfs_safedisconnect(struct nfsmount *nmp)
{
  rpcclnt_safedisconnect(nmp->nm_rpcclnt);
}
#endif

int nfs_request(struct nfsmount *nmp, int procnum,
                FAR const void *request, size_t reqlen,
                FAR void *response, size_t resplen)
{
  struct rpcclnt *clnt = nmp->nm_rpcclnt;
  struct nfs_reply_header replyh;
  int trylater_delay;
  int error;

tryagain:
  error = rpcclnt_request(clnt, procnum, nmp->nm_rpcclnt->rc_prog->prog_id,
                          nmp->nm_rpcclnt->rc_prog->prog_version, request, reqlen,
                          response, resplen);
  if (error != 0)
    {
      fdbg("ERROR: rpcclnt_request failed: %d\n", error);
      return error;
    }

  memcpy(&replyh, response, sizeof(struct nfs_reply_header));

  if (replyh.nfs_status != 0)
    {
      if (fxdr_unsigned(uint32_t, replyh.nfs_status) > 32)
        {
          error = EOPNOTSUPP;
        }
      else
        {
          /* NFS_ERRORS are the same as NuttX errno values */

          error = fxdr_unsigned(uint32_t, replyh.nfs_status);
        }

      return error;
    }

  if (replyh.rpc_verfi.authtype != 0)
    {
      error = fxdr_unsigned(int, replyh.rpc_verfi.authtype);

      if ((nmp->nm_flag & NFSMNT_NFSV3) && error == EAGAIN)
        {
          error = 0;
          trylater_delay *= NFS_TIMEOUTMUL;
          if (trylater_delay > NFS_MAXTIMEO)
            {
              trylater_delay = NFS_MAXTIMEO;
            }

          goto tryagain;
        }

      /* Check for a stale file handle.  We don't do anything special
       * a stale handle other than report a special debug error message.
       */

      if (error == ESTALE)
        {
          fdbg("ERROR %s: ESTALE on mount from server\n",
               nmp->nm_rpcclnt->rc_prog->prog_name);
        }
      else
        {
          fdbg("ERROR %s: unknown error %d from server\n",
               nmp->nm_rpcclnt->rc_prog->prog_name, error);
        }

      return error;
    }

  fvdbg("NFS_SUCCESS\n");
  return OK;
}

#undef COMP
#ifdef COMP

/* Terminate any outstanding RPCs. */

int nfs_nmcancelreqs(struct nfsmount *nmp)
{
  return 0;
}
#endif
