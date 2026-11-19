#include "audio.h"
#include "common.h"
#include "input.h"
#include "midi.h"
#include "vfs.h"
#include <cmath>
#include <cstdio>
static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    fs::path scratch;
    try {
        scratch = fs::temp_directory_path() / (L"einweggerat-native-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(scratch);
        const auto *v = windows_vfs();
        auto path = scratch / L"unicode-\u65e5\u672c\u8a9e.bin";
        auto name = path.u8string();
        auto *f = v->open(name.c_str(), RETRO_VFS_FILE_ACCESS_READ_WRITE, 0);
        require(f, "VFS create failed");
        const char bytes[] = "mapped Windows VFS";
        require(v->write(f, bytes, sizeof(bytes)) == sizeof(bytes), "VFS write failed");
        require(v->flush(f) == 0 && v->seek(f, 0, RETRO_VFS_SEEK_POSITION_START) == 0,
                "VFS flush/seek failed");
        char output[64]{};
        require(v->read(f, output, sizeof(bytes)) == sizeof(bytes) &&
                    !std::memcmp(bytes, output, sizeof(bytes)),
                "VFS read/write round trip failed");
        require(v->truncate(f, 5) == 5 && v->size(f) == 5, "VFS truncate failed");
        require(v->close(f) == 0, "VFS close failed");
        f = v->open(name.c_str(), RETRO_VFS_FILE_ACCESS_READ, 0);
        require(f, "VFS mapped open failed");
        require(v->read(f, output, 64) == 5 && v->read(f, output, 1) == 0, "VFS mapped EOF failed");
        require(v->seek(f, -3, RETRO_VFS_SEEK_POSITION_END) == 2 && v->read(f, output, 3) == 3 &&
                    !std::memcmp(output, bytes + 2, 3),
                "VFS mapped random access failed");
        require(v->write(f, bytes, 1) == -1 && v->truncate(f, 0) == -1, "VFS permissions violated");
        require(v->seek(f, INT64_MIN, RETRO_VFS_SEEK_POSITION_CURRENT) == -1, "VFS seek underflow accepted");
        require(v->seek(f, INT64_MAX, RETRO_VFS_SEEK_POSITION_START) == INT64_MAX &&
                    v->seek(f, 1, RETRO_VFS_SEEK_POSITION_CURRENT) == -1,
                "VFS seek overflow accepted");
        v->close(f);
        int64_t length = 0;
        require(v->stat_64(name.c_str(), &length) == RETRO_VFS_STAT_IS_VALID && length == 5,
                "VFS stat64 failed");
        auto renamed = (scratch / L"renamed.bin").u8string();
        require(v->rename(name.c_str(), renamed.c_str()) == 0, "VFS rename failed");
        auto *dir = v->opendir(scratch.u8string().c_str(), false);
        require(dir, "VFS opendir failed");
        bool found = false;
        while (v->readdir(dir))
            if (std::string(v->dirent_get_name(dir)) == "renamed.bin")
                found = !v->dirent_is_dir(dir);
        v->closedir(dir);
        require(found, "VFS directory listing failed");
        require(v->remove(renamed.c_str()) == 0, "VFS remove failed");
        f = v->open(name.c_str(), RETRO_VFS_FILE_ACCESS_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING, 0);
        require(!f, "VFS update unexpectedly created a missing file");
        f = v->open(name.c_str(), RETRO_VFS_FILE_ACCESS_WRITE, 0);
        require(f && v->close(f) == 0, "VFS empty create failed");
        f = v->open(name.c_str(), RETRO_VFS_FILE_ACCESS_READ, 0);
        require(f && v->read(f, output, 1) == 0, "VFS empty file failed");
        v->close(f);
        retro_vfs_interface_info info{5, nullptr};
        require(!vfs_environment(&info), "VFS accepted unsupported version");
        info.required_interface_version = 1;
        require(vfs_environment(&info) && info.required_interface_version == 4,
                "VFS version negotiation failed");
        std::puts("PASS native Windows VFS: Unicode, mapping, EOF, writes, update, permissions, truncate, "
                  "seek overflow, directories");
        XINPUT_GAMEPAD pad{};
        pad.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_DPAD_LEFT;
        pad.bLeftTrigger = 255;
        auto mask = Input::xinput_buttons(pad);
        require(mask == ((1 << RETRO_DEVICE_ID_JOYPAD_B) | (1 << RETRO_DEVICE_ID_JOYPAD_START) |
                         (1 << RETRO_DEVICE_ID_JOYPAD_LEFT) | (1 << RETRO_DEVICE_ID_JOYPAD_L2)),
                "XInput mapping failed");
        Input controls;
        controls.focus(false);
        controls.poll(false, false);
        require(controls.state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK) == 0,
                "Focus lost retained inputs");
        std::puts("PASS XInput mapping and focus clearing");
        Midi midi;
        midi.init(scratch);
        require(!midi.enabled() && !midi.write(0x90, 0) && !midi.flush(),
                "MIDI enabled without root einweggerat.sf2");
        fs::create_directories(scratch / L"midi");
        if (argc > 1) {
            fs::copy_file(fs::u8path(argv[1]), scratch / L"midi/einweggerat.sf2");
            midi.init(scratch);
            require(!midi.enabled(), "MIDI searched a subdirectory");
            fs::copy_file(fs::u8path(argv[1]), scratch / L"einweggerat.sf2");
            midi.init(scratch);
            require(midi.enabled(), midi.error.c_str());
            for (uint8_t byte : {0x90, 60, 100})
                require(midi.write(byte, 0), "MIDI note write failed");
            require(midi.flush(), "MIDI flush failed");
            std::vector<float> samples(48000 * 2);
            midi.render(samples.data(), 48000);
            double energy = 0;
            for (float value : samples) {
                require(std::isfinite(value), "MIDI generated nonfinite sample");
                energy += value * value;
            }
            require(energy > 0.001, "SpessaSynth produced silence");
            require(midi.write(61, 0) && midi.write(90, 0) && midi.write(0xf8, 0) && midi.flush(),
                    "MIDI running status/realtime failed");
        }
        midi.stop();
        std::puts("PASS SpessaSynth: root SoundFont gating, note synthesis, running status and realtime");
        Audio audio;
        audio.start(48000);
        std::vector<int16_t> silence(960, 0);
        for (int i = 0; i < 30; ++i) {
            audio.push(silence.data(), 480);
            Sleep(10);
        }
        require(audio.running(), audio.error().c_str());
        audio.start(44100);
        require(audio.running(), audio.error().c_str());
        audio.stop();
        std::puts("PASS real WASAPI shared playback and source-rate reconfiguration");
        fs::remove_all(scratch);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
