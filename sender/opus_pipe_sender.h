// Copyright 2026 Nicholas Guetter. MIT License, see LICENSE.

// FrameCast: feeds Ogg Opus (read from a file descriptor, e.g. from
//   ffmpeg -f pulse -i @DEFAULT_MONITOR@ -c:a libopus -frame_duration 10
//          -page_duration 10000 -flush_packets 1 -f ogg -)
// into the audio Sender of a negotiated Cast Streaming session. Each Opus
// packet becomes one Cast audio frame.

#ifndef CAST_FRAMECAST_SENDER_OPUS_PIPE_SENDER_H_
#define CAST_FRAMECAST_SENDER_OPUS_PIPE_SENDER_H_

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "cast/streaming/public/sender.h"
#include "cast/streaming/public/environment.h"
#include "platform/api/time.h"
#include "util/alarm.h"
#include "util/weak_ptr.h"

namespace openscreen::cast {

class OpusPipeSender final {
 public:
  OpusPipeSender(Environment& environment,
                 int input_fd,
                 std::unique_ptr<Sender> audio_sender);
  ~OpusPipeSender();

  OpusPipeSender(const OpusPipeSender&) = delete;
  OpusPipeSender& operator=(const OpusPipeSender&) = delete;

 private:
  // Reader thread: parses Ogg pages and posts each Opus packet.
  void ReadLoop();

  // Main task runner.
  void SendPacket(std::vector<uint8_t> packet, Clock::time_point arrived);
  void LogStats();

  Environment& env_;
  const int input_fd_;
  std::unique_ptr<Sender> audio_sender_;

  // Audio timeline: RTP time counts samples; the reference time is the
  // arrival time of the first packet plus the samples sent since then.
  Clock::time_point start_ = Clock::time_point::min();
  int64_t samples_ = 0;

  int packets_in_ = 0;
  int packets_sent_ = 0;
  int packets_dropped_ = 0;
  int resyncs_ = 0;
  Alarm stats_alarm_;

  std::atomic<bool> stop_{false};
  std::thread reader_;

  WeakPtrFactory<OpusPipeSender> weak_factory_{this};
};

}  // namespace openscreen::cast

#endif  // CAST_FRAMECAST_SENDER_OPUS_PIPE_SENDER_H_
