/*
 * Part of Very Secure FTPd
 * Licence: GPL v2
 * Author: Chris Evans
 * ftpdataio.c
 *
 * Code to handle FTP data connections. This includes both PORT (server
 * connects) and PASV (client connects) modes of data transfer. This
 * includes sends and receives, files and directories.
 */

#include "ftpdataio.h"
#include "session.h"
#include "ftpcmdio.h"
#include "ftpcodes.h"
#include "utility.h"
#include "tunables.h"
#include "defs.h"
#include "str.h"
#include "strlist.h"
#include "sysutil.h"
#include "logging.h"
#include "secbuf.h"
#include "sysstr.h"
#include "sysdeputil.h"
#include "ascii.h"
#include "oneprocess.h"
#include "twoprocess.h"
#include "ls.h"
#include "ssl.h"
#include "readwrite.h"

static void init_data_sock_params(struct vsf_session* p_sess, int sock_fd);
static filesize_t calc_num_send(int file_fd, filesize_t init_offset);
static struct vsf_transfer_ret do_file_send_sendfile(
  struct vsf_session* p_sess, int net_fd, int file_fd,
  filesize_t curr_file_offset, filesize_t bytes_to_send);
static struct vsf_transfer_ret do_file_send_rwloop(
  struct vsf_session* p_sess, int file_fd, int is_ascii);
static struct vsf_transfer_ret do_file_recv(
  struct vsf_session* p_sess, int file_fd, int is_ascii);
static void handle_sigalrm(void* p_private);
static void start_data_alarm(struct vsf_session* p_sess);
static void handle_io(int retval, int fd, void* p_private);
static int transfer_dir_internal(
  struct vsf_session* p_sess, int is_control, struct vsf_sysutil_dir* p_dir,
  const struct mystr* p_base_dir_str, const struct mystr* p_option_str,
  const struct mystr* p_filter_str, int is_verbose);
static int write_dir_list(struct vsf_session* p_sess,
                          struct mystr_list* p_dir_list,
                          enum EVSFRWTarget target);
static unsigned int get_chunk_size();

void
vsf_ftpdataio_dispose_transfer_fd(struct vsf_session* p_sess)
{
  int retval;
  if (p_sess->data_fd == -1)
  {
    bug("no data descriptor in vsf_ftpdataio_dispose_transfer_fd");
  }
  /* Clear the data connection alarm */
  vsf_sysutil_clear_alarm();
  vsf_sysutil_uninstall_io_handler();
  if (p_sess->p_data_ssl != 0)
  {
    ssl_data_close(p_sess);
  }
  /* This close() blocks because we set SO_LINGER */
  retval = vsf_sysutil_close_failok(p_sess->data_fd);
  if (vsf_sysutil_retval_is_error(retval))
  {
    /* Do it again without blocking. */
    vsf_sysutil_deactivate_linger_failok(p_sess->data_fd);
    (void) vsf_sysutil_close_failok(p_sess->data_fd);
  }
  p_sess->data_fd = -1;
}

int
vsf_ftpdataio_get_pasv_fd(struct vsf_session* p_sess)
{
  int remote_fd;
  struct vsf_sysutil_sockaddr* p_accept_addr = 0;
  vsf_sysutil_sockaddr_alloc(&p_accept_addr);
  remote_fd = vsf_sysutil_accept_timeout(p_sess->pasv_listen_fd, p_accept_addr,
                                         tunable_accept_timeout);
  if (vsf_sysutil_retval_is_error(remote_fd))
  {
    vsf_cmdio_write(p_sess, FTP_BADSENDCONN,
                    "Failed to establish connection.");
    vsf_sysutil_sockaddr_clear(&p_accept_addr);
    return remote_fd;
  }
  /* SECURITY:
   * Reject the connection if it wasn't from the same IP as the
   * control connection.
   */
  if (!tunable_pasv_promiscuous)
  {
    if (!vsf_sysutil_sockaddr_addr_equal(p_sess->p_remote_addr, p_accept_addr))
    {
      vsf_cmdio_write(p_sess, FTP_BADSENDCONN, "Security: Bad IP connecting.");
      vsf_sysutil_close(remote_fd);
      vsf_sysutil_sockaddr_clear(&p_accept_addr);
      return -1;
    }
  }
  vsf_sysutil_sockaddr_clear(&p_accept_addr);
  init_data_sock_params(p_sess, remote_fd);
  return remote_fd;
}

