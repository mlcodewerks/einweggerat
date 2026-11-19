#include "dsp_chain.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>

static void require(bool v, const std::string &message) { if (!v) throw std::runtime_error(message); }
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    try {
        const auto scratch = std::filesystem::u8path(argv[2]);
        std::filesystem::create_directories(scratch);
        dsp_chain chain;
        std::string error;
        std::vector<int16_t> input{10000, -2000, 8000, -4000}, output;
        chain.process(input.data(), 2, output);
        require(output == input, "Empty chain changed audio");
        auto halve = dsp_default_stage("panning");
        halve.parameters["left_mix"] = "0.5 0";
        auto swap = dsp_default_stage("panning");
        swap.parameters["left_mix"] = "0 1"; swap.parameters["right_mix"] = "1 0";
        dsp_settings settings{true, {halve, swap}};
        require(chain.configure(settings, error), error);
        chain.process(input.data(), 2, output);
        require(output == std::vector<int16_t>({-2000, 5000, -4000, 4000}), "Chain order or duplicate filter state incorrect");
        std::swap(settings.stages[0], settings.stages[1]);
        require(chain.configure(settings, error), error);
        chain.process(input.data(), 2, output);
        require(output == std::vector<int16_t>({-1000, 10000, -2000, 8000}), "Reordering did not change output");
        settings.stages[1].enabled = false;
        require(chain.configure(settings, error), error);
        chain.process(input.data(), 2, output);
        require(output[0] == -2000 && output[1] == 10000, "Stage bypass failed");
        settings.enabled = false;
        require(chain.configure(settings, error), error);
        chain.process(input.data(), 2, output);
        require(output == input, "Master bypass failed");
        require(input[0] == 10000, "Caller-owned audio was modified");

        require(dsp_save(scratch / "roundtrip.dsp", settings, error), error);
        dsp_settings restored;
        require(dsp_load(scratch / "roundtrip.dsp", restored, error), error);
        require(!restored.enabled && restored.stages.size() == 2 && !restored.stages[1].enabled &&
            restored.stages[0].parameters == settings.stages[0].parameters &&
            restored.stages[1].parameters == settings.stages[1].parameters, "Preset roundtrip lost settings");
        require(dsp_save(scratch / "roundtrip.dsp", restored, error), "Replacing an existing preset failed");

        settings = {true, {halve}};
        require(chain.configure(settings, error), error);
        auto invalid = settings;
        invalid.stages[0].parameters["left_mix"] = "nan 0";
        require(!chain.configure(invalid, error), "NaN accepted");
        invalid.stages[0].parameters["left_mix"] = "1 2e";
        require(!chain.configure(invalid, error), "Malformed number accepted");
        chain.process(input.data(), 2, output);
        require(output[0] == 5000, "Invalid edit replaced active chain");
        for (auto id : {"echo", "eq", "tremolo", "vibrato", "chorus", "iir", "phaser"}) {
            invalid = {true, {dsp_default_stage(id)}};
            auto &p = invalid.stages[0].parameters;
            if (std::string(id) == "echo") p["delay"] = "200 300";
            if (std::string(id) == "eq") p["block_size_log2"] = "12";
            if (std::string(id) == "tremolo" || std::string(id) == "vibrato") p["freq"] = "0";
            if (std::string(id) == "chorus") p["depth_ms"] = "1000";
            if (std::string(id) == "iir") p["frequency"] = "48000";
            if (std::string(id) == "phaser") p["stages"] = "999";
            require(!chain.configure(invalid, error), "Unsafe parameter accepted");
        }
        invalid = {true, std::vector<dsp_stage>(33, halve)};
        require(!chain.configure(invalid, error), "Chain limit ignored");

        std::vector<int16_t> signal(8192);
        for (size_t i = 0; i < signal.size(); ++i) signal[i] = static_cast<int16_t>(4000 * std::sin(i * 0.04));
        for (const auto &filter : dsp_filters()) {
            settings = {true, {dsp_default_stage(filter.id)}};
            require(chain.configure(settings, error), error);
            chain.process(signal.data(), signal.size() / 2, output);
            require(!output.empty(), filter.name);
            for (auto rate : {32000.f, 44100.f, 48000.f, 96000.f}) {
                chain.reset(rate);
                require(chain.error().empty(), chain.error());
                chain.process(signal.data(), signal.size() / 2, output);
                require(!output.empty(), "Sample rate reset lost audio");
            }
            chain.reset(48000);
        }
        unsigned presets = 0;
        for (const auto &file : std::filesystem::directory_iterator(std::filesystem::u8path(argv[1]))) {
            if (file.path().extension() != ".dsp") continue;
            require(dsp_load(file.path(), settings, error), error);
            require(chain.configure(settings, error), error);
            chain.process(signal.data(), signal.size() / 2, output);
            require(!output.empty(), "Upstream preset produced no output"); ++presets;
        }
        require(presets >= 18, "Upstream presets missing");

        // Stress the largest supported FFT, small uneven calls, and consecutive EQs.
        auto eq = dsp_default_stage("eq"); eq.parameters["block_size_log2"] = "11";
        settings = {true, {eq, eq}};
        require(chain.configure(settings, error), error);
        size_t emitted = 0;
        for (int i = 0; i < 200; ++i) {
            chain.process(signal.data(), 137, output); emitted += output.size() / 2;
        }
        require(emitted > 200 * 137 - 4096 && emitted <= 200 * 137, "FFT chain lost/duplicated frames");
        eq.parameters["impulse_response_output"] = (scratch / "impulse.txt").u8string();
        require(chain.configure({true, {eq}}, error, true), error);
        require(std::filesystem::file_size(scratch / "impulse.txt") > 0, "Release EQ impulse export failed");
        eq.parameters["impulse_response_output"] = (scratch / "absent" / "impulse.txt").u8string();
        require(!chain.configure({true, {eq}}, error, true), "EQ export failure was not reported");

        // Configuration edits can race the producer; each call sees one whole chain.
        require(chain.configure({true, {halve}}, error), error);
        std::atomic<bool> failed{false};
        std::thread producer([&] {
            std::vector<int16_t> pcm;
            for (int i = 0; i < 2000; ++i) {
                chain.process(input.data(), 2, pcm);
                if (pcm.size() != 4 || (pcm[0] != 5000 && pcm[0] != -2000)) failed = true;
            }
        });
        for (int i = 0; i < 100; ++i)
            if (!chain.configure({true, {i % 2 ? halve : swap}}, error)) failed = true;
        producer.join(); require(!failed, "Concurrent chain replacement corrupted audio");
        // Non-default WSOLA pitch/tempo must emit the requested duration and pitch.
        {
            auto stage = dsp_default_stage("native_wsola");
            stage.parameters["pitch"] = "3.25"; stage.parameters["tempo"] = "-15";
            require(chain.configure({true, {stage}}, error), error);
            std::vector<int16_t> tone(96000 * 2), processed;
            for (size_t i = 0; i < tone.size() / 2; ++i)
                tone[2*i] = tone[2*i+1] = static_cast<int16_t>(8000 * std::sin(6.283185307179586 * 440 * i / 48000));
            for (size_t i = 0; i < tone.size() / 2; i += 137) {
                chain.process(tone.data() + 2*i, std::min<size_t>(137, tone.size()/2 - i), output);
                processed.insert(processed.end(), output.begin(), output.end());
            }
            const size_t frames = processed.size() / 2;
            require(frames > 100000 && frames < 114000, "Native tempo duration incorrect");
            size_t crossings = 0, start = 12000;
            for (size_t i = start + 1; i < frames; ++i)
                if (processed[2*(i-1)] <= 0 && processed[2*i] > 0) ++crossings;
            double hz = crossings * 48000.0 / (frames - start);
            double expected = 440 * std::pow(2.0, 3.25 / 12.0);
            require(std::abs(hz - expected) < expected * 0.06, "Fractional pitch shift incorrect");
        }
        std::printf("DSP: all %zu filters, %u presets, order, bypass, persistence, validation, FFT bursts, fractional pitch/tempo, export and concurrency passed\n", dsp_filters().size(), presets);
    } catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
