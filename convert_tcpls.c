#include <libsyscall_intercept_hook_point.h>
#include <syscall.h>
#include <string.h>
#include <openssl/pem.h>
#include <openssl/engine.h>

#include <log.h>

#include "picotls.h"
#include "picotcpls.h"
#include "picotls/openssl.h"

#include "convert_tcpls.h"

#include <unistd.h>
#include <fcntl.h>

const char * cert = "../assets/server.crt";
const char * cert_key = "../assets/server.key";

/* 10 MiB buf */
#define TCPLS_BUFFER_SIZE 10485760
static ptls_buffer_t tcpls_buf;
static int tcpls_buf_read_offset = 0;


static ptls_context_t *ctx;
//static tcpls_t *tcpls;
static list_t *tcpls_con_l = NULL;
static list_t *ours_addr_list = NULL;

static int handle_connection_event(tcpls_event_t event, int socket, int transportid, void
    *cbdata);
static int handle_stream_event(tcpls_t *tcpls, tcpls_event_t
      event, streamid_t streamid, int transportid, void *cbdata);
static int handle_client_connection_event(tcpls_event_t event, int socket, int
        transportid, void *cbdata);
static int handle_client_stream_event(tcpls_t *tcpls, tcpls_event_t event, streamid_t
        streamid, int transportid, void *cbdata);

static void shift_buffer(ptls_buffer_t *buf, size_t delta) {
  if (delta != 0) {
    assert(delta <= buf->off);
    if (delta != buf->off)
      memmove(buf->base, buf->base + delta, buf->off - delta);
    buf->off -= delta;
  }
}


int set_blocking_mode(int socket, bool is_blocking)
{
    int ret = 0;
    int flags = syscall_no_intercept(SYS_fcntl, socket, F_GETFL, 0);
    if ((flags & O_NONBLOCK) && !is_blocking) {
      log_debug("set_blocking_mode(): socket %d was already in non-blocking mode", socket);
      return ret;
    }
    if (!(flags & O_NONBLOCK) && is_blocking) {
      log_debug("set_blocking_mode(): socket was already in blocking mode", socket);
      return ret;
    }
   if (flags == -1) return 0;
   flags = is_blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
   is_blocking ? log_debug("Set socket %d in blocking mode", socket) : log_debug("set socket %d in non-blocking mode", socket);
   return !syscall_no_intercept(SYS_fcntl, socket, F_SETFL, flags);
}


/*************************EVENT CALLBACKS**********************/

static int handle_connection_event(tcpls_event_t event, int socket, int
    transportid, void *cbdata) {
  list_t *conntcpls = (list_t*) cbdata;
  struct tcpls_con *con;
  if(!conntcpls){
    return 0;
  }
  switch(event){
    case CONN_OPENED:
      log_debug("connection_event_call_back: CONNECTION OPENED %d", socket);
      for (int i = 0; i < conntcpls->size; i++) {
        con = list_get(conntcpls, i);
        if (con->sd == socket) {
          con->transportid = transportid;
          con->state = CONNECTED;
          break;
        }
      }
      break;
    case CONN_CLOSED:
      log_debug("connection_event_call_back: CONNECTION CLOSED %d",socket);
      for (int i = 0; i < conntcpls->size; i++) {
        con = list_get(conntcpls, i);
        if (con->sd == socket) {
          list_remove(conntcpls, con);
          break;
        }
      }
      break;
    default:
      break;
  }
  return 0;
}

static int handle_client_connection_event(tcpls_event_t event, int socket, int
    transportid, void *cbdata) {
  list_t *conntcpls = (list_t*) cbdata;
  struct tcpls_con *con = list_get(conntcpls, 0);
  assert(con);
  switch (event) {
    case CONN_CLOSED:
      log_debug("connection_event_call_back: Received a CONN_CLOSED; removing\
          the socket %d transportid %d", socket, transportid);
      break;
    case CONN_OPENED:
      log_debug("connection_event_call_back: Received a CONN_OPENED; adding the\
          socket descriptor %d transport id %d", socket, transportid);
      /** uncessary in this simple intercept */
      con->sd = socket;
      con->transportid = transportid;
      break;
    default: break;
  }
  return 0;
}


