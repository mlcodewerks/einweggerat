#pragma once
#include "audio.h"
#include "common.h"
#include <memory>
class Midi {
  public:
    Midi();
    ~Midi();
    void init(const fs::path &directory);
    void stop();
    bool enabled() const;
    bool write(uint8_t byte, uint32_t delta_us);
    bool flush();
    void frame(double fps);
    void render(float *stereo, uint32_t frames);
    Audio audio;
    std::string error;

  private:
    struct State;
    std::unique_ptr<State> state;
    double remainder = 0;
};
