// Copyright 2026 Nicholas Guetter. MIT License, see LICENSE.
// Based on Open Screen's cast/standalone_sender, Copyright The Chromium
// Authors, BSD-style license; see THIRD_PARTY_NOTICES.md.

// FrameCast: Cast sender agent. See framecast_agent.h.

#include "cast/framecast_sender/framecast_agent.h"

#include <cstdio>
#include <string>
#include <utility>

#include "cast/common/channel/message_util.h"
#include "cast/common/public/cast_streaming_app_ids.h"
#include "cast/streaming/capture_configs.h"
#include "cast/streaming/public/capture_recommendations.h"
#include "cast/streaming/public/constants.h"
#include "json/value.h"
#include "util/json/json_helpers.h"
#include "util/osp_logging.h"
#include "util/stringprintf.h"

namespace openscreen::cast {

using DeviceMediaPolicy = SenderSocketFactory::DeviceMediaPolicy;

FramecastAgent::FramecastAgent(TaskRunner& task_runner,
                               std::unique_ptr<TrustStore> cast_trust_store,
                               ShutdownCallback shutdown_callback)
    : task_runner_(task_runner),
      shutdown_callback_(std::move(shutdown_callback)),
      connection_handler_(router_, *this),
      socket_factory_(*this,
                      task_runner_,
                      std::move(cast_trust_store),
                      CastCRLTrustStore::Create()),
      connection_factory_(
          TlsConnectionFactory::CreateFactory(socket_factory_, task_runner_)),
      message_port_(router_),
      heartbeat_alarm_(&Clock::now, task_runner_) {
  router_.AddHandlerForLocalId(kPlatformSenderId, this);
  socket_factory_.set_factory(connection_factory_.get());
}

FramecastAgent::~FramecastAgent() {
  // A normal stop (SIGTERM/SIGINT) is not a failure.
  if (!failed_) {
    failed_ = true;
    SetStatus("stopped");
  }
  Shutdown();
}

void FramecastAgent::SetStatus(const std::string& status) {
  OSP_LOG_INFO << "STATUS " << status;
  if (!settings_ || settings_->status_path.empty()) {
    return;
  }
  const std::string tmp = settings_->status_path + ".tmp";
  if (FILE* f = std::fopen(tmp.c_str(), "w")) {
    std::fprintf(f, "%s\n", status.c_str());
    std::fclose(f);
    std::rename(tmp.c_str(), settings_->status_path.c_str());
  }
}

void FramecastAgent::Fail(const std::string& reason) {
  if (!failed_) {
    failed_ = true;
    SetStatus("failed " + reason);
  }
  Shutdown();
}

void FramecastAgent::Connect(FramecastSettings settings) {
  OSP_CHECK(!settings_);
  settings_ = std::move(settings);
  SetStatus("connecting");
  task_runner_.PostTask([this] {
    socket_factory_.Connect(settings_->receiver_endpoint,
                            DeviceMediaPolicy::kIncludesVideo, &router_);
  });
}

void FramecastAgent::OnConnected(SenderSocketFactory* factory,
                                 const IPEndpoint& endpoint,
                                 std::unique_ptr<CastSocket> socket) {
  if (message_port_.GetSocketId() != ToCastSocketId(nullptr)) {
    OSP_LOG_WARN << "Already connected, dropping peer at: " << endpoint;
    return;
  }
  message_port_.SetSocket(socket->GetWeakPtr());
  router_.TakeSocket(this, std::move(socket));

  SetStatus("launching");
  platform_remote_connection_.emplace(VirtualConnection{
      kPlatformSenderId, kPlatformReceiverId, message_port_.GetSocketId()});
  heartbeat_alarm_.ScheduleFromNow([this] { SendHeartbeat(); },
                                   std::chrono::seconds(5));
  connection_handler_.OpenRemoteConnection(
      *platform_remote_connection_,
      [this](bool success) { OnReceiverMessagingOpened(success); });
}

void FramecastAgent::SendHeartbeat() {
  // Cast receivers close the connection when the sender stops sending PINGs.
  if (!platform_remote_connection_) {
    return;
  }
  router_.Send(*platform_remote_connection_,
               MakeSimpleUTF8Message(kHeartbeatNamespace, R"({"type":"PING"})"));
  heartbeat_alarm_.ScheduleFromNow([this] { SendHeartbeat(); },
                                   std::chrono::seconds(5));
}

void FramecastAgent::OnError(SenderSocketFactory* factory,
                             const IPEndpoint& endpoint,
                             const Error& error) {
  OSP_LOG_ERROR << "Cannot connect: " << error;
  Fail("connect");
}

void FramecastAgent::OnClose(CastSocket* cast_socket) {
  OSP_LOG_INFO << "Cast socket closed.";
  Fail("closed");
}

void FramecastAgent::OnError(CastSocket* socket, const Error& error) {
  OSP_LOG_ERROR << "Cast socket error: " << error;
  Fail("socket");
}

bool FramecastAgent::IsConnectionAllowed(
    const VirtualConnection& virtual_conn) const {
  return true;
}

void FramecastAgent::OnMessage(VirtualConnectionRouter* router,
                               CastSocket* socket,
                               proto::CastMessage message) {
  if (message_port_.GetSocketId() == ToCastSocketId(socket) &&
      !message_port_.source_id().empty() &&
      message_port_.source_id() == message.destination_id()) {
    message_port_.OnMessage(router, socket, std::move(message));
    return;
  }

  if (message.destination_id() != kPlatformSenderId &&
      message.destination_id() != kBroadcastId) {
    return;
  }
  if (message.namespace_() == kHeartbeatNamespace) {
    if (GetPayload(message).find("\"PING\"") != std::string::npos &&
        platform_remote_connection_) {
      router_.Send(*platform_remote_connection_,
                   MakeSimpleUTF8Message(kHeartbeatNamespace,
                                         R"({"type":"PONG"})"));
    }
    return;
  }
  if (message.namespace_() != kReceiverNamespace ||
      message_port_.GetSocketId() != ToCastSocketId(socket)) {
    return;
  }

  const ErrorOr<Json::Value> payload = json::Parse(GetPayload(message));
  if (payload.is_error() || !payload.value().isObject()) {
    OSP_LOG_ERROR << "Cannot parse receiver message.";
    return;
  }

  if (HasType(payload.value(), CastMessageType::kReceiverStatus)) {
    HandleReceiverStatus(payload.value());
  } else if (HasType(payload.value(), CastMessageType::kLaunchError)) {
    std::string reason;
    if (!json::TryParseString(payload.value()[kMessageKeyReason], &reason)) {
      reason = "UNKNOWN";
    }
    OSP_LOG_ERROR << "The receiver cannot launch the mirroring app: " << reason;
    Fail("launch " + reason);
  } else if (HasType(payload.value(), CastMessageType::kInvalidRequest)) {
    std::string reason;
    if (!json::TryParseString(payload.value()[kMessageKeyReason], &reason)) {
      reason = "UNKNOWN";
    }
    OSP_LOG_ERROR << "The receiver says the request is invalid: " << reason;
  }
}

void FramecastAgent::HandleReceiverStatus(const Json::Value& status) {
  const Json::Value& details =
      (status[kMessageKeyStatus].isObject() &&
       status[kMessageKeyStatus][kMessageKeyApplications].isArray())
          ? status[kMessageKeyStatus][kMessageKeyApplications][0]
          : Json::Value();

  std::string running_app_id;
  if (!json::TryParseString(details[kMessageKeyAppId], &running_app_id) ||
      running_app_id != GetCastStreamingAudioVideoAppId()) {
    if (has_launched_) {
      // Someone stopped the mirroring app or started another app on the TV.
      // Do not send STOP: it would stop the other app.
      app_session_id_.clear();
      if (!failed_) {
        failed_ = true;
        SetStatus("ended");
      }
      Shutdown();
    }
    return;
  }
  has_launched_ = true;

  std::string session_id;
  if (!json::TryParseString(details[kMessageKeySessionId], &session_id) ||
      session_id.empty()) {
    Fail("nosession");
    return;
  }
  if (app_session_id_ != session_id) {
    if (app_session_id_.empty()) {
      app_session_id_ = session_id;
    } else {
      // Another sender took over the mirroring app.
      app_session_id_.clear();
      if (!failed_) {
        failed_ = true;
        SetStatus("ended");
      }
      Shutdown();
      return;
    }
  }

  if (remote_connection_) {
    return;
  }

  std::string transport_id;
  if (!json::TryParseString(details[kMessageKeyTransportId], &transport_id) ||
      transport_id.empty()) {
    Fail("notransport");
    return;
  }

  remote_connection_.emplace(VirtualConnection{
      MakeUniqueSessionId("streaming_sender"), transport_id,
      message_port_.GetSocketId()});
  connection_handler_.OpenRemoteConnection(
      *remote_connection_,
      [this](bool success) { OnRemoteMessagingOpened(success); });
}

void FramecastAgent::OnReceiverMessagingOpened(bool success) {
  if (!success) {
    Fail("connect");
    return;
  }
  static constexpr char kLaunchMessageTemplate[] =
      R"({{"type":"LAUNCH", "requestId":{}, "appId":"{}", "language": "en-US",
       "supportedAppTypes":["WEB"]}})";
  router_.Send(*platform_remote_connection_,
               MakeSimpleUTF8Message(
                   kReceiverNamespace,
                   StringFormat(kLaunchMessageTemplate, next_request_id_++,
                                GetCastStreamingAudioVideoAppId())));
}