static int handle_client_stream_event(tcpls_t *tcpls, tcpls_event_t event, streamid_t streamid,
    int transportid, void *cbdata) {
  list_t *conntcpls = (list_t*) cbdata;
  struct tcpls_con *con = list_get(conntcpls, 0);
  assert(con);
  switch (event) {
    case STREAM_OPENED:
      log_debug("stream_event_call_back: Handling stream_opened callback\
          transportid :%d:%p", transportid, tcpls);
      con->streamid = streamid;
      break;
    case STREAM_CLOSED:
      log_debug("stream_event_call_back: Handling stream_closed callback %d:%p",
          transportid, tcpls);
      break;
    default: break;
  }
  return 0;
}

static int handle_stream_event(tcpls_t *tcpls, tcpls_event_t event,
  streamid_t streamid, int transportid, void *cbdata) {
  list_t *conntcpls = (list_t*) cbdata;
  struct tcpls_con *con;
  assert(conntcpls);
  switch(event){
    case STREAM_OPENED:
      log_debug("stream_event_call_back: STREAM OPENED streamid :%d transportid\
          :%d", streamid, transportid);
      for (int i = 0; i < conntcpls->size; i++) {
        con = list_get(conntcpls, i);
        if (con->tcpls == tcpls && con->transportid == transportid) {
          con->streamid = streamid;
          con->is_primary = 1;
        }
      }
      break;
    case STREAM_CLOSED:
      log_debug("stream_event_call_back: STREAM CLOSED streamid :%d transportid\
          :%d", streamid, transportid);
      for (int i = 0; i < conntcpls->size; i++) {
        con = list_get(conntcpls, i);
        if ( con->tcpls == tcpls && con->transportid == transportid) {
          log_debug("We're stopping to write on the connection linked to\
              transportid %d %d\n", transportid, con->sd);
          con->is_primary = 0;
        }
      }
      break;
    default:
      break;
  }
  return 0;
}

static int load_private_key(ptls_context_t *ctx, const char *fn){
  static ptls_openssl_sign_certificate_t sc;
  FILE *fp;
  EVP_PKEY *pkey;
  if ((fp = fopen(fn, "rb")) == NULL) {
    log_debug("failed to open file:%s:%s\n", fn, strerror(errno));
    return -1;
  }
  pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
  fclose(fp);
  if (pkey == NULL) {
    log_debug("failed to read private key from file:%s\n", fn);
    return -1;
  }
  ptls_openssl_init_sign_certificate(&sc, pkey);
  EVP_PKEY_free(pkey);
  ctx->sign_certificate = &sc.super;
  return 0;
}

static ptls_context_t *set_tcpls_ctx_options(int is_server){
  if(ctx)
    goto done;
  /*ERR_load_crypto_strings();*/
  /*OpenSSL_add_all_algorithms();*/
  ctx = (ptls_context_t *)malloc(sizeof(*ctx));
  memset(ctx, 0, sizeof(ptls_context_t));
  ctx->support_tcpls_options = 1;
  ctx->random_bytes = ptls_openssl_random_bytes;
  ctx->key_exchanges = ptls_openssl_key_exchanges;
  ctx->cipher_suites = ptls_openssl_cipher_suites;
  ctx->get_time = &ptls_get_time;
  if(!tcpls_con_l)
    tcpls_con_l = new_list(sizeof(struct tcpls_con),2);
  if(!ours_addr_list)
    ours_addr_list = new_list(sizeof(struct sockaddr), 2);
  ctx->cb_data = tcpls_con_l;
  if(!is_server){
    ctx->send_change_cipher_spec = 1;
    ctx->stream_event_cb = &handle_client_stream_event;
    ctx->connection_event_cb = &handle_client_connection_event;
  }else{
    ctx->stream_event_cb = &handle_stream_event;
    ctx->connection_event_cb = &handle_connection_event;
    if (ptls_load_certificates(ctx, (char *)cert) != 0)
      log_debug("failed to load certificate:%s:%s\n", cert, strerror(errno));
    if(load_private_key(ctx, (char*)cert_key)!=0)
      log_debug("failed apply ket :%s:%s\n", cert_key, strerror(errno));
  }
done:
  return ctx;
}