int
vsf_ftpdataio_get_port_fd(struct vsf_session* p_sess)
{
  int retval;
  int remote_fd;
  if (tunable_connect_from_port_20)
  {
    if (tunable_one_process_model)
    {
      remote_fd = vsf_one_process_get_priv_data_sock(p_sess);
    }
    else
    {
      remote_fd = vsf_two_process_get_priv_data_sock(p_sess);
    }
  }
  else
  {
    remote_fd = vsf_sysutil_get_ipsock(p_sess->p_port_sockaddr);
    if (vsf_sysutil_sockaddr_same_family(p_sess->p_port_sockaddr,
                                         p_sess->p_local_addr))
    {
      static struct vsf_sysutil_sockaddr* s_p_addr;
      vsf_sysutil_sockaddr_clone(&s_p_addr, p_sess->p_local_addr);
      retval = vsf_sysutil_bind(remote_fd, s_p_addr);
      if (retval != 0)
      {
        die("vsf_sysutil_bind");
      }
    }
  }
  retval = vsf_sysutil_connect_timeout(remote_fd, p_sess->p_port_sockaddr,
                                       tunable_connect_timeout);
  if (vsf_sysutil_retval_is_error(retval))
  {
    vsf_cmdio_write(p_sess, FTP_BADSENDCONN,
                    "Failed to establish connection.");
    vsf_sysutil_close(remote_fd);
    return -1;
  }
  init_data_sock_params(p_sess, remote_fd);
  return remote_fd;
}

int
vsf_ftpdataio_post_mark_connect(struct vsf_session* p_sess)
{
  if (p_sess->data_use_ssl)
  {
    if (!ssl_accept(p_sess, p_sess->data_fd))
    {
      vsf_cmdio_write(
        p_sess, FTP_DATATLSBAD, "Secure connection negotiation failed.");
      return 0;
    }
  }
  return 1;
}

static void
handle_sigalrm(void* p_private)
{
  struct vsf_session* p_sess = (struct vsf_session*) p_private;
  if (!p_sess->data_progress)
  {
    vsf_cmdio_write_noblock(p_sess, FTP_DATA_TIMEOUT,
                            "Data timeout. Reconnect. Sorry.");
    vsf_sysutil_exit(0);
  }
  p_sess->data_progress = 0;
  start_data_alarm(p_sess);
}

void
start_data_alarm(struct vsf_session* p_sess)
{
  if (tunable_data_connection_timeout > 0)
  {
    vsf_sysutil_install_sighandler(kVSFSysUtilSigALRM, handle_sigalrm, p_sess);
    vsf_sysutil_set_alarm(tunable_data_connection_timeout);
  }
}

static void
init_data_sock_params(struct vsf_session* p_sess, int sock_fd)
{
  if (p_sess->data_fd != -1)
  {
    bug("data descriptor still present in init_data_sock_params");
  }
  p_sess->data_fd = sock_fd;
  p_sess->data_progress = 0;
  vsf_sysutil_activate_keepalive(sock_fd);
  /* And in the vague hope it might help... */
  vsf_sysutil_set_iptos_throughput(sock_fd);
  /* Set up lingering, so that we wait for all data to transfer, and report
   * more accurate transfer rates.
   */
  vsf_sysutil_activate_linger(sock_fd);
  /* Start the timeout monitor */
  vsf_sysutil_install_io_handler(handle_io, p_sess);
  start_data_alarm(p_sess);
}

static void
handle_io(int retval, int fd, void* p_private)
{
  long curr_sec;
  long curr_usec;
  unsigned int bw_rate;
  double elapsed;
  double pause_time;
  double rate_ratio;
  struct vsf_session* p_sess = (struct vsf_session*) p_private;
  if (p_sess->data_fd != fd || vsf_sysutil_retval_is_error(retval) ||
      retval == 0)
  {
    return;
  }
  /* Note that the session hasn't stalled, i.e. don't time it out */
  p_sess->data_progress = 1;
  /* Apply bandwidth quotas via a little pause, if necessary */
  if (p_sess->bw_rate_max == 0)
  {
    return;
  }
  /* Calculate bandwidth rate */
  vsf_sysutil_update_cached_time();
  curr_sec = vsf_sysutil_get_cached_time_sec();
  curr_usec = vsf_sysutil_get_cached_time_usec();
  elapsed = (double) (curr_sec - p_sess->bw_send_start_sec);
  elapsed += (double) (curr_usec - p_sess->bw_send_start_usec) /
             (double) 1000000;
  if (elapsed == (double) 0)
  {
    elapsed = (double) 0.01;
  }
  bw_rate = (unsigned int) ((double) retval / elapsed);
  if (bw_rate <= p_sess->bw_rate_max)
  {
    p_sess->bw_send_start_sec = curr_sec;
    p_sess->bw_send_start_usec = curr_usec;
    return;
  }
  /* Tut! Rate exceeded, calculate a pause to bring things back into line */
  rate_ratio = (double) bw_rate / (double) p_sess->bw_rate_max;
  pause_time = (rate_ratio - (double) 1) * elapsed;
  vsf_sysutil_sleep(pause_time);
  vsf_sysutil_update_cached_time();
  p_sess->bw_send_start_sec = vsf_sysutil_get_cached_time_sec();
  p_sess->bw_send_start_usec = vsf_sysutil_get_cached_time_usec();
}

