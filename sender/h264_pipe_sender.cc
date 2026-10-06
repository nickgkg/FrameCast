// Copyright 2026 Nicholas Guetter. MIT License, see LICENSE.
// Based on Open Screen's cast/standalone_sender, Copyright The Chromium
// Authors, BSD-style license; see THIRD_PARTY_NOTICES.md.

// FrameCast: H.264 Annex-B pipe -> Cast Streaming. See h264_pipe_sender.h.

#include "cast/framecast_sender/h264_pipe_sender.h"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <utility>

#include "cast/streaming/public/encoded_frame.h"
#include "platform/base/span.h"
#include "util/chrono_helpers.h"
#include "util/osp_logging.h"

namespace openscreen::cast {

namespace {

constexpr auto kStatsInterval = std::chrono::seconds(5);
// Larger than any sane access unit; protects against a stream with no AUDs.
constexpr size_t kMaxAccessUnitSize = 8 << 20;

// Returns the NAL unit type of the NAL that starts at `p` (just after the
// start code).
int NalType(const uint8_t* p) {
  return p[0] & 0x1f;
}

// True if the access unit contains an IDR slice.
bool ContainsIdr(const std::vector<uint8_t>& au) {
  const size_t n = au.size();
  for (size_t i = 0; i + 3 < n; ++i) {
    if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
      if (NalType(&au[i + 3]) == 5) {
        return true;
      }
      i += 2;
    }
  }
  return false;
}

}  // namespace

H264PipeSender::H264PipeSender(Environment& environment,
                               int input_fd,
                               int key_request_fd,
                               SenderSession::ConfiguredSenders senders,
                               ShutdownCallback shutdown_callback)
    : env_(environment),
      input_fd_(input_fd),
      key_request_fd_(key_request_fd),
      ivf_(senders.video_config.codec == VideoCodec::kVp8),
      video_sender_(std::move(senders.video_sender)),
      shutdown_callback_(std::move(shutdown_callback)),
      stats_alarm_(env_.now_function(), env_.task_runner()) {
  OSP_CHECK(video_sender_);
  OSP_LOG_INFO << "H264PipeSender: video ssrc=" << video_sender_->config().sender_ssrc
               << " playout delay="
               << to_milliseconds(video_sender_->config().target_playout_delay).count()
               << "ms";
  video_sender_->SetObserver(this);
  stats_alarm_.ScheduleFromNow([this] { LogStats(); }, kStatsInterval);
  reader_ = std::thread([this] { ReadLoop(); });
}

H264PipeSender::~H264PipeSender() {
  video_sender_->SetObserver(nullptr);
  stop_ = true;
  if (reader_.joinable()) {
    reader_.join();
  }
}

void H264PipeSender::ReadLoop() {
  const WeakPtr<H264PipeSender> weak = weak_factory_.GetWeakPtr();
  TaskRunner& task_runner = env_.task_runner();

  std::vector<uint8_t> buf;   // Bytes not yet assigned to an access unit.
  std::vector<uint8_t> chunk(256 << 10);
  size_t scan_from = 0;       // Where to resume the start-code search.
  ssize_t au_start = -1;      // Offset of the current AU's AUD start code.

  while (!stop_) {
    pollfd pfd{input_fd_, POLLIN, 0};
    const int pr = poll(&pfd, 1, 100);
    if (pr < 0 && errno != EINTR) {
      break;
    }
    if (pr <= 0) {
      continue;
    }
    const ssize_t got = read(input_fd_, chunk.data(), chunk.size());
    if (got < 0 && (errno == EINTR || errno == EAGAIN)) {
      continue;
    }
    if (got <= 0) {
      OSP_LOG_INFO << "H264PipeSender: end of input";
      break;
    }
    buf.insert(buf.end(), chunk.begin(), chunk.begin() + got);

    if (ivf_) {
      // IVF: 32-byte file header, then 12-byte frame headers (LE size, pts).
      size_t pos = ivf_header_done_ ? 0 : 32;
      if (!ivf_header_done_ && buf.size() < 32) {
        continue;
      }
      ivf_header_done_ = true;
      while (buf.size() >= pos + 12) {
        const uint32_t size = buf[pos] | (buf[pos + 1] << 8) |
                              (buf[pos + 2] << 16) |
                              (static_cast<uint32_t>(buf[pos + 3]) << 24);
        if (buf.size() < pos + 12 + size) {
          break;
        }
        std::vector<uint8_t> au(buf.begin() + pos + 12,
                                buf.begin() + pos + 12 + size);
        pos += 12 + size;
        const Clock::time_point now = Clock::now();
        task_runner.PostTask([weak, au = std::move(au), now]() mutable {
          if (H264PipeSender* self = weak.get()) {
            self->SendAccessUnit(std::move(au), now);
          }
        });
      }
      buf.erase(buf.begin(), buf.begin() + pos);
      continue;
    }

    // Find AUD NAL units (00 00 01 09, possibly with a leading 00). Each one
    // ends the previous access unit.
    size_t i = scan_from;
    for (; i + 3 < buf.size(); ++i) {
      if (buf[i] != 0 || buf[i + 1] != 0 || buf[i + 2] != 1) {
        continue;
      }
      if (NalType(&buf[i + 3]) == 9) {
        const size_t sc = (i > 0 && buf[i - 1] == 0) ? i - 1 : i;
        if (au_start >= 0 && sc > static_cast<size_t>(au_start)) {
          std::vector<uint8_t> au(buf.begin() + au_start, buf.begin() + sc);
          const Clock::time_point now = Clock::now();
          task_runner.PostTask([weak, au = std::move(au), now]() mutable {
            if (H264PipeSender* self = weak.get()) {
              self->SendAccessUnit(std::move(au), now);
            }
          });
        }
        au_start = static_cast<ssize_t>(sc);
      }
      i += 2;
    }
    scan_from = (i > 3) ? i - 3 : 0;

    // Drop consumed bytes.
    const size_t keep_from =
        au_start >= 0 ? static_cast<size_t>(au_start) : scan_from;
    if (keep_from > 0) {
      buf.erase(buf.begin(), buf.begin() + keep_from);
      scan_from -= keep_from;
      if (au_start >= 0) {
        au_start = 0;
      }
    }
    if (buf.size() > kMaxAccessUnitSize) {
      OSP_LOG_ERROR << "H264PipeSender: no access unit delimiters in input "
                       "(use x264 aud=1)";
      buf.clear();
      scan_from = 0;
      au_start = -1;
    }
  }

  if (!stop_) {
    task_runner.PostTask([weak] {
      if (H264PipeSender* self = weak.get()) {
        if (self->shutdown_callback_) {
          self->shutdown_callback_();
        }
      }
    });
  }
}

