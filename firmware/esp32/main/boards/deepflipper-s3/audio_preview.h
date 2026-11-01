#pragma once
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <mutex>
#include "preview_session.h"

class AudioPreview {
public:
    static AudioPreview& Get();
    bool Busy();
    std::string Begin(uint32_t samples);
    bool Chunk(const std::string& id, size_t offset, const uint8_t* data, size_t size);
    bool Commit(const std::string& id);
    bool Stop(const std::string& id = "");
    void AppendStatus(cJSON* output);

private:
    std::mutex mutex_;
    dfpreview::Session session_;
    uint8_t* data_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> ready_{false};
    void Run();
};
