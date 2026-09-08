#include "filter.h"

#include <cstring>
#include <new>

#ifndef E_PROP_SET_UNSUPPORTED
#define E_PROP_SET_UNSUPPORTED static_cast<HRESULT>(0x80070492L)
#endif

namespace onlycam {
namespace {

constexpr uint32_t kDefaultFps = 30;
constexpr REFERENCE_TIME kUnitsPerSecond = 10000000;

const VideoCap kCaps[] = {
    {1920, 1080}, {1280, 720}, {960, 540}, {640, 480}, {640, 360}, {320, 240},
};

void FreeMediaTypeContents(AM_MEDIA_TYPE* mt) {
    if (!mt) {
        return;
    }
    if (mt->cbFormat != 0 && mt->pbFormat != nullptr) {
        CoTaskMemFree(mt->pbFormat);
        mt->cbFormat = 0;
        mt->pbFormat = nullptr;
    }
    if (mt->pUnk) {
        mt->pUnk->Release();
        mt->pUnk = nullptr;
    }
}

bool CopyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src) {
    *dst = *src;
    if (src->cbFormat != 0 && src->pbFormat != nullptr) {
        dst->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src->cbFormat));
        if (!dst->pbFormat) {
            dst->cbFormat = 0;
            return false;
        }
        memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
    } else {
        dst->pbFormat = nullptr;
        dst->cbFormat = 0;
    }
    if (dst->pUnk) {
        dst->pUnk->AddRef();
    }
    return true;
}

class PinEnumerator : public IEnumPins {
public:
    PinEnumerator(OnlyCamFilter* filter, ULONG index) : filter_(filter), index_(index) {
        filter_->AddRef();
    }
    ~PinEnumerator() { filter_->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_count_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG count = InterlockedDecrement(&ref_count_);
        if (count == 0) {
            delete this;
        }
        return count;
    }

    STDMETHODIMP Next(ULONG count, IPin** pins, ULONG* fetched) override {
        if (!pins) {
            return E_POINTER;
        }
        ULONG delivered = 0;
        while (delivered < count && index_ == 0) {
            IPin* pin = static_cast<IPin*>(filter_->Pin());
            pin->AddRef();
            pins[delivered++] = pin;
            index_ = 1;
        }
        if (fetched) {
            *fetched = delivered;
        }
        return delivered == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override {
        index_ += count;
        return index_ > 1 ? S_FALSE : S_OK;
    }
    STDMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumPins** out) override {
        if (!out) {
            return E_POINTER;
        }
        *out = new (std::nothrow) PinEnumerator(filter_, index_);
        return *out ? S_OK : E_OUTOFMEMORY;
    }

private:
    volatile LONG ref_count_ = 1;
    OnlyCamFilter* filter_;
    ULONG index_;
};

class MediaTypeEnumerator : public IEnumMediaTypes {
public:
    MediaTypeEnumerator(OnlyCamPin* pin, ULONG index) : pin_(pin), index_(index) {
        static_cast<IPin*>(pin_)->AddRef();
    }
    ~MediaTypeEnumerator() { static_cast<IPin*>(pin_)->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_count_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG count = InterlockedDecrement(&ref_count_);
        if (count == 0) {
            delete this;
        }
        return count;
    }

    STDMETHODIMP Next(ULONG count, AM_MEDIA_TYPE** types, ULONG* fetched) override {
        if (!types) {
            return E_POINTER;
        }
        size_t total = 0;
        Capabilities(&total);
        ULONG delivered = 0;
        while (delivered < count && index_ < total) {
            AM_MEDIA_TYPE* mt = nullptr;
            BYTE scc[sizeof(VIDEO_STREAM_CONFIG_CAPS)]{};
            if (FAILED(pin_->GetStreamCaps(static_cast<int>(index_), &mt, scc))) {
                break;
            }
            types[delivered++] = mt;
            ++index_;
        }
        if (fetched) {
            *fetched = delivered;
        }
        return delivered == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override {
        size_t total = 0;
        Capabilities(&total);
        index_ += count;
        return index_ > total ? S_FALSE : S_OK;
    }
    STDMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumMediaTypes** out) override {
        if (!out) {
            return E_POINTER;
        }
        *out = new (std::nothrow) MediaTypeEnumerator(pin_, index_);
        return *out ? S_OK : E_OUTOFMEMORY;
    }

private:
    volatile LONG ref_count_ = 1;
    OnlyCamPin* pin_;
    ULONG index_;
};

}  // namespace