int
vsf_ftpdataio_transfer_dir(struct vsf_session* p_sess, int is_control,
                           struct vsf_sysutil_dir* p_dir,
                           const struct mystr* p_base_dir_str,
                           const struct mystr* p_option_str,
                           const struct mystr* p_filter_str,
                           int is_verbose)
{
  return transfer_dir_internal(p_sess, is_control, p_dir, p_base_dir_str,
                               p_option_str, p_filter_str, is_verbose);
}

static int
transfer_dir_internal(struct vsf_session* p_sess, int is_control,
                      struct vsf_sysutil_dir* p_dir,
                      const struct mystr* p_base_dir_str,
                      const struct mystr* p_option_str,
                      const struct mystr* p_filter_str,
                      int is_verbose)
{
  struct mystr_list dir_list = INIT_STRLIST;
  struct mystr_list subdir_list = INIT_STRLIST;
  struct mystr dir_prefix_str = INIT_MYSTR;
  struct mystr_list* p_subdir_list = 0;
  struct str_locate_result loc_result = str_locate_char(p_option_str, 'R');
  int failed = 0;
  enum EVSFRWTarget target = kVSFRWData;
  if (is_control)
  {
    target = kVSFRWControl;
  }
  if (loc_result.found && tunable_ls_recurse_enable)
  {
    p_subdir_list = &subdir_list;
  }
  vsf_ls_populate_dir_list(&dir_list, p_subdir_list, p_dir, p_base_dir_str,
                           p_option_str, p_filter_str, is_verbose);
  if (p_subdir_list)
  {
    int retval;
    str_copy(&dir_prefix_str, p_base_dir_str);
    str_append_text(&dir_prefix_str, ":\r\n");
    retval = ftp_write_str(p_sess, &dir_prefix_str, target);
    if (retval != 0)
    {
      failed = 1;
    }
  }
  if (!failed)
  {
    failed = write_dir_list(p_sess, &dir_list, target);
  }
  /* Recurse into the subdirectories if required... */
  if (!failed)
  {
    struct mystr sub_str = INIT_MYSTR;
    unsigned int num_subdirs = str_list_get_length(&subdir_list);
    unsigned int subdir_index;
    for (subdir_index = 0; subdir_index < num_subdirs; subdir_index++)
    {
      int retval;
      struct vsf_sysutil_dir* p_subdir;
      const struct mystr* p_subdir_str = 
        str_list_get_pstr(&subdir_list, subdir_index);
      if (str_equal_text(p_subdir_str, ".") ||
          str_equal_text(p_subdir_str, ".."))
      {
        continue;
      }
      str_copy(&sub_str, p_base_dir_str);
      str_append_char(&sub_str, '/');
      str_append_str(&sub_str, p_subdir_str);
      p_subdir = str_opendir(&sub_str);
      if (p_subdir == 0)
      {
        /* Unreadable, gone missing, etc. - no matter */
        continue;
      }
      str_alloc_text(&dir_prefix_str, "\r\n");
      retval = ftp_write_str(p_sess, &dir_prefix_str, target);
      if (retval != 0)
      {
        failed = 1;
        break;
      }
      retval = transfer_dir_internal(p_sess, is_control, p_subdir, &sub_str,
                                     p_option_str, p_filter_str, is_verbose);
      vsf_sysutil_closedir(p_subdir);
      if (retval != 0)
      {
        failed = 1;
        break;
      }
    }
    str_free(&sub_str);
  }
  str_list_free(&dir_list);
  str_list_free(&subdir_list);
  str_free(&dir_prefix_str);
  if (!failed)
  {
    return 0;
  }
  else
  {
    return -1;
  }
}

