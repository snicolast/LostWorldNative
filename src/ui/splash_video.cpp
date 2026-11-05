#include "splash_video.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgiformat.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <wincodec.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

// What the media engine tells us, from its own threads
class Notify : public IMFMediaEngineNotify {
public:
    std::atomic<bool> failed{false}, ended{false};

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *out = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override {
        if (event == MF_MEDIA_ENGINE_EVENT_ERROR) {
            fprintf(stderr, "[menu] splash video: Windows could not play it (error %u, 0x%08lX)\n", unsigned(param1), param2);
            failed = true;
        } else if (event == MF_MEDIA_ENGINE_EVENT_ENDED) ended = true;
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
};

template <class T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

}  // namespace

struct SplashVideo::Impl {
    bool mf = false;   // MFStartup done
    Notify* notify = nullptr;
    IMFMediaEngine* engine = nullptr;
    IWICImagingFactory* wic = nullptr;
    IWICBitmap* bitmap = nullptr;   // the picture is copied into it, at the size asked
    int bw = 0, bh = 0;
    bool have = false;   // a picture has come at least once
};

bool SplashVideo::open(const std::wstring& path) {
    close();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // any mode will do: what it returns does not matter
    impl_ = new Impl;
    Impl& m = *impl_;
    IMFMediaEngineClassFactory* factory = nullptr;
    IMFAttributes* attr = nullptr;
    HRESULT hr = MFStartup(MF_VERSION);
    m.mf = SUCCEEDED(hr);
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attr, 3);
    if (SUCCEEDED(hr)) {
        m.notify = new Notify;
        attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, m.notify);
        // No window and no Direct3D device: frame-server mode, the pictures copied out on the CPU
        attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
        attr->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, 4 /* AudioCategory_GameEffects */);
        hr = factory->CreateInstance(0, attr, &m.engine);
    }
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m.wic));
    if (SUCCEEDED(hr)) {
        wchar_t full[MAX_PATH];
        DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr);
        BSTR url = SysAllocString(n > 0 && n < MAX_PATH ? full : path.c_str());
        m.engine->SetAutoPlay(FALSE);
        m.engine->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
        hr = m.engine->SetSource(url);   // the engine keeps its own copy
        SysFreeString(url);
    }
    release(attr);
    release(factory);
    if (FAILED(hr)) {
        fprintf(stderr, "[menu] splash video: no media engine (0x%08lX)\n", hr);
        close();
        return false;
    }
    return true;
}

void SplashVideo::close() {
    if (!impl_) return;
    Impl& m = *impl_;
    if (m.engine) m.engine->Shutdown();
    release(m.engine);
    release(m.bitmap);
    release(m.wic);
    release(m.notify);
    if (m.mf) MFShutdown();
    delete impl_;
    impl_ = nullptr;
}

void SplashVideo::play() {
    if (impl_ && impl_->engine) impl_->engine->Play();
}

void SplashVideo::set_volume(float v) {
    if (impl_ && impl_->engine) impl_->engine->SetVolume(v < 0 ? 0.0 : v > 1 ? 1.0 : double(v));
}

bool SplashVideo::failed() const { return impl_ && impl_->notify->failed; }

bool SplashVideo::ended() const { return impl_ && (impl_->notify->ended || impl_->engine->IsEnded()); }

double SplashVideo::position() const { return impl_ ? impl_->engine->GetCurrentTime() : 0; }

bool SplashVideo::size(int& w, int& h) const {
    DWORD x = 0, y = 0;
    if (!impl_ || FAILED(impl_->engine->GetNativeVideoSize(&x, &y)) || x == 0 || y == 0) return false;
    w = int(x), h = int(y);
    return true;
}

bool SplashVideo::frame(int w, int h, std::vector<uint32_t>& px, bool always) {
    if (!impl_ || w <= 0 || h <= 0) return false;
    Impl& m = *impl_;
    LONGLONG pts = 0;
    bool fresh = m.engine->OnVideoStreamTick(&pts) == S_OK;
    if (fresh) m.have = true;
    if (!m.have || (!fresh && !always)) return false;
    if (!m.bitmap || m.bw != w || m.bh != h) {
        release(m.bitmap);
        if (FAILED(m.wic->CreateBitmap(UINT(w), UINT(h), GUID_WICPixelFormat32bppBGRA, WICBitmapCacheOnLoad, &m.bitmap))) return false;
        m.bw = w, m.bh = h;
    }
    MFVideoNormalizedRect src{0, 0, 1, 1};
    RECT dst{0, 0, w, h};
    MFARGB border{0, 0, 0, 255};
    if (FAILED(m.engine->TransferVideoFrame(m.bitmap, &src, &dst, &border))) return false;
    WICRect all{0, 0, w, h};
    IWICBitmapLock* lock = nullptr;
    if (FAILED(m.bitmap->Lock(&all, WICBitmapLockRead, &lock))) return false;
    UINT stride = 0, bytes = 0;
    BYTE* data = nullptr;
    lock->GetStride(&stride);
    lock->GetDataPointer(&bytes, &data);
    px.resize(size_t(w) * h);
    if (data)
        for (int y = 0; y < h; y++) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(data + size_t(y) * stride);
            for (int x = 0; x < w; x++) px[size_t(y) * w + x] = row[x] | 0xFF000000u;   // opaque
        }
    lock->Release();
    return data != nullptr;
}
