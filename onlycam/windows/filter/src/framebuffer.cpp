#include "framebuffer.h"

#include <sddl.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace onlycam {
namespace {

// Everyone may open the section and low integrity processes (browser and
// conferencing app sandboxes) may read from it.
constexpr wchar_t kSecurityDescriptor[] = L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)";

HANDLE CreateMapping() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{};
    SECURITY_ATTRIBUTES* attributes_ptr = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kSecurityDescriptor, SDDL_REVISION_1,
                                                             &descriptor, nullptr)) {
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
        attributes_ptr = &attributes;
    }
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, attributes_ptr, PAGE_READWRITE, 0,
                                        kMappingSize, kMappingName);
    if (descriptor) {
        LocalFree(descriptor);
    }
    return mapping;
}

}  // namespace

FrameReader::~FrameReader() {
    Close();
}

bool FrameReader::Open() {
    if (header_) {
        return true;
    }
    SetLastError(ERROR_SUCCESS);
    mapping_ = CreateMapping();
    if (!mapping_) {
        return false;
    }
    const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
    void* view = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, kMappingSize);
    if (!view) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    header_ = static_cast<SharedHeader*>(view);
    payload_ = static_cast<uint8_t*>(view) + kHeaderSize;
    if (created || header_->magic != kMagic) {
        ZeroMemory(header_, kHeaderSize);
        header_->magic = kMagic;
        header_->version = kVersion;
        header_->header_size = kHeaderSize;
        header_->max_width = kMaxWidth;
        header_->max_height = kMaxHeight;
        header_->format = kFormatBGR24;
    }
    scratch_ = static_cast<uint8_t*>(malloc(kMaxFrameBytes));
    return scratch_ != nullptr;
}

void FrameReader::Close() {
    if (header_) {
        UnmapViewOfFile(header_);
        header_ = nullptr;
        payload_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    free(scratch_);
    scratch_ = nullptr;
}

void FrameReader::AddConsumer(uint32_t width, uint32_t height, uint32_t fps) {
    if (!header_) {
        return;
    }
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&header_->consumer_count));
    SetWanted(width, height, fps);
}

void FrameReader::RemoveConsumer() {
    if (!header_) {
        return;
    }
    if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&header_->consumer_count), 0, 0) > 0) {
        InterlockedDecrement(reinterpret_cast<volatile LONG*>(&header_->consumer_count));
    }
}

void FrameReader::SetWanted(uint32_t width, uint32_t height, uint32_t fps) {
    if (!header_) {
        return;
    }
    header_->want_width = width;
    header_->want_height = height;
    header_->want_fps = fps;
    header_->consumer_tick_ms = GetTickCount64();
}

bool FrameReader::SenderAlive() const {
    if (!header_ || !header_->sender_active) {
        return false;
    }
    const uint64_t now = GetTickCount64();
    const uint64_t tick = header_->sender_tick_ms;
    return now >= tick && (now - tick) <= kSenderTimeoutMs;
}

bool FrameReader::ReadFrame(uint8_t* dst, uint32_t width, uint32_t height, uint64_t* frame_index) {
    if (!header_ || !scratch_ || !SenderAlive()) {
        return false;
    }

    uint32_t src_width = 0;
    uint32_t src_height = 0;
    uint64_t index = 0;
    bool copied = false;
    for (int attempt = 0; attempt < 8 && !copied; ++attempt) {
        const uint32_t before = header_->sequence;
        if (before & 1u) {
            Sleep(1);
            continue;
        }
        src_width = header_->width;
        src_height = header_->height;
        index = header_->frame_index;
        if (src_width == 0 || src_height == 0 || src_width > kMaxWidth || src_height > kMaxHeight) {
            return false;
        }
        memcpy(scratch_, payload_, static_cast<size_t>(src_width) * src_height * 3);
        MemoryBarrier();
        copied = header_->sequence == before;
    }
    if (!copied) {
        return false;
    }
    if (frame_index) {
        *frame_index = index;
    }

    const size_t dst_stride = static_cast<size_t>(width) * 3;
    const size_t src_stride = static_cast<size_t>(src_width) * 3;
    if (src_width == width && src_height == height) {
        memcpy(dst, scratch_, dst_stride * height);
        return true;
    }

    // Nearest neighbour rescale; both buffers are bottom-up so rows map directly.
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t src_y = std::min(src_height - 1, y * src_height / height);
        const uint8_t* src_row = scratch_ + src_y * src_stride;
        uint8_t* dst_row = dst + y * dst_stride;
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t src_x = std::min(src_width - 1, x * src_width / width);
            memcpy(dst_row + static_cast<size_t>(x) * 3, src_row + static_cast<size_t>(src_x) * 3, 3);
        }
    }
    return true;
}

}  // namespace onlycam
