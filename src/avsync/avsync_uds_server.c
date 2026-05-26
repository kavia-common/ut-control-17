/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2026 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "avsync/avsync_uds_server.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

typedef struct AvSyncServerSessionEntry {
  uint32_t clock_id;
  uint64_t session_id;
  int shm_fd; /* server-owned (dup) */
  uint32_t shm_size_bytes;
  uint32_t shm_layout_version;
  uint32_t flags;
  bool valid;
} AvSyncServerSessionEntry;

typedef struct AvSyncServerConsumerAttachment {
  uint32_t clock_id;
  uint64_t session_id;
  AvSyncConsumerKind consumer_kind;

  /* Peer identity (best-effort via SO_PEERCRED); used for DETACH matching. */
  int peer_pid;
  int peer_uid;
  int peer_gid;

  int event_fd; /* server-owned eventfd used for notifications */
} AvSyncServerConsumerAttachment;

struct AvSyncUdsServer {
  AvSyncUdsServerConfig cfg;

  int listen_fd;
  pthread_t thread;
  bool thread_started;

  pthread_mutex_t lock;
  bool stop_requested;

  AvSyncServerSessionEntry* sessions;
  size_t session_count;
  size_t session_cap;

  AvSyncServerConsumerAttachment* consumers;
  size_t consumer_count;
  size_t consumer_cap;

  /* Cached copy for unlink() if filesystem socket is used. */
  char* bound_filesystem_path;
};

static int avsync_set_cloexec_if_needed(int fd) {
  if (fd < 0) return -EINVAL;
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0) return -errno;
  if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) return -errno;
  return 0;
}

static int avsync_make_sockaddr(const char* uds_path,
                                struct sockaddr_un* out_addr,
                                socklen_t* out_len,
                                char** out_filesystem_path_dup_or_null) {
  if (!out_addr || !out_len) return -EINVAL;
  if (out_filesystem_path_dup_or_null) *out_filesystem_path_dup_or_null = NULL;

  const char* path = uds_path;
  if (!path || path[0] == '\0') path = AVSYNC_UDS_DEFAULT_PATH;

  memset(out_addr, 0, sizeof(*out_addr));
  out_addr->sun_family = AF_UNIX;

  /* Abstract namespace. */
  if (path[0] == '@') {
    const char* name = path + 1;
    size_t name_len = strlen(name);
    if (name_len == 0) return -EINVAL;
    if (1 + name_len > sizeof(out_addr->sun_path)) return -ENAMETOOLONG;

    out_addr->sun_path[0] = '\0';
    memcpy(out_addr->sun_path + 1, name, name_len);
    *out_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
    return 0;
  }

  /* Filesystem path. */
  size_t path_len = strlen(path);
  if (path_len == 0) return -EINVAL;
  if (path_len >= sizeof(out_addr->sun_path)) return -ENAMETOOLONG;

  memcpy(out_addr->sun_path, path, path_len + 1); /* include NUL */
  *out_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);

  if (out_filesystem_path_dup_or_null) {
    *out_filesystem_path_dup_or_null = strdup(path);
    if (!*out_filesystem_path_dup_or_null) return -ENOMEM;
  }
  return 0;
}

static int avsync_send_reply_with_optional_fds(int conn_fd,
                                               const void* reply_buf,
                                               size_t reply_len,
                                               const int* fds_or_null,
                                               size_t fd_count) {
  if (!reply_buf || reply_len == 0) return -EINVAL;
  if (fd_count > 0 && !fds_or_null) return -EINVAL;

  struct iovec iov;
  iov.iov_base = (void*)reply_buf;
  iov.iov_len = reply_len;

  char cmsg_buf[CMSG_SPACE(sizeof(int) * AVSYNC_IPC_V1_EXPECTED_FD_COUNT)];
  memset(cmsg_buf, 0, sizeof(cmsg_buf));

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  if (fd_count > 0) {
    if (fd_count > AVSYNC_IPC_V1_EXPECTED_FD_COUNT) return -EINVAL;

    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
    memcpy(CMSG_DATA(cmsg), fds_or_null, sizeof(int) * fd_count);
  }

  ssize_t n = sendmsg(conn_fd, &msg, 0);
  if (n < 0) return -errno;
  if ((size_t)n != reply_len) return -EIO;
  return 0;
}

static AvSyncServerSessionEntry* avsync_find_session_locked(AvSyncUdsServer* s, uint32_t clock_id) {
  for (size_t i = 0; i < s->session_count; ++i) {
    if (s->sessions[i].valid && s->sessions[i].clock_id == clock_id) return &s->sessions[i];
  }
  return NULL;
}

