#include "screenrecordimpl.h"

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec\avcodec.h>
#include <libavdevice\avdevice.h>
#include <libavformat\avformat.h>
#include <libavutil\fifo.h>
#include <libavutil\imgutils.h>
#include <libswresample\swresample.h>
#include <libswscale\swscale.h>
//#include
#ifdef __cplusplus
}
#endif

#include "screenrecordimpl.h"
#include <fmt/core.h>
#include <thread>

// g_collectFrameCnt is equal to g_encodeFrameCnt to prove that the number of
// codec frames is the same.
int g_collectFrameCnt = 0; // Number of frames collected
int g_encodeFrameCnt = 0; // coded frame number

inline constexpr auto str2fourcc(const char str[4])
{
    return ((str[3] << 24) + (str[2] << 16) + (str[1] << 8) + str[0]);
}

ScreenRecordImpl::ScreenRecordImpl(
    const std::string &path, int width, int height, int fps, int quality, int offsetx, int offsety)
    : m_filePath(path), m_width(width), m_height(height), m_fps(fps), m_quality(quality),
      m_offsetx(offsetx), m_offsety(offsety), m_vIndex(-1), m_vOutIndex(-1),
      m_state(RecordState::NotStarted)
{}

ScreenRecordImpl::~ScreenRecordImpl()
{
    stop();
}

void ScreenRecordImpl::init(
    const std::string &path, int width, int height, int fps, int quality, int offsetx, int offsety)
{
    m_filePath = path;
    m_width = width;
    m_height = height;
    m_fps = fps;
    m_quality = quality;
    m_offsetx = offsetx;
    m_offsety = offsety;
}

void ScreenRecordImpl::start()
{
    if (m_state == RecordState::NotStarted) {
        m_state = RecordState::Started;
        g_collectFrameCnt = 0;
        g_encodeFrameCnt = 0;
        m_recordThread = std::make_unique<std::thread>(&ScreenRecordImpl::screenRecordThreadProc,
                                                       this);
    } else if (m_state == RecordState::Paused) {
        m_state = RecordState::Started;
        m_cvNotPause.notify_one();
    }
}

void ScreenRecordImpl::pause()
{
    m_state = RecordState::Paused;
}

void ScreenRecordImpl::stop()
{
    if (m_state == RecordState::Paused) {
        m_cvNotPause.notify_one();
    }
    m_state = RecordState::Stopped;

    if (m_captureThread->joinable()) {
        m_captureThread->join();
    }
    if (m_recordThread->joinable()) {
        m_recordThread->join();
    }
}

int ScreenRecordImpl::openVideo()
{
    int ret = -1;
    AVInputFormat *ifmt = av_find_input_format("gdigrab");
    AVDictionary *options = nullptr;
    AVCodec *decoder = nullptr;
    // Set the acquisition frame rate
    av_dict_set(&options, "framerate", std::to_string(m_fps).c_str(), 0);
    av_dict_set(&options, "probesize", (std::to_string(m_fps * 2) + "M").c_str(), 0);
    av_dict_set(&options, "offset_x", std::to_string(m_offsetx).c_str(), 0);
    av_dict_set(&options, "offset_y", std::to_string(m_offsety).c_str(), 0);
    av_dict_set(&options,
                "video_size",
                (std::to_string(m_width) + "x" + std::to_string(m_height)).c_str(),
                0);

    if (avformat_open_input(&m_vFmtCtx, "desktop", ifmt, &options) != 0) {
        fmt::print(stderr, "Cant not open video input stream\n");
        return -1;
    }

    if (avformat_find_stream_info(m_vFmtCtx, nullptr) < 0) {
        fmt::print(stderr, "Couldn't find stream information\n");
        return -1;
    }

    for (uint32_t i = 0; i < m_vFmtCtx->nb_streams; ++i) {
        AVStream *stream = m_vFmtCtx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            decoder = avcodec_find_decoder(stream->codecpar->codec_id);
            if (decoder == nullptr) {
                fmt::print(stderr, "avcodec_find_decoder failed\n");
                return -1;
            }
            // Copy parameters from the video stream to codecCtx
            m_vDecodeCtx = avcodec_alloc_context3(decoder);
            if ((ret = avcodec_parameters_to_context(m_vDecodeCtx, stream->codecpar)) < 0) {
                fmt::print(stderr,
                           "Video avcodec_parameters_to_context failed,error code: {}\n",
                           ret);
                return -1;
            }
            m_vIndex = i;
            break;
        }
    }

    m_vDecodeCtx->thread_count = std::thread::hardware_concurrency();
    m_vDecodeCtx->thread_type = FF_THREAD_SLICE;

  if (avcodec_open2(m_vDecodeCtx, decoder, &m_dict) < 0) {
    fmt::print(stderr, "avcodec_open2 failed\n");
    return -1;
  }

  m_swsCtx = sws_getContext(m_vDecodeCtx->width, m_vDecodeCtx->height,
                            m_vDecodeCtx->pix_fmt, m_width, m_height,
                            AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr,
                            nullptr, nullptr);

  return m_swsCtx != nullptr ? 0 : -1;
}

