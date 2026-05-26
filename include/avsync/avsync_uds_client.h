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

#ifndef AVSYNC_UDS_CLIENT_H
#define AVSYNC_UDS_CLIENT_H

/**
 * Vendor-private AVSync UDS client API for sink processes.
 *
 * This library is intended to be linked by AudioSink/VideoSink implementations.
 * It connects to the AVSync session server over an AF_UNIX socket and requests
 * the current AVClock session descriptor for a given clockId. On success, the
 * server responds with:
 *  - shm FD (for shared snapshot reads)
 *  - per-consumer eventfd (for wakeups)
 * passed via SCM_RIGHTS.
 *
 * Wire protocol and semantics are aligned with the protocol document:
 *   avclock-cross-process-shm-eventfd-protocol_v4.md
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "avsync/avsync_ipc_protocol.h"

/**
 * Session descriptor returned to sinks.
 *
 * Note: The shm layout structs are defined by the protocol and are not
 * repeated here; sinks map shm and interpret it according to the shared
 * layout agreed with the AVClock-side implementation.
 */
typedef struct AvSyncSessionDescriptor {
  uint32_t clock_id;           /* IAVClock.Id */
  uint64_t session_id;         /* unique per open() session */
  uint32_t shm_size_bytes;     /* mmap length */
  uint32_t shm_layout_version; /* shm header.version */
  uint32_t flags;              /* reserved for future features */

  int shm_fd;   /* received via SCM_RIGHTS */
  int event_fd; /* received via SCM_RIGHTS (per consumer) */
} AvSyncSessionDescriptor;

/**
 * Return value conventions for avsync_uds_client_* calls:
 *   - 0  : success
 *   - >0 : AvSyncIpcStatus (server returned an explicit error status)
 *   - <0 : local error (typically -errno or -EINVAL / -EPROTO / -ENOSYS)
 */

/* PUBLIC_INTERFACE */
int avsync_uds_client_get_session(const char* uds_path,
                                  uint32_t clock_id,
                                  AvSyncConsumerKind consumer_kind,
                                  AvSyncSessionDescriptor* out_desc);
/**
 * Perform the UDS GET_SESSION handshake and receive shm/eventfd via SCM_RIGHTS.
 *
 * @param uds_path UDS endpoint. If NULL or empty, AVSYNC_UDS_DEFAULT_PATH is used.
 *                 If uds_path begins with '@', it is treated as a Linux abstract
 *                 namespace address (leading NUL in sun_path).
 * @param clock_id IAVClock.Id to request.
 * @param consumer_kind AVSYNC_CONSUMER_AUDIO or AVSYNC_CONSUMER_VIDEO.
 * @param out_desc Output descriptor. On success, caller owns shm_fd/event_fd and
 *                 must eventually call avsync_uds_client_close_session().
 *
 * @return 0 on success; >0 AvSyncIpcStatus on server failure; <0 on local failure.
 */

/* PUBLIC_INTERFACE */
int avsync_uds_client_detach(const char* uds_path,
                             uint32_t clock_id,
                             AvSyncConsumerKind consumer_kind,
                             uint64_t session_id);
/**
 * Best-effort DETACH notification to the server (optional protocol feature).
 *
 * Detach can also be implicit by closing the FDs and/or the UDS connection;
 * this call exists to allow server-side bookkeeping cleanup.
 *
 * @return 0 on success; >0 AvSyncIpcStatus on server failure; <0 on local failure.
 */

/* PUBLIC_INTERFACE */
void avsync_uds_client_close_session(AvSyncSessionDescriptor* desc);
/**
 * Close any open FDs inside the descriptor and reset them to -1.
 * Safe to call multiple times.
 */

/* PUBLIC_INTERFACE */
const char* avsync_ipc_status_to_string(uint32_t status);
/**
 * Convert an AvSyncIpcStatus code to a stable string for logging.
 * Unknown values return "UNKNOWN_STATUS".
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVSYNC_UDS_CLIENT_H */
