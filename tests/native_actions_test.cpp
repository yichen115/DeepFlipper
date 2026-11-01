#include "firmware/esp32/main/boards/deepflipper-s3/native_actions.h"
#include <cassert>
#include <algorithm>
#include <iostream>
extern "C" int desktop_reference_lock_status(int locked);

struct Fake : dfnative::Transport {
    bool alive=true,busy=false,app=false,tx=false,locked=false,fail_load=false,fail_stop=false,reset=false;
    int64_t now=0,cancel_at=-1,lock_at=-1,ready_at=-1,start_delay=0;
    int info_frames=0;
    int key=-1;
    std::string file_data="hello";
    std::string name;
    std::vector<uint32_t> calls;
    bool NativeCancelled() override{return cancel_at>=0 && now>=cancel_at;}
    bool Alive() const override{return alive;}
    int64_t NowMs() const override{return now;}
    void Pump() override{now+=100;if(ready_at>=0&&now>=ready_at)app=true;}
    void Reset() override{reset=true;alive=false;tx=false;app=false;busy=false;}
    bool AppRunning() const override{return app;}
    std::string AppName() const override{return name;}
    void SetAppName(const std::string& value) override{name=value;}
    dfnative::Reply Native(uint32_t tag,const std::vector<uint8_t>& = {},dfnative::Sink sink={},bool cleanup=false,int short_key=-1) override{
        calls.push_back(tag);now+=5;
        dfnative::Reply r;r.done=true;r.message.tag=4;
        if(!cleanup && NativeCancelled()){r.error="cancelled";return r;}
        if(tag==66){r.message.tag=4;r.status=desktop_reference_lock_status(locked||(lock_at>=0&&now>=lock_at));}
        if(tag==17){r.message.tag=18;dfwire::UInt(r.message.payload,1,busy);}
        if(tag==16){busy=true;app=start_delay==0;ready_at=now+start_delay;}
        if(tag==48&&fail_load)r.status=22;
        if(tag==49)tx=true;
        if(tag==50)tx=false;
        if(tag==47){if(fail_stop)r.status=22;else{busy=false;app=false;tx=false;}}
        if(tag==23)key=short_key;
        if(tag==63){r.message.tag=64;dfwire::UInt(r.message.payload,1,1);dfwire::String(r.message.payload,2,"Cannot parse file");}
        if(tag==28){r.message.tag=29;dfwire::UInt(r.message.payload,1,1000000);dfwire::UInt(r.message.payload,2,500000);}
        if(tag==9){r.message.tag=10;std::vector<uint8_t> f;dfwire::String(f,4,file_data);dfwire::Bytes(r.message.payload,1,f);if(sink)assert(sink(r.message));}
        if(tag==53){r.message.tag=54;dfwire::UInt(r.message.payload,1,1);}
        if(tag==55){r.message.tag=56;dfwire::UInt(r.message.payload,2,1);}
        if(tag==35){r.message.tag=36;std::vector<uint8_t>d;dfwire::UInt(d,6,2026);dfwire::Bytes(r.message.payload,1,d);}
        if(tag==32||tag==44){
            for(int i=0;i<120;++i){
                dfwire::Message m;m.tag=tag==32?33:45;m.more=i!=119;
                dfwire::String(m.payload,1,"key"+std::to_string(i));
                dfwire::String(m.payload,2,std::string(80,'x'));
                assert(!sink||sink(m));++info_frames;r.message=m;
            }
        }
        if(tag==7){
            for(int page=0;page<2;++page){
                dfwire::Message m;m.tag=8;m.more=page==0;
                for(int i=page*3;i<page*3+3;++i){
                    std::vector<uint8_t> file;
                    dfwire::String(file,2,"remote"+std::to_string(i)+".ir");dfwire::UInt(file,3,100+i);
                    dfwire::Bytes(m.payload,1,file);
                }
                assert(!sink||sink(m));r.message=m;
            }
        }
        return r;
    }
};

struct Result{
    cJSON* json;
    explicit Result(const std::string& text):json(cJSON_Parse(text.c_str())){assert(json);}
    ~Result(){cJSON_Delete(json);}
    bool ok()const{return cJSON_IsTrue(cJSON_GetObjectItem(json,"ok"));}
    bool flag(const char* key)const{return cJSON_IsTrue(cJSON_GetObjectItem(json,key));}
    std::string text(const char* key)const{auto p=cJSON_GetObjectItem(json,key);return cJSON_IsString(p)?p->valuestring:"";}
};

