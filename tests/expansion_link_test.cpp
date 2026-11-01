#include "firmware/esp32/main/boards/deepflipper-s3/expansion_link.h"
#include <cassert>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstring>
extern "C" int expansion_reference_roundtrip(const uint8_t*, size_t, uint8_t*, size_t*, uint8_t*, uint32_t*, uint8_t*);


using dfexpansion::ConnectResult;

struct Peer {
    bool present = true, fast_supported = true, start_ok = true, hello_ok = true;
    std::vector<uint32_t> attempts;
    ConnectResult operator()(uint32_t baud) {
        attempts.push_back(baud);
        if (!present) return ConnectResult::NoPeer;
        if ((baud == dfexpansion::kPreferredBaud && !fast_supported) || !start_ok || !hello_ok)
            return ConnectResult::Failed;
        return ConnectResult::Ready;
    }
};

int main() {
    assert(dfexpansion::kPresenceByte == 0xAA);
    for (uint8_t type : {uint8_t(1), uint8_t(2), uint8_t(3), uint8_t(4), uint8_t(5)}) {
        std::vector<uint8_t> payload;
        if (type == 2 || type == 4) payload = {0};
        if (type == 3) { auto bytes=dfexpansion::BaudPayload(460800); payload.assign(bytes.begin(),bytes.end()); }
        if (type == 5) payload = std::vector<uint8_t>(64, 0x5a);
        auto input=dfexpansion::EncodeFrame(type,payload.data(),payload.size());
        uint8_t output[128], decoded=0, data_size=0;
        uint32_t baud=0; size_t output_size=sizeof(output);
        assert(expansion_reference_roundtrip(input.data(),input.size(),output,&output_size,&decoded,&baud,&data_size)==0);
        assert(decoded==type);
        if(type==3) assert(baud==460800);
        if(type==5) assert(data_size==64);
        assert(std::vector<uint8_t>(output,output+output_size)==input);
        input.back()^=1;output_size=sizeof(output);
        assert(expansion_reference_roundtrip(input.data(),input.size(),output,&output_size,&decoded,&baud,&data_size)==2);
    }
    assert((dfexpansion::BaudPayload(460800) == std::array<uint8_t, 4>{0x00, 0x08, 0x07, 0x00}));
    assert((dfexpansion::BaudPayload(115200) == std::array<uint8_t, 4>{0x00, 0xc2, 0x01, 0x00}));
    {
        dfexpansion::LinkRate rate;
        Peer peer; peer.present = false;
        for (int i = 0; i < 5; ++i) assert(!rate.Connect(peer));
        assert(!rate.Fallback() && !rate.Active());
        peer.present = true;
        assert(rate.Connect(peer) && rate.Active() && !rate.Fallback());
        assert(peer.attempts.back() == 460800);
    }
    for (int failure = 0; failure < 3; ++failure) {
        dfexpansion::LinkRate rate;
        Peer peer;
        peer.fast_supported = failure != 0;
        peer.start_ok = failure != 1;
        peer.hello_ok = failure != 2;
        assert(!rate.Connect(peer) && rate.Fallback() && !rate.Active());
        // No immediate loop/retry: production's UART task must apply its backoff.
        assert(peer.attempts == std::vector<uint32_t>{460800});
        peer.start_ok = peer.hello_ok = true;
        assert(rate.Connect(peer) && rate.Active());
        assert(peer.attempts == (std::vector<uint32_t>{460800, 115200}));
    }
    {
        dfexpansion::LinkRate rate;
        Peer peer;
        assert(rate.Connect(peer));
        // Connect is invoked again only after transport loss. Even if the peer
        // disappears at that point, latch low speed for the next discovery.
        peer.present = false;
        assert(!rate.Connect(peer) && rate.Fallback());
        peer.present = true;
        assert(rate.Connect(peer) && peer.attempts.back() == 115200);
        peer.hello_ok = false;
        assert(!rate.Connect(peer));
        peer.hello_ok = true;
        assert(rate.Connect(peer) && peer.attempts.back() == 115200);
        // A fresh boot is the only automatic return to the preferred speed.
        dfexpansion::LinkRate rebooted;
        assert(rebooted.Connect(peer) && peer.attempts.back() == 460800);
    }
    std::cout << "PASS: Expansion 460800 negotiation bytes, discovery, fallback and reconnect policy\n";
    std::cout << "PASS: Expansion frame interoperability against Flipper upstream encoder/decoder\n";
}