int ScreenRecordImpl::openOutput() {
  int ret = -1;
  AVStream *vStream = nullptr;
  std::string outFilePath = m_filePath;

  ret = avformat_alloc_output_context2(&m_oFmtCtx, nullptr, "mp4",
                                       outFilePath.c_str());
  if (ret < 0) {
    fmt::print(stderr, "avformat_alloc_output_context2 failed\n");
    return -1;
  }

  if (m_vFmtCtx->streams[m_vIndex]->codecpar->codec_type ==
      AVMEDIA_TYPE_VIDEO) {
    vStream = avformat_new_stream(m_oFmtCtx, nullptr);
    if (vStream == nullptr) {
      fmt::print(stderr, "can not new stream for output\n");
      return -1;
    }
    // AVFormatContext first created stream index is 0, the second created
    // stream index is 1
    m_vOutIndex = vStream->index;
    vStream->time_base = AVRational{1, m_fps};

    m_vEncodeCtx = avcodec_alloc_context3(nullptr);
    if (nullptr == m_vEncodeCtx) {
      fmt::print(stderr, "avcodec_alloc_context3 failed\n");
      return -1;
    }

    // Set the encoding parameters
    setEncoderParams();

    // Find the video encoder
    AVCodec *encoder = avcodec_find_encoder(m_vEncodeCtx->codec_id);
    if (encoder == nullptr) {
      fmt::print(stderr, "Can not find the encoder, id: {}\n",
                 m_vEncodeCtx->codec_id);
      return -1;
    }

    // Open the video encoder
    ret = avcodec_open2(m_vEncodeCtx, encoder, nullptr);
    if (ret < 0) {
      fmt::print(stderr, "Can not open encoder id: {} error code: {}\n",
                 encoder->id, ret);
      return -1;
    }
    // Pass the parameters in codecCtx to the output stream
    ret = avcodec_parameters_from_context(vStream->codecpar, m_vEncodeCtx);
    if (ret < 0) {
      fmt::print(stderr,
                 "Output avcodec_parameters_from_context,error code:{}\n", ret);
      return -1;
    }
  }
  // Open the output file
  if ((m_oFmtCtx->oformat->flags & AVFMT_NOFILE) == 0) {
    if (avio_open(&m_oFmtCtx->pb, outFilePath.c_str(), AVIO_FLAG_WRITE) < 0) {
      fmt::print(stderr, "avio_open failed\n");
      return -1;
    }
  }

  // Write the file header
  if (avformat_write_header(m_oFmtCtx, &m_dict) < 0) {
    fmt::print(stderr, "avformat_write_header failed\n");
    return -1;
  }
  return 0;
}