static int avsync_ensure_sessions_cap_locked(AvSyncUdsServer* s, size_t needed) {
  if (s->session_cap >= needed) return 0;
  size_t new_cap = s->session_cap ? s->session_cap * 2 : 4;
  if (new_cap < needed) new_cap = needed;
  AvSyncServerSessionEntry* p = (AvSyncServerSessionEntry*)realloc(s->sessions, new_cap * sizeof(*p));
  if (!p) return -ENOMEM;
  s->sessions = p;
  s->session_cap = new_cap;
  return 0;
}

static int avsync_ensure_consumers_cap_locked(AvSyncUdsServer* s, size_t needed) {
  if (s->consumer_cap >= needed) return 0;
  size_t new_cap = s->consumer_cap ? s->consumer_cap * 2 : 8;
  if (new_cap < needed) new_cap = needed;
  AvSyncServerConsumerAttachment* p =
      (AvSyncServerConsumerAttachment*)realloc(s->consumers, new_cap * sizeof(*p));
  if (!p) return -ENOMEM;
  s->consumers = p;
  s->consumer_cap = new_cap;
  return 0;
}

static void avsync_remove_consumers_for_session_locked(AvSyncUdsServer* s, uint32_t clock_id, uint64_t session_id) {
  size_t w = 0;
  for (size_t r = 0; r < s->consumer_count; ++r) {
    AvSyncServerConsumerAttachment* c = &s->consumers[r];
    if (c->clock_id == clock_id && c->session_id == session_id) {
      if (c->event_fd >= 0) close(c->event_fd);
      continue;
    }
    if (w != r) s->consumers[w] = s->consumers[r];
    ++w;
  }
  s->consumer_count = w;
}

static void avsync_remove_consumers_for_detach_locked(AvSyncUdsServer* s,
                                                      uint32_t clock_id,
                                                      uint64_t session_id,
                                                      AvSyncConsumerKind consumer_kind,
                                                      int peer_pid) {
  size_t w = 0;
  for (size_t r = 0; r < s->consumer_count; ++r) {
    AvSyncServerConsumerAttachment* c = &s->consumers[r];
    if (c->clock_id == clock_id && c->session_id == session_id && c->consumer_kind == consumer_kind &&
        (peer_pid <= 0 || c->peer_pid == peer_pid)) {
      if (c->event_fd >= 0) close(c->event_fd);
      continue;
    }
    if (w != r) s->consumers[w] = s->consumers[r];
    ++w;
  }
  s->consumer_count = w;
}

static int avsync_get_peer_cred(int fd, int* out_pid, int* out_uid, int* out_gid) {
  if (out_pid) *out_pid = -1;
  if (out_uid) *out_uid = -1;
  if (out_gid) *out_gid = -1;

#ifdef SO_PEERCRED
  struct ucred cred;
  socklen_t len = sizeof(cred);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
    if (out_pid) *out_pid = (int)cred.pid;
    if (out_uid) *out_uid = (int)cred.uid;
    if (out_gid) *out_gid = (int)cred.gid;
    return 0;
  }
  return -errno;
#else
  (void)fd;
  return -ENOSYS;
#endif
}

static bool avsync_is_authorized(AvSyncUdsServer* s, int peer_pid, int peer_uid, int peer_gid) {
  if (!s->cfg.auth_cb) return true;
  return s->cfg.auth_cb(peer_pid, peer_uid, peer_gid, s->cfg.auth_user_data);
}