void FramecastAgent::OnRemoteMessagingOpened(bool success) {
  if (!remote_connection_) {
    return;
  }
  if (!success) {
    Fail("busy");
    return;
  }
  CreateAndStartSession();
}

void FramecastAgent::CreateAndStartSession() {
  SetStatus("negotiating");
  environment_ =
      std::make_unique<Environment>(&Clock::now, task_runner_, IPEndpoint{});

  SenderSession::Configuration config{
      settings_->receiver_endpoint.address,
      *this,
      environment_.get(),
      &message_port_,
      remote_connection_->local_id,
      remote_connection_->peer_id,
      settings_->use_android_rtp_hack,
      /*enable_dscp=*/true};
  current_session_ = std::make_unique<SenderSession>(std::move(config));

  VideoCaptureConfig video_config;
  video_config.codec = settings_->vp8 ? VideoCodec::kVp8 : VideoCodec::kH264;
  video_config.max_frame_rate = SimpleFraction{settings_->fps, 1};
  video_config.max_bit_rate = settings_->max_bitrate;
  video_config.resolutions.emplace_back(
      Resolution{settings_->width, settings_->height});
  video_config.target_playout_delay = settings_->playout_delay;

  // Note: with a silent audio stream of zero-length Opus frames, the classic
  // Chromecast sent no RTCP for either stream. Audio must be real Opus.
  std::vector<AudioCaptureConfig> audio_configs;
  if (settings_->audio_fd >= 0) {
    AudioCaptureConfig audio_config;
    audio_config.codec = AudioCodec::kOpus;
    audio_config.channels = 2;
    audio_config.bit_rate = settings_->audio_bitrate;
    audio_config.sample_rate = 48000;
    audio_config.target_playout_delay = settings_->playout_delay;
    audio_configs.push_back(audio_config);
  }
  const Error error =
      current_session_->Negotiate(audio_configs, {video_config});
  if (!error.ok()) {
    OSP_LOG_ERROR << "Cannot negotiate: " << error;
    Fail("negotiate");
  }
}

