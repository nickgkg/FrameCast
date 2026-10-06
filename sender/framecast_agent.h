// Copyright 2026 Nicholas Guetter. MIT License, see LICENSE.
// Based on Open Screen's cast/standalone_sender, Copyright The Chromium
// Authors, BSD-style license; see THIRD_PARTY_NOTICES.md.

// FrameCast: a single-use Cast sender agent. It connects to a Cast device,
// launches the built-in mirroring receiver app (0F5096E8), negotiates an H.264
// (+ optional Opus) Cast Streaming session and then streams H.264 and Ogg Opus
// read from pipes.
//
// Trimmed from openscreen's LoopingFileCastAgent (cast/standalone_sender).

#ifndef CAST_FRAMECAST_SENDER_FRAMECAST_AGENT_H_
#define CAST_FRAMECAST_SENDER_FRAMECAST_AGENT_H_

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "cast/common/channel/cast_message_handler.h"
#include "cast/common/channel/cast_socket_message_port.h"
#include "cast/common/channel/connection_namespace_handler.h"
#include "cast/common/channel/virtual_connection_router.h"
#include "cast/common/public/cast_socket.h"
#include "cast/common/public/trust_store.h"
#include "cast/framecast_sender/h264_pipe_sender.h"
#include "cast/framecast_sender/opus_pipe_sender.h"
#include "cast/sender/public/sender_socket_factory.h"
#include "cast/streaming/public/sender_session.h"
#include "platform/api/task_runner.h"
#include "platform/api/tls_connection_factory.h"
#include "platform/base/ip_address.h"
#include "util/alarm.h"

namespace Json {
class Value;
}

namespace openscreen::cast {

struct FramecastSettings {
  IPEndpoint receiver_endpoint;
  int input_fd = 0;
  // Ogg Opus input, or -1 for a video-only session.
  int audio_fd = -1;
  int audio_bitrate = 128'000;
  // Gets one byte when the video needs a key frame, or -1.
  int key_request_fd = -1;
  int max_bitrate = 4'000'000;
  int width = 1280;
  int height = 720;
  int fps = 30;
  std::chrono::milliseconds playout_delay{150};
  bool use_android_rtp_hack = false;
  // Offer VP8 and read IVF from the input instead of H.264 Annex-B.
  bool vp8 = false;
  // Optional file that gets one-line status updates for the control script:
  // "connecting", "launching", "negotiating", "streaming", "ended" (the TV
  // switched to another app), "input-ended", "stopped", "failed <reason>".
  std::string status_path;
};

class FramecastAgent final
    : public SenderSocketFactory::Client,
      public VirtualConnectionRouter::SocketErrorHandler,
      public ConnectionNamespaceHandler::VirtualConnectionPolicy,
      public CastMessageHandler,
      public SenderSession::Client {
 public:
  using ShutdownCallback = std::function<void()>;

  FramecastAgent(TaskRunner& task_runner,
                 std::unique_ptr<TrustStore> cast_trust_store,
                 ShutdownCallback shutdown_callback);
  ~FramecastAgent();

  void Connect(FramecastSettings settings);

  // Stops streaming and tells the receiver to stop the mirroring app. Keeps
  // the connection open, so that the STOP message can go out before the
  // agent is destroyed.
  void StopReceiverApp();

 private:
  // SenderSocketFactory::Client overrides.
  void OnConnected(SenderSocketFactory* factory,
                   const IPEndpoint& endpoint,
                   std::unique_ptr<CastSocket> socket) override;
  void OnError(SenderSocketFactory* factory,
               const IPEndpoint& endpoint,
               const Error& error) override;

  // VirtualConnectionRouter::SocketErrorHandler overrides.
  void OnClose(CastSocket* cast_socket) override;
  void OnError(CastSocket* socket, const Error& error) override;

  // ConnectionNamespaceHandler::VirtualConnectionPolicy overrides.
  bool IsConnectionAllowed(
      const VirtualConnection& virtual_conn) const override;

  // CastMessageHandler overrides.
  void OnMessage(VirtualConnectionRouter* router,
                 CastSocket* socket,
                 proto::CastMessage message) override;

  // SenderSession::Client overrides.
  void OnNegotiated(const SenderSession* session,
                    SenderSession::ConfiguredSenders senders,
                    capture_recommendations::Recommendations
                        capture_recommendations) override;
  void OnCapabilitiesDetermined(const SenderSession* session,
                                RemotingCapabilities capabilities) override {}
  void OnError(const SenderSession* session, const Error& error) override;

  void HandleReceiverStatus(const Json::Value& status);
  void OnReceiverMessagingOpened(bool success);
  void OnRemoteMessagingOpened(bool success);
  void CreateAndStartSession();
  void SendHeartbeat();
  void SendStop();
  void SetStatus(const std::string& status);
  void Fail(const std::string& reason);
  void Shutdown();

  TaskRunner& task_runner_;
  ShutdownCallback shutdown_callback_;
  VirtualConnectionRouter router_;
  ConnectionNamespaceHandler connection_handler_;
  SenderSocketFactory socket_factory_;
  std::unique_ptr<TlsConnectionFactory> connection_factory_;
  CastSocketMessagePort message_port_;
  Alarm heartbeat_alarm_;

  int next_request_id_ = 1;
  std::optional<FramecastSettings> settings_;
  std::string app_session_id_;
  std::optional<VirtualConnection> remote_connection_;
  std::optional<VirtualConnection> platform_remote_connection_;
  bool has_launched_ = false;
  bool failed_ = false;

  std::unique_ptr<Environment> environment_;
  std::unique_ptr<SenderSession> current_session_;
  std::unique_ptr<H264PipeSender> pipe_sender_;
  std::unique_ptr<OpusPipeSender> audio_pipe_sender_;
};

}  // namespace openscreen::cast

#endif  // CAST_FRAMECAST_SENDER_FRAMECAST_AGENT_H_
