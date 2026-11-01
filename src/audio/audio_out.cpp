#include "audio_out.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <cstdio>
#include <string>

#include "../common/clock.h"

namespace {
template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, without pulling in ksmedia.h
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

std::wstring default_device_id(IMMDeviceEnumerator* enumerator) {
    IMMDevice* device = nullptr;
    std::wstring id;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        LPWSTR s = nullptr;
        if (SUCCEEDED(device->GetId(&s))) {
            id = s;
            CoTaskMemFree(s);
        }
    }
    release(device);
    return id;
}
}  // namespace

bool AudioOut::start() {
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void AudioOut::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AudioOut::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool warned = false;
    while (running_) {
        bool opened = play_default_device();
        if (!opened && !warned) {
            fprintf(stderr, "[audio] could not open the audio output; retrying in the background\n");
            warned = true;
        }
        // Device changed: open the new one now. None available: try again every half second
        for (int i = 0; i < (opened ? 1 : 10) && running_; i++) Sleep(50);
    }
    CoUninitialize();
}

// Plays on the current default device until it fails, the default changes or
// the output stops. Returns false if the device could not be opened at all.
bool AudioOut::play_default_device() {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* mix = nullptr;
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                         __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) &&
              SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
              SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) &&
              SUCCEEDED(client->GetMixFormat(&mix));
    // Float at the device's own rate: the resampling is ours (better than the
    // Windows default one), Windows only maps the channels to the speakers
    const auto mode = AudioStream::Speakers(speakers_.load());
    const int channels = AudioStream::channels(mode);
    WAVEFORMATEXTENSIBLE fmt{};
    if (ok) {
        fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels = WORD(channels);
        fmt.Format.nSamplesPerSec = mix->nSamplesPerSec;
        fmt.Format.wBitsPerSample = 32;
        fmt.Format.nBlockAlign = WORD(channels * 4);
        fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * fmt.Format.nBlockAlign;
        fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        fmt.Samples.wValidBitsPerSample = 32;
        fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        if (channels == 4) {   // the rear pair on the device's back speakers, or its side ones (5.1 and 7.1 layouts)
            DWORD device_mask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE
                                    ? reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask : 0;
            bool side_only = (device_mask & SPEAKER_SIDE_LEFT) && !(device_mask & SPEAKER_BACK_LEFT);
            fmt.dwChannelMask |= side_only ? SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT : SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        }
        fmt.SubFormat = kSubtypeFloat;
        ok = SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                          AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                              AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                          200000 /* 20 ms */, 0, &fmt.Format, nullptr)) &&
             SUCCEEDED(client->SetEventHandle(event)) &&
             SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render)));
    }
    UINT32 buffer_frames = 0;
    if (ok) ok = SUCCEEDED(client->GetBufferSize(&buffer_frames)) && SUCCEEDED(client->Start());
    const bool opened = ok;
    if (ok) {
        stream_.set_output_rate(int(fmt.Format.nSamplesPerSec));
        printf("[audio] output at %u Hz, %d channels (the device has %u)\n", unsigned(fmt.Format.nSamplesPerSec),
               channels, unsigned(mix->nChannels));
    }

    std::wstring id = ok ? default_device_id(enumerator) : std::wstring();
    double checked = now_seconds();
    while (ok && running_) {
        WaitForSingleObject(event, 100);
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;   // e.g. the device was unplugged
        UINT32 room = buffer_frames - padding;
        if (room) {
            BYTE* data = nullptr;
            if (FAILED(render->GetBuffer(room, &data))) break;
            auto now_mode = AudioStream::Speakers(speakers_.load());
            if (AudioStream::channels(now_mode) != channels) {   // another layout: reopen with its channels
                render->ReleaseBuffer(room, AUDCLNT_BUFFERFLAGS_SILENT);
                break;
            }
            stream_.pull(reinterpret_cast<float*>(data), room, now_mode);
            render->ReleaseBuffer(room, 0);
        }
        if (now_seconds() - checked > 1.0) {   // the user picked another default output
            checked = now_seconds();
            if (default_device_id(enumerator) != id) break;
        }
    }
    if (client) client->Stop();
    if (mix) CoTaskMemFree(mix);
    release(render);
    release(client);
    release(device);
    release(enumerator);
    CloseHandle(event);
    return opened;
}