/**
 * Only handle 1 read/write socket so far; we do not need more capability at the
 * moment
 */

static int tcpls_hold_data_to_read_or_write(fd_set *readfds, fd_set *writefds) {
  struct tcpls_con *con;
  /** check whether we have something to read */
  int res = 0;
  if (tcpls_buf.off - tcpls_buf_read_offset > 0 && readfds) {
    for (int i = 0; i < tcpls_con_l->size; i++) {
      con = list_get(tcpls_con_l, i);
      if (FD_ISSET(con->sd, readfds)) {
        FD_ZERO(readfds);
        FD_SET(con->sd, readfds);
        res += 1;
        break;
      }
    }
  }
  /** check whether we have something to write */
  if (writefds) {
    for (int i = 0; i < tcpls_con_l->size; i++) {
      con = list_get(tcpls_con_l, i);
      if (FD_ISSET(con->sd, writefds)) {
        FD_ZERO(writefds);
        FD_SET(con->sd, writefds);
        res += 1;
        break;
      }
    }
  }
  return res;
}


/**
 * returns 1 and set modifies readfds/writefds appropriatly if we still
 * have something to read/write in tcpls
 *
 */

int handle_select(long arg1, long arg2, long *result) {
  fd_set *readfds = (fd_set*) arg1;
  fd_set *writefds = (fd_set*) arg2;
  if ((*result = tcpls_hold_data_to_read_or_write(readfds, writefds)) > 0) {
    return SYSCALL_SKIP;
  }
  return SYSCALL_RUN;
}

static int tcpls_do_handshake(int sd, tcpls_t *tcpls){
  int result = -1;
  ptls_handshake_properties_t prop = {NULL};
  prop.socket = sd;
  if ((result = tcpls_handshake(tcpls->tls, &prop)) != 0) {
    log_debug("tcpls_handshake failed with ret (%d)\n", result);
  }
  /* if the hanshake succeeds, we'll need a buffer for recv/read */
  ptls_buffer_init(&tcpls_buf, "", 0);
  ptls_buffer_reserve(&tcpls_buf, TCPLS_BUFFER_SIZE);
  return result;
}

int _tcpls_init(int is_server){
  const char *host = is_server ? "SERVER" : "CLIENT";
  log_debug("Init new tcpls context for %s", host);
  set_tcpls_ctx_options(is_server);
  return 0;
}

struct tcpls_con * _tcpls_alloc_con_info(int sd, int is_server, int af_family){
  struct tcpls_con con;
  log_debug("1 adding new socket descriptor :%d", sd);
  con.sd = sd;
  con.state = CLOSED;
  con.af_family = af_family;
  con.tcpls = tcpls_new(ctx, is_server);
  list_add(tcpls_con_l, &con); 
  log_debug("adding new socket descriptor :%d",sd);
  return list_get(tcpls_con_l, tcpls_con_l->size-1);
}

struct tcpls_con *_tcpls_lookup(int sd){
  struct tcpls_con * con;
  if (!tcpls_con_l)
    return NULL;
  if (!tcpls_con_l->size)
    return NULL;
  for(int i = 0; i < tcpls_con_l->size; i++){
    con = list_get(tcpls_con_l, i);
    if(con->sd == sd){
      return con;
    }
  }
  return NULL;
}

