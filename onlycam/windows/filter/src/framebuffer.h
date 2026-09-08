// Shared memory contract between the OnlyCam application (writer) and the
// OnlyCam DirectShow filter (reader).
#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>

#include <cstdint>

namespace onlycam {

constexpr wchar_t kMappingName[] = L"Local\\OnlyCamFrameBuffer";
constexpr uint32_t kMagic = 0x3143464F;  // "OCF1"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kHeaderSize = 256;
constexpr uint32_t kMaxWidth = 1920;
constexpr uint32_t kMaxHeight = 1080;
constexpr uint32_t kMaxFrameBytes = kMaxWidth * kMaxHeight * 3;
constexpr uint32_t kMappingSize = kHeaderSize + kMaxFrameBytes;

// Pixel format of the frame payload: 24 bit BGR, bottom-up rows, no padding.
constexpr uint32_t kFormatBGR24 = 0;

// A writer is considered gone when its heartbeat is older than this.
constexpr uint64_t kSenderTimeoutMs = 1500;

#pragma pack(push, 4)
struct SharedHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t max_width;
    uint32_t max_height;
    // Seqlock guarding the payload: odd while a frame is being written.
    volatile uint32_t sequence;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t frame_index;
    volatile uint64_t sender_tick_ms;
    volatile uint32_t sender_active;
    // Written by the filter so the application can render at the negotiated size.
    volatile uint32_t consumer_count;
    volatile uint32_t want_width;
    volatile uint32_t want_height;
    volatile uint32_t want_fps;
    volatile uint64_t consumer_tick_ms;
};
#pragma pack(pop)

// Reads frames published by the application and keeps the negotiated format
// visible to it. Safe to use from a single streaming thread.
class FrameReader {
public:
    ~FrameReader();

    bool Open();
    void Close();

    void AddConsumer(uint32_t width, uint32_t height, uint32_t fps);
    void RemoveConsumer();
    void SetWanted(uint32_t width, uint32_t height, uint32_t fps);

    bool SenderAlive() const;
    // Copies the newest frame scaled to width x height into dst (BGR24,
    // bottom-up, stride = width * 3). Returns false when no frame is available.
    bool ReadFrame(uint8_t* dst, uint32_t width, uint32_t height, uint64_t* frame_index);

private:
    SharedHeader* header_ = nullptr;
    uint8_t* payload_ = nullptr;
    HANDLE mapping_ = nullptr;
    uint8_t* scratch_ = nullptr;
};

}  // namespace onlycam
