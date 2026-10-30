#include "firmware/esp32/main/boards/deepflipper-s3/rpc_wire.h"
#include "firmware/esp32/main/boards/deepflipper-s3/voice_endpoint.h"
#include "firmware/esp32/main/boards/deepflipper-s3/native_policy.h"
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
    // Native has_next chunks and command-id-zero app events share the stream
    // with companion envelopes. They must be decoded independently.
    std::vector<uint8_t> payload;
    dfwire::UInt(payload,1,1);
    auto native=dfwire::NativeRequest(9,8,payload);
    auto event=dfwire::NativeRequest(0,58,payload);
    auto custom=dfwire::NativeRequest(7,501,std::vector<uint8_t>{10,2,'{','}'});
    std::vector<uint8_t> combined=custom;
    combined.insert(combined.end(),event.begin(),event.end());
    combined.insert(combined.end(),native.begin(),native.end());
    dfwire::Message message;
    assert(dfwire::Next(combined,message)==dfwire::NextResult::Ready && message.id==7 && message.tag==501);
    assert(dfwire::Next(combined,message)==dfwire::NextResult::Ready && message.id==0 && message.tag==58);
    assert(dfwire::Next(combined,message)==dfwire::NextResult::Ready && message.id==9 && message.tag==8);
    assert(combined.empty());
    // A legitimate native directory page can exceed 2 KiB (8 long names).
    std::vector<uint8_t> directory;
    for (int i=0;i<8;++i){
        std::vector<uint8_t> file;
        dfwire::String(file,2,std::string(255,'a'));
        dfwire::UInt(file,3,100);
        dfwire::Bytes(directory,1,file);
    }
    auto page=dfwire::NativeRequest(10,8,directory);
    assert(page.size()>2048 && dfwire::Next(page,message)==dfwire::NextResult::Ready && message.tag==8);
    assert(page.empty());
    std::vector<uint8_t> partial;
    for(size_t i=0;i<native.size();++i){
        partial.push_back(native[i]);
        auto next=dfwire::Next(partial,message);
        assert(next==(i+1==native.size()?dfwire::NextResult::Ready:dfwire::NextResult::NeedMore));
    }
    std::vector<uint8_t> multi{8,9,24,1,66,0};
    assert(dfwire::Decode(multi,message) && message.more && message.id==9 && message.tag==8);
    assert(!dfwire::Decode({0},message));
    assert(!dfwire::Decode({8,1,66,4,1},message));
    std::vector<uint8_t> huge{255,255,255,255,15};
    assert(dfwire::Next(huge,message)==dfwire::NextResult::Invalid);
    std::vector<uint8_t> malformed_length{128,128,128,128,128};
    assert(dfwire::Next(malformed_length,message)==dfwire::NextResult::Invalid);
    auto keys=dfwire::ShortInput(20,5);
    assert(keys.size()<=64);
    for(uint32_t type:{0U,2U,1U}){
        assert(dfwire::Next(keys,message)==dfwire::NextResult::Ready && message.tag==23 && message.id==20+(type==0?0:type==2?1:2));
        dfwire::Reader reader(message.payload);dfwire::Field field;
        assert(reader.Next(field) && field.tag==1 && field.number==5);
        assert(reader.Next(field) && field.tag==2 && field.number==type && reader.Done());
    }
    assert(keys.empty() && dfwire::ShortInput(1,6).empty());
    assert(dfnative::Path("/ext/infrared/TV Remote.ir"));
    for(const auto* path:{"/int/key","/ext2/file","/ext/../secret","/ext/a/./b","/ext/a\\b"})assert(!dfnative::Path(path));
    assert(dfnative::Saved("infrared","/ext/infrared/TV.ir"));
    assert(!dfnative::Saved("infrared","/ext/nfc/TV.ir"));
    assert(!dfnative::Saved("nfc","/ext/nfc/card.sub"));
    assert(dfnative::Pin("PB6")==-1 && dfnative::Pin("PB7")==-1 && dfnative::Pin("5V")==-1);
    assert(dfnative::App("NFC") && dfnative::App("/ext/apps/Tools/test.fap"));
    assert(!dfnative::App("/ext/apps/../bad.fap") && !dfnative::App("arbitrary shell"));
    for (const auto* url : {"ws://192.168.1.210:8000/xiaozhi/v1/",
                            "wss://voice.example.com/xiaozhi/v1/", "ws://deepflipper.local:8000/",
                            "wss://voice.example.com"}) {
        assert(deepflipper::ValidVoiceEndpoint(url));
    }
    for (const auto* url : {"ws://localhost:8000/", "ws://LOCALHOST./", "ws://127.0.0.1/", "ws://0.0.0.0/", "https://voice.example.com/", "ws://", "wss:///xiaozhi/v1/",
                            "ws://:8000/", "ws://host:0/", "ws://host:65536/",
                            "ws://host:abc/", "ws://host:/", "ws://user:pass@host/",
                            "ws://host/?token=secret", "ws://host/#fragment", "ws://host\r\nx",
                            "ws://host\\other/", "ws://[]:8000/", "ws://[fd00::1]:8000/xiaozhi/v1/", "ws://[::1]suffix/"}) {
        assert(!deepflipper::ValidVoiceEndpoint(url));
    }
    assert(!deepflipper::ValidVoiceEndpoint(std::string("ws://host/") + std::string(193, 'x')));
    assert(!deepflipper::ValidVoiceEndpoint(std::string("ws://host/\0hidden", 17)));
    std::cout << "PASS: C++ RPC codec golden bytes, bounds, truncation and status errors\n";
    std::cout << "PASS: WS/WSS voice endpoints, LAN/DNS and invalid URL rejection\n";
    std::cout << "PASS: native RPC streaming, async events, batched keys and SD/action path boundaries\n";
}
