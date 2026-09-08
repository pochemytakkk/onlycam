// Minimal DirectShow capture source filter that publishes the frames written
// by the OnlyCam application as a system camera device.
#pragma once

#include <windows.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>

#include "framebuffer.h"

namespace onlycam {

extern const GUID CLSID_OnlyCamFilter;

// Live object accounting shared with the class factory in dll.cpp.
void DllAddRef();
void DllRelease();

constexpr wchar_t kFilterName[] = L"OnlyCam";
constexpr wchar_t kPinName[] = L"Capture";

struct VideoCap {
    uint32_t width;
    uint32_t height;
};

const VideoCap* Capabilities(size_t* count);

class OnlyCamFilter;

class OnlyCamPin : public IPin, public IAMStreamConfig, public IKsPropertySet, public IQualityControl {
public:
    explicit OnlyCamPin(OnlyCamFilter* filter);
    ~OnlyCamPin();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* receive_pin, const AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP ReceiveConnection(IPin* connector, const AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* info) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* direction) override;
    STDMETHODIMP QueryId(LPWSTR* id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** types) override;
    STDMETHODIMP QueryInternalConnections(IPin** pins, ULONG* count) override;
    STDMETHODIMP EndOfStream() override;
    STDMETHODIMP BeginFlush() override;
    STDMETHODIMP EndFlush() override;
    STDMETHODIMP NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate) override;

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** mt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) override;
    STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** mt, BYTE* scc) override;

    // IKsPropertySet
    STDMETHODIMP Set(REFGUID set, DWORD id, void* instance_data, DWORD instance_length,
                     void* property_data, DWORD data_length) override;
    STDMETHODIMP Get(REFGUID set, DWORD id, void* instance_data, DWORD instance_length,
                     void* property_data, DWORD data_length, DWORD* returned) override;
    STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD* support) override;

    // IQualityControl
    STDMETHODIMP Notify(IBaseFilter* self, Quality quality) override;
    STDMETHODIMP SetSink(IQualityControl* sink) override;

    HRESULT Activate();
    HRESULT Deactivate();
    void SetRunning(bool running);
    bool IsConnected() const { return connected_pin_ != nullptr; }

private:
    static DWORD WINAPI ThreadProc(void* context);
    void StreamLoop();
    void FillNoSignal(uint8_t* dst, size_t bytes);
    HRESULT NegotiateAllocator(IPin* receive_pin);
    void ReleaseAllocator();
    void BuildMediaType(AM_MEDIA_TYPE* mt, uint32_t width, uint32_t height, uint32_t fps) const;

    OnlyCamFilter* filter_;
    IPin* connected_pin_ = nullptr;
    IMemInputPin* mem_input_ = nullptr;
    IMemAllocator* allocator_ = nullptr;
    AM_MEDIA_TYPE media_type_{};
    bool media_type_valid_ = false;

    uint32_t width_ = 1280;
    uint32_t height_ = 720;
    uint32_t fps_ = 30;

    HANDLE thread_ = nullptr;
    HANDLE stop_event_ = nullptr;
    volatile LONG running_ = 0;
    CRITICAL_SECTION lock_{};
    FrameReader reader_;
};

class OnlyCamFilter : public IBaseFilter, public IAMFilterMiscFlags {
public:
    OnlyCamFilter();
    ~OnlyCamFilter();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPersist
    STDMETHODIMP GetClassID(CLSID* clsid) override;

    // IMediaFilter
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME start) override;
    STDMETHODIMP GetState(DWORD timeout, FILTER_STATE* state) override;
    STDMETHODIMP SetSyncSource(IReferenceClock* clock) override;
    STDMETHODIMP GetSyncSource(IReferenceClock** clock) override;

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** pins) override;
    STDMETHODIMP FindPin(LPCWSTR id, IPin** pin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override;
    STDMETHODIMP QueryVendorInfo(LPWSTR* vendor) override;

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override;

    OnlyCamPin* Pin() { return &pin_; }
    IReferenceClock* Clock() { return clock_; }
    REFERENCE_TIME StartTime() const { return start_time_; }

private:
    volatile LONG ref_count_ = 1;
    FILTER_STATE state_ = State_Stopped;
    IFilterGraph* graph_ = nullptr;
    IReferenceClock* clock_ = nullptr;
    REFERENCE_TIME start_time_ = 0;
    WCHAR name_[MAX_FILTER_NAME]{};
    CRITICAL_SECTION lock_{};
    OnlyCamPin pin_;
};

}  // namespace onlycam