const VideoCap* Capabilities(size_t* count) {
    if (count) {
        *count = sizeof(kCaps) / sizeof(kCaps[0]);
    }
    return kCaps;
}

// ---------------------------------------------------------------- OnlyCamPin

OnlyCamPin::OnlyCamPin(OnlyCamFilter* filter) : filter_(filter) {
    InitializeCriticalSection(&lock_);
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BuildMediaType(&media_type_, width_, height_, fps_);
    media_type_valid_ = true;
}

OnlyCamPin::~OnlyCamPin() {
    Deactivate();
    Disconnect();
    FreeMediaTypeContents(&media_type_);
    if (stop_event_) {
        CloseHandle(stop_event_);
    }
    DeleteCriticalSection(&lock_);
}

STDMETHODIMP OnlyCamPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
    } else if (riid == IID_IAMStreamConfig) {
        *ppv = static_cast<IAMStreamConfig*>(this);
    } else if (riid == IID_IKsPropertySet) {
        *ppv = static_cast<IKsPropertySet*>(this);
    } else if (riid == IID_IQualityControl) {
        *ppv = static_cast<IQualityControl*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) OnlyCamPin::AddRef() {
    return filter_->AddRef();
}

STDMETHODIMP_(ULONG) OnlyCamPin::Release() {
    return filter_->Release();
}

void OnlyCamPin::BuildMediaType(AM_MEDIA_TYPE* mt, uint32_t width, uint32_t height, uint32_t fps) const {
    ZeroMemory(mt, sizeof(*mt));
    VIDEOINFOHEADER* vih = static_cast<VIDEOINFOHEADER*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    if (!vih) {
        return;
    }
    ZeroMemory(vih, sizeof(*vih));
    vih->AvgTimePerFrame = kUnitsPerSecond / (fps ? fps : kDefaultFps);
    vih->dwBitRate = width * height * 24 * (fps ? fps : kDefaultFps);
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = static_cast<LONG>(width);
    vih->bmiHeader.biHeight = static_cast<LONG>(height);
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = 24;
    vih->bmiHeader.biCompression = BI_RGB;
    vih->bmiHeader.biSizeImage = width * height * 3;

    mt->majortype = MEDIATYPE_Video;
    mt->subtype = MEDIASUBTYPE_RGB24;
    mt->formattype = FORMAT_VideoInfo;
    mt->bFixedSizeSamples = TRUE;
    mt->bTemporalCompression = FALSE;
    mt->lSampleSize = vih->bmiHeader.biSizeImage;
    mt->cbFormat = sizeof(VIDEOINFOHEADER);
    mt->pbFormat = reinterpret_cast<BYTE*>(vih);
}

STDMETHODIMP OnlyCamPin::QueryAccept(const AM_MEDIA_TYPE* mt) {
    if (!mt || mt->majortype != MEDIATYPE_Video || mt->subtype != MEDIASUBTYPE_RGB24 ||
        mt->formattype != FORMAT_VideoInfo || mt->cbFormat < sizeof(VIDEOINFOHEADER) || !mt->pbFormat) {
        return S_FALSE;
    }
    const VIDEOINFOHEADER* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt->pbFormat);
    if (vih->bmiHeader.biBitCount != 24 || vih->bmiHeader.biCompression != BI_RGB) {
        return S_FALSE;
    }
    const LONG width = vih->bmiHeader.biWidth;
    const LONG height = vih->bmiHeader.biHeight < 0 ? -vih->bmiHeader.biHeight : vih->bmiHeader.biHeight;
    if (width <= 0 || height <= 0 || width > static_cast<LONG>(kMaxWidth) ||
        height > static_cast<LONG>(kMaxHeight)) {
        return S_FALSE;
    }
    return S_OK;
}