static int avsync_handle_get_session(AvSyncUdsServer* s,
                                     int conn_fd,
                                     const AvSyncGetSessionRequestWire* req,
                                     int peer_pid,
                                     int peer_uid,
                                     int peer_gid) {
  AvSyncGetSessionReplyWire reply;
  memset(&reply, 0, sizeof(reply));
  reply.hdr.magic = AVSYNC_IPC_MAGIC;
  reply.hdr.version = (uint16_t)AVSYNC_IPC_PROTOCOL_VERSION;
  reply.hdr.msgType = (uint16_t)AVSYNC_MSG_GET_SESSION_REPLY;
  reply.hdr.msgSize = (uint32_t)sizeof(reply);
  reply.fdCountExpected = 0;

  if (req->hdr.version != (uint16_t)AVSYNC_IPC_PROTOCOL_VERSION) {
    reply.status = AVSYNC_STATUS_ERR_UNSUPPORTED_VERSION;
    return avsync_send_reply_with_optional_fds(conn_fd, &reply, sizeof(reply), NULL, 0);
  }

  if (req->consumerKind != (uint32_t)AVSYNC_CONSUMER_AUDIO &&
      req->consumerKind != (uint32_t)AVSYNC_CONSUMER_VIDEO) {
    reply.status = AVSYNC_STATUS_ERR_BAD_REQUEST;
    return avsync_send_reply_with_optional_fds(conn_fd, &reply, sizeof(reply), NULL, 0);
  }

  if (!avsync_is_authorized(s, peer_pid, peer_uid, peer_gid)) {
    reply.status = AVSYNC_STATUS_ERR_UNAUTHORIZED;
    return avsync_send_reply_with_optional_fds(conn_fd, &reply, sizeof(reply), NULL, 0);
  }

  pthread_mutex_lock(&s->lock);
  AvSyncServerSessionEntry* sess = avsync_find_session_locked(s, req->clockId);
  if (!sess) {
    pthread_mutex_unlock(&s->lock);
    reply.status = AVSYNC_STATUS_ERR_NO_ACTIVE_SESSION;
    return avsync_send_reply_with_optional_fds(conn_fd, &reply, sizeof(reply), NULL, 0);
  }

  /* Create a per-consumer eventfd attachment. */
  int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (efd < 0) {
    pthread_mutex_unlock(&s->lock);
    reply.status = AVSYNC_STATUS_ERR_INTERNAL;
    return avsync_send_reply_with_optional_fds(conn_fd, &reply, sizeof(reply), NULL, 0);
  }

  int rc = avsync_ensure_consumers_cap_locked(s, s->consumer_count + 1);
  if (rc != 0) {
    close(efd);
    pthread_mutex_unlock(&s->lock);
    reply.status = AVSYNC_STATUS_ERR_INTERNAL;
    return avsync_send_reply_with_optional_fds(conn_fd, &reply, sizeof(reply), NULL, 0);
  }

  AvSyncServerConsumerAttachment* c = &s->consumers[s->consumer_count++];
  memset(c, 0, sizeof(*c));
  c->clock_id = sess->clock_id;
  c->session_id = sess->session_id;
  c->consumer_kind = (AvSyncConsumerKind)req->consumerKind;
  c->peer_pid = peer_pid;
  c->peer_uid = peer_uid;
  c->peer_gid = peer_gid;
  c->event_fd = efd;

  /* Prepare OK reply. */
  reply.status = AVSYNC_STATUS_OK;
  reply.clockId = sess->clock_id;
  memcpy(&reply.sessionId, &sess->session_id, sizeof(sess->session_id));
  reply.shmSizeBytes = sess->shm_size_bytes;
  reply.shmLayoutVersion = sess->shm_layout_version;
  reply.flags = sess->flags;
  reply.fdCountExpected = AVSYNC_IPC_V1_EXPECTED_FD_COUNT;

  int send_fds[AVSYNC_IPC_V1_EXPECTED_FD_COUNT];
  send_fds[0] = sess->shm_fd;
  send_fds[1] = c->event_fd;

  pthread_mutex_unlock(&s->lock);

  return avsync_send_reply_with_optional_fds(conn_fd, &reply, sizeof(reply), send_fds, AVSYNC_IPC_V1_EXPECTED_FD_COUNT);
}

