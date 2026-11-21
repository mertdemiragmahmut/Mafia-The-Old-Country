#pragma once

#include "dualsense_hid.hpp"
#include "wasapi_haptics.hpp"
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <mutex>

namespace ds {

class PipeServer {
public:
    PipeServer(DualSenseHidManager& hid, WasapiHapticsEngine& haptics);
    ~PipeServer();

    void Start();
    void Stop();

private:
    void AcceptLoop();
    void ClientThread(void* pipeHandle);
    std::string ProcessCommand(const std::string& line);

    DualSenseHidManager& hid_;
    WasapiHapticsEngine& haptics_;

    std::atomic<bool> running_{false};
    void* stopEvent_ = nullptr;
    std::thread acceptThread_;
    std::mutex clientsMutex_;
    std::vector<std::thread> clientThreads_;
};

} // namespace ds
