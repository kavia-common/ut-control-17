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

#include "avsync/avsync_uds_client.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

#define AVSYNC_CMSG_FD_CAP 8u

static void avsync_reset_desc(AvSyncSessionDescriptor* desc) {
  if (!desc) return;
  memset(desc, 0, sizeof(*desc));
  desc->shm_fd = -1;
  desc->event_fd = -1;
}

static int avsync_set_cloexec_if_needed(int fd) {
  if (fd < 0) return -EINVAL;
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0) return -errno;
  if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) return -errno;
  return 0;
}

/**
 * Build a sockaddr_un for either filesystem-path or abstract-namespace sockets.
 *
 * Convention:
 *   - If uds_path begins with '@', it is treated as an abstract address
 *     (Linux-only), and the leading byte in sun_path is set to NUL.
 *   - Otherwise, it is treated as a filesystem path.
 */
static int avsync_make_sockaddr(const char* uds_path,
                                struct sockaddr_un* out_addr,
                                socklen_t* out_len) {
  if (!out_addr || !out_len) return -EINVAL;

  const char* path = uds_path;
  if (!path || path[0] == '\0') path = AVSYNC_UDS_DEFAULT_PATH;

  memset(out_addr, 0, sizeof(*out_addr));
  out_addr->sun_family = AF_UNIX;

  /* Abstract namespace. */
  if (path[0] == '@') {
    const char* name = path + 1;
    size_t name_len = strlen(name);
    if (name_len == 0) return -EINVAL;

    /* sun_path[0] is NUL for abstract namespace; remaining bytes are name. */
    if (1 + name_len > sizeof(out_addr->sun_path)) return -ENAMETOOLONG;
    out_addr->sun_path[0] = '\0';
    memcpy(out_addr->sun_path + 1, name, name_len);

    /* Length excludes trailing NUL; includes leading abstract NUL byte. */
    *out_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
    return 0;
  }

  /* Filesystem path. */
  size_t path_len = strlen(path);
  if (path_len == 0) return -EINVAL;
  if (path_len >= sizeof(out_addr->sun_path)) return -ENAMETOOLONG;

  memcpy(out_addr->sun_path, path, path_len + 1); /* include NUL */
  *out_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
  return 0;
}

static int avsync_open_and_connect(const char* uds_path) {
  struct sockaddr_un addr;
  socklen_t addr_len = 0;
  int rc = avsync_make_sockaddr(uds_path, &addr, &addr_len);
  if (rc != 0) return rc;

  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) return -errno;

  /* If SOCK_CLOEXEC was not supported (or defined as 0), set it manually. */
  rc = avsync_set_cloexec_if_needed(fd);
  if (rc != 0) {
    close(fd);
    return rc;
  }

  if (connect(fd, (const struct sockaddr*)&addr, addr_len) < 0) {
    rc = -errno;
    close(fd);
    return rc;
  }

  return fd;
}

static int avsync_send_all(int fd, const void* buf, size_t len) {
  const uint8_t* p = (const uint8_t*)buf;
  size_t remaining = len;

  while (remaining > 0) {
    ssize_t n = send(fd, p, remaining, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -errno;
    }
    if (n == 0) return -EPIPE;
    p += (size_t)n;
    remaining -= (size_t)n;
  }
  return 0;
}

static int avsync_recv_reply_with_fds(int fd,
                                      void* reply_buf,
                                      size_t reply_buf_len,
                                      int* out_fds,
                                      size_t out_fds_cap,
                                      size_t* out_fd_count,
                                      ssize_t* out_bytes_read) {
  if (!reply_buf || reply_buf_len == 0 || !out_fds || !out_fd_count || !out_bytes_read)
    return -EINVAL;

  *out_fd_count = 0;
  *out_bytes_read = 0;

  struct iovec iov;
  iov.iov_base = reply_buf;
  iov.iov_len = reply_buf_len;

  /* Reserve ancillary space for up to AVSYNC_CMSG_FD_CAP fds. */
  char cmsg_buf[CMSG_SPACE(sizeof(int) * AVSYNC_CMSG_FD_CAP)];
  memset(cmsg_buf, 0, sizeof(cmsg_buf));

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf;
  msg.msg_controllen = sizeof(cmsg_buf);

  ssize_t n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
  if (n < 0) {
    if (errno == EINTR) return -EINTR;
    return -errno;
  }
  *out_bytes_read = n;

  for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
       cmsg != NULL;
       cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
      size_t data_len = (size_t)(cmsg->cmsg_len - CMSG_LEN(0));
      size_t fd_count = data_len / sizeof(int);
      const int* fds = (const int*)CMSG_DATA(cmsg);

      size_t take = fd_count;
      if (take > out_fds_cap) take = out_fds_cap;

      for (size_t i = 0; i < take; ++i) {
        out_fds[i] = fds[i];
      }
      *out_fd_count = take;

      /* Close any extra fds we didn't take to avoid leaks. */
      for (size_t i = take; i < fd_count; ++i) {
        if (fds[i] >= 0) close(fds[i]);
      }

      /* Only consume the first SCM_RIGHTS for this protocol. */
      break;
    }
  }

  return 0;
}