void ScreenRecordImpl::screenRecordThreadProc() {
  int ret = -1;
  // Reduce the atomic variable granularity
  bool done = false;
  int vFrameIndex = 0;

  //  av_register_all();
  avdevice_register_all();
  //  avcodec_register_all();

  if (openVideo() < 0) {
    return;
  }
  if (openOutput() < 0) {
    return;
  }

  initBuffer();

  // Start the video data collection thread
  m_captureThread = std::make_unique<std::thread>(
      &ScreenRecordImpl::screenAcquireThreadProc, this);

  while (true) {
    if (m_state == RecordState::Stopped && !done) {
      done = true;
    }
    if (done) {
      std::lock_guard<std::mutex> lk(m_mtx);
      if (av_fifo_size(m_vFifoBuf) < m_vOutFrameSize) {
        break;
      }
    }
    {
      std::unique_lock<std::mutex> lk(m_mtx);
      m_cvNotEmpty.wait(
          lk, [this] { return av_fifo_size(m_vFifoBuf) >= m_vOutFrameSize; });
    }
    av_fifo_generic_read(m_vFifoBuf, m_vOutFrameBuf, m_vOutFrameSize, nullptr);
    m_cvNotFull.notify_one();

    // Set the video frame parameters
    m_vOutFrame->pts = vFrameIndex;
    ++vFrameIndex;
    m_vOutFrame->format = m_vEncodeCtx->pix_fmt;
    m_vOutFrame->width = m_width;
    m_vOutFrame->height = m_height;
    AVPacket pkt = {nullptr};
    av_init_packet(&pkt);

    ret = avcodec_send_frame(m_vEncodeCtx, m_vOutFrame);
    if (ret != 0) {
      fmt::print(stderr, "video avcodec_send_frame failed, ret: {}\n", ret);
      av_packet_unref(&pkt);
      continue;
    }
    ret = avcodec_receive_packet(m_vEncodeCtx, &pkt);
    if (ret != 0) {
      av_packet_unref(&pkt);
      if (ret == AVERROR(EAGAIN)) {
        fmt::print(stderr, "EAGAIN avcodec_receive_packet\n");
        continue;
      }
      fmt::print(stderr, "video avcodec_receive_packet failed, ret: {}\n", ret);
      return;
    }
    pkt.stream_index = m_vOutIndex;
    av_packet_rescale_ts(&pkt, m_vEncodeCtx->time_base,
                         m_oFmtCtx->streams[m_vOutIndex]->time_base);
    pkt.duration = g_encodeFrameCnt * m_fps;

    ret = av_interleaved_write_frame(m_oFmtCtx, &pkt);
    if (ret == 0) {
      fmt::print(stdout, "\rWrite video packet id: {}", ++g_encodeFrameCnt);
    } else {
      fmt::print(stderr, "video av_interleaved_write_frame failed, ret:{}\n",
                 ret);
    }
    av_packet_unref(&pkt);
  }
  flushEncoder();
  av_write_trailer(m_oFmtCtx);
  release();
  fmt::print(stderr, "parent thread exit\n");
}

