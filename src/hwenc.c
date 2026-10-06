// Copyright 2026 Nicholas Guetter. MIT License, see LICENSE.

// hwenc: raw frames on stdin -> H.264 Annex-B on stdout, using the Frame's
// hardware encoder (qcom-iris, /dev/video-enc0) through the V4L2 stateful
// encoder API. Output has an AUD before each access unit and SPS/PPS before
// each IDR, as framecast_sender expects.
//
//   hwenc [-d dev] [-W w] [-H h] [-f fps] [-b bps] [-g gop] [-p rgba|nv12]
//         [-k fifo] [-v]
//
// -k: each byte written to the FIFO (by framecast_sender -K) makes the next
// frame an IDR.
//
// Build: gcc -O2 -o hwenc hwenc.c

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define NUM_OUT 4
#define NUM_CAP 4

static int verbose;
static int fd;

#define LOG(...) fprintf(stderr, "hwenc: " __VA_ARGS__)
#define DIE(...) do { LOG(__VA_ARGS__); exit(1); } while (0)

static int xioctl(unsigned long req, void* arg) {
  int r;
  do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
  return r;
}

static void set_ctrl(uint32_t id, int32_t value, const char* name) {
  struct v4l2_control c = {.id = id, .value = value};
  if (xioctl(VIDIOC_S_CTRL, &c) < 0)
    LOG("warning: %s=%d: %s\n", name, value, strerror(errno));
  else if (verbose)
    LOG("%s=%d\n", name, value);
}

struct buf {
  void* p;
  size_t len;
  int queued;
};

static int read_full(int in, uint8_t* p, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(in, p + got, n - got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return 0;
    got += (size_t)r;
  }
  return 1;
}

