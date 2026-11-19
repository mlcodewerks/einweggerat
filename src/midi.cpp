#include "midi.h"
#include "spessasynth/synthesizer/synth.h"
#include <cmath>
#include <deque>
#include <mutex>
struct Midi::State {
    SS_Processor *processor = nullptr;
    std::vector<uint8_t> bank_bytes;
    struct Event {
        double time;
        std::vector<uint8_t> bytes;
    };
    std::deque<Event> events;
    std::vector<uint8_t> message;
    uint8_t running_status = 0;
    unsigned expected = 0;
    bool sysex = false;
    size_t committed = 0;
    double event_time = 0, current_time = 0;
    std::mutex mutex;
    ~State() { ss_processor_free(processor); }
    bool enqueue() {
        if (events.size() >= 4096) {
            message.clear();
            return false;
        }
        events.push_back({event_time, std::move(message)});
        message.clear();
        return true;
    }
};
Midi::Midi() : state(std::make_unique<State>()) {}
Midi::~Midi() { stop(); }
void Midi::init(const fs::path &directory) {
    stop();
    error.clear();
    auto path = directory / L"einweggerat.sf2";
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
        return;
    // Pass the SoundFont through memory so Unicode executable paths work too.
    try {
        state->bank_bytes = read_file(path);
        SS_File *file = ss_file_open_from_memory(state->bank_bytes.data(), state->bank_bytes.size(), false);
        if (!file)
            throw std::runtime_error("Cannot open einweggerat.sf2");
        SS_SoundBank *bank = ss_soundbank_load(file);
        ss_file_close(file);
        if (!bank)
            throw std::runtime_error("Invalid einweggerat.sf2 SoundFont");
        const SS_ProcessorOptions options{true, 256, SS_INTERP_HERMITE, false, true};
        auto *processor = ss_processor_create(48000, &options);
        if (!processor || !ss_processor_load_soundbank(processor, bank, "main", 0, false)) {
            ss_processor_free(processor);
            ss_soundbank_free(bank);
            throw std::runtime_error("SpessaSynth initialization failed");
        }
        state->processor = processor;
    } catch (const std::exception &e) {
        error = e.what();
    }
}
void Midi::stop() {
    audio.stop();
    state = std::make_unique<State>();
    remainder = 0;
}
bool Midi::enabled() const { return state->processor != nullptr; }
bool Midi::write(uint8_t byte, uint32_t delta_us) {
    std::lock_guard<std::mutex> lock(state->mutex);
    auto &s = *state;
    if (!s.processor)
        return false;
    s.event_time = std::max(s.event_time + delta_us / 1000000.0, s.current_time);
    if (byte >= 0xf8) {
        if (byte == 0xff) {
            if (s.events.size() >= 4096)
                return false;
            s.events.push_back({s.event_time, {byte}});
        }
        return true;
    }
    if (s.sysex) {
        if (byte == 0xf7) {
            s.sysex = false;
            s.message.push_back(byte);
            return s.enqueue();
        }
        if (!(byte & 0x80)) {
            if (s.message.size() >= 65535) {
                s.sysex = false;
                s.message.clear();
                return false;
            }
            s.message.push_back(byte);
            return true;
        }
        s.sysex = false;
        s.message.clear();
    }
    if (byte & 0x80) {
        s.message.clear();
        s.running_status = byte < 0xf0 ? byte : 0;
        if (byte == 0xf0) {
            s.sysex = true;
            s.message.push_back(byte);
            return true;
        }
        s.expected = (byte < 0xf0) ? ((byte & 0xe0) == 0xc0 ? 2 : 3)
                                   : (byte == 0xf2 ? 3 : (byte == 0xf1 || byte == 0xf3 ? 2 : 1));
        s.message.push_back(byte);
        return s.expected == 1 ? s.enqueue() : true;
    }
    if (s.message.empty()) {
        if (!s.running_status)
            return false;
        s.message.push_back(s.running_status);
        s.expected = (s.running_status & 0xe0) == 0xc0 ? 2 : 3;
    }
    s.message.push_back(byte);
    return s.message.size() == s.expected ? s.enqueue() : true;
}
bool Midi::flush() {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->processor)
        return false;
    state->committed = state->events.size();
    return true;
}
void Midi::render(float *stereo, uint32_t frames) {
    std::lock_guard<std::mutex> lock(state->mutex);
    auto &s = *state;
    std::fill(stereo, stereo + frames * 2, 0.0f);
    if (!s.processor)
        return;
    while (frames) {
        while (s.committed && s.events.front().time <= s.current_time + 0.5 / 48000) {
            auto &event = s.events.front();
            if (event.bytes[0] == 0xff)
                ss_processor_system_reset(s.processor);
            else
                ss_processor_process_message(s.processor, event.bytes.data(),
                                             event.bytes.size() - (event.bytes[0] == 0xf0 ? 1 : 0), 0,
                                             s.processor->current_time);
            s.events.pop_front();
            --s.committed;
        }
        auto count = std::min(frames, uint32_t(SS_MAX_SOUND_CHUNK));
        if (s.committed)
            count = std::min(count,
                             uint32_t(std::clamp(std::round((s.events.front().time - s.current_time) * 48000),
                                                 1.0, double(count))));
        ss_processor_render_interleaved(s.processor, stereo, count);
        s.current_time += double(count) / 48000;
        stereo += count * 2;
        frames -= count;
    }
}
void Midi::frame(double fps) {
    if (!enabled() || !std::isfinite(fps) || fps < 1)
        return;
    if (!audio.running())
        audio.start(48000);
    flush();
    remainder += 48000 / fps;
    unsigned count = unsigned(remainder);
    remainder -= count;
    std::vector<float> samples(count * 2);
    render(samples.data(), count);
    std::vector<int16_t> pcm(samples.size());
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = int16_t(
            std::clamp(std::isfinite(samples[i]) ? samples[i] * 32768.0f : 0.0f, -32768.0f, 32767.0f));
    audio.push(pcm.data(), count);
}