void ScreenRecordImpl::screenAcquireThreadProc() {
  int ret = -1;
  AVPacket pkt = {nullptr};
  av_init_packet(&pkt);
  int y_size = m_width * m_height;
  AVFrame *oldFrame = av_frame_alloc();
  AVFrame *newFrame = av_frame_alloc();

  int newFrameBufSize =
      av_image_get_buffer_size(m_vEncodeCtx->pix_fmt, m_width, m_height, 1);
  auto *newFrameBuf = static_cast<uint8_t *>(av_malloc(newFrameBufSize));
  av_image_fill_arrays(newFrame->data, newFrame->linesize, newFrameBuf,
                       m_vEncodeCtx->pix_fmt, m_width, m_height, 1);

  while (m_state != RecordState::Stopped) {
    if (m_state == RecordState::Paused) {
      std::unique_lock<std::mutex> lk(m_mtxPause);
      m_cvNotPause.wait(lk, [this] { return m_state != RecordState::Paused; });
    }
    if (av_read_frame(m_vFmtCtx, &pkt) < 0) {
      fmt::print(stderr, "video av_read_frame < 0\n");
      continue;
    }
    if (pkt.stream_index != m_vIndex) {
      fmt::print(stderr, "not a video packet from video input\n");
      av_packet_unref(&pkt);
    }
    ret = avcodec_send_packet(m_vDecodeCtx, &pkt);
    if (ret != 0) {
      fmt::print(stderr, "avcodec_send_packet failed, ret:{}\n", ret);
      av_packet_unref(&pkt);
      continue;
    }
    ret = avcodec_receive_frame(m_vDecodeCtx, oldFrame);
    if (ret != 0) {
      fmt::print(stderr, "avcodec_receive_frame failed, ret:{}\n", ret);
      av_packet_unref(&pkt);
      continue;
    }
    ++g_collectFrameCnt;

    // srcSliceH is the input height or output height
    sws_scale(m_swsCtx, oldFrame->data, oldFrame->linesize, 0, oldFrame->height,
              newFrame->data, newFrame->linesize);

    {
      std::unique_lock<std::mutex> lk(m_mtx);
      m_cvNotFull.wait(
          lk, [this] { return av_fifo_space(m_vFifoBuf) >= m_vOutFrameSize; });
    }

    av_fifo_generic_write(m_vFifoBuf, newFrame->data[0], y_size, nullptr);
    av_fifo_generic_write(m_vFifoBuf, newFrame->data[1], y_size / 4, nullptr);
    av_fifo_generic_write(m_vFifoBuf, newFrame->data[2], y_size / 4, nullptr);

    m_cvNotEmpty.notify_one();

    av_packet_unref(&pkt);
  }
  flushDecoder();

  av_free(newFrameBuf);
  av_frame_free(&oldFrame);
  av_frame_free(&newFrame);
}

void ScreenRecordImpl::setEncoderParams() {

  if ((m_quality < 1) || (m_quality > 30)) {
    m_quality = 5;
  }

  m_vEncodeCtx->width = m_width;
  m_vEncodeCtx->height = m_height;
  m_vEncodeCtx->codec_type = AVMEDIA_TYPE_VIDEO;
  m_vEncodeCtx->time_base.num = 1;
  m_vEncodeCtx->time_base.den = m_fps;
  m_vEncodeCtx->gop_size = m_fps * 2;
  m_vEncodeCtx->profile = FF_PROFILE_MPEG4_ADVANCED_SIMPLE;
  m_vEncodeCtx->qmin = m_quality - (m_quality > 1 ? 1 : 0);
  m_vEncodeCtx->qmax = m_quality + 1;
  m_vEncodeCtx->pix_fmt = AV_PIX_FMT_YUV420P;
  m_vEncodeCtx->codec_id = AV_CODEC_ID_MPEG4;
  m_vEncodeCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  m_vEncodeCtx->codec_tag = str2fourcc("mp4v");

  av_dict_set(&m_dict, "movflags", "isml+frag_keyframe+separate_moof", 0);
  av_dict_set(&m_dict, "preset", "superfast", 0);
}

void ScreenRecordImpl::flushDecoder() {
  int ret = -1;
  AVPacket pkt = {nullptr};
  av_init_packet(&pkt);
  int y_size = m_width * m_height;
  AVFrame *oldFrame = av_frame_alloc();
  AVFrame *newFrame = av_frame_alloc();

  ret = avcodec_send_packet(m_vDecodeCtx, nullptr);
  fmt::print(stderr, "flush avcodec_send_packet, ret: {}\n", ret);
  while (ret >= 0) {
    ret = avcodec_receive_frame(m_vDecodeCtx, oldFrame);
    if (ret < 0) {
      av_packet_unref(&pkt);
      if (ret == AVERROR(EAGAIN)) {
        fmt::print(stderr, "flush EAGAIN avcodec_receive_frame\n");
        continue;
      } else if (ret == AVERROR_EOF) {
        fmt::print(stderr, "flush video decoder finished\n");
        break;
      }
      fmt::print(stderr, "flush video avcodec_receive_frame error, ret: {}\n",
                 ret);
      return;
    }
    ++g_collectFrameCnt;
    sws_scale(m_swsCtx, (const uint8_t *const *)oldFrame->data,
              oldFrame->linesize, 0, m_vEncodeCtx->height, newFrame->data,
              newFrame->linesize);

    {
      std::unique_lock<std::mutex> lk(m_mtx);
      m_cvNotFull.wait(
          lk, [this] { return av_fifo_space(m_vFifoBuf) >= m_vOutFrameSize; });
    }
    av_fifo_generic_write(m_vFifoBuf, newFrame->data[0], y_size, nullptr);
    av_fifo_generic_write(m_vFifoBuf, newFrame->data[1], y_size / 4, nullptr);
    av_fifo_generic_write(m_vFifoBuf, newFrame->data[2], y_size / 4, nullptr);
    m_cvNotEmpty.notify_one();
  }
  fmt::print(stderr, "collect frame count: {}\n", g_collectFrameCnt);
}

