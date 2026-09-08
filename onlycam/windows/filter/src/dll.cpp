// COM plumbing: class factory, self registration and DirectShow category
// registration for the OnlyCam virtual camera.
#include <windows.h>
#include <strsafe.h>

#include <new>

#include "filter.h"

namespace onlycam {

// {87ABDBEA-15FF-4540-8574-06F0888F609A}
extern const GUID CLSID_OnlyCamFilter = {
    0x87abdbea, 0x15ff, 0x4540, {0x85, 0x74, 0x06, 0xf0, 0x88, 0x8f, 0x60, 0x9a}};

}  // namespace onlycam

namespace {

HMODULE g_module = nullptr;
volatile LONG g_lock_count = 0;
volatile LONG g_object_count = 0;

class ClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
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

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        onlycam::OnlyCamFilter* filter = new (std::nothrow) onlycam::OnlyCamFilter();
        if (!filter) {
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = filter->QueryInterface(riid, ppv);
        filter->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) {
            InterlockedIncrement(&g_lock_count);
        } else {
            InterlockedDecrement(&g_lock_count);
        }
        return S_OK;
    }

private:
    volatile LONG ref_count_ = 1;
};

void GuidToString(const GUID& guid, wchar_t* buffer, size_t count) {
    StringCchPrintfW(buffer, count, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", guid.Data1,
                     guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
                     guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

LONG SetValue(HKEY key, const wchar_t* name, const wchar_t* value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                          static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
}

HRESULT RegisterClass() {
    wchar_t clsid[64]{};
    GuidToString(onlycam::CLSID_OnlyCamFilter, clsid, 64);

    wchar_t module_path[MAX_PATH]{};
    if (GetModuleFileNameW(g_module, module_path, MAX_PATH) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    wchar_t key_path[128]{};
    StringCchPrintfW(key_path, 128, L"CLSID\\%s", clsid);

    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(HKEY_CLASSES_ROOT, key_path, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                  KEY_WRITE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(result);
    }
    SetValue(key, nullptr, onlycam::kFilterName);

    HKEY server_key = nullptr;
    result = RegCreateKeyExW(key, L"InprocServer32", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE,
                             nullptr, &server_key, nullptr);
    if (result == ERROR_SUCCESS) {
        SetValue(server_key, nullptr, module_path);
        SetValue(server_key, L"ThreadingModel", L"Both");
        RegCloseKey(server_key);
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(result);
}

void UnregisterClass_() {
    wchar_t clsid[64]{};
    GuidToString(onlycam::CLSID_OnlyCamFilter, clsid, 64);
    wchar_t key_path[128]{};
    StringCchPrintfW(key_path, 128, L"CLSID\\%s\\InprocServer32", clsid);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, key_path);
    StringCchPrintfW(key_path, 128, L"CLSID\\%s", clsid);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, key_path);
}

HRESULT RegisterInCategory(bool register_it) {
    IFilterMapper2* mapper = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFilterMapper2, reinterpret_cast<void**>(&mapper));
    if (FAILED(hr)) {
        return hr;
    }

    if (!register_it) {
        hr = mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, onlycam::kFilterName,
                                      onlycam::CLSID_OnlyCamFilter);
        mapper->Release();
        return hr;
    }

    REGPINTYPES pin_type{};
    pin_type.clsMajorType = &MEDIATYPE_Video;
    pin_type.clsMinorType = &MEDIASUBTYPE_RGB24;

    REGFILTERPINS2 pin{};
    pin.dwFlags = REG_PINFLAG_B_OUTPUT;
    pin.cInstances = 1;
    pin.nMediaTypes = 1;
    pin.lpMediaType = &pin_type;
    pin.nMediums = 0;
    pin.lpMedium = nullptr;
    pin.clsPinCategory = &PIN_CATEGORY_CAPTURE;

    REGFILTER2 filter{};
    filter.dwVersion = 2;
    filter.dwMerit = MERIT_DO_NOT_USE;
    filter.cPins2 = 1;
    filter.rgPins2 = &pin;

    hr = mapper->RegisterFilter(onlycam::CLSID_OnlyCamFilter, onlycam::kFilterName, nullptr,
                                &CLSID_VideoInputDeviceCategory, onlycam::kFilterName, &filter);
    mapper->Release();
    return hr;
}

}  // namespace

namespace onlycam {

void DllAddRef() { InterlockedIncrement(&g_object_count); }

void DllRelease() { InterlockedDecrement(&g_object_count); }

}  // namespace onlycam

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;
    if (rclsid != onlycam::CLSID_OnlyCamFilter) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    ClassFactory* factory = new (std::nothrow) ClassFactory();
    if (!factory) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return (g_lock_count == 0 && g_object_count == 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    HRESULT hr = RegisterClass();
    if (FAILED(hr)) {
        return hr;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    hr = RegisterInCategory(true);
    CoUninitialize();
    return hr;
}

STDAPI DllUnregisterServer() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HRESULT hr = RegisterInCategory(false);
    CoUninitialize();
    UnregisterClass_();
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