/* XXX - really, this should be refactored into a "buffered writer" object */
static int
write_dir_list(struct vsf_session* p_sess, struct mystr_list* p_dir_list,
               enum EVSFRWTarget target)
{
  /* This function writes out a list of strings to the client, over the
   * data socket. We now coalesce the strings into fewer write() syscalls,
   * which saved 33% CPU time writing a large directory.
   */
  int retval = 0;
  unsigned int dir_index_max = str_list_get_length(p_dir_list);
  unsigned int dir_index;
  struct mystr buf_str = INIT_MYSTR;
  str_reserve(&buf_str, VSFTP_DIR_BUFSIZE);
  for (dir_index = 0; dir_index < dir_index_max; dir_index++)
  {
    str_append_str(&buf_str, str_list_get_pstr(p_dir_list, dir_index));
    if (dir_index == dir_index_max - 1 ||
        str_getlen(&buf_str) +
          str_getlen(str_list_get_pstr(p_dir_list, dir_index + 1)) >
            VSFTP_DIR_BUFSIZE)
    {
      /* Writeout needed - we're either at the end, or we filled the buffer */
      int writeret = ftp_write_str(p_sess, &buf_str, target);
      if (writeret != 0)
      {
        retval = 1;
        break;
      }
      str_empty(&buf_str);
    }
  }
  str_free(&buf_str);
  return retval;
}

struct vsf_transfer_ret
vsf_ftpdataio_transfer_file(struct vsf_session* p_sess, int remote_fd,
                            int file_fd, int is_recv, int is_ascii)
{
  if (!is_recv)
  {
    if (is_ascii || p_sess->data_use_ssl)
    {
      return do_file_send_rwloop(p_sess, file_fd, is_ascii);
    }
    else
    {
      filesize_t curr_offset = vsf_sysutil_get_file_offset(file_fd);
      filesize_t num_send = calc_num_send(file_fd, curr_offset);
      return do_file_send_sendfile(
        p_sess, remote_fd, file_fd, curr_offset, num_send);
    }
  }
  else
  {
    return do_file_recv(p_sess, file_fd, is_ascii);
  }
}

static struct vsf_transfer_ret
do_file_send_rwloop(struct vsf_session* p_sess, int file_fd, int is_ascii)
{
  static char* p_readbuf;
  static char* p_asciibuf;
  struct vsf_transfer_ret ret_struct = { 0, 0 };
  unsigned int chunk_size = get_chunk_size();
  char* p_writefrom_buf;
  if (p_readbuf == 0)
  {
    /* NOTE!! * 2 factor because we can double the data by doing our ASCII
     * linefeed mangling
     */
    vsf_secbuf_alloc(&p_asciibuf, VSFTP_DATA_BUFSIZE * 2);
    vsf_secbuf_alloc(&p_readbuf, VSFTP_DATA_BUFSIZE);
  }
  if (is_ascii)
  {
    p_writefrom_buf = p_asciibuf;
  }
  else
  {
    p_writefrom_buf = p_readbuf;
  }
  while (1)
  {
    unsigned int num_to_write;
    int retval = vsf_sysutil_read(file_fd, p_readbuf, chunk_size);
    if (vsf_sysutil_retval_is_error(retval))
    {
      vsf_cmdio_write(p_sess, FTP_BADSENDFILE, "Failure reading local file.");
      ret_struct.retval = -1;
      return ret_struct;
    }
    else if (retval == 0)
    {
      /* Success - cool */
      vsf_cmdio_write(p_sess, FTP_TRANSFEROK, "File send OK.");
      return ret_struct;
    }
    if (is_ascii)
    {
      num_to_write = vsf_ascii_bin_to_ascii(p_readbuf, p_asciibuf,
                                            (unsigned int) retval);
    }
    else
    {
      num_to_write = (unsigned int) retval;
    }
    retval = ftp_write_data(p_sess, p_writefrom_buf, num_to_write);
    if (vsf_sysutil_retval_is_error(retval) ||
        (unsigned int) retval != num_to_write)
    {
      vsf_cmdio_write(p_sess, FTP_BADSENDNET,
                      "Failure writing network stream.");
      ret_struct.retval = -1;
      return ret_struct;
    }
    ret_struct.transferred += (unsigned int) retval;
  }
}

