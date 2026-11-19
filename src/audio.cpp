#include "audio.h"
#include <audioclient.h>
#include <avrt.h>
#include <cmath>
#include <mmdeviceapi.h>
void Audio::start(double new_rate) {
    if (!std::isfinite(new_rate) || new_rate < 8000 || new_rate > 384000)
        throw std::runtime_error("Unsupported audio rate");
    if (active && rate == unsigned(std::lround(new_rate)))
        return;
    stop();
    rate = unsigned(std::lround(new_rate));
    dsp.reset(float(new_rate));
    ring.assign(rate * 2, 0);
    read_pos = write_pos = count = 0;
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event)
        throw std::runtime_error("Cannot create WASAPI stop event");
    std::promise<void> ready;
    auto result = ready.get_future();
    thread = std::thread(&Audio::worker, this, std::move(ready));
    try {
        result.get();
    } catch (...) {
        stop();
        throw;
    }
}
void Audio::stop() {
    if (stop_event)
        SetEvent(stop_event);
    if (thread.joinable())
        thread.join();
    if (stop_event)
        CloseHandle(stop_event);
    stop_event = nullptr;
    active = false;
}
void Audio::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    count = read_pos = write_pos = 0;
}
std::string Audio::error() {
    std::lock_guard<std::mutex> lock(mutex);
    return last_error;
}
double Audio::queued_seconds() {
    std::lock_guard<std::mutex> lock(mutex);
    return double(count) / (2 * rate);
}
size_t Audio::push(const int16_t *samples, size_t frames) {
    if (!samples || !frames || !active)
        return frames;
    dsp.process(samples, frames, processed);
    std::lock_guard<std::mutex> lock(mutex);
    const size_t wanted = processed.size();
    if (wanted > ring.size() - count) {
        // Keep latency bounded during fast-forward without blocking the UI thread.
        dropped += (wanted - std::min(wanted, ring.size() - count)) / 2;
    }
    const size_t accepted = std::min(wanted, ring.size() - count);
    for (size_t i = 0; i < accepted; ++i) {
        ring[write_pos] = processed[i];
        write_pos = (write_pos + 1) % ring.size();
    }
    count += accepted;
    return frames;
}
void Audio::worker(std::promise<void> ready) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE event = nullptr, priority = nullptr;
    bool signalled = false;
    try {
        check(com, "Initialize WASAPI COM");
        ComPtr<IMMDeviceEnumerator> enumerator;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
              "WASAPI enumerator");
        ComPtr<IMMDevice> endpoint;
        check(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint), "Default audio endpoint");
        ComPtr<IAudioClient> client;
        check(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void **>(client.GetAddressOf())),
              "WASAPI client");
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = rate * 4;
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                     AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 1000000, 0, &format, nullptr),
              "WASAPI shared PCM stream");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            throw std::runtime_error("WASAPI event creation failed");
        check(client->SetEventHandle(event), "WASAPI event handle");
        ComPtr<IAudioRenderClient> render;
        check(client->GetService(IID_PPV_ARGS(&render)), "WASAPI render service");
        UINT32 capacity = 0;
        check(client->GetBufferSize(&capacity), "WASAPI buffer size");
        BYTE *initial = nullptr;
        check(render->GetBuffer(capacity, &initial), "Prime WASAPI");
        check(render->ReleaseBuffer(capacity, AUDCLNT_BUFFERFLAGS_SILENT), "Prime WASAPI silence");
        check(client->Start(), "Start WASAPI");
        DWORD task = 0;
        priority = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        active = true;
        {
            std::lock_guard<std::mutex> lock(mutex);
            last_error.clear();
        }
        ready.set_value();
        signalled = true;
        HANDLE handles[] = {stop_event, event};
        bool primed = false;
        while (WaitForMultipleObjects(2, handles, FALSE, 2000) != WAIT_OBJECT_0) {
            UINT32 padding = 0;
            check(client->GetCurrentPadding(&padding), "WASAPI padding");
            UINT32 available = capacity - std::min(capacity, padding);
            if (!available)
                continue;
            BYTE *output = nullptr;
            check(render->GetBuffer(available, &output), "WASAPI buffer");
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!primed && count >= rate * 2 / 20)
                    primed = true;
                size_t take = primed ? std::min<size_t>(available * 2, count) : 0;
                auto *pcm = reinterpret_cast<int16_t *>(output);
                for (size_t i = 0; i < take; ++i) {
                    pcm[i] = ring[read_pos];
                    read_pos = (read_pos + 1) % ring.size();
                }
                std::fill(pcm + take, pcm + available * 2, int16_t(0));
                count -= take;
                if (primed && take < available * 2) {
                    underruns += available - take / 2;
                    primed = false;
                }
            }
            check(render->ReleaseBuffer(available, 0), "WASAPI submit");
        }
        client->Stop();
    } catch (const std::exception &failure) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            last_error = failure.what();
        }
        if (!signalled)
            ready.set_exception(std::current_exception());
    }
    active = false;
    if (priority)
        AvRevertMmThreadCharacteristics(priority);
    if (event)
        CloseHandle(event);
    if (SUCCEEDED(com))
        CoUninitialize();
}