STDMETHODIMP OnlyCamPin::Connect(IPin* receive_pin, const AM_MEDIA_TYPE* mt) {
    if (!receive_pin) {
        return E_POINTER;
    }
    EnterCriticalSection(&lock_);
    if (connected_pin_) {
        LeaveCriticalSection(&lock_);
        return VFW_E_ALREADY_CONNECTED;
    }

    HRESULT hr = VFW_E_NO_ACCEPTABLE_TYPES;
    AM_MEDIA_TYPE candidate{};
    bool connected = false;

    auto try_type = [&](const AM_MEDIA_TYPE* type) -> bool {
        if (QueryAccept(type) != S_OK) {
            return false;
        }
        if (FAILED(receive_pin->ReceiveConnection(this, type))) {
            return false;
        }
        FreeMediaTypeContents(&media_type_);
        if (!CopyMediaType(&media_type_, type)) {
            receive_pin->Disconnect();
            return false;
        }
        media_type_valid_ = true;
        const VIDEOINFOHEADER* vih = reinterpret_cast<const VIDEOINFOHEADER*>(media_type_.pbFormat);
        width_ = static_cast<uint32_t>(vih->bmiHeader.biWidth);
        height_ = static_cast<uint32_t>(vih->bmiHeader.biHeight < 0 ? -vih->bmiHeader.biHeight
                                                                    : vih->bmiHeader.biHeight);
        fps_ = vih->AvgTimePerFrame > 0
                   ? static_cast<uint32_t>(kUnitsPerSecond / vih->AvgTimePerFrame)
                   : kDefaultFps;
        return true;
    };

    if (mt && mt->majortype == MEDIATYPE_Video && mt->pbFormat) {
        connected = try_type(mt);
    }
    if (!connected && media_type_valid_) {
        connected = try_type(&media_type_);
    }
    if (!connected) {
        size_t count = 0;
        const VideoCap* caps = Capabilities(&count);
        for (size_t i = 0; i < count && !connected; ++i) {
            BuildMediaType(&candidate, caps[i].width, caps[i].height, kDefaultFps);
            connected = try_type(&candidate);
            FreeMediaTypeContents(&candidate);
        }
    }
    if (!connected) {
        LeaveCriticalSection(&lock_);
        return hr;
    }

    connected_pin_ = receive_pin;
    connected_pin_->AddRef();
    hr = NegotiateAllocator(receive_pin);
    if (FAILED(hr)) {
        receive_pin->Disconnect();
        connected_pin_->Release();
        connected_pin_ = nullptr;
        LeaveCriticalSection(&lock_);
        return hr;
    }
    LeaveCriticalSection(&lock_);
    return S_OK;
}

HRESULT OnlyCamPin::NegotiateAllocator(IPin* receive_pin) {
    ReleaseAllocator();
    HRESULT hr = receive_pin->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&mem_input_));
    if (FAILED(hr)) {
        return hr;
    }

    IMemAllocator* allocator = nullptr;
    if (FAILED(mem_input_->GetAllocator(&allocator)) || !allocator) {
        hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator,
                              reinterpret_cast<void**>(&allocator));
        if (FAILED(hr)) {
            return hr;
        }
    }

    ALLOCATOR_PROPERTIES request{};
    request.cBuffers = 4;
    request.cbBuffer = static_cast<long>(width_ * height_ * 3);
    request.cbAlign = 1;
    request.cbPrefix = 0;

    ALLOCATOR_PROPERTIES preferred{};
    if (SUCCEEDED(mem_input_->GetAllocatorRequirements(&preferred))) {
        if (preferred.cBuffers > request.cBuffers) {
            request.cBuffers = preferred.cBuffers;
        }
        if (preferred.cbAlign > request.cbAlign) {
            request.cbAlign = preferred.cbAlign;
        }
        if (preferred.cbPrefix > request.cbPrefix) {
            request.cbPrefix = preferred.cbPrefix;
        }
    }

    ALLOCATOR_PROPERTIES actual{};
    hr = allocator->SetProperties(&request, &actual);
    if (FAILED(hr) || actual.cbBuffer < request.cbBuffer) {
        allocator->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }
    hr = mem_input_->NotifyAllocator(allocator, FALSE);
    if (FAILED(hr)) {
        allocator->Release();
        return hr;
    }
    allocator_ = allocator;
    return S_OK;
}

void OnlyCamPin::ReleaseAllocator() {
    if (allocator_) {
        allocator_->Decommit();
        allocator_->Release();
        allocator_ = nullptr;
    }
    if (mem_input_) {
        mem_input_->Release();
        mem_input_ = nullptr;
    }
}