static struct vsf_transfer_ret
do_file_send_sendfile(struct vsf_session* p_sess, int net_fd, int file_fd,
                      filesize_t curr_file_offset, filesize_t bytes_to_send)
{
  int retval;
  unsigned int chunk_size = 0;
  struct vsf_transfer_ret ret_struct = { 0, 0 };
  filesize_t init_file_offset = curr_file_offset;
  filesize_t bytes_sent;
  if (p_sess->bw_rate_max)
  {
    chunk_size = get_chunk_size();
  }
  /* Just because I can ;-) */
  retval = vsf_sysutil_sendfile(net_fd, file_fd, &curr_file_offset,
                                bytes_to_send, chunk_size);
  bytes_sent = curr_file_offset - init_file_offset;
  ret_struct.transferred = bytes_sent;
  if (vsf_sysutil_retval_is_error(retval))
  {
    vsf_cmdio_write(p_sess, FTP_BADSENDNET, "Failure writing network stream.");
    ret_struct.retval = -1;
    return ret_struct;
  }
  else if (bytes_sent != bytes_to_send)
  {
    vsf_cmdio_write(p_sess, FTP_BADSENDFILE,
                    "Failure writing network stream.");
    ret_struct.retval = -1;
    return ret_struct;
  }
  vsf_cmdio_write(p_sess, FTP_TRANSFEROK, "File send OK.");
  return ret_struct; 
}

static filesize_t
calc_num_send(int file_fd, filesize_t init_offset)
{
  static struct vsf_sysutil_statbuf* s_p_statbuf;
  filesize_t bytes_to_send;
  /* Work out how many bytes to send based on file size minus current offset */
  vsf_sysutil_fstat(file_fd, &s_p_statbuf);
  bytes_to_send = vsf_sysutil_statbuf_get_size(s_p_statbuf);
  if (init_offset < 0 || bytes_to_send < 0)
  {
    die("calc_num_send: negative file offset or send count");
  }
  /* Don't underflow if some bonehead sets a REST greater than the file size */
  if (init_offset > bytes_to_send)
  {
    bytes_to_send = 0;
  }
  else
  {
    bytes_to_send -= init_offset;
  }
  return bytes_to_send;
}

static struct vsf_transfer_ret
do_file_recv(struct vsf_session* p_sess, int file_fd, int is_ascii)
{
  static char* p_recvbuf;
  unsigned int num_to_write;
  struct vsf_transfer_ret ret_struct = { 0, 0 };
  unsigned int chunk_size = get_chunk_size();
  if (p_recvbuf == 0)
  {
    vsf_secbuf_alloc(&p_recvbuf, VSFTP_DATA_BUFSIZE);
  }
  while (1)
  {
    int retval = ftp_read_data(p_sess, p_recvbuf, chunk_size);
    if (vsf_sysutil_retval_is_error(retval))
    {
      vsf_cmdio_write(p_sess, FTP_BADSENDNET,
                      "Failure reading network stream.");
      ret_struct.retval = -1;
      return ret_struct;
    }
    else if (retval == 0)
    {
      /* Transfer done, nifty */
      vsf_cmdio_write(p_sess, FTP_TRANSFEROK, "File receive OK.");
      return ret_struct;
    }
    num_to_write = (unsigned int) retval;
    ret_struct.transferred += num_to_write;
    if (is_ascii)
    {
      /* Handle ASCII conversion if we have to. Note that using the same
       * buffer for source and destination is safe, because the ASCII ->
       * binary transform only ever results in a smaller file.
       */
      num_to_write = vsf_ascii_ascii_to_bin(p_recvbuf, p_recvbuf,
                                            num_to_write);
    }
    retval = vsf_sysutil_write_loop(file_fd, p_recvbuf, num_to_write);
    if (vsf_sysutil_retval_is_error(retval) ||
        (unsigned int) retval != num_to_write)
    {
      vsf_cmdio_write(p_sess, FTP_BADSENDFILE,
                      "Failure writing to local file.");
      ret_struct.retval = -1;
      return ret_struct;
    }
  }
}

static unsigned int
get_chunk_size()
{
  unsigned int ret = VSFTP_DATA_BUFSIZE;
  if (tunable_trans_chunk_size < VSFTP_DATA_BUFSIZE &&
      tunable_trans_chunk_size > 0)
  {
    ret = tunable_trans_chunk_size;
    if (ret < 4096)
    {
      ret = 4096;
    }
  }
  return ret;
}

