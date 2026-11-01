#include <cassert>
#include <cstdint>
#include <iostream>
#define MAIN_EVENT_SPEECH_STOP_READY 32768
#define ESP_ERROR_CHECK(expr) assert((expr)==0)
enum State {kDeviceStateIdle,kDeviceStateSpeaking,kDeviceStateListening};
enum Mode {kListeningModeManualStop,kListeningModeAutoStop};
static int timer_starts,timer_stops,cleared;
static uint64_t timer_delay;
static int64_t now_us;
static int64_t esp_timer_get_time(){return now_us;}
static int esp_timer_stop(void*){timer_stops++;return 0;}
static int esp_timer_start_once(void*,uint64_t delay){timer_starts++;timer_delay=delay;return 0;}
static void xEventGroupClearBits(void*,int flag){assert(flag==MAIN_EVENT_SPEECH_STOP_READY);cleared++;}
class Application {
public:
    struct Audio {bool idle=false;bool IsPlaybackIdle(){return idle;}uint32_t OutputDrainMs(){return 80;}} audio_service_;
    bool pending_speech_stop_=false,speech_stop_timer_armed_=false;
    int64_t speech_stop_deadline_us_=0;
    void* speech_stop_timer_=nullptr;void* event_group_=nullptr;
    State state=kDeviceStateSpeaking;Mode listening_mode_=kListeningModeManualStop;
    State GetDeviceState(){return state;}
    void SetDeviceState(State next){state=next;}
    void CancelSpeechStop();void RequestSpeechStop();void CompleteSpeechStop();
};
#include "speech-stop-production.inc"
int main(){
    Application app;app.RequestSpeechStop();
    assert(app.pending_speech_stop_&&timer_starts==0&&app.state==kDeviceStateSpeaking);
    app.audio_service_.idle=true;app.RequestSpeechStop();
    assert(timer_starts==1&&timer_delay==80000&&app.state==kDeviceStateSpeaking);
    app.RequestSpeechStop();assert(timer_starts==1);
    app.CompleteSpeechStop();assert(app.state==kDeviceStateSpeaking&&app.speech_stop_timer_armed_);
    now_us+=80000;app.CompleteSpeechStop();assert(app.state==kDeviceStateIdle&&!app.pending_speech_stop_);
    app.state=kDeviceStateSpeaking;app.listening_mode_=kListeningModeAutoStop;
    app.RequestSpeechStop();now_us+=80000;app.CompleteSpeechStop();assert(app.state==kDeviceStateListening);
    app.state=kDeviceStateSpeaking;app.RequestSpeechStop();
    app.CancelSpeechStop();app.CompleteSpeechStop();assert(app.state==kDeviceStateSpeaking);
    assert(timer_stops>0&&cleared>0&&!app.pending_speech_stop_);
    app.state=kDeviceStateIdle;app.RequestSpeechStop();assert(!app.pending_speech_stop_);
    std::cout<<"PASS: actual speech stop waits for PCM drain and I2S tail before idle/listening; abort cancels stale completion\n";
}