STDMETHODIMP OnlyCamPin::ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) {
    return E_UNEXPECTED;
}

STDMETHODIMP OnlyCamPin::Disconnect() {
    Deactivate();
    EnterCriticalSection(&lock_);
    ReleaseAllocator();
    if (connected_pin_) {
        connected_pin_->Release();
        connected_pin_ = nullptr;
    }
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP OnlyCamPin::ConnectedTo(IPin** pin) {
    if (!pin) {
        return E_POINTER;
    }
    EnterCriticalSection(&lock_);
    *pin = connected_pin_;
    if (*pin) {
        (*pin)->AddRef();
    }
    LeaveCriticalSection(&lock_);
    return *pin ? S_OK : VFW_E_NOT_CONNECTED;
}

STDMETHODIMP OnlyCamPin::ConnectionMediaType(AM_MEDIA_TYPE* mt) {
    if (!mt) {
        return E_POINTER;
    }
    EnterCriticalSection(&lock_);
    HRESULT hr = VFW_E_NOT_CONNECTED;
    if (connected_pin_ && media_type_valid_) {
        hr = CopyMediaType(mt, &media_type_) ? S_OK : E_OUTOFMEMORY;
    }
    LeaveCriticalSection(&lock_);
    return hr;
}

STDMETHODIMP OnlyCamPin::QueryPinInfo(PIN_INFO* info) {
    if (!info) {
        return E_POINTER;
    }
    info->pFilter = static_cast<IBaseFilter*>(filter_);
    info->pFilter->AddRef();
    info->dir = PINDIR_OUTPUT;
    wcscpy_s(info->achName, kPinName);
    return S_OK;
}

STDMETHODIMP OnlyCamPin::QueryDirection(PIN_DIRECTION* direction) {
    if (!direction) {
        return E_POINTER;
    }
    *direction = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP OnlyCamPin::QueryId(LPWSTR* id) {
    if (!id) {
        return E_POINTER;
    }
    const size_t bytes = (wcslen(kPinName) + 1) * sizeof(WCHAR);
    *id = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (!*id) {
        return E_OUTOFMEMORY;
    }
    memcpy(*id, kPinName, bytes);
    return S_OK;
}

STDMETHODIMP OnlyCamPin::EnumMediaTypes(IEnumMediaTypes** types) {
    if (!types) {
        return E_POINTER;
    }
    *types = new (std::nothrow) MediaTypeEnumerator(this, 0);
    return *types ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP OnlyCamPin::QueryInternalConnections(IPin**, ULONG* count) {
    if (count) {
        *count = 0;
    }
    return E_NOTIMPL;
}

STDMETHODIMP OnlyCamPin::EndOfStream() {
    return E_UNEXPECTED;
}

STDMETHODIMP OnlyCamPin::BeginFlush() {
    return E_UNEXPECTED;
}

STDMETHODIMP OnlyCamPin::EndFlush() {
    return E_UNEXPECTED;
}

STDMETHODIMP OnlyCamPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) {
    return S_OK;
}

STDMETHODIMP OnlyCamPin::SetFormat(AM_MEDIA_TYPE* mt) {
    if (!mt) {
        return E_POINTER;
    }
    if (QueryAccept(mt) != S_OK) {
        return E_INVALIDARG;
    }
    EnterCriticalSection(&lock_);
    if (connected_pin_) {
        LeaveCriticalSection(&lock_);
        return VFW_E_ALREADY_CONNECTED;
    }
    FreeMediaTypeContents(&media_type_);
    const bool copied = CopyMediaType(&media_type_, mt);
    if (copied) {
        const VIDEOINFOHEADER* vih = reinterpret_cast<const VIDEOINFOHEADER*>(media_type_.pbFormat);
        width_ = static_cast<uint32_t>(vih->bmiHeader.biWidth);
        height_ = static_cast<uint32_t>(vih->bmiHeader.biHeight < 0 ? -vih->bmiHeader.biHeight
                                                                    : vih->bmiHeader.biHeight);
        if (vih->AvgTimePerFrame > 0) {
            fps_ = static_cast<uint32_t>(kUnitsPerSecond / vih->AvgTimePerFrame);
        }
        media_type_valid_ = true;
    }
    LeaveCriticalSection(&lock_);
    return copied ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP OnlyCamPin::GetFormat(AM_MEDIA_TYPE** mt) {
    if (!mt) {
        return E_POINTER;
    }
    AM_MEDIA_TYPE* copy = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!copy) {
        return E_OUTOFMEMORY;
    }
    EnterCriticalSection(&lock_);
    const bool ok = CopyMediaType(copy, &media_type_);
    LeaveCriticalSection(&lock_);
    if (!ok) {
        CoTaskMemFree(copy);
        return E_OUTOFMEMORY;
    }
    *mt = copy;
    return S_OK;
}

STDMETHODIMP OnlyCamPin::GetNumberOfCapabilities(int* count, int* size) {
    if (!count || !size) {
        return E_POINTER;
    }
    size_t total = 0;
    Capabilities(&total);
    *count = static_cast<int>(total);
    *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP OnlyCamPin::GetStreamCaps(int index, AM_MEDIA_TYPE** mt, BYTE* scc) {
    if (!mt || !scc) {
        return E_POINTER;
    }
    size_t total = 0;
    const VideoCap* caps = Capabilities(&total);
    if (index < 0 || index >= static_cast<int>(total)) {
        return S_FALSE;
    }

    AM_MEDIA_TYPE* type = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!type) {
        return E_OUTOFMEMORY;
    }
    BuildMediaType(type, caps[index].width, caps[index].height, kDefaultFps);
    if (!type->pbFormat) {
        CoTaskMemFree(type);
        return E_OUTOFMEMORY;
    }

    VIDEO_STREAM_CONFIG_CAPS* config = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(scc);
    ZeroMemory(config, sizeof(*config));
    config->guid = FORMAT_VideoInfo;
    config->VideoStandard = AnalogVideo_None;
    config->InputSize.cx = static_cast<LONG>(caps[index].width);
    config->InputSize.cy = static_cast<LONG>(caps[index].height);
    config->MinCroppingSize = config->InputSize;
    config->MaxCroppingSize = config->InputSize;
    config->CropGranularityX = 1;
    config->CropGranularityY = 1;
    config->MinOutputSize = config->InputSize;
    config->MaxOutputSize = config->InputSize;
    config->OutputGranularityX = 1;
    config->OutputGranularityY = 1;
    config->MinFrameInterval = kUnitsPerSecond / 60;
    config->MaxFrameInterval = kUnitsPerSecond / 1;
    config->MinBitsPerSecond =
        static_cast<LONG>(caps[index].width * caps[index].height * 24);
    config->MaxBitsPerSecond = config->MinBitsPerSecond * 60;

    *mt = type;
    return S_OK;
}

STDMETHODIMP OnlyCamPin::Set(REFGUID, DWORD, void*, DWORD, void*, DWORD) {
    return E_NOTIMPL;
}

STDMETHODIMP OnlyCamPin::Get(REFGUID set, DWORD id, void*, DWORD, void* property_data,
                             DWORD data_length, DWORD* returned) {
    if (set != AMPROPSETID_Pin || id != AMPROPERTY_PIN_CATEGORY) {
        return E_PROP_SET_UNSUPPORTED;
    }
    if (!property_data && !returned) {
        return E_POINTER;
    }
    if (returned) {
        *returned = sizeof(GUID);
    }
    if (!property_data) {
        return S_OK;
    }
    if (data_length < sizeof(GUID)) {
        return E_UNEXPECTED;
    }
    *static_cast<GUID*>(property_data) = PIN_CATEGORY_CAPTURE;
    return S_OK;
}

STDMETHODIMP OnlyCamPin::QuerySupported(REFGUID set, DWORD id, DWORD* support) {
    if (set != AMPROPSETID_Pin || id != AMPROPERTY_PIN_CATEGORY) {
        return E_PROP_SET_UNSUPPORTED;
    }
    if (support) {
        *support = KSPROPERTY_SUPPORT_GET;
    }
    return S_OK;
}

STDMETHODIMP OnlyCamPin::Notify(IBaseFilter*, Quality) {
    return E_NOTIMPL;
}

STDMETHODIMP OnlyCamPin::SetSink(IQualityControl*) {
    return S_OK;
}

void OnlyCamPin::SetRunning(bool running) {
    InterlockedExchange(&running_, running ? 1 : 0);
}

HRESULT OnlyCamPin::Activate() {
    EnterCriticalSection(&lock_);
    if (thread_) {
        LeaveCriticalSection(&lock_);
        return S_OK;
    }
    if (!connected_pin_ || !allocator_ || !mem_input_) {
        LeaveCriticalSection(&lock_);
        return VFW_E_NOT_CONNECTED;
    }
    HRESULT hr = allocator_->Commit();
    if (FAILED(hr)) {
        LeaveCriticalSection(&lock_);
        return hr;
    }
    ResetEvent(stop_event_);
    thread_ = CreateThread(nullptr, 0, &OnlyCamPin::ThreadProc, this, 0, nullptr);
    LeaveCriticalSection(&lock_);
    return thread_ ? S_OK : E_FAIL;
}

HRESULT OnlyCamPin::Deactivate() {
    HANDLE thread = nullptr;
    EnterCriticalSection(&lock_);
    thread = thread_;
    thread_ = nullptr;
    LeaveCriticalSection(&lock_);

    if (thread) {
        SetEvent(stop_event_);
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
    }
    EnterCriticalSection(&lock_);
    if (allocator_) {
        allocator_->Decommit();
    }
    LeaveCriticalSection(&lock_);
    return S_OK;
}

DWORD WINAPI OnlyCamPin::ThreadProc(void* context) {
    static_cast<OnlyCamPin*>(context)->StreamLoop();
    return 0;
}

void OnlyCamPin::FillNoSignal(uint8_t* dst, size_t bytes) {
    memset(dst, 0x1E, bytes);
}

void OnlyCamPin::StreamLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const uint32_t width = width_;
    const uint32_t height = height_;
    const uint32_t fps = fps_ ? fps_ : kDefaultFps;
    const size_t frame_bytes = static_cast<size_t>(width) * height * 3;
    const REFERENCE_TIME frame_duration = kUnitsPerSecond / fps;

    reader_.Open();
    reader_.AddConsumer(width, height, fps);

    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    uint64_t frame_number = 0;
    uint64_t last_index = 0;
    while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
        // Pace the stream on the wall clock.
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsed_ms =
            static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
        const double due_ms = static_cast<double>(frame_number) * 1000.0 / fps;
        if (elapsed_ms < due_ms) {
            const DWORD sleep_ms = static_cast<DWORD>(due_ms - elapsed_ms);
            if (WaitForSingleObject(stop_event_, sleep_ms) == WAIT_OBJECT_0) {
                break;
            }
        }

        if (!InterlockedCompareExchange(&running_, 0, 0)) {
            Sleep(10);
            start.QuadPart = 0;
            QueryPerformanceCounter(&start);
            frame_number = 0;
            continue;
        }

        IMediaSample* sample = nullptr;
        IMemAllocator* allocator = allocator_;
        IMemInputPin* input = mem_input_;
        if (!allocator || !input) {
            break;
        }
        allocator->AddRef();
        input->AddRef();
        HRESULT hr = allocator->GetBuffer(&sample, nullptr, nullptr, 0);
        if (SUCCEEDED(hr) && sample) {
            BYTE* buffer = nullptr;
            if (SUCCEEDED(sample->GetPointer(&buffer)) && buffer) {
                reader_.SetWanted(width, height, fps);
                if (!reader_.ReadFrame(buffer, width, height, &last_index)) {
                    FillNoSignal(buffer, frame_bytes);
                }
                sample->SetActualDataLength(static_cast<long>(frame_bytes));
                REFERENCE_TIME sample_start = static_cast<REFERENCE_TIME>(frame_number) * frame_duration;
                REFERENCE_TIME sample_stop = sample_start + frame_duration;
                sample->SetTime(&sample_start, &sample_stop);
                sample->SetSyncPoint(TRUE);
                sample->SetDiscontinuity(frame_number == 0);
                sample->SetPreroll(FALSE);
                input->Receive(sample);
            }
            sample->Release();
        }
        input->Release();
        allocator->Release();
        ++frame_number;
    }

    reader_.RemoveConsumer();
    reader_.Close();
    CoUninitialize();
}