void FramecastAgent::OnNegotiated(
    const SenderSession* session,
    SenderSession::ConfiguredSenders senders,
    capture_recommendations::Recommendations capture_recommendations) {
  if (!senders.video_sender) {
    OSP_LOG_ERROR << "The receiver did not accept the H.264 video stream.";
    Fail("codec");
    return;
  }
  OSP_LOG_INFO << "Negotiated: video " << senders.video_config.codec
               << (senders.audio_sender ? " + audio" : "");
  if (senders.audio_sender) {
    audio_pipe_sender_ = std::make_unique<OpusPipeSender>(
        *environment_, settings_->audio_fd, std::move(senders.audio_sender));
  } else if (settings_->audio_fd >= 0) {
    OSP_LOG_WARN << "The receiver did not accept the Opus audio stream.";
  }
  pipe_sender_ = std::make_unique<H264PipeSender>(
      *environment_, settings_->input_fd, settings_->key_request_fd,
      std::move(senders), [this] {
        if (!failed_) {
          failed_ = true;
          SetStatus("input-ended");
        }
        Shutdown();
      });
  SetStatus("streaming");
}

void FramecastAgent::OnError(const SenderSession* session,
                             const Error& error) {
  OSP_LOG_ERROR << "Session error: " << error;
  Fail("session");
}

void FramecastAgent::StopReceiverApp() {
  if (!failed_) {
    failed_ = true;
    SetStatus("stopped");
  }
  audio_pipe_sender_.reset();
  pipe_sender_.reset();
  current_session_.reset();
  environment_.reset();
  SendStop();
}

void FramecastAgent::SendStop() {
  if (app_session_id_.empty() ||
      message_port_.GetSocketId() == ToCastSocketId(nullptr)) {
    return;
  }
  OSP_LOG_INFO << "Stopping the mirroring app on the receiver...";
  static constexpr char kStopMessageTemplate[] =
      R"({{"type":"STOP", "requestId":{}, "sessionId":"{}"}})";
  router_.Send(
      VirtualConnection{kPlatformSenderId, kPlatformReceiverId,
                        message_port_.GetSocketId()},
      MakeSimpleUTF8Message(
          kReceiverNamespace,
          StringFormat(kStopMessageTemplate, next_request_id_++,
                       app_session_id_.c_str())));
  app_session_id_.clear();
}

void FramecastAgent::Shutdown() {
  heartbeat_alarm_.Cancel();
  audio_pipe_sender_.reset();
  pipe_sender_.reset();
  current_session_.reset();
  environment_.reset();

  if (platform_remote_connection_) {
    const VirtualConnection connection = *platform_remote_connection_;
    platform_remote_connection_.reset();
    connection_handler_.CloseRemoteConnection(connection);
  }
  if (remote_connection_) {
    const VirtualConnection connection = *remote_connection_;
    remote_connection_.reset();
    connection_handler_.CloseRemoteConnection(connection);
  }

  SendStop();

  if (message_port_.GetSocketId() != ToCastSocketId(nullptr)) {
    router_.CloseSocket(message_port_.GetSocketId());
    message_port_.SetSocket({});
  }

  if (shutdown_callback_) {
    const ShutdownCallback callback = std::move(shutdown_callback_);
    callback();
  }
}

}  // namespace openscreen::cast
