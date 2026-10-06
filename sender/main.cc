// Copyright 2026 Nicholas Guetter. MIT License, see LICENSE.
// Based on Open Screen's cast/standalone_sender, Copyright The Chromium
// Authors, BSD-style license; see THIRD_PARTY_NOTICES.md.

// FrameCast sender: low-latency Cast Streaming (mirroring) of an H.264 stream
// read from stdin to a Chromecast.
//
//   framecap ... | hwenc -k keyfifo |
//     framecast_sender -K keyfifo -A audiofifo 192.168.1.20[:8009]
//
// See framecast.sh for the full pipeline.

#include <fcntl.h>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "cast/common/public/trust_store.h"
#include "cast/framecast_sender/framecast_agent.h"
#include "platform/api/time.h"
#include "platform/base/ip_address.h"
#include "platform/impl/logging.h"
#include "platform/impl/platform_client_posix.h"
#include "platform/impl/task_runner.h"
#include "third_party/getopt/getopt.h"
#include "util/chrono_helpers.h"
#include "util/string_parse.h"

namespace openscreen::cast {
namespace {

constexpr int kDefaultCastPort = 8009;

void LogUsage(const char* argv0) {
  std::cerr << "usage: " << argv0 << R"( [options] addr[:port]

Reads H.264 Annex-B (with AUDs, SPS/PPS before each IDR, no B-frames) from
stdin and streams it to the Cast device with the mirroring receiver app.

options:
  -m, --max-bitrate=BPS   Video bit rate announced to the receiver (4000000).
  -W, --width=N           Video width announced in the offer (1280).
  -H, --height=N          Video height announced in the offer (720).
  -f, --fps=N             Frame rate announced in the offer (30).
  -D, --delay=MS          Target playout delay in milliseconds (150).
  -A, --audio=FILE        Also send audio: Ogg Opus, 48 kHz stereo, from FILE
                          (a pipe, e.g. ffmpeg -c:a libopus -f ogg).
  -B, --audio-bitrate=BPS Audio bit rate announced to the receiver (128000).
  -K, --key-requests=FILE Write one byte to FILE (a FIFO) when the video needs
                          a key frame (hwenc -k reads it).
  -s, --status=FILE       Write one-line status updates to FILE.
  -a, --android-hack      Use the legacy RTP payload types.
  -8, --vp8               Offer VP8 and read IVF (ffmpeg -f ivf) from stdin.
  -v, --verbose           Verbose logging.
)";
}

IPEndpoint ParseAsEndpoint(const char* string_form) {
  const ErrorOr<IPEndpoint> parsed_endpoint = IPEndpoint::Parse(string_form);
  if (parsed_endpoint.is_value()) {
    return parsed_endpoint.value();
  }
  const ErrorOr<IPAddress> parsed_address = IPAddress::Parse(string_form);
  if (parsed_address.is_value()) {
    return {parsed_address.value(), kDefaultCastPort};
  }
  return {};
}

bool ParseInt(const char* s, int& out) {
  return string_parse::ParseAsciiNumber(std::string_view(s), out);
}

int FramecastMain(int argc, char* argv[]) {
  const get_opt::option kOptions[] = {
      {"max-bitrate", required_argument, nullptr, 'm'},
      {"width", required_argument, nullptr, 'W'},
      {"height", required_argument, nullptr, 'H'},
      {"fps", required_argument, nullptr, 'f'},
      {"delay", required_argument, nullptr, 'D'},
      {"status", required_argument, nullptr, 's'},
      {"audio", required_argument, nullptr, 'A'},
      {"audio-bitrate", required_argument, nullptr, 'B'},
      {"key-requests", required_argument, nullptr, 'K'},
      {"android-hack", no_argument, nullptr, 'a'},
      {"vp8", no_argument, nullptr, '8'},
      {"verbose", no_argument, nullptr, 'v'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0}};

  FramecastSettings settings;
  bool verbose = false;
  int delay_ms = 150;
  int ch = -1;
  while ((ch = getopt_long(argc, argv, "m:W:H:f:D:s:A:B:K:av8h", kOptions,
                           nullptr)) != -1) {
    bool ok = true;
    switch (ch) {
      case 'm':
        ok = ParseInt(get_opt::optarg, settings.max_bitrate);
        break;
      case 'W':
        ok = ParseInt(get_opt::optarg, settings.width);
        break;
      case 'H':
        ok = ParseInt(get_opt::optarg, settings.height);
        break;
      case 'f':
        ok = ParseInt(get_opt::optarg, settings.fps);
        break;
      case 'D':
        ok = ParseInt(get_opt::optarg, delay_ms);
        break;
      case 's':
        settings.status_path = get_opt::optarg;
        break;
      case 'A':
        settings.audio_fd = open(get_opt::optarg, O_RDONLY | O_CLOEXEC);
        if (settings.audio_fd < 0) {
          std::cerr << "Cannot open " << get_opt::optarg << "\n";
          return 1;
        }
        break;
      case 'B':
        ok = ParseInt(get_opt::optarg, settings.audio_bitrate);
        break;
      case 'K':
        // O_RDWR: a FIFO opens without waiting for the reader.
        settings.key_request_fd =
            open(get_opt::optarg, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (settings.key_request_fd < 0) {
          std::cerr << "Cannot open " << get_opt::optarg << "\n";
          return 1;
        }
        break;
      case 'a':
        settings.use_android_rtp_hack = true;
        break;
      case '8':
        settings.vp8 = true;
        break;
      case 'v':
        verbose = true;
        break;
      default:
        ok = false;
    }
    if (!ok) {
      LogUsage(argv[0]);
      return 1;
    }
  }
  if (get_opt::optind != argc - 1) {
    LogUsage(argv[0]);
    return 1;
  }
  settings.receiver_endpoint = ParseAsEndpoint(argv[get_opt::optind]);
  if (!settings.receiver_endpoint.port) {
    std::cerr << "Bad address: " << argv[get_opt::optind] << "\n";
    return 1;
  }
  settings.playout_delay = std::chrono::milliseconds(delay_ms);
  settings.input_fd = 0;

  SetLogLevel(verbose ? LogLevel::kVerbose : LogLevel::kInfo);

  auto* const task_runner = new TaskRunnerImpl(&Clock::now);
  PlatformClientPosix::Create(milliseconds(50),
                              std::unique_ptr<TaskRunnerImpl>(task_runner));

  int exit_code = 0;
  std::unique_ptr<FramecastAgent> agent;
  // Set when the stop sequence below runs. The agent can still shut down on
  // its own then (e.g. the encoder exits at the same time), and that must not
  // end the stop sequence before the STOP message goes out.
  bool stopping = false;
  task_runner->PostTask([&] {
    agent = std::make_unique<FramecastAgent>(
        *task_runner, CastTrustStore::Create(), [&] {
          if (!stopping) {
            task_runner->RequestStopSoon();
          }
        });
    agent->Connect(settings);
  });

  // Runs until SIGINT/SIGTERM or until the agent shuts down.
  task_runner->RunUntilSignaled();

  // systemd and the control script can both send SIGTERM; a second one must
  // not kill the process before the STOP message goes out.
  std::signal(SIGTERM, SIG_IGN);
  std::signal(SIGINT, SIG_IGN);
  stopping = true;

  // Send STOP to the receiver first, and give it half a second to go out
  // before the connection closes.
  task_runner->PostTask([&] {
    if (agent) {
      agent->StopReceiverApp();
    }
  });
  task_runner->PostTaskWithDelay(
      [&] {
        agent.reset();
        task_runner->RequestStopSoon();
      },
      milliseconds(500));
  task_runner->RunUntilStopped();
  // The agent must go before the task runner it uses.
  agent.reset();
  PlatformClientPosix::ShutDown();
  return exit_code;
}

}  // namespace
}  // namespace openscreen::cast

int main(int argc, char* argv[]) {
  std::signal(SIGPIPE, SIG_IGN);
  return openscreen::cast::FramecastMain(argc, argv);
}