static int avsync_handle_detach(AvSyncUdsServer* s,
                                int conn_fd,
                                const AvSyncDetachRequestWire* req,
                                int peer_pid,
                                int peer_uid,
                                int peer_gid) {
  (void)peer_uid;
  (void)peer_gid;

  AvSyncDetachReplyWire reply;
  memset(&reply, 0, sizeof(reply));
  reply.hdr.magic = AVSYNC_IPC_MAGIC;
  reply.hdr.version = (uint16_t)AVSYNC_IPC_PROTOCOL_VERSION;
  reply.hdr.msgType = (uint16_t)AVSYNC_MSG_DETACH_REPLY;
  reply.hdr.msgSize = (uint32_t)sizeof(reply);

  if (req->hdr.version != (uint16_t)AVSYNC_IPC_PROTOCOL_VERSION) {
    reply.status = AVSYNC_STATUS_ERR_UNSUPPORTED_VERSION;
    return avsync_send_reply_with_optional_fds(conn_fd, &reply, sizeof(reply), NULL, 0);
  }

  if (req->consumerKind != (uint32_t)AVSYNC_CONSUMER_AUDIO &&
      req->consumerKind != (uint32_t)AVSYNC_CONSUMER_VIDEO) {
    reply.status = AVSYNC_STATUS_ERR_BAD_REQUEST;
    return avsync_send_reply_with_optional_fds(conn_fd, &reply, sizeof(reply), NULL, 0);
  }

  if (!avsync_is_authorized(s, peer_pid, peer_uid, peer_gid)) {
    reply.status = AVSYNC_STATUS_ERR_UNAUTHORIZED;
    return avsync_send_reply_with_optional_fds(conn_fd, &reply, sizeof(reply), NULL, 0);
  }

  uint64_t session_id = 0;
  memcpy(&session_id, &req->sessionId, sizeof(session_id));

  pthread_mutex_lock(&s->lock);
  avsync_remove_consumers_for_detach_locked(s,
                                           req->clockId,
                                           session_id,
                                           (AvSyncConsumerKind)req->consumerKind,
                                           peer_pid);
  pthread_mutex_unlock(&s->lock);

  reply.status = AVSYNC_STATUS_OK;
  return avsync_send_reply_with_optional_fds(conn_fd, &reply, sizeof(reply), NULL, 0);
}

static void avsync_handle_connection(AvSyncUdsServer* s, int conn_fd) {
  int peer_pid = -1;
  int peer_uid = -1;
  int peer_gid = -1;
  (void)avsync_get_peer_cred(conn_fd, &peer_pid, &peer_uid, &peer_gid);

  /* Read exactly one SEQPACKET message; clients use one-shot request/reply. */
  uint8_t buf[sizeof(AvSyncGetSessionRequestWire)];
  ssize_t n = recv(conn_fd, buf, sizeof(buf), 0);
  if (n <= 0) return;

  if ((size_t)n < sizeof(AvSyncIpcHeaderWire)) return;

  const AvSyncIpcHeaderWire* hdr = (const AvSyncIpcHeaderWire*)buf;
  if (hdr->magic != AVSYNC_IPC_MAGIC) return;
  if (hdr->msgSize != (uint32_t)n) return;

  if (hdr->msgType == (uint16_t)AVSYNC_MSG_GET_SESSION) {
    if ((size_t)n != sizeof(AvSyncGetSessionRequestWire)) return;
    (void)avsync_handle_get_session(s, conn_fd, (const AvSyncGetSessionRequestWire*)buf, peer_pid, peer_uid, peer_gid);
    return;
  }

  if (hdr->msgType == (uint16_t)AVSYNC_MSG_DETACH) {
    if ((size_t)n != sizeof(AvSyncDetachRequestWire)) return;
    (void)avsync_handle_detach(s, conn_fd, (const AvSyncDetachRequestWire*)buf, peer_pid, peer_uid, peer_gid);
    return;
  }

  /* Unknown msgType => ignore (or could reply BAD_REQUEST). */
}

static void* avsync_server_thread_main(void* arg) {
  AvSyncUdsServer* s = (AvSyncUdsServer*)arg;

  while (1) {
    pthread_mutex_lock(&s->lock);
    bool stop = s->stop_requested;
    int listen_fd = s->listen_fd;
    pthread_mutex_unlock(&s->lock);

    if (stop) break;
    if (listen_fd < 0) break;

    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = listen_fd;
    pfd.events = POLLIN;

    int prc = poll(&pfd, 1, 250 /*ms*/);
    if (prc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (prc == 0) continue;

    if (pfd.revents & POLLIN) {
      for (;;) {
        int conn_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (conn_fd < 0) {
          if (errno == EINTR) continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          break;
        }
        (void)avsync_set_cloexec_if_needed(conn_fd);
        avsync_handle_connection(s, conn_fd);
        close(conn_fd);
      }
    }
  }

  return NULL;
}

static int avsync_open_listen_socket_and_bind(AvSyncUdsServer* s) {
  struct sockaddr_un addr;
  socklen_t addr_len = 0;
  char* fs_path_dup = NULL;

  int rc = avsync_make_sockaddr(s->cfg.uds_path, &addr, &addr_len, &fs_path_dup);
  if (rc != 0) return rc;

  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    free(fs_path_dup);
    return -errno;
  }

  rc = avsync_set_cloexec_if_needed(fd);
  if (rc != 0) {
    close(fd);
    free(fs_path_dup);
    return rc;
  }

  /* If filesystem socket, unlink any stale path before bind(). */
  if (fs_path_dup) {
    unlink(fs_path_dup);
  }

  if (bind(fd, (const struct sockaddr*)&addr, addr_len) < 0) {
    rc = -errno;
    close(fd);
    free(fs_path_dup);
    return rc;
  }

  if (listen(fd, 16) < 0) {
    rc = -errno;
    close(fd);
    if (fs_path_dup) {
      unlink(fs_path_dup);
    }
    free(fs_path_dup);
    return rc;
  }

  /* Non-blocking accept loop via poll(). */
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);

  pthread_mutex_lock(&s->lock);
  s->listen_fd = fd;
  s->bound_filesystem_path = fs_path_dup; /* may be NULL */
  pthread_mutex_unlock(&s->lock);

  return 0;
}

