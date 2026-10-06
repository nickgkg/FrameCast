// Copyright 2026 Nicholas Guetter. MIT License, see LICENSE.

// FrameCast: Ogg Opus pipe -> Cast Streaming audio. See opus_pipe_sender.h.

#include "cast/framecast_sender/opus_pipe_sender.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

#include "cast/streaming/public/encoded_frame.h"
#include "platform/base/span.h"
#include "util/chrono_helpers.h"
#include "util/osp_logging.h"

namespace openscreen::cast {

namespace {

constexpr auto kStatsInterval = std::chrono::seconds(5);
constexpr int kSampleRate = 48000;
// If packets arrive this much later than their place on the timeline (input
// stalled), move the timeline forward instead of sending audio late.
constexpr auto kMaxLag = std::chrono::milliseconds(150);
// If packets arrive faster than real time (audio that collected in the pipe
// while the session started), drop them so the audio does not play late.
constexpr auto kMaxLead = std::chrono::milliseconds(60);

// Samples (at 48 kHz) in an Opus packet, from its TOC byte (RFC 6716 3.1).
int OpusPacketSamples(const std::vector<uint8_t>& p) {
  if (p.empty()) {
    return 0;
  }
  const int config = p[0] >> 3;
  int frame;  // In units of 2.5 ms (120 samples).
  if (config < 12) {
    static const int kSilk[] = {4, 8, 16, 24};
    frame = kSilk[config & 3];
  } else if (config < 16) {
    frame = (config & 1) ? 8 : 4;
  } else {
    static const int kCelt[] = {1, 2, 4, 8};
    frame = kCelt[config & 3];
  }
  int frames;
  switch (p[0] & 3) {
    case 0:
      frames = 1;
      break;
    case 3:
      frames = p.size() > 1 ? (p[1] & 0x3f) : 0;
      break;
    default:
      frames = 2;
  }
  return frames * frame * 120;
}

}  // namespace

OpusPipeSender::OpusPipeSender(Environment& environment,
                               int input_fd,
                               std::unique_ptr<Sender> audio_sender)
    : env_(environment),
      input_fd_(input_fd),
      audio_sender_(std::move(audio_sender)),
      stats_alarm_(env_.now_function(), env_.task_runner()) {
  OSP_CHECK(audio_sender_);
  OSP_LOG_INFO << "OpusPipeSender: audio ssrc="
               << audio_sender_->config().sender_ssrc
               << " timebase=" << audio_sender_->config().rtp_timebase;
  stats_alarm_.ScheduleFromNow([this] { LogStats(); }, kStatsInterval);
  reader_ = std::thread([this] { ReadLoop(); });
}

OpusPipeSender::~OpusPipeSender() {
  stop_ = true;
  if (reader_.joinable()) {
    reader_.join();
  }
}

void OpusPipeSender::ReadLoop() {
  const WeakPtr<OpusPipeSender> weak = weak_factory_.GetWeakPtr();
  TaskRunner& task_runner = env_.task_runner();

  std::vector<uint8_t> buf;
  std::vector<uint8_t> chunk(64 << 10);
  std::vector<uint8_t> packet;  // Packet that may continue on the next page.
  int header_packets = 0;       // OpusHead and OpusTags are skipped.

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
      OSP_LOG_INFO << "OpusPipeSender: end of input";
      break;
    }
    buf.insert(buf.end(), chunk.begin(), chunk.begin() + got);

