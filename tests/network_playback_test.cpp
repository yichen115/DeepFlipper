#include <cassert>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <iostream>
#include "firmware/esp32/main/audio/network_playback.h"

struct AudioStreamPacket { int sample_rate=24000,frame_duration=60;std::vector<uint8_t> payload;uint32_t network_turn=0; };
static uint64_t fake_time_us;
static uint64_t esp_timer_get_time(){return fake_time_us;}
static void esp_opus_dec_reset(void*){}
#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240
struct Codec {int output_sample_rate() const{return 24000;}};
class AudioService {
public:
    std::mutex audio_queue_mutex_,decoder_mutex_;
    std::condition_variable audio_queue_cv_;
    NetworkPlayback network_playback_;
    uint32_t playback_generation_=0;
    void* opus_decoder_=nullptr;
    Codec* codec_=nullptr;
    std::vector<int> timestamp_queue_,audio_decode_queue_,audio_playback_queue_,audio_testing_queue_;
    struct {std::function<void()> on_playback_drained;} callbacks_;
    bool MarkPlaybackDrainedLocked(){return false;}
    void ResetDecoder(bool preserve_network=false);
    void BeginNetworkPlayback();void EndNetworkPlayback();bool NetworkPlaybackActive();
    void RecordNetworkPacket(AudioStreamPacket&,bool);NetworkPlaybackStatistics PlaybackStatistics();
    uint32_t OutputDrainMs() const;
};
#include "network-playback-production.inc"
int main(){
    NetworkPlayback gate;
    assert(gate.Ready(0,1,0)); // Direct PCM has no network gate.
    gate.Begin();auto turn=gate.Packet(5000,100,24000,60,true);
    assert(!gate.Ready(turn,1,5000)&&!gate.Ready(turn,7,5599));
    assert(gate.Ready(turn,1,5600)); // Short segments have bounded waits.
    assert(gate.Ready(turn,8,5000)&&gate.Ready(0,1,5000));
    gate.End();assert(gate.Ready(turn,1,5000));
    gate.Begin();turn=gate.Packet(0,100,24000,60,true);gate.Packet(180,120,24000,60,false);
    gate.QueueDrop(turn);gate.Decoded(turn,false,false,80);gate.Decoded(turn,false,true,90);
    gate.OutputStart(turn,200);gate.OutputDone(turn,260,1440);gate.OutputStart(turn,380);gate.OutputDone(turn,440,1440);
    auto stats=gate.Snapshot();
    assert(stats.packets==2&&stats.state_drops==1&&stats.queue_drops==1);
    assert(stats.decode_errors==2&&stats.resample_errors==1&&stats.decode_max_ms==90);
    assert(stats.rx_gap_max_ms==180&&stats.output_gap_max_ms==120&&stats.played_samples==2880);
    gate.Reset();assert(gate.Ready(turn,1,1000)&&!gate.Active());
    // Reproduce the reported 397 ms delivery gap after the queued audio drains.
    gate.Begin();turn=gate.Packet(0,100,24000,60,true);
    gate.OutputStart(turn,0);gate.OutputDone(turn,60,1440);gate.QueueEmpty(turn,60);
    gate.Packet(457,100,24000,60,true);
    assert(gate.Snapshot().starvation_events==1&&gate.Snapshot().starvation_max_ms==337);
    assert(!gate.Ready(turn,1,457)&&!gate.Ready(turn,7,1056));
    assert(gate.Ready(turn,8,457)&&gate.Ready(turn,1,1057));
    gate.End();assert(gate.Ready(turn,1,458)); // Final short tail flushes.
    // The user's 265 network packets are 15.9 s. A later 1.08 s prompt must
    // not turn this into 283 packets / 17 s or invent a 15.74 s output gap.
    gate.Begin();turn=gate.CurrentTurn();
    for(int i=0;i<265;i++){
        gate.Packet(i*60,100,24000,60,true);gate.OutputStart(turn,i*60);gate.OutputDone(turn,(i+1)*60,1440);
    }
    gate.QueueEmpty(turn,15900);gate.End();
    for(int i=0;i<18;i++){gate.OutputStart(0,31640+i*60);gate.OutputDone(0,31700+i*60,1440);}
    stats=gate.Snapshot();assert(stats.played==265&&stats.played_samples==381600);
    assert(stats.output_gap_max_ms==0&&stats.starvation_events==0);
    assert(gate.Packet(40000,100,24000,60,true)==0);
    gate.Begin();gate.OutputDone(turn,50000,1440);gate.Decoded(turn,true,false,200);gate.QueueDrop(turn);
    assert(gate.Snapshot().played==0&&gate.Snapshot().decoded==0&&gate.Snapshot().queue_drops==0);
    AudioService service;
    Codec codec;service.codec_=&codec;assert(service.OutputDrainMs()==80);
    service.audio_decode_queue_.push_back(99); // Previous turn residue.
    service.BeginNetworkPlayback();assert(service.audio_decode_queue_.empty());
    const auto generation=service.playback_generation_;
    assert(service.NetworkPlaybackActive()); // Before the main task publishes Speaking.
    AudioStreamPacket packet;packet.payload={1,2,3};service.RecordNetworkPacket(packet,true);
    assert(packet.network_turn==service.PlaybackStatistics().turn);
    service.audio_decode_queue_={0,1,2,3};
    service.ResetDecoder(true); // Delayed Speaking state callback must preserve the startup burst.
    assert(service.playback_generation_==generation&&service.audio_decode_queue_.size()==4);
    service.EndNetworkPlayback();assert(!service.NetworkPlaybackActive());
    service.ResetDecoder(true);assert(service.audio_decode_queue_.size()==4); // Stop can precede the delayed state callback.
    service.ResetDecoder();assert(service.audio_decode_queue_.empty());
    assert(service.PlaybackStatistics().packets==1); // Last-turn diagnostics remain readable.
    service.BeginNetworkPlayback();assert(service.PlaybackStatistics().packets==0);
    std::cout<<"PASS: actual network playback startup ordering, bounded jitter/refill and per-turn diagnostics without local prompt contamination\n";
}
