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

#ifndef AVSYNC_IPC_PROTOCOL_H
#define AVSYNC_IPC_PROTOCOL_H

/**
 * Vendor-private AVSync IPC protocol definitions shared by:
 *  - sink-side UDS client (AudioSink/VideoSink)
 *  - server-side session server (AVClock process or broker)
 *
 * Wire protocol and semantics are aligned with the protocol design doc:
 *   avclock-cross-process-shm-eventfd-protocol_v4.md
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef AVSYNC_UDS_DEFAULT_PATH
#define AVSYNC_UDS_DEFAULT_PATH "/run/avsync/avclock.sock"
#endif

/**
 * IPC protocol version (UDS message protocol version).
 * Must match the server's supported version for the handshake to succeed.
 */
#define AVSYNC_IPC_PROTOCOL_VERSION 1u

/**
 * For ipc protocol v1, the server returns exactly 2 FDs in SCM_RIGHTS:
 *   [0] shm FD, [1] eventfd (unique per consumer attachment)
 */
#define AVSYNC_IPC_V1_EXPECTED_FD_COUNT 2u

/* Protocol constants (aligned with the protocol document). */
#define AVSYNC_IPC_MAGIC 0x41565349u /* 'AVSI' */

/**
 * IPC message types.
 *
 * Note: The protocol document also mentions optional PING/PONG. They are
 * included here for completeness, even if a given implementation does not
 * currently use them.
 */
typedef enum AvSyncIpcMsgType {
  AVSYNC_MSG_GET_SESSION = 1,
  AVSYNC_MSG_GET_SESSION_REPLY = 2,
  AVSYNC_MSG_DETACH = 3,
  AVSYNC_MSG_DETACH_REPLY = 4,
  AVSYNC_MSG_PING = 5,
  AVSYNC_MSG_PONG = 6
} AvSyncIpcMsgType;

/**
 * Consumer kind (sink role) requesting an eventfd attachment.
 * Values match the protocol document.
 */
typedef enum AvSyncConsumerKind {
  AVSYNC_CONSUMER_AUDIO = 1,
  AVSYNC_CONSUMER_VIDEO = 2
} AvSyncConsumerKind;

/**
 * Server status codes returned in replies.
 * Values match the protocol document.
 */
typedef enum AvSyncIpcStatus {
  AVSYNC_STATUS_OK = 0,
  AVSYNC_STATUS_ERR_UNSUPPORTED_VERSION = 1,
  AVSYNC_STATUS_ERR_BAD_REQUEST = 2,
  AVSYNC_STATUS_ERR_CLOCK_NOT_FOUND = 3,
  AVSYNC_STATUS_ERR_NO_ACTIVE_SESSION = 4,
  AVSYNC_STATUS_ERR_UNAUTHORIZED = 5,
  AVSYNC_STATUS_ERR_INTERNAL = 6
} AvSyncIpcStatus;

#pragma pack(push, 1)
typedef struct AvSyncIpcHeaderWire {
  uint32_t magic;
  uint16_t version; /* ipcProtocolVersion */
  uint16_t msgType; /* AvSyncIpcMsgType */
  uint32_t msgSize; /* bytes including header */
  uint32_t reserved0;
} AvSyncIpcHeaderWire;

typedef struct AvSyncGetSessionRequestWire {
  AvSyncIpcHeaderWire hdr;
  uint32_t clockId;
  uint32_t consumerKind;
  uint32_t desiredShmAccess; /* 0 = read-only (expected for sinks) */
  uint32_t reserved0;
} AvSyncGetSessionRequestWire;

typedef struct AvSyncGetSessionReplyWire {
  AvSyncIpcHeaderWire hdr;
  uint32_t status; /* AvSyncIpcStatus */
  uint32_t clockId;
  uint64_t sessionId;
  uint32_t shmSizeBytes;
  uint32_t shmLayoutVersion;
  uint32_t flags;
  uint32_t fdCountExpected;
} AvSyncGetSessionReplyWire;

typedef struct AvSyncDetachRequestWire {
  AvSyncIpcHeaderWire hdr;
  uint32_t clockId;
  uint32_t consumerKind;
  uint64_t sessionId;
} AvSyncDetachRequestWire;

typedef struct AvSyncDetachReplyWire {
  AvSyncIpcHeaderWire hdr;
  uint32_t status; /* AvSyncIpcStatus */
  uint32_t reserved0;
} AvSyncDetachReplyWire;
#pragma pack(pop)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVSYNC_IPC_PROTOCOL_H */
