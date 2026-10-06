// Copyright 2026 Nicholas Guetter. MIT License, see LICENSE.
// Based on Open Screen's cast/standalone_sender, Copyright The Chromium
// Authors, BSD-style license; see THIRD_PARTY_NOTICES.md.

// FrameCast: feeds an H.264 Annex-B stream (read from a file descriptor, e.g.
// stdin from ffmpeg/x264) into a negotiated Cast Streaming session.
//
// The input must carry an access unit delimiter (AUD, NAL type 9) at the start
// of each access unit (x264 option aud=1) and SPS/PPS before each IDR frame
// (x264 option repeat-headers=1). It must not use B-frames.
//
// If key_request_fd >= 0, one byte is written to it whenever the stream needs
// a key frame (receiver picture loss, or a dropped frame); hwenc reads it and
// forces an IDR.

#ifndef CAST_FRAMECAST_SENDER_H264_PIPE_SENDER_H_
#define CAST_FRAMECAST_SENDER_H264_PIPE_SENDER_H_

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "cast/streaming/public/sender_session.h"
#include "platform/api/time.h"
#include "util/alarm.h"
#include "util/weak_ptr.h"

namespace openscreen::cast {

class H264PipeSender final : public Sender::Observer {
 public:
  using ShutdownCallback = std::function<void()>;

  H264PipeSender(Environment& environment,
                 int input_fd,
                 int key_request_fd,
                 SenderSession::ConfiguredSenders senders,
                 ShutdownCallback shutdown_callback);
  ~H264PipeSender() override;

  H264PipeSender(const H264PipeSender&) = delete;
  H264PipeSender& operator=(const H264PipeSender&) = delete;

 private:
  // Runs on the reader thread: splits the byte stream into access units and
  // posts each one to the main task runner.
  void ReadLoop();

  // Main task runner.
  void SendAccessUnit(std::vector<uint8_t> au, Clock::time_point captured);
  void LogStats();
  void RequestKeyFrame();

  // Sender::Observer overrides.
  void OnFrameCanceled(FrameId frame_id) override {}
  void OnPictureLost() override;

  Environment& env_;
  const int input_fd_;
  const int key_request_fd_;
  Clock::time_point last_key_request_ = Clock::time_point::min();
  // Input is IVF-framed VP8 (for receivers that need VP8) instead of H.264.
  const bool ivf_;
  bool ivf_header_done_ = false;
  std::unique_ptr<Sender> video_sender_;
  ShutdownCallback shutdown_callback_;

  // Video timeline.
  Clock::time_point video_start_ = Clock::time_point::min();
  RtpTimeTicks last_video_rtp_;
  FrameId last_video_frame_id_;
  bool have_sent_video_ = false;
  // After a dropped frame, later frames reference a frame the receiver never
  // got; skip until the next key frame.
  bool waiting_for_key_ = true;

  // Statistics, logged every few seconds.
  int frames_in_ = 0;
  int frames_sent_ = 0;
  int frames_dropped_ = 0;
  int key_requests_ = 0;
  int64_t bytes_sent_ = 0;

  Alarm stats_alarm_;

  std::atomic<bool> stop_{false};
  std::thread reader_;

  // Must be last: invalidated first, so tasks posted by the reader thread after
  // destruction starts do nothing.
  WeakPtrFactory<H264PipeSender> weak_factory_{this};
};

}  // namespace openscreen::cast

#endif  // CAST_FRAMECAST_SENDER_H264_PIPE_SENDER_H_