// ------------------------------------------------------------- OnlyCamFilter

OnlyCamFilter::OnlyCamFilter() : pin_(this) {
    DllAddRef();
    InitializeCriticalSection(&lock_);
    wcscpy_s(name_, kFilterName);
}

OnlyCamFilter::~OnlyCamFilter() {
    if (clock_) {
        clock_->Release();
    }
    DeleteCriticalSection(&lock_);
    DllRelease();
}

STDMETHODIMP OnlyCamFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter ||
        riid == IID_IBaseFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
    } else if (riid == IID_IAMFilterMiscFlags) {
        *ppv = static_cast<IAMFilterMiscFlags*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) OnlyCamFilter::AddRef() {
    return InterlockedIncrement(&ref_count_);
}

STDMETHODIMP_(ULONG) OnlyCamFilter::Release() {
    const LONG count = InterlockedDecrement(&ref_count_);
    if (count == 0) {
        delete this;
    }
    return count;
}

STDMETHODIMP OnlyCamFilter::GetClassID(CLSID* clsid) {
    if (!clsid) {
        return E_POINTER;
    }
    *clsid = CLSID_OnlyCamFilter;
    return S_OK;
}

STDMETHODIMP OnlyCamFilter::Stop() {
    EnterCriticalSection(&lock_);
    state_ = State_Stopped;
    LeaveCriticalSection(&lock_);
    pin_.SetRunning(false);
    pin_.Deactivate();
    return S_OK;
}