static int write_full(int out, const uint8_t* p, size_t n) {
  while (n) {
    ssize_t r = write(out, p, n);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return 0;
    p += r;
    n -= (size_t)r;
  }
  return 1;
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char** argv) {
  const char* dev = "/dev/video-enc0";
  int w = 1280, h = 720, fps = 30, bps = 4000000, gop = -1, rgba = 1;
  const char* key_path = NULL;
  int opt;
  while ((opt = getopt(argc, argv, "d:W:H:f:b:g:p:k:v")) != -1) {
    switch (opt) {
      case 'd': dev = optarg; break;
      case 'W': w = atoi(optarg); break;
      case 'H': h = atoi(optarg); break;
      case 'f': fps = atoi(optarg); break;
      case 'b': bps = atoi(optarg); break;
      case 'g': gop = atoi(optarg); break;
      case 'k': key_path = optarg; break;
      case 'p': rgba = strcmp(optarg, "nv12") != 0; break;
      case 'v': verbose = 1; break;
      default: DIE("bad option\n");
    }
  }
  if (gop < 0) gop = fps;
  signal(SIGPIPE, SIG_IGN);

  int key_fd = -1;
  if (key_path) {
    // O_RDWR: a FIFO opens without waiting for the writer.
    key_fd = open(key_path, O_RDWR | O_NONBLOCK);
    if (key_fd < 0) DIE("open %s: %s\n", key_path, strerror(errno));
  }

  fd = open(dev, O_RDWR | O_NONBLOCK);
  if (fd < 0) DIE("open %s: %s\n", dev, strerror(errno));

  // Raw input (OUTPUT queue) first: the coded size follows from it.
  struct v4l2_format of = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
  of.fmt.pix_mp.width = w;
  of.fmt.pix_mp.height = h;
  of.fmt.pix_mp.pixelformat = rgba ? V4L2_PIX_FMT_RGBA32 : V4L2_PIX_FMT_NV12;
  of.fmt.pix_mp.num_planes = 1;
  if (xioctl(VIDIOC_S_FMT, &of) < 0) DIE("S_FMT output: %s\n", strerror(errno));

  struct v4l2_format cf = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
  cf.fmt.pix_mp.width = w;
  cf.fmt.pix_mp.height = h;
  cf.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
  cf.fmt.pix_mp.num_planes = 1;
  if (xioctl(VIDIOC_S_FMT, &cf) < 0) DIE("S_FMT capture: %s\n", strerror(errno));

  // Visible area (the driver pads the height to 16 lines).
  struct v4l2_selection sel = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
                               .target = V4L2_SEL_TGT_CROP,
                               .r = {0, 0, (uint32_t)w, (uint32_t)h}};
  if (xioctl(VIDIOC_S_SELECTION, &sel) < 0)
    LOG("warning: S_SELECTION crop: %s\n", strerror(errno));

  struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
  parm.parm.output.timeperframe.numerator = 1;
  parm.parm.output.timeperframe.denominator = fps;
  if (xioctl(VIDIOC_S_PARM, &parm) < 0) LOG("warning: S_PARM: %s\n", strerror(errno));

  xioctl(VIDIOC_G_FMT, &of);
  xioctl(VIDIOC_G_FMT, &cf);
  const uint32_t stride = of.fmt.pix_mp.plane_fmt[0].bytesperline;
  const uint32_t padded_h = of.fmt.pix_mp.height;
  LOG("input %ux%u %.4s stride %u size %u; coded %ux%u bufsize %u\n",
      of.fmt.pix_mp.width, padded_h, (char*)&of.fmt.pix_mp.pixelformat, stride,
      of.fmt.pix_mp.plane_fmt[0].sizeimage, cf.fmt.pix_mp.width, cf.fmt.pix_mp.height,
      cf.fmt.pix_mp.plane_fmt[0].sizeimage);

  set_ctrl(V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_MAIN, "profile");
  set_ctrl(V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_4_0, "level");
  set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CBR, "bitrate_mode");
  set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE, bps, "bitrate");
  set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, bps, "peak_bitrate");
  set_ctrl(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop, "gop");
  set_ctrl(V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, "b_frames");
  set_ctrl(V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1, "prepend_sps_pps");
  set_ctrl(V4L2_CID_MPEG_VIDEO_HEADER_MODE, V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME,
           "header_mode");

  struct buf obufs[NUM_OUT], cbufs[NUM_CAP];
  struct v4l2_requestbuffers rb = {.count = NUM_OUT, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                   .memory = V4L2_MEMORY_MMAP};
  if (xioctl(VIDIOC_REQBUFS, &rb) < 0) DIE("REQBUFS output: %s\n", strerror(errno));
  const int nout = rb.count < NUM_OUT ? rb.count : NUM_OUT;
  rb = (struct v4l2_requestbuffers){.count = NUM_CAP, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                    .memory = V4L2_MEMORY_MMAP};
  if (xioctl(VIDIOC_REQBUFS, &rb) < 0) DIE("REQBUFS capture: %s\n", strerror(errno));
  const int ncap = rb.count < NUM_CAP ? rb.count : NUM_CAP;

  for (int t = 0; t < 2; ++t) {
    const int n = t ? ncap : nout;
    struct buf* bufs = t ? cbufs : obufs;
    for (int i = 0; i < n; ++i) {
      struct v4l2_plane pl[1] = {0};
      struct v4l2_buffer b = {.index = i, .memory = V4L2_MEMORY_MMAP, .m.planes = pl, .length = 1,
                              .type = t ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                        : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
      if (xioctl(VIDIOC_QUERYBUF, &b) < 0) DIE("QUERYBUF: %s\n", strerror(errno));
      bufs[i].len = pl[0].length;
      bufs[i].p = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pl[0].m.mem_offset);
      if (bufs[i].p == MAP_FAILED) DIE("mmap: %s\n", strerror(errno));
      bufs[i].queued = 0;
      if (t) {
        if (xioctl(VIDIOC_QBUF, &b) < 0) DIE("QBUF capture: %s\n", strerror(errno));
        bufs[i].queued = 1;
      }
    }
  }
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (xioctl(VIDIOC_STREAMON, &type) < 0) DIE("STREAMON capture: %s\n", strerror(errno));
  type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  if (xioctl(VIDIOC_STREAMON, &type) < 0) DIE("STREAMON output: %s\n", strerror(errno));

  const size_t row = (size_t)w * (rgba ? 4 : 1);
  const size_t frame_bytes = rgba ? row * h : (size_t)w * h * 3 / 2;
  uint8_t* frame = malloc(frame_bytes);
  static const uint8_t aud[] = {0, 0, 0, 1, 9, 0xf0};
  int eof = 0, in_count = 0, out_count = 0, pending = 0, forced = 0;
  double t_stats = now_ms(), lat_sum = 0;
  double sent_at[64] = {0};

  while (!eof || pending > 0) {
    // Feed one input frame when an OUTPUT buffer is free.
    int free_idx = -1;
    for (int i = 0; i < nout; ++i)
      if (!obufs[i].queued) { free_idx = i; break; }
    // While frames are in the encoder, read input only when it is ready, so
    // that finished frames go out at once instead of after the next input.
    struct pollfd ip = {.fd = 0, .events = POLLIN};
    if (!eof && free_idx >= 0 && (pending == 0 || poll(&ip, 1, 0) > 0)) {
      if (!read_full(0, frame, frame_bytes)) {
        eof = 1;
        struct v4l2_encoder_cmd cmd = {.cmd = V4L2_ENC_CMD_STOP};
        xioctl(VIDIOC_ENCODER_CMD, &cmd);
        if (pending == 0) break;
      } else {
        uint8_t* dst = obufs[free_idx].p;
        size_t used;
        if (rgba) {
          for (int y = 0; y < h; ++y) memcpy(dst + (size_t)y * stride, frame + y * row, row);
          used = (size_t)stride * padded_h;
        } else {
          for (int y = 0; y < h; ++y) memcpy(dst + (size_t)y * stride, frame + (size_t)y * w, w);
          uint8_t* uv = dst + (size_t)stride * padded_h;
          for (int y = 0; y < h / 2; ++y)
            memcpy(uv + (size_t)y * stride, frame + (size_t)w * h + (size_t)y * w, w);
          used = (size_t)stride * padded_h * 3 / 2;
        }
        if (used > obufs[free_idx].len) used = obufs[free_idx].len;
        if (key_fd >= 0) {
          char drain[64];
          if (read(key_fd, drain, sizeof drain) > 0) {
            struct v4l2_control c = {.id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, .value = 1};
            if (xioctl(VIDIOC_S_CTRL, &c) < 0) LOG("force key frame: %s\n", strerror(errno));
            ++forced;
          }
        }
        struct v4l2_plane pl[1] = {{.bytesused = (uint32_t)used, .length = (uint32_t)obufs[free_idx].len}};
        struct v4l2_buffer b = {.index = free_idx, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                .memory = V4L2_MEMORY_MMAP, .m.planes = pl, .length = 1};
        // Real frame times: the driver's rate control derives the frame rate
        // from them (1 us steps made it starve every frame of bits).
        const int64_t ts_us = (int64_t)in_count * 1000000 / fps;
        b.timestamp.tv_sec = ts_us / 1000000;
        b.timestamp.tv_usec = ts_us % 1000000;
        if (xioctl(VIDIOC_QBUF, &b) < 0) DIE("QBUF output: %s\n", strerror(errno));
        obufs[free_idx].queued = 1;
        sent_at[in_count % 64] = now_ms();
        ++in_count;
        ++pending;
        continue;  // Fill all free buffers before waiting.
      }
    }

    struct pollfd fds[2] = {{.fd = fd, .events = POLLIN | POLLOUT | POLLPRI},
                            {.fd = eof ? -1 : 0, .events = POLLIN}};
    int pr = poll(fds, 2, 2000);
    const struct pollfd p = fds[0];
    if (pr == 0) {
      LOG("timeout: in=%d out=%d pending=%d\n", in_count, out_count, pending);
      if (eof) break;
      continue;
    }
    if (pr < 0) {
      if (errno == EINTR) continue;
      DIE("poll: %s\n", strerror(errno));
    }
    if (p.revents & POLLPRI) {
      struct v4l2_event ev;
      while (xioctl(VIDIOC_DQEVENT, &ev) == 0)
        if (verbose) LOG("event %u\n", ev.type);
    }
    if (p.revents & POLLERR) DIE("device error\n");

    // Encoded data.
    for (;;) {
      struct v4l2_plane pl[1] = {0};
      struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP,
                              .m.planes = pl, .length = 1};
      if (xioctl(VIDIOC_DQBUF, &b) < 0) {
        if (errno != EAGAIN) LOG("DQBUF capture: %s\n", strerror(errno));
        break;
      }
      const uint8_t* data = (const uint8_t*)cbufs[b.index].p + pl[0].data_offset;
      const size_t n = pl[0].bytesused - pl[0].data_offset;
      if (n > 0) {
        const int64_t ts_us = (int64_t)b.timestamp.tv_sec * 1000000 + b.timestamp.tv_usec;
        const int id = (int)((ts_us * fps + 500000) / 1000000);
        lat_sum += now_ms() - sent_at[id % 64];
        if (verbose && out_count < 3)
          LOG("frame %d: %zu bytes, flags 0x%x, first %02x %02x %02x %02x %02x\n", id, n, b.flags,
              data[0], data[1], data[2], data[3], data[4]);
        if (!write_full(1, aud, sizeof aud) || !write_full(1, data, n)) DIE("write failed\n");
        ++out_count;
        --pending;
      }
      const int last = b.flags & V4L2_BUF_FLAG_LAST;
      pl[0].bytesused = 0;
      if (xioctl(VIDIOC_QBUF, &b) < 0) DIE("QBUF capture: %s\n", strerror(errno));
      if (last) { pending = 0; break; }
    }

    // Free input buffers.
    for (;;) {
      struct v4l2_plane pl[1] = {0};
      struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP,
                              .m.planes = pl, .length = 1};
      if (xioctl(VIDIOC_DQBUF, &b) < 0) break;
      obufs[b.index].queued = 0;
    }

    if (now_ms() - t_stats > 5000) {
      LOG("in=%d out=%d forced IDR=%d encode latency avg %.1f ms\n", in_count, out_count, forced,
          out_count ? lat_sum / out_count : 0);
      t_stats = now_ms();
    }
  }
  LOG("done: in=%d out=%d encode latency avg %.1f ms\n", in_count, out_count,
      out_count ? lat_sum / out_count : 0);
  type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  xioctl(VIDIOC_STREAMOFF, &type);
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  xioctl(VIDIOC_STREAMOFF, &type);
  close(fd);
  return 0;
}