    // Ogg pages: "OggS", version, flags, granule(8), serial(4), seq(4),
    // crc(4), segment count, segment table, data.
    size_t pos = 0;
    for (;;) {
      if (buf.size() < pos + 27) {
        break;
      }
      if (std::memcmp(&buf[pos], "OggS", 4) != 0) {
        // Lost sync: find the next page.
        size_t next = pos + 1;
        while (next + 4 <= buf.size() &&
               std::memcmp(&buf[next], "OggS", 4) != 0) {
          ++next;
        }
        pos = next;
        packet.clear();
        continue;
      }
      const size_t nseg = buf[pos + 26];
      if (buf.size() < pos + 27 + nseg) {
        break;
      }
      size_t data_len = 0;
      for (size_t s = 0; s < nseg; ++s) {
        data_len += buf[pos + 27 + s];
      }
      if (buf.size() < pos + 27 + nseg + data_len) {
        break;
      }
      const uint8_t* seg_table = &buf[pos + 27];
      const uint8_t* data = &buf[pos + 27 + nseg];
      for (size_t s = 0; s < nseg; ++s) {
        packet.insert(packet.end(), data, data + seg_table[s]);
        data += seg_table[s];
        if (seg_table[s] < 255) {
          if (header_packets < 2) {
            ++header_packets;
          } else if (!packet.empty()) {
            const Clock::time_point now = Clock::now();
            task_runner.PostTask([weak, p = std::move(packet), now]() mutable {
              if (OpusPipeSender* self = weak.get()) {
                self->SendPacket(std::move(p), now);
              }
            });
          }
          packet.clear();
        }
      }
      pos += 27 + nseg + data_len;
    }
    buf.erase(buf.begin(), buf.begin() + std::min(pos, buf.size()));
  }
}

void OpusPipeSender::SendPacket(std::vector<uint8_t> packet,
                                Clock::time_point arrived) {
  ++packets_in_;
  const int samples = OpusPacketSamples(packet);
  if (samples <= 0) {
    ++packets_dropped_;
    return;
  }
  const auto duration = std::chrono::duration_cast<Clock::duration>(
      std::chrono::microseconds(int64_t{samples} * 1000000 / kSampleRate));

  if (start_ == Clock::time_point::min()) {
    start_ = arrived - duration;
  }
  Clock::time_point reference = start_ + std::chrono::duration_cast<Clock::duration>(
      std::chrono::microseconds(samples_ * 1000000 / kSampleRate));
  if (arrived - duration - reference > kMaxLag) {
    // The input stalled. Skip the gap so the audio stays in sync.
    samples_ = std::chrono::duration_cast<std::chrono::microseconds>(
                   arrived - duration - start_)
                   .count() *
               kSampleRate / 1000000;
    reference = start_ + std::chrono::duration_cast<Clock::duration>(
        std::chrono::microseconds(samples_ * 1000000 / kSampleRate));
    ++resyncs_;
  } else if (reference - (arrived - duration) > kMaxLead) {
    ++packets_dropped_;
    return;
  }
  const RtpTimeTicks rtp = RtpTimeTicks() + RtpTimeDelta::FromTicks(samples_);
  samples_ += samples;

  if (audio_sender_->GetInFlightMediaDuration(rtp) >
      audio_sender_->GetMaxInFlightMediaDuration()) {
    ++packets_dropped_;
    return;
  }

  EncodedFrame frame;
  frame.frame_id = audio_sender_->GetNextFrameId();
  // Each Opus packet decodes on its own.
  frame.dependency = EncodedFrame::Dependency::kKeyFrame;
  frame.referenced_frame_id = frame.frame_id;
  frame.rtp_timestamp = rtp;
  frame.reference_time = reference;
  frame.capture_begin_time = reference;
  frame.capture_end_time = reference + duration;
  frame.data = ByteView(packet.data(), packet.size());

  if (audio_sender_->EnqueueFrame(frame) != Sender::OK) {
    ++packets_dropped_;
    return;
  }
  ++packets_sent_;
}

void OpusPipeSender::LogStats() {
  OSP_LOG_INFO << "OpusPipeSender: in=" << packets_in_
               << " sent=" << packets_sent_ << " dropped=" << packets_dropped_
               << " resyncs=" << resyncs_ << " rtt="
               << to_milliseconds(audio_sender_->GetCurrentRoundTripTime())
                      .count()
               << "ms inflight=" << audio_sender_->GetInFlightFrameCount();
  packets_in_ = packets_sent_ = packets_dropped_ = resyncs_ = 0;
  stats_alarm_.ScheduleFromNow([this] { LogStats(); }, kStatsInterval);
}

}  // namespace openscreen::cast