int main(){
    for(const auto* kind:{"infrared","subghz","nfc","rfid","ibutton"}){
        Fake f;const dfnative::SavedKind* spec=nullptr;
        for(const auto& item:dfnative::kSaved)if(std::string(kind)==item.kind)spec=&item;
        assert(spec);
        auto args=std::string("{\"kind\":\"")+kind+"\",\"path\":\""+spec->folder+"/test"+spec->extension+"\",\"duration_ms\":300}";
        dfnative::Actions actions(f,30000);Result r(actions.Execute("native.saved",args));
        assert(r.ok()&&r.flag("stopped")&&!r.flag("target_state_verified"));
        assert(!f.busy&&!f.tx&&!f.app&&!f.reset);
        assert(std::find(f.calls.begin(),f.calls.end(),49)!=f.calls.end() ? spec->transmit : !spec->transmit);
        assert(f.calls.back()==17);
    }
    {
        Fake f;f.busy=true;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.saved","{\"kind\":\"nfc\",\"path\":\"/ext/nfc/a.nfc\"}"));
        assert(!r.ok()&&r.text("code")=="busy"&&f.busy);
        assert(std::find(f.calls.begin(),f.calls.end(),16)==f.calls.end());
    }
    {
        Fake f;f.locked=true;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.app.open","{\"name\":\"NFC\"}"));
        assert(!r.ok()&&r.text("code")=="locked"&&f.calls.size()==1);
    }
    for(bool cancel:{false,true}){
        Fake f;if(cancel)f.cancel_at=200;else f.lock_at=200;
        dfnative::Actions a(f,30000);
        Result r(a.Execute("native.saved","{\"kind\":\"infrared\",\"path\":\"/ext/infrared/a.ir\",\"duration_ms\":5000}"));
        assert(!r.ok()&&r.flag("stopped")&&!f.tx&&!f.busy);
        assert(r.text("code")== (cancel?"cancelled":"locked"));
    }
    {
        Fake f;f.fail_load=true;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.saved","{\"kind\":\"nfc\",\"path\":\"/ext/nfc/a.nfc\"}"));
        assert(!r.ok()&&r.flag("stopped")&&!f.busy&&r.text("stage")=="load_saved_file");
        assert(r.text("message")=="Cannot parse file");
    }
    {
        Fake f;f.fail_stop=true;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.saved","{\"kind\":\"subghz\",\"path\":\"/ext/subghz/a.sub\",\"duration_ms\":100}"));
        assert(!r.ok()&&!r.flag("stopped")&&f.reset);
        assert(r.text("code")=="stop_unconfirmed");
    }
    {
        Fake f;f.start_delay=500;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.saved","{\"kind\":\"nfc\",\"path\":\"/ext/nfc/a.nfc\",\"duration_ms\":100}"));
        assert(r.ok()&&r.flag("stopped")&&f.now>=600);
    }
    {
        Fake f;f.start_delay=10000;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.saved","{\"kind\":\"nfc\",\"path\":\"/ext/nfc/a.nfc\"}"));
        assert(!r.ok()&&f.reset&&r.text("code")=="app_start_unconfirmed");
        assert(std::find(f.calls.begin(),f.calls.end(),48)==f.calls.end());
    }
    {
        Fake f;dfnative::Actions a(f,200);
        Result r(a.Execute("native.saved","{\"kind\":\"infrared\",\"path\":\"/ext/infrared/a.ir\",\"duration_ms\":5000}"));
        assert(!r.ok()&&r.flag("stopped")&&r.text("code")=="timeout"&&!f.tx&&!f.busy);
    }
    {
        Fake f;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.system.info","{}"));
        assert(r.ok()&&r.flag("truncated")&&f.info_frames==240);
        assert(cJSON_GetArraySize(cJSON_GetObjectItem(r.json,"info"))>0);
        assert(std::find(f.calls.begin(),f.calls.end(),66)==f.calls.end());
    }
    for(bool pin_locked:{false,true}){
        Fake f;f.locked=pin_locked;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.app.state","{}"));
        assert(r.ok()&&r.flag("locked")==pin_locked);
        Result info(a.Execute("native.system.info","{}"));
        assert(info.ok());
    }
    {
        Fake f;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.files","{\"path\":\"/ext/infrared\",\"offset\":2,\"limit\":2}"));
        assert(r.ok());auto items=cJSON_GetObjectItem(r.json,"items");assert(cJSON_GetArraySize(items)==2);
        assert(std::string(cJSON_GetObjectItem(cJSON_GetArrayItem(items,0),"name")->valuestring)=="remote2.ir");
        assert(cJSON_GetObjectItem(r.json,"next_offset")->valueint==4&&cJSON_GetObjectItem(r.json,"total")->valueint==6);
    }
    {
        Fake f;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.input","{\"key\":\"back\"}"));
        assert(r.ok()&&f.key==5&&r.text("result")=="input_delivered_not_semantic_action_verified");
    }
    std::cout<<"PASS: native action workflows, five saved kinds, startup/timeout, busy/lock/cancel, cleanup failure, bounded streaming and paged files\n";
    std::cout<<"PASS: desktop IsLocked semantics against actual Flipper handler and read-only system info\n";
    {
        Fake f;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.file.read","{\"path\":\"/ext/test.txt\"}"));assert(r.ok()&&r.text("hex")=="68656C6C6F");
        Result invalid(a.Execute("native.file.read","{\"path\":\"/ext/../int/x\"}"));assert(!invalid.ok());
        Result gpio(a.Execute("native.gpio","{\"pin\":\"PA7\",\"action\":\"read\"}"));assert(gpio.ok());
        Result uart(a.Execute("native.gpio","{\"pin\":\"PB6\",\"action\":\"write\",\"value\":1}"));assert(!uart.ok());
        Result sd(a.Execute("native.storage.info","{\"path\":\"/ext\"}"));assert(sd.ok());
        Result clock(a.Execute("native.clock","{\"action\":\"get\"}"));assert(clock.ok());
        Result key(a.Execute("native.input","{\"key\":\"ok\",\"press_type\":\"long\"}"));assert(key.ok()&&f.key==10);
    }
    {
        Fake f;f.file_data="{\"ok\":true,\"request_id\":\"123_1\",\"path\":\"/ext/infrared/captured.ir\"}";
        dfnative::Actions a(f,30000);Result r(a.Execute("native.capture","{\"kind\":\"ir\",\"request_id\":\"123_1\",\"timeout_ms\":500}"));
        assert(r.ok()&&r.flag("stopped")&&!f.app&&!f.busy);
    }
    std::cout<<"PASS: capture adapter lifecycle, bounded SD reads, clock, GPIO and long input\n";
}
