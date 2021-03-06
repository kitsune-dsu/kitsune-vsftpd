/*
 * Part of Very Secure FTPd
 * Licence: GPL
 * Author: Chris Evans
 * postprivparent.c
 *
 * This file contains all privileged parent services offered after logging
 * in. This includes e.g. chown() of uploaded files, issuing of port 20
 * sockets.
 */

#include "postprivparent.h"
#include "session.h"
#include "privops.h"
#include "privsock.h"
#include "utility.h"
#include "tunables.h"
#include "defs.h"
#include "sysutil.h"
#include "str.h"
#include "secutil.h"
#include "sysstr.h"
#include "sysdeputil.h"

/* Kitsune */
#include "twoprocess.h" /* needed for twoproc_handle_sigchld */

static void minimize_privilege(struct vsf_session* p_sess);
static void process_post_login_req(struct vsf_session* p_sess);
static void cmd_process_chown(struct vsf_session* p_sess);
static void cmd_process_get_data_sock(struct vsf_session* p_sess);

void
vsf_priv_parent_postlogin(struct vsf_session* p_sess)
{
	/* Kitsune */
	if (!kitsune_is_updating()) {
 		minimize_privilege(p_sess);
  } else {
    /* Set sigchld function pointer (normally done in twoprocess.c) */    
    vsf_sysutil_default_sig(kVSFSysUtilSigCHLD);
    vsf_sysutil_install_async_sighandler(kVSFSysUtilSigCHLD, twoproc_handle_sigchld);
  }
  /* End Kitsune */


	/* We're still here... */
  while (1)
  {
		/* Kitsune update point */
    kitsune_update("postprivparent.c");

    process_post_login_req(p_sess);
  }
}

static void
process_post_login_req(struct vsf_session* p_sess)
{
  char cmd;
  vsf_sysutil_unblock_sig(kVSFSysUtilSigCHLD);
  vsf_sysutil_kitsune_set_update_point(NULL);   /* Kitsune */  
  /* Blocks */
  cmd = priv_sock_get_cmd(p_sess->parent_fd);
  vsf_sysutil_block_sig(kVSFSysUtilSigCHLD);
  if (tunable_chown_uploads && cmd == PRIV_SOCK_CHOWN)
  {
    cmd_process_chown(p_sess);
  }
  else if (tunable_connect_from_port_20 && cmd == PRIV_SOCK_GET_DATA_SOCK)
  {
    cmd_process_get_data_sock(p_sess);
  }
  else
  {
    die("bad request in process_post_login_req");
  }
}

static void
minimize_privilege(struct vsf_session* p_sess)
{
  /* So, we logged in and forked a totally unprivileged child. Our job
   * now is to minimize the privilege we need in order to act as a helper
   * to the child.
   *
   * In some happy circumstances, we can exit and be done with root
   * altogether.
   */
  if (!p_sess->is_anonymous && tunable_session_support)
  {
    /* Need to hang around to update logs, utmp, wtmp etc. on logout.
     * Need to keep privs to do this. */
    return;
  }
  if (!tunable_chown_uploads && !tunable_connect_from_port_20 &&
      !tunable_max_per_ip && !tunable_max_clients)
  {
    /* Cool. We're outta here. */
    vsf_sysutil_exit(0);
  }
  {
    unsigned int caps = 0;
    struct mystr user_str = INIT_MYSTR;
    struct mystr dir_str = INIT_MYSTR;
    str_alloc_text(&user_str, tunable_nopriv_user);
    str_alloc_text(&dir_str, tunable_secure_chroot_dir);
    if (tunable_chown_uploads)
    {
      caps |= kCapabilityCAP_CHOWN;
    }
    if (tunable_connect_from_port_20)
    {
      caps |= kCapabilityCAP_NET_BIND_SERVICE;
    }
    vsf_secutil_change_credentials(&user_str, &dir_str, 0, caps,
                                   VSF_SECUTIL_OPTION_CHROOT);
    str_free(&user_str);
    str_free(&dir_str);
  }
}

static void
cmd_process_chown(struct vsf_session* p_sess)
{
  int the_fd = priv_sock_recv_fd(p_sess->parent_fd);
  vsf_privop_do_file_chown(p_sess, the_fd);
  vsf_sysutil_close(the_fd);
  priv_sock_send_result(p_sess->parent_fd, PRIV_SOCK_RESULT_OK);
}

static void
cmd_process_get_data_sock(struct vsf_session* p_sess)
{
  int sock_fd = vsf_privop_get_ftp_port_sock(p_sess);
  priv_sock_send_result(p_sess->parent_fd, PRIV_SOCK_RESULT_OK);
  priv_sock_send_fd(p_sess->parent_fd, sock_fd);
  vsf_sysutil_close(sock_fd);
}