int _tcpls_free_con(int sd){
  int i;
  struct tcpls_con * con;
  if(!tcpls_con_l || !tcpls_con_l->size)
    return -1;
  for(i=0; i < tcpls_con_l->size; i++){
    con = list_get(tcpls_con_l, i);
    if(con->sd == sd){
      //tcpls_free(con->tcpls);
      list_remove(tcpls_con_l, con);
      //free(con);
      return 0;
    }
  }
  return -1;
}

int _handle_tcpls_connect(int sd, struct sockaddr * dest, tcpls_t * tcpls){
  int result = -1;
  struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
  if(dest->sa_family == AF_INET){
    result = tcpls_add_v4(tcpls->tls, (struct sockaddr_in*)dest, 1, 0, 0);
    if(result && result!=TCPLS_ADDR_EXIST){
      return result;
    }
  }
  if(dest->sa_family == AF_INET6){
    result = tcpls_add_v6(tcpls->tls, (struct sockaddr_in6*)dest, 1, 0, 0);
    if(result && result!=TCPLS_ADDR_EXIST){
      return result;
    }
  }
  result = tcpls_connect(tcpls->tls, NULL, dest, &timeout, sd);
  return result;
}

int _tcpls_do_tcpls_accept(int sd, struct sockaddr *addr){
  int result = -1;
  struct tcpls_con *con;
  struct sockaddr our_addr;
  socklen_t salen = sizeof(struct sockaddr);
  con = _tcpls_alloc_con_info(sd, 1, addr->sa_family);
  if(!con){
    log_debug("failed to alloc con %d", sd);
    return result;
  }
  if(addr->sa_family == AF_INET){
    tcpls_add_v4(con->tcpls->tls, (struct sockaddr_in*)addr, 1, 0, 0);
  }
  else if(addr->sa_family == AF_INET6){
    tcpls_add_v6(con->tcpls->tls, (struct sockaddr_in6*)addr, 1, 0, 0);
  }
  if (syscall_no_intercept(SYS_getsockname, sd, (struct sockaddr *) &our_addr, &salen) < -1) {
    log_debug("getsockname(2) failed %d:%d", errno, sd);
  }
  if(our_addr.sa_family == AF_INET){
    tcpls_add_v4(con->tcpls->tls, (struct sockaddr_in*)&our_addr, 0, 0, 1);
  }
  else if(our_addr.sa_family == AF_INET6){
    tcpls_add_v6(con->tcpls->tls, (struct sockaddr_in6*)&our_addr, 0, 0, 1);
  }
  result = tcpls_accept(con->tcpls, sd, NULL, 0);
  if(result < 0)
    log_debug("TCPLS tcpls_accept failed %d\n", result);
  return result;
}

int _tcpls_set_ours_addr(struct sockaddr *addr){
  if(!ours_addr_list)
   return -1;
  list_add(ours_addr_list, addr);
  return ours_addr_list->size;
}

int _tcpls_handshake(int sd, tcpls_t *tcpls){
  if(ptls_handshake_is_complete(tcpls->tls)){
    return 0;
  }
  return  tcpls_do_handshake(sd, tcpls);
}

