#include "firmware/esp32/main/boards/deepflipper-s3/rpc_wire.h"
#include "firmware/esp32/main/boards/deepflipper-s3/voice_endpoint.h"
#include <cassert>
#include <iostream>

int main() {
    const std::vector<uint8_t> golden{9,8,7,0xa2,0x1f,4,10,2,'{','}'};
    assert(dfwire::Request(7,"{}")==golden);
    assert(!dfwire::Request(1,std::string(767,'x')).empty());
    assert(dfwire::Request(1,std::string(768,'x')).empty());
    uint32_t value=0;
    size_t pos=0;
    std::vector<uint8_t> bad{255,255,255,255,31};
    assert(!dfwire::ReadVarint(bad,pos,value));
    std::vector<uint8_t> encoded;
    dfwire::Varint(encoded,0xffffffff);
    pos=0;
    assert(dfwire::ReadVarint(encoded,pos,value) && value==0xffffffff);
    std::string json;
    uint32_t id=0;
    const std::vector<uint8_t> response{8,7,0xaa,0x1f,4,10,2,'{','}'};
    assert(dfwire::Response(response,id,json) && id==7 && json=="{}");
    auto truncated=response;
    truncated.pop_back();
    assert(!dfwire::Response(truncated,id,json));
    auto rejected=response;
    rejected.insert(rejected.end(),{16,4});
    assert(!dfwire::Response(rejected,id,json));
    auto excessive=response;
    excessive[4]=127;
    assert(!dfwire::Response(excessive,id,json));
    for (const auto* url : {"ws://192.168.1.210:8000/xiaozhi/v1/",
                            "wss://voice.example.com/xiaozhi/v1/", "ws://deepflipper.local:8000/",
                            "ws://[fd00::1]:8000/xiaozhi/v1/", "wss://voice.example.com"}) {
        assert(deepflipper::ValidVoiceEndpoint(url));
    }
    for (const auto* url : {"https://voice.example.com/", "ws://", "wss:///xiaozhi/v1/",
                            "ws://:8000/", "ws://host:0/", "ws://host:65536/",
                            "ws://host:abc/", "ws://host:/", "ws://user:pass@host/",
                            "ws://host/?token=secret", "ws://host/#fragment", "ws://host\r\nx",
                            "ws://host\\other/", "ws://[]:8000/", "ws://[::1]suffix/"}) {
        assert(!deepflipper::ValidVoiceEndpoint(url));
    }
    assert(!deepflipper::ValidVoiceEndpoint(std::string("ws://host/") + std::string(193, 'x')));
    assert(!deepflipper::ValidVoiceEndpoint(std::string("ws://host/\0hidden", 17)));
    std::cout << "PASS: C++ RPC codec golden bytes, bounds, truncation and status errors\n";
    std::cout << "PASS: WS/WSS voice endpoints, LAN/DNS/IPv6 and invalid URL rejection\n";
}