const char* avsync_ipc_status_to_string(uint32_t status) {
  switch (status) {
    case AVSYNC_STATUS_OK:
      return "OK";
    case AVSYNC_STATUS_ERR_UNSUPPORTED_VERSION:
      return "ERR_UNSUPPORTED_VERSION";
    case AVSYNC_STATUS_ERR_BAD_REQUEST:
      return "ERR_BAD_REQUEST";
    case AVSYNC_STATUS_ERR_CLOCK_NOT_FOUND:
      return "ERR_CLOCK_NOT_FOUND";
    case AVSYNC_STATUS_ERR_NO_ACTIVE_SESSION:
      return "ERR_NO_ACTIVE_SESSION";
    case AVSYNC_STATUS_ERR_UNAUTHORIZED:
      return "ERR_UNAUTHORIZED";
    case AVSYNC_STATUS_ERR_INTERNAL:
      return "ERR_INTERNAL";
    default:
      return "UNKNOWN_STATUS";
  }
}

void avsync_uds_client_close_session(AvSyncSessionDescriptor* desc) {
  if (!desc) return;

  if (desc->shm_fd >= 0) {
    close(desc->shm_fd);
    desc->shm_fd = -1;
  }
  if (desc->event_fd >= 0) {
    close(desc->event_fd);
    desc->event_fd = -1;
  }
}

int avsync_uds_client_get_session(const char* uds_path,
                                  uint32_t clock_id,
                                  AvSyncConsumerKind consumer_kind,
                                  AvSyncSessionDescriptor* out_desc) {
  if (!out_desc) return -EINVAL;
  avsync_reset_desc(out_desc);

  if (consumer_kind != AVSYNC_CONSUMER_AUDIO && consumer_kind != AVSYNC_CONSUMER_VIDEO)
    return -EINVAL;

  int conn_fd = avsync_open_and_connect(uds_path);
  if (conn_fd < 0) return conn_fd;

  AvSyncGetSessionRequestWire req;
  memset(&req, 0, sizeof(req));
  req.hdr.magic = AVSYNC_IPC_MAGIC;
  req.hdr.version = (uint16_t)AVSYNC_IPC_PROTOCOL_VERSION;
  req.hdr.msgType = (uint16_t)AVSYNC_MSG_GET_SESSION;
  req.hdr.msgSize = (uint32_t)sizeof(req);
  req.clockId = clock_id;
  req.consumerKind = (uint32_t)consumer_kind;
  req.desiredShmAccess = 0u; /* read-only */

  int rc = avsync_send_all(conn_fd, &req, sizeof(req));
  if (rc != 0) {
    close(conn_fd);
    return rc;
  }

  AvSyncGetSessionReplyWire reply;
  memset(&reply, 0, sizeof(reply));

  int fds[AVSYNC_IPC_V1_EXPECTED_FD_COUNT];
  for (size_t i = 0; i < AVSYNC_IPC_V1_EXPECTED_FD_COUNT; ++i) fds[i] = -1;

  size_t fd_count = 0;
  ssize_t bytes_read = 0;

  rc = avsync_recv_reply_with_fds(conn_fd,
                                  &reply,
                                  sizeof(reply),
                                  fds,
                                  AVSYNC_IPC_V1_EXPECTED_FD_COUNT,
                                  &fd_count,
                                  &bytes_read);

  /* We don't need the connection after the one-shot exchange. */
  close(conn_fd);

  if (rc != 0) {
    for (size_t i = 0; i < fd_count; ++i) {
      if (fds[i] >= 0) close(fds[i]);
    }
    return rc;
  }

  if ((size_t)bytes_read < sizeof(reply)) {
    for (size_t i = 0; i < fd_count; ++i) {
      if (fds[i] >= 0) close(fds[i]);
    }
    return -EPROTO;
  }

  if (reply.hdr.magic != AVSYNC_IPC_MAGIC ||
      reply.hdr.version != (uint16_t)AVSYNC_IPC_PROTOCOL_VERSION ||
      reply.hdr.msgType != (uint16_t)AVSYNC_MSG_GET_SESSION_REPLY ||
      reply.hdr.msgSize != (uint32_t)sizeof(reply)) {
    for (size_t i = 0; i < fd_count; ++i) {
      if (fds[i] >= 0) close(fds[i]);
    }
    return -EPROTO;
  }

  const uint32_t status = reply.status;
  if (status != AVSYNC_STATUS_OK) {
    /* Protocol requires no FDs on error; if any were sent, close them. */
    for (size_t i = 0; i < fd_count; ++i) {
      if (fds[i] >= 0) close(fds[i]);
    }
    return (int)status;
  }

  if (reply.fdCountExpected != AVSYNC_IPC_V1_EXPECTED_FD_COUNT ||
      fd_count != AVSYNC_IPC_V1_EXPECTED_FD_COUNT) {
    for (size_t i = 0; i < fd_count; ++i) {
      if (fds[i] >= 0) close(fds[i]);
    }
    return -EPROTO;
  }

  /* Avoid potential unaligned access by copying sessionId. */
  uint64_t session_id = 0;
  memcpy(&session_id, &reply.sessionId, sizeof(session_id));

  out_desc->clock_id = reply.clockId;
  out_desc->session_id = session_id;
  out_desc->shm_size_bytes = reply.shmSizeBytes;
  out_desc->shm_layout_version = reply.shmLayoutVersion;
  out_desc->flags = reply.flags;
  out_desc->shm_fd = fds[0];
  out_desc->event_fd = fds[1];

  /*
   * Ensure CLOEXEC is set even if MSG_CMSG_CLOEXEC wasn't supported at runtime.
   * (If it was supported, this is harmless.)
   */
  (void)avsync_set_cloexec_if_needed(out_desc->shm_fd);
  (void)avsync_set_cloexec_if_needed(out_desc->event_fd);

  return 0;
}

