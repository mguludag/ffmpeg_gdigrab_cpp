#ifndef SCREENRECORDIMPL_H
#define SCREENRECORDIMPL_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#ifdef __cplusplus
extern "C" {
#endif
struct AVFormatContext;
struct AVCodecContext;
struct AVCodec;
struct AVFifoBuffer;
struct AVAudioFifo;
struct AVFrame;
struct SwsContext;
struct SwrContext;
struct AVDictionary;
#ifdef __cplusplus
}
#endif

class ScreenRecordImpl
{
public:
    enum RecordState {
        NotStarted,
        Started,
        Paused,
        Stopped,
        Unknown,
    };

    ScreenRecordImpl(const std::string &path = "test",  int width = 0, int height = 0, int fps = 10, int quality = 5, int offsetx = 0, int offsety = 0);
    ~ScreenRecordImpl();

    // Initialize the video parameters
    void init(const std::string& path = "test",  int width = 0, int height = 0, int fps = 10, int quality = 5, int offsetx = 0, int offsety = 0);

    void start();
    void pause();
    void stop();

private:
    // Read video frames from fifobuf, encoding write output stream, generate files
    void screenRecordThreadProc();
    // Read the frame from the video input stream, write fifobuf
    void screenAcquireThreadProc();
    int openVideo();
    int openOutput();
    void setEncoderParams();
    void flushDecoder();
    void flushEncoder();
    void initBuffer();
    void release();

private:
    std::string m_filePath;
    int m_width;
    int m_height;
    int m_fps;
    int m_quality;
    int m_offsetx;
    int m_offsety;

    std::unique_ptr<std::thread> m_recordThread = nullptr;
    std::unique_ptr<std::thread> m_captureThread = nullptr;

    int m_vIndex;    //Input video stream index
    int m_vOutIndex; // output video stream index
    AVFormatContext* m_vFmtCtx = nullptr;
    AVFormatContext* m_oFmtCtx = nullptr;
    AVCodecContext* m_vDecodeCtx = nullptr;
    AVCodecContext* m_vEncodeCtx = nullptr;
    AVDictionary* m_dict = nullptr;
    SwsContext* m_swsCtx = nullptr;
    AVFifoBuffer* m_vFifoBuf = nullptr;
    AVFrame* m_vOutFrame = nullptr;
    uint8_t* m_vOutFrameBuf = nullptr;
    int m_vOutFrameSize{}; // byte of an output frame
    RecordState m_state;

    // The encoding speed is generally slower than the acquisition speed, so you can remove m_cvNotEmpty
    std::condition_variable m_cvNotFull;
    std::condition_variable m_cvNotEmpty;
    std::mutex m_mtx;
    std::condition_variable m_cvNotPause;
    std::mutex m_mtxPause;
};

#endif // SCREENRECORDIMPL_H