size_t _tcpls_do_recv(int sd, uint8_t *buf, size_t size, int flags, tcpls_t *tcpls) {
  int ret = -1;
  struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
  if (tcpls_buf.off-tcpls_buf_read_offset >= size) {
    memcpy(buf, tcpls_buf.base+tcpls_buf_read_offset, size);
    if (flags != MSG_PEEK)
      tcpls_buf_read_offset += size;
    if (tcpls_buf_read_offset >= 0.9 * TCPLS_BUFFER_SIZE) {
      shift_buffer(&tcpls_buf, tcpls_buf_read_offset);
      tcpls_buf_read_offset = 0;
    }
    /** Set a non-blocking mode to ensure application's select won't block
     *  if no any more bytes are awaing on the socket */
    if(!set_blocking_mode(sd, 0))
      log_debug("set_blocking_mode failed");
    return size;
  }
  else if (tcpls_buf.off-tcpls_buf_read_offset > 0) {
    /** Just returns bytes we already have, if any */
    memcpy(buf, tcpls_buf.base+tcpls_buf_read_offset,
        tcpls_buf.off-tcpls_buf_read_offset);
    ret = tcpls_buf.off-tcpls_buf_read_offset;
    if (flags != MSG_PEEK)
      tcpls_buf.off -= ret;
    if (!set_blocking_mode(sd, 1))
      log_debug("set_blocking mode failed");
    return ret;
  }
  else {
    while (((ret = tcpls_receive(tcpls->tls, &tcpls_buf, &timeout)) == TCPLS_HOLD_DATA_TO_READ) ||
        (ret == TCPLS_OK && tcpls_buf.off-tcpls_buf_read_offset == 0))
      ;

    if (ret == TCPLS_OK) {
      if (tcpls_buf.off-tcpls_buf_read_offset >= size) {
        memcpy(buf, tcpls_buf.base+tcpls_buf_read_offset, size);
        if (flags != MSG_PEEK)
          tcpls_buf_read_offset += size;
        if (tcpls_buf_read_offset >= 0.9 * TCPLS_BUFFER_SIZE) {
          shift_buffer(&tcpls_buf, tcpls_buf_read_offset);
          tcpls_buf_read_offset = 0;
        }
        if(!set_blocking_mode(sd, 0))
          log_debug("set_blocking_mode failed");
        return size;
      }
      else {
        memcpy(buf, tcpls_buf.base+tcpls_buf_read_offset,
            tcpls_buf.off-tcpls_buf_read_offset);
        ret = tcpls_buf.off-tcpls_buf_read_offset;
        if (flags != MSG_PEEK)
          tcpls_buf.off -= ret;
        if(!set_blocking_mode(sd, 1))
          log_debug("set_blocking_mode failed");
        return ret;
      }

    }
    else{
      log_debug("TCPLS tcpls_receive return error %d code on socket descriptor %d", ret, sd);
      return 0;
    }
  }
}

size_t _tcpls_do_recvfrom(int sd, uint8_t *buf, size_t size, int flags, tcpls_t *tcpls) {
  return _tcpls_do_recv(sd, buf, size, flags, tcpls);
}

ssize_t _tcpls_do_send(uint8_t *buf, size_t size, tcpls_t *tcpls){
  streamid_t streamid = 0;
  int ret, sret;
  struct tcpls_con *con;
  con = list_get(tcpls_con_l, 0);
  if (con && tcpls->tls->is_server)
    streamid = con->streamid;
  else {
    streamid = 0;
  }
  /**
   * The application when seeing TCPLS_HOLD_DATA_TO_SEND, should wait to call
   * again
   *
   * this is a bit inneficient -- ideally we would want to wait for a socket
   * event to tell us that we have room for sending more; and letting the app
   * to do something else in the meantime. that seems difficult to get for an
   * interception layer
   */
  if ((ret = tcpls_send(tcpls->tls, streamid, buf, size)) == TCPLS_HOLD_DATA_TO_SEND) {
    /** Kernel's sending buffer is fool, but tcpls hold some encrypted data to
     * send */
    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(con->sd, &writefds);
    struct timeval timeout = {.tv_sec=2, .tv_usec=0};
    while (ret == TCPLS_HOLD_DATA_TO_SEND && (sret = syscall_no_intercept(SYS_select, con->sd+1, NULL, &writefds, NULL, &timeout)) > 0) {
      /*Sending TCPLS's internal data */
      ret = tcpls_send(tcpls->tls, streamid, buf, 0);
      FD_ZERO(&writefds);
      FD_SET(con->sd, &writefds);
    }
    if (ret == TCPLS_OK)
      return size;
    else {
      log_debug("Something went wrong while looping on sending hold data: ret=%d", ret);
      return ret;
    }
  }
  else if (ret == TCPLS_OK) {
    return size;
  }
  else {
    log_debug("tcpls_send did not successfully send all its data, and returned error %d", ret);
    return ret;
  }
}