int avsync_uds_client_detach(const char* uds_path,
                             uint32_t clock_id,
                             AvSyncConsumerKind consumer_kind,
                             uint64_t session_id) {
  if (consumer_kind != AVSYNC_CONSUMER_AUDIO && consumer_kind != AVSYNC_CONSUMER_VIDEO)
    return -EINVAL;

  int conn_fd = avsync_open_and_connect(uds_path);
  if (conn_fd < 0) return conn_fd;

  AvSyncDetachRequestWire req;
  memset(&req, 0, sizeof(req));
  req.hdr.magic = AVSYNC_IPC_MAGIC;
  req.hdr.version = (uint16_t)AVSYNC_IPC_PROTOCOL_VERSION;
  req.hdr.msgType = (uint16_t)AVSYNC_MSG_DETACH;
  req.hdr.msgSize = (uint32_t)sizeof(req);
  req.clockId = clock_id;
  req.consumerKind = (uint32_t)consumer_kind;
  memcpy(&req.sessionId, &session_id, sizeof(session_id)); /* avoid unaligned store assumptions */

  int rc = avsync_send_all(conn_fd, &req, sizeof(req));
  if (rc != 0) {
    close(conn_fd);
    return rc;
  }

  AvSyncDetachReplyWire reply;
  memset(&reply, 0, sizeof(reply));

  /* Detach reply is not expected to include FDs; still use recvmsg for symmetry. */
  int dummy_fds[1] = {-1};
  size_t fd_count = 0;
  ssize_t bytes_read = 0;

  rc = avsync_recv_reply_with_fds(conn_fd,
                                  &reply,
                                  sizeof(reply),
                                  dummy_fds,
                                  1,
                                  &fd_count,
                                  &bytes_read);

  close(conn_fd);

  if (rc != 0) {
    for (size_t i = 0; i < fd_count; ++i) {
      if (dummy_fds[i] >= 0) close(dummy_fds[i]);
    }
    return rc;
  }

  if (fd_count > 0) {
    /* Close unexpected FDs to avoid leaks. */
    for (size_t i = 0; i < fd_count; ++i) {
      if (dummy_fds[i] >= 0) close(dummy_fds[i]);
    }
  }

  if ((size_t)bytes_read < sizeof(reply)) return -EPROTO;

  if (reply.hdr.magic != AVSYNC_IPC_MAGIC ||
      reply.hdr.version != (uint16_t)AVSYNC_IPC_PROTOCOL_VERSION ||
      reply.hdr.msgType != (uint16_t)AVSYNC_MSG_DETACH_REPLY ||
      reply.hdr.msgSize != (uint32_t)sizeof(reply)) {
    return -EPROTO;
  }

  if (reply.status != AVSYNC_STATUS_OK) return (int)reply.status;

  return 0;
}

#else /* !__linux__ */

const char* avsync_ipc_status_to_string(uint32_t status) {
  (void)status;
  return "UNSUPPORTED_PLATFORM";
}

void avsync_uds_client_close_session(AvSyncSessionDescriptor* desc) {
  if (!desc) return;
  desc->shm_fd = -1;
  desc->event_fd = -1;
}

int avsync_uds_client_get_session(const char* uds_path,
                                  uint32_t clock_id,
                                  AvSyncConsumerKind consumer_kind,
                                  AvSyncSessionDescriptor* out_desc) {
  (void)uds_path;
  (void)clock_id;
  (void)consumer_kind;
  if (out_desc) {
    memset(out_desc, 0, sizeof(*out_desc));
    out_desc->shm_fd = -1;
    out_desc->event_fd = -1;
  }
  return -ENOSYS;
}

int avsync_uds_client_detach(const char* uds_path,
                             uint32_t clock_id,
                             AvSyncConsumerKind consumer_kind,
                             uint64_t session_id) {
  (void)uds_path;
  (void)clock_id;
  (void)consumer_kind;
  (void)session_id;
  return -ENOSYS;
}

#endif /* __linux__ */