int avsync_uds_server_create(const AvSyncUdsServerConfig* cfg, AvSyncUdsServer** out_server) {
  if (!out_server) return -EINVAL;
  *out_server = NULL;

  AvSyncUdsServer* s = (AvSyncUdsServer*)calloc(1, sizeof(*s));
  if (!s) return -ENOMEM;

  s->cfg = (cfg != NULL) ? *cfg : (AvSyncUdsServerConfig){0};
  s->listen_fd = -1;
  s->thread_started = false;
  s->stop_requested = false;
  pthread_mutex_init(&s->lock, NULL);

  *out_server = s;
  return 0;
}

void avsync_uds_server_destroy(AvSyncUdsServer* server) {
  if (!server) return;

  (void)avsync_uds_server_stop(server);

  pthread_mutex_lock(&server->lock);

  if (server->listen_fd >= 0) {
    close(server->listen_fd);
    server->listen_fd = -1;
  }

  if (server->bound_filesystem_path) {
    unlink(server->bound_filesystem_path);
    free(server->bound_filesystem_path);
    server->bound_filesystem_path = NULL;
  }

  for (size_t i = 0; i < server->session_count; ++i) {
    if (server->sessions[i].valid && server->sessions[i].shm_fd >= 0) {
      close(server->sessions[i].shm_fd);
      server->sessions[i].shm_fd = -1;
    }
  }

  for (size_t i = 0; i < server->consumer_count; ++i) {
    if (server->consumers[i].event_fd >= 0) close(server->consumers[i].event_fd);
    server->consumers[i].event_fd = -1;
  }

  free(server->sessions);
  server->sessions = NULL;
  server->session_count = 0;
  server->session_cap = 0;

  free(server->consumers);
  server->consumers = NULL;
  server->consumer_count = 0;
  server->consumer_cap = 0;

  pthread_mutex_unlock(&server->lock);

  pthread_mutex_destroy(&server->lock);
  free(server);
}

int avsync_uds_server_start(AvSyncUdsServer* server) {
  if (!server) return -EINVAL;

  pthread_mutex_lock(&server->lock);
  if (server->thread_started) {
    pthread_mutex_unlock(&server->lock);
    return 0;
  }
  server->stop_requested = false;
  pthread_mutex_unlock(&server->lock);

  int rc = avsync_open_listen_socket_and_bind(server);
  if (rc != 0) return rc;

  if (pthread_create(&server->thread, NULL, avsync_server_thread_main, server) != 0) {
    pthread_mutex_lock(&server->lock);
    int fd = server->listen_fd;
    server->listen_fd = -1;
    pthread_mutex_unlock(&server->lock);

    if (fd >= 0) close(fd);
    return -errno;
  }

  pthread_mutex_lock(&server->lock);
  server->thread_started = true;
  pthread_mutex_unlock(&server->lock);

  return 0;
}

int avsync_uds_server_stop(AvSyncUdsServer* server) {
  if (!server) return -EINVAL;

  pthread_mutex_lock(&server->lock);
  bool started = server->thread_started;
  server->stop_requested = true;
  int fd = server->listen_fd;
  pthread_mutex_unlock(&server->lock);

  /* Wake poll()/accept by shutting down the listening socket. */
  if (fd >= 0) (void)shutdown(fd, SHUT_RDWR);

  if (started) {
    (void)pthread_join(server->thread, NULL);
  }

  pthread_mutex_lock(&server->lock);
  server->thread_started = false;

  if (server->listen_fd >= 0) {
    close(server->listen_fd);
    server->listen_fd = -1;
  }

  if (server->bound_filesystem_path) {
    unlink(server->bound_filesystem_path);
    free(server->bound_filesystem_path);
    server->bound_filesystem_path = NULL;
  }

  pthread_mutex_unlock(&server->lock);

  return 0;
}

