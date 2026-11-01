#include "image_io.h"

#include <windows.h>
#include <wincodec.h>

#include <cstdio>
#include <utility>

#include "../common/paths.h"   // widen

namespace {

// One WIC factory for the process (packs, crosshairs, menus); COM is set up
// on each thread that uses it (fine if the thread already has it)
IWICImagingFactory* wic() {
    thread_local const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)com;
    static IWICImagingFactory* factory = [] {
        IWICImagingFactory* f = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        return f;
    }();
    return factory;
}

// The first frame of a picture file in that pixel format, 4 bytes per pixel
template <class T>
bool decode(const std::wstring& path, const GUID& format, int& w, int& h, std::vector<T>& out) {
    static_assert(4 % sizeof(T) == 0);
    IWICImagingFactory* f = wic();
    if (!f) return false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapSource* conv = nullptr;
    bool ok = false;
    if (SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                               &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(WICConvertBitmapSource(format, frame, &conv))) {
        UINT uw = 0, uh = 0;
        conv->GetSize(&uw, &uh);
        w = int(uw);
        h = int(uh);
        out.resize(size_t(uw) * uh * (4 / sizeof(T)));
        ok = w > 0 && h > 0 &&
             SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, UINT(out.size() * sizeof(T)), reinterpret_cast<BYTE*>(out.data())));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return ok;
}

// A PNG of BGRA bytes
bool encode_png(const std::string& path, int w, int h, const uint8_t* bgra) {
    IWICImagingFactory* f = wic();
    if (!f) return false;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;
    UINT bytes = UINT(w) * UINT(h) * 4;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(f->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(widen(path).c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(UINT(w), UINT(h))) && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
        IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
        SUCCEEDED(frame->WritePixels(UINT(h), UINT(w) * 4, bytes, const_cast<uint8_t*>(bgra))) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit())) {
        ok = true;
    }
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    return ok;
}

}  // namespace

bool png_load(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba) {
    return decode(widen(path), GUID_WICPixelFormat32bppRGBA, w, h, rgba);
}

bool image_load_bgra(const std::wstring& path, int& w, int& h, std::vector<uint32_t>& px) {
    return decode(path, GUID_WICPixelFormat32bppBGRA, w, h, px);
}

bool png_save(const std::string& path, int w, int h, const uint8_t* rgba) {
    std::vector<uint8_t> bgra(rgba, rgba + size_t(w) * h * 4);   // the PNG encoder takes BGRA
    for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    return encode_png(path, w, h, bgra.data());
}

bool png_save_bgra(const std::string& path, int w, int h, const uint32_t* px) {
    return encode_png(path, w, h, reinterpret_cast<const uint8_t*>(px));
}

bool bmp_save(const std::string& path, int w, int h, const void* bgra) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + DWORD(w) * DWORD(h) * 4;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;   // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    bool ok = fwrite(&fh, sizeof(fh), 1, f) == 1 && fwrite(&ih, sizeof(ih), 1, f) == 1 &&
              fwrite(bgra, 4, size_t(w) * h, f) == size_t(w) * h;
    return fclose(f) == 0 && ok;
}

std::vector<std::string> list_files(const std::string& dir, const char* pattern) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA((dir + "/" + pattern).c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        out.push_back(fd.cFileName);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    return out;
}

void list_hashes(const std::string& dir, std::unordered_set<uint64_t>& out) {
    for (const std::string& name : list_files(dir, "*.png")) {
        unsigned long long hash = 0;
        if (sscanf(name.c_str(), "%16llx", &hash) == 1) out.insert(hash);
    }
}