STDMETHODIMP OnlyCamFilter::Pause() {
    EnterCriticalSection(&lock_);
    state_ = State_Paused;
    LeaveCriticalSection(&lock_);
    pin_.SetRunning(false);
    if (pin_.IsConnected()) {
        return pin_.Activate();
    }
    return S_OK;
}

STDMETHODIMP OnlyCamFilter::Run(REFERENCE_TIME start) {
    EnterCriticalSection(&lock_);
    start_time_ = start;
    state_ = State_Running;
    LeaveCriticalSection(&lock_);
    HRESULT hr = S_OK;
    if (pin_.IsConnected()) {
        hr = pin_.Activate();
    }
    pin_.SetRunning(true);
    return hr;
}

STDMETHODIMP OnlyCamFilter::GetState(DWORD, FILTER_STATE* state) {
    if (!state) {
        return E_POINTER;
    }
    EnterCriticalSection(&lock_);
    *state = state_;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP OnlyCamFilter::SetSyncSource(IReferenceClock* clock) {
    EnterCriticalSection(&lock_);
    if (clock) {
        clock->AddRef();
    }
    if (clock_) {
        clock_->Release();
    }
    clock_ = clock;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP OnlyCamFilter::GetSyncSource(IReferenceClock** clock) {
    if (!clock) {
        return E_POINTER;
    }
    EnterCriticalSection(&lock_);
    *clock = clock_;
    if (*clock) {
        (*clock)->AddRef();
    }
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP OnlyCamFilter::EnumPins(IEnumPins** pins) {
    if (!pins) {
        return E_POINTER;
    }
    *pins = new (std::nothrow) PinEnumerator(this, 0);
    return *pins ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP OnlyCamFilter::FindPin(LPCWSTR id, IPin** pin) {
    if (!pin) {
        return E_POINTER;
    }
    if (id && wcscmp(id, kPinName) == 0) {
        *pin = static_cast<IPin*>(&pin_);
        (*pin)->AddRef();
        return S_OK;
    }
    *pin = nullptr;
    return VFW_E_NOT_FOUND;
}

STDMETHODIMP OnlyCamFilter::QueryFilterInfo(FILTER_INFO* info) {
    if (!info) {
        return E_POINTER;
    }
    EnterCriticalSection(&lock_);
    wcscpy_s(info->achName, name_);
    info->pGraph = graph_;
    if (info->pGraph) {
        info->pGraph->AddRef();
    }
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP OnlyCamFilter::JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) {
    EnterCriticalSection(&lock_);
    graph_ = graph;  // Weak reference, as required by DirectShow.
    if (name) {
        wcsncpy_s(name_, name, MAX_FILTER_NAME - 1);
    }
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP OnlyCamFilter::QueryVendorInfo(LPWSTR*) {
    return E_NOTIMPL;
}

STDMETHODIMP_(ULONG) OnlyCamFilter::GetMiscFlags() {
    return AM_FILTER_MISC_FLAGS_IS_SOURCE;
}

}  // namespace onlycam