int avsync_uds_server_set_active_session(AvSyncUdsServer* server, const AvSyncUdsServerSessionInfo* session) {
  if (!server || !session) return -EINVAL;
  if (session->shm_fd < 0) return -EINVAL;

  int shm_dup = dup(session->shm_fd);
  if (shm_dup < 0) return -errno;
  (void)avsync_set_cloexec_if_needed(shm_dup);

  pthread_mutex_lock(&server->lock);

  AvSyncServerSessionEntry* existing = avsync_find_session_locked(server, session->clock_id);
  if (!existing) {
    int rc = avsync_ensure_sessions_cap_locked(server, server->session_count + 1);
    if (rc != 0) {
      pthread_mutex_unlock(&server->lock);
      close(shm_dup);
      return rc;
    }
    existing = &server->sessions[server->session_count++];
    memset(existing, 0, sizeof(*existing));
  } else {
    /* Replace: drop existing consumers for the old session and close old shm FD. */
    avsync_remove_consumers_for_session_locked(server, existing->clock_id, existing->session_id);
    if (existing->shm_fd >= 0) close(existing->shm_fd);
  }

  existing->clock_id = session->clock_id;
  existing->session_id = session->session_id;
  existing->shm_fd = shm_dup;
  existing->shm_size_bytes = session->shm_size_bytes;
  existing->shm_layout_version = session->shm_layout_version;
  existing->flags = session->flags;
  existing->valid = true;

  pthread_mutex_unlock(&server->lock);

  return 0;
}

int avsync_uds_server_clear_active_session(AvSyncUdsServer* server, uint32_t clock_id, uint64_t session_id) {
  if (!server) return -EINVAL;

  pthread_mutex_lock(&server->lock);
  AvSyncServerSessionEntry* sess = avsync_find_session_locked(server, clock_id);
  if (!sess || !sess->valid || sess->session_id != session_id) {
    pthread_mutex_unlock(&server->lock);
    return 0;
  }

  avsync_remove_consumers_for_session_locked(server, clock_id, session_id);

  if (sess->shm_fd >= 0) close(sess->shm_fd);
  memset(sess, 0, sizeof(*sess));
  sess->shm_fd = -1;
  sess->valid = false;

  pthread_mutex_unlock(&server->lock);
  return 0;
}

int avsync_uds_server_notify_consumers(AvSyncUdsServer* server, uint32_t clock_id, uint64_t session_id) {
  if (!server) return -EINVAL;

  pthread_mutex_lock(&server->lock);

  int notified = 0;
  for (size_t i = 0; i < server->consumer_count; ++i) {
    AvSyncServerConsumerAttachment* c = &server->consumers[i];
    if (c->clock_id != clock_id || c->session_id != session_id) continue;
    if (c->event_fd < 0) continue;

    uint64_t one = 1;
    ssize_t n = write(c->event_fd, &one, sizeof(one));
    if (n == (ssize_t)sizeof(one)) {
      notified++;
    }
    /* Best-effort: ignore EAGAIN/EINTR/others; consumer will catch up on next notify. */
  }

  pthread_mutex_unlock(&server->lock);
  return notified;
}

#else /* !__linux__ */

int avsync_uds_server_create(const AvSyncUdsServerConfig* cfg, AvSyncUdsServer** out_server) {
  (void)cfg;
  if (out_server) *out_server = NULL;
  return -ENOSYS;
}

void avsync_uds_server_destroy(AvSyncUdsServer* server) {
  (void)server;
}

int avsync_uds_server_start(AvSyncUdsServer* server) {
  (void)server;
  return -ENOSYS;
}

int avsync_uds_server_stop(AvSyncUdsServer* server) {
  (void)server;
  return -ENOSYS;
}

int avsync_uds_server_set_active_session(AvSyncUdsServer* server, const AvSyncUdsServerSessionInfo* session) {
  (void)server;
  (void)session;
  return -ENOSYS;
}

int avsync_uds_server_clear_active_session(AvSyncUdsServer* server, uint32_t clock_id, uint64_t session_id) {
  (void)server;
  (void)clock_id;
  (void)session_id;
  return -ENOSYS;
}

int avsync_uds_server_notify_consumers(AvSyncUdsServer* server, uint32_t clock_id, uint64_t session_id) {
  (void)server;
  (void)clock_id;
  (void)session_id;
  return -ENOSYS;
}

#endif /* __linux__ */
