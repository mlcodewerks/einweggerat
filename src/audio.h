#pragma once
#include "common.h"
#include "dsp_chain.h"
#include <atomic>
#include <future>
#include <mutex>
#include <thread>
class Audio {
  public:
    ~Audio() { stop(); }
    void start(double rate);
    void stop();
    void clear();
    size_t push(const int16_t *samples, size_t frames);
    double queued_seconds();
    std::string error();
    bool running() const { return active; }
    dsp_chain dsp;
    std::atomic<uint64_t> underruns{0}, dropped{0};

  private:
    std::mutex mutex;
    std::thread thread;
    HANDLE stop_event = nullptr;
    std::vector<int16_t> ring, processed;
    size_t read_pos = 0, write_pos = 0, count = 0;
    unsigned rate = 48000;
    std::string last_error;
    std::atomic<bool> active{false};
    void worker(std::promise<void> ready);
};