void H264PipeSender::SendAccessUnit(std::vector<uint8_t> au,
                                    Clock::time_point captured) {
  ++frames_in_;
  if (au.empty()) {
    return;
  }
  // VP8: bit 0 of the first byte is 0 for a key frame.
  const bool is_key = ivf_ ? (au[0] & 1) == 0 : ContainsIdr(au);
  if (!is_key && (waiting_for_key_ || video_sender_->NeedsKeyFrame())) {
    RequestKeyFrame();
  }
  if (waiting_for_key_ && !is_key) {
    ++frames_dropped_;
    return;
  }

  if (video_start_ == Clock::time_point::min()) {
    video_start_ = captured;
  }
  RtpTimeTicks rtp = RtpTimeTicks::FromTimeSinceOrigin(
      captured - video_start_, video_sender_->config().rtp_timebase);
  if (have_sent_video_ && rtp <= last_video_rtp_) {
    rtp = last_video_rtp_ + RtpTimeDelta::FromTicks(1);
  }

  if (video_sender_->GetInFlightMediaDuration(rtp) >
      video_sender_->GetMaxInFlightMediaDuration()) {
    ++frames_dropped_;
    waiting_for_key_ = true;
    RequestKeyFrame();
    return;
  }

  EncodedFrame frame;
  frame.frame_id = video_sender_->GetNextFrameId();
  if (is_key) {
    frame.dependency = EncodedFrame::Dependency::kKeyFrame;
    frame.referenced_frame_id = frame.frame_id;
  } else {
    frame.dependency = EncodedFrame::Dependency::kDependent;
    frame.referenced_frame_id = last_video_frame_id_;
  }
  frame.rtp_timestamp = rtp;
  frame.reference_time = captured;
  frame.capture_begin_time = captured;
  frame.capture_end_time = captured;
  frame.data = ByteView(au.data(), au.size());

  const Sender::EnqueueFrameResult result = video_sender_->EnqueueFrame(frame);
  if (result != Sender::OK) {
    ++frames_dropped_;
    waiting_for_key_ = true;
    RequestKeyFrame();
    OSP_VLOG << "H264PipeSender: EnqueueFrame rejected (" << result << ")";
    return;
  }
  waiting_for_key_ = false;
  have_sent_video_ = true;
  last_video_rtp_ = rtp;
  last_video_frame_id_ = frame.frame_id;
  ++frames_sent_;
  bytes_sent_ += static_cast<int64_t>(au.size());
}

void H264PipeSender::OnPictureLost() {
  RequestKeyFrame();
}

void H264PipeSender::RequestKeyFrame() {
  if (key_request_fd_ < 0) {
    return;
  }
  // The encoder makes the next frame an IDR; do not ask again for each frame
  // that is already in the pipe.
  const Clock::time_point now = Clock::now();
  if (last_key_request_ != Clock::time_point::min() &&
      now - last_key_request_ < std::chrono::milliseconds(200)) {
    return;
  }
  last_key_request_ = now;
  ++key_requests_;
  const char k = 'k';
  (void)!write(key_request_fd_, &k, 1);
}

void H264PipeSender::LogStats() {
  const auto secs = std::chrono::duration_cast<std::chrono::seconds>(kStatsInterval).count();
  OSP_LOG_INFO << "H264PipeSender: in=" << frames_in_ << " sent=" << frames_sent_
               << " dropped=" << frames_dropped_
               << " keyreq=" << key_requests_ << " kbps="
               << (bytes_sent_ * 8 / 1000 / secs) << " rtt="
               << to_milliseconds(video_sender_->GetCurrentRoundTripTime()).count()
               << "ms inflight=" << video_sender_->GetInFlightFrameCount();
  frames_in_ = frames_sent_ = frames_dropped_ = key_requests_ = 0;
  bytes_sent_ = 0;
  stats_alarm_.ScheduleFromNow([this] { LogStats(); }, kStatsInterval);
}

}  // namespace openscreen::cast
