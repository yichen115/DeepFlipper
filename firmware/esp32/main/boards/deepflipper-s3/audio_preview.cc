#include "audio_preview.h"
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "application.h"
#include "board.h"
#include "deepflipper_bridge.h"

namespace {
int64_t Now() { return esp_timer_get_time() / 1000; }
}  // namespace
AudioPreview& AudioPreview::Get() {
    static AudioPreview preview;
    return preview;
}
bool AudioPreview::Busy() {
    std::lock_guard<std::mutex> guard(mutex_);
    return session_.Busy() || data_ != nullptr;
}
std::string AudioPreview::Begin(uint32_t samples) {
    if (!samples || samples > dfpreview::kMaxSamples)
        return "";
    std::string id;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (session_.Busy() || data_)
            return "";
        uint8_t* allocated = static_cast<uint8_t*>(
            heap_caps_malloc(size_t(samples) * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!allocated)
            return "";
        if (!task_ && xTaskCreate([](void* arg) { static_cast<AudioPreview*>(arg)->Run(); },
                                  "audio_preview", 6144, this, 2, &task_) != pdPASS) {
            heap_caps_free(allocated);
            return "";
        }
        data_ = allocated;
        char token[17];
        snprintf(token, sizeof(token), "%08lx%08lx", (unsigned long)esp_random(),
                 (unsigned long)esp_random());
        id = token;
        session_.Begin(id, samples, Now());
        ready_ = false;
    }
    // Close the idle voice channel before granting ownership of the audio queue.
    // This also drains its receive task, so a late TTS packet cannot mix in.
    Application::GetInstance().SuspendVoiceSession(
        [this, id]() {
            std::lock_guard<std::mutex> guard(mutex_);
            if (session_.id == id && session_.Busy())
                ready_ = true;
        },
        [this, id]() {
            std::lock_guard<std::mutex> guard(mutex_);
            return session_.id == id && data_ && session_.stage != dfpreview::Stage::Stopping;
        });
    xTaskNotifyGive(task_);
    return id;
}
bool AudioPreview::Chunk(const std::string& id, size_t offset, const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!data || !data_ || !session_.Chunk(id, offset, size, Now()))
        return false;
    memcpy(data_ + offset, data, size);
    return true;
}
bool AudioPreview::Commit(const std::string& id) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!session_.Commit(id))
        return false;
    xTaskNotifyGive(task_);
    return true;
}
bool AudioPreview::Stop(const std::string& id) {
    std::lock_guard<std::mutex> guard(mutex_);
    bool okay = session_.Stop(id);
    if (task_)
        xTaskNotifyGive(task_);
    return okay;
}
void AudioPreview::AppendStatus(cJSON* output) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto json = cJSON_AddObjectToObject(output, "audio_preview");
    cJSON_AddStringToObject(json, "id", session_.id.c_str());
    cJSON_AddStringToObject(json, "state", dfpreview::Name(session_.stage));
    cJSON_AddStringToObject(json, "error", session_.error.c_str());
    cJSON_AddBoolToObject(json, "busy", session_.Busy() || data_ != nullptr);
    cJSON_AddNumberToObject(json, "received", session_.received);
    cJSON_AddNumberToObject(json, "total", session_.total);
    cJSON_AddNumberToObject(json, "duration_ms", session_.total * 1000 / (dfpreview::kRate * 2));
}
void AudioPreview::Run() {
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool owns_output = false;
        size_t offset = 0;
        int64_t started = 0;
        std::vector<int16_t> pending;
        while (true) {
            dfpreview::Stage stage;
            bool expired;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                stage = session_.stage;
                expired = session_.Expired(Now());
                if (expired) {
                    session_.stage = dfpreview::Stage::Failed;
                    session_.error = "upload_timeout";
                    stage = session_.stage;
                }
                if (stage == dfpreview::Stage::Queued && !ready_ &&
                    Now() - session_.touched > 15000) {
                    session_.stage = dfpreview::Stage::Failed;
                    session_.error = "audio_prepare_timeout";
                    stage = session_.stage;
                }
            }
            if (stage == dfpreview::Stage::Uploading ||
                (stage == dfpreview::Stage::Queued && !ready_)) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
                continue;
            }
            if (stage == dfpreview::Stage::Queued) {
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (session_.stage != dfpreview::Stage::Queued)
                        continue;
                    session_.stage = dfpreview::Stage::Playing;
                    stage = session_.stage;
                    owns_output = true;
                }
                Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
                started = Now();
            }
            auto state = app.GetDeviceState();
            const bool valid = DeepFlipperBridge::Get().Linked() &&
                               !DeepFlipperBridge::Get().Muted() &&
                               Board::GetInstance().GetAudioCodec()->output_volume() > 0 &&
                               (state == kDeviceStateIdle || state == kDeviceStateStarting);
            if (stage == dfpreview::Stage::Playing && !valid) {
                std::lock_guard<std::mutex> guard(mutex_);
                session_.stage = dfpreview::Stage::Failed;
                session_.error = "playback_interrupted";
                stage = session_.stage;
            }
            if (stage == dfpreview::Stage::Playing) {
                size_t total;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    total = session_.total;
                }
                if (Now() - started > int64_t(total * 1000 / (dfpreview::kRate * 2)) + 5000) {
                    std::lock_guard<std::mutex> guard(mutex_);
                    session_.stage = dfpreview::Stage::Failed;
                    session_.error = "playback_timeout";
                    continue;
                }
                if (offset < total) {
                    const size_t bytes = std::min<size_t>(2880, total - offset);
                    if (pending.empty()) {
                        pending.resize(bytes / 2);
                        memcpy(pending.data(), data_ + offset, bytes);
                    }
                    if (audio.PushPcmToPlaybackQueue(pending))
                        offset += bytes;
                    else {
                        vTaskDelay(pdMS_TO_TICKS(5));
                        continue;
                    }
                } else if (audio.IsPlaybackIdle()) {
                    // I2S can retain a short DMA tail after the software queue drains.
                    vTaskDelay(pdMS_TO_TICKS(70));
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (session_.stage == dfpreview::Stage::Playing)
                        session_.stage = dfpreview::Stage::Completed;
                    stage = session_.stage;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }
                if (stage == dfpreview::Stage::Playing)
                    continue;
            }
            if (owns_output && stage != dfpreview::Stage::Completed)
                audio.ResetDecoder();
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (session_.stage == dfpreview::Stage::Stopping)
                    session_.stage = dfpreview::Stage::Cancelled;
                if (data_) {
                    heap_caps_free(data_);
                    data_ = nullptr;
                }
            }
            app.Schedule([this]() {
                if (!Busy() && Application::GetInstance().GetDeviceState() == kDeviceStateIdle)
                    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            });
            break;
        }
    }
}