void ScreenRecordImpl::flushEncoder() {
  int ret = -11;
  AVPacket pkt = {nullptr};
  av_init_packet(&pkt);
  ret = avcodec_send_frame(m_vEncodeCtx, nullptr);
  fmt::print(stderr, "\navcodec_send_frame ret:{}\n", ret);
  while (ret >= 0) {
    ret = avcodec_receive_packet(m_vEncodeCtx, &pkt);
    if (ret < 0) {
      av_packet_unref(&pkt);
      if (ret == AVERROR(EAGAIN)) {
        fmt::print(stderr, "flush EAGAIN avcodec_receive_packet\n");
        continue;
      } else if (ret == AVERROR_EOF) {
        fmt::print(stderr, "flush video encoder finished\n");
        break;
      }
      fmt::print(stderr, "video avcodec_receive_packet failed, ret: {}\n", ret);
      return;
    }
    pkt.stream_index = m_vOutIndex;
    av_packet_rescale_ts(&pkt, m_vEncodeCtx->time_base,
                         m_oFmtCtx->streams[m_vOutIndex]->time_base);

    ret = av_interleaved_write_frame(m_oFmtCtx, &pkt);
    if (ret == 0) {
      fmt::print(stderr, "flush Write video packet id: {}\n",
                 ++g_encodeFrameCnt);
    } else {
      fmt::print(stderr, "video av_interleaved_write_frame failed, ret: {}\n",
                 ret);
    }
    av_packet_unref(&pkt);
  }
}

void ScreenRecordImpl::initBuffer() {
  m_vOutFrameSize =
      av_image_get_buffer_size(m_vEncodeCtx->pix_fmt, m_width, m_height, 1);
  m_vOutFrameBuf = static_cast<uint8_t *>(av_malloc(m_vOutFrameSize));
  m_vOutFrame = av_frame_alloc();
  // Let the AVFrame pointer point to buf, then write data to buf
  av_image_fill_arrays(m_vOutFrame->data, m_vOutFrame->linesize, m_vOutFrameBuf,
                       m_vEncodeCtx->pix_fmt, m_width, m_height, 1);
  // Apply for fps*4 frame buffer
  if ((m_vFifoBuf = av_fifo_alloc_array(m_fps * 4, m_vOutFrameSize)) ==
      nullptr) {
    fmt::print(stderr, "av_fifo_alloc_array failed\n");
    return;
  }
}

void ScreenRecordImpl::release() {
  av_frame_free(&m_vOutFrame);
  av_free(m_vOutFrameBuf);

  if (m_vDecodeCtx != nullptr) {
    avcodec_free_context(&m_vDecodeCtx);
    m_vDecodeCtx = nullptr;
  }
  if (m_vEncodeCtx != nullptr) {
    avcodec_free_context(&m_vEncodeCtx);
    m_vEncodeCtx = nullptr;
  }
  if (m_vFifoBuf != nullptr) {
    av_fifo_freep(&m_vFifoBuf);
  }
  if (m_vFmtCtx != nullptr) {
    avformat_close_input(&m_vFmtCtx);
    m_vFmtCtx = nullptr;
  }
  avio_close(m_oFmtCtx->pb);
  avformat_free_context(m_oFmtCtx);
}
