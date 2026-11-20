#include "firmware/esp32/main/boards/deepflipper-s3/native_actions.h"
#include <cassert>
#include <algorithm>
#include <iostream>
#include <map>
extern "C" int desktop_reference_lock_status(int locked);

struct Fake : dfnative::Transport {
    bool alive=true,busy=false,app=false,tx=false,locked=false,fail_load=false,fail_stop=false,reset=false;
    int64_t now=0,cancel_at=-1,lock_at=-1,ready_at=-1,start_delay=0,mailbox_at=0;
    int info_frames=0;
    int key=-1;
    bool old_mailbox=false, bad_command=false,lose_on_remove=false;
    std::string command;
    std::string file_data="hello";
    std::string name;
    std::string retained;
    int64_t retained_deadline = 0;
    std::vector<uint32_t> calls;
    bool NativeCancelled() override{return cancel_at>=0 && now>=cancel_at;}
    bool Alive() const override{return alive;}
    int64_t NowMs() const override{return now;}
    void Pump() override{now+=100;if(ready_at>=0&&now>=ready_at)app=true;}
    void Reset() override{reset=true;alive=false;tx=false;app=false;busy=false;retained.clear();}
    bool AppRunning() const override{return app;}
    std::string AppName() const override{return name;}
    void SetAppName(const std::string& value) override{name=value;}
    std::string RetainedApp() const override{return retained;}
    int64_t RetainedDeadline() const override{return retained_deadline;}
    void RetainApp(const std::string& value,int64_t deadline) override{retained=value;retained_deadline=deadline;}
    dfnative::Reply Native(uint32_t tag,const std::vector<uint8_t>& payload = {},dfnative::Sink sink={},bool cleanup=false,int short_key=-1) override{
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
        if(tag==65){
            dfwire::Reader reader(payload);dfwire::Field field;
            while(reader.Next(field))if(field.tag==1)command=field.text();
            if(bad_command)r.status=22;
        }
        if(tag==12&&lose_on_remove)Reset();
        if(tag==9){
            std::string path;size_t offset=0,size=4096;dfwire::Reader reader(payload);dfwire::Field field;
            while(reader.Next(field)) {
                if(field.tag==1)path=field.text();
                if(field.tag==2)offset=field.number;
                if(field.tag==3)size=field.number;
            }
            if(path.find("/deepflipper/jobs/")!=std::string::npos &&
               ((size==1 && !old_mailbox)||(size!=1 && (!app||now<mailbox_at)))){r.status=7;return r;}
            r.message.tag=10;std::vector<uint8_t> f;dfwire::String(f,4,file_data.substr(std::min(offset,file_data.size()),size));dfwire::Bytes(r.message.payload,1,f);if(sink)assert(sink(r.message));
        }
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

struct CatalogFake : Fake {
    struct Registration {std::string name, data;uint32_t size;};
    std::vector<Registration> registrations;
    std::map<std::string,std::pair<uint32_t,std::string>> files;
    bool missing_catalog=false;
    int rejected_stat=-1;
    dfnative::Reply Native(uint32_t tag,const std::vector<uint8_t>& payload={},dfnative::Sink sink={},bool cleanup=false,int key=-1) override {
        if(tag!=7&&tag!=9&&tag!=24&&tag!=14)return Fake::Native(tag,payload,sink,cleanup,key);
        calls.push_back(tag);now+=5;
        dfnative::Reply reply;reply.done=true;reply.message.tag=4;
        if(NativeCancelled()){reply.error="cancelled";return reply;}
        std::string path;dfwire::Reader request(payload);dfwire::Field field;
        while(request.Next(field))if(field.tag==1)path=field.text();
        if(tag==7){
            assert(path==dfcatalog::kDirectory);
            if(missing_catalog){reply.status=7;return reply;}
            reply.message.tag=8;
            for(const auto& registration:registrations){
                std::vector<uint8_t> item;dfwire::String(item,2,registration.name);dfwire::UInt(item,3,registration.size);
                dfwire::Bytes(reply.message.payload,1,item);
            }
            if(sink)assert(sink(reply.message));
        }else if(tag==9){
            auto name=path.substr(path.find_last_of('/')+1);
            auto found=std::find_if(registrations.begin(),registrations.end(),[&](const auto& r){return r.name==name;});
            if(found==registrations.end()){reply.status=7;return reply;}
            reply.message.tag=10;std::vector<uint8_t> data;dfwire::String(data,4,found->data);dfwire::Bytes(reply.message.payload,1,data);
            if(sink)assert(sink(reply.message));
        }else {
            auto file=files.find(path);
            if(file==files.end()){reply.status=7;return reply;}
            if(tag==24){
                if(rejected_stat>=0){reply.status=rejected_stat;return reply;}
                reply.message.tag=25;std::vector<uint8_t> item;dfwire::UInt(item,3,file->second.first);dfwire::Bytes(reply.message.payload,1,item);
            }else {reply.message.tag=15;dfwire::String(reply.message.payload,1,file->second.second);}
        }
        return reply;
    }
    void Add(const std::string& id,const std::string& path){
        const std::string digest="0123456789abcdef0123456789abcdef";
        const auto data="{\"format\":\"deepflipper.app\",\"v\":1,\"app_id\":\""+id+"\",\"name\":\"Test App\",\"summary\":\"Test actions\",\"app\":\""+path+"\",\"sdk\":2,\"fap_size\":1234,\"fap_md5\":\""+digest+"\"}";
        registrations.push_back({id+".json",data,uint32_t(data.size())});
        files[path]={1234,digest};
    }
};

#include "native_screen_test.inc"

int main(){
    screen_tests();
    {
        CatalogFake f;f.Add("test.app","/ext/apps/Tools/test.fap");dfnative::Actions a(f,30000);
        Result r(a.Execute("native.app.catalog","{}"));
        assert(r.ok());auto items=cJSON_GetObjectItem(r.json,"items");assert(cJSON_GetArraySize(items)==1);
        auto item=cJSON_GetArrayItem(items,0);assert(cJSON_IsTrue(cJSON_GetObjectItem(item,"file_verified")));
        assert(cJSON_IsFalse(cJSON_GetObjectItem(item,"runtime_verified")));
        assert(std::find(f.calls.begin(),f.calls.end(),16)==f.calls.end()&&f.command.empty());
    }
    {
        CatalogFake f;f.missing_catalog=true;dfnative::Actions a(f,30000);Result r(a.Execute("native.app.catalog","{}"));
        assert(r.ok()&&cJSON_GetArraySize(cJSON_GetObjectItem(r.json,"items"))==0);
        f.locked=true;Result locked(a.Execute("native.app.catalog","{}"));assert(!locked.ok()&&locked.text("code")=="locked");
    }
    {
        CatalogFake f;for(int i=0;i<6;++i)f.Add("test"+std::to_string(i),"/ext/apps/Tools/test"+std::to_string(i)+".fap");
        dfnative::Actions a(f,30000);Result r(a.Execute("native.app.catalog","{\"offset\":2,\"limit\":2}"));
        assert(r.ok()&&cJSON_GetArraySize(cJSON_GetObjectItem(r.json,"items"))==2);
        assert(cJSON_GetObjectItem(r.json,"next_offset")->valueint==4&&cJSON_GetObjectItem(r.json,"total")->valueint==6);
    }
    for(int problem=0;problem<6;++problem){
        CatalogFake f;f.Add("test.app","/ext/apps/Tools/test.fap");
        if(problem==0)f.registrations[0].data="invalid";
        if(problem==1)f.files.clear();
        if(problem==2)f.files.begin()->second.first=1235;
        if(problem==3)f.files.begin()->second.second=std::string(32,'f');
        if(problem==4)f.registrations[0].size=768;
        if(problem==5){auto& value=f.registrations[0];value.data.insert(1,"\"v\":1,");value.size=value.data.size();}
        dfnative::Actions a(f,30000);Result r(a.Execute("native.app.catalog","{}"));
        assert(r.ok()&&cJSON_GetArraySize(cJSON_GetObjectItem(r.json,"items"))==0);
        assert(cJSON_GetArraySize(cJSON_GetObjectItem(r.json,"rejected"))==1&&f.command.empty());
    }
    {
        CatalogFake f;f.Add("test.app","/ext/apps/Tools/test.fap");f.rejected_stat=5;
        dfnative::Actions a(f,30000);Result r(a.Execute("native.app.catalog","{}"));
        assert(!r.ok()&&r.text("code")=="storage_not_ready");
    }
    for(const char* path:{"/ext/apps/../test.fap","/ext/apps//test.fap","/ext/apps/C:/test.fap"}){
        assert(!dfcatalog::AppPath(path));
    }
    std::cout<<"PASS: production app catalog discovery, pagination, no app launch, PIN lock, stale/malformed/duplicate registrations and digest binding\n";
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
        Fake f;f.file_data="{\"v\":1,\"ok\":true,\"request_id\":\"123_1\",\"action\":\"ir.learn\",\"data\":{\"path\":\"/ext/infrared/captured.ir\"}}";
        dfnative::Actions a(f,30000);Result r(a.Execute("native.capture","{\"kind\":\"ir\",\"request_id\":\"123_1\",\"timeout_ms\":500}"));
        assert(r.ok()&&r.flag("stopped")&&!f.app&&!f.busy);
        Result command(f.command);assert(command.text("action")=="ir.learn");
    }
    for (const char* action : {"describe", "subghz.decode"}) {
        Fake f;
        f.file_data=std::string("{\"v\":1,\"ok\":true,\"request_id\":\"123_1\",\"action\":\"")+action+"\",\"data\":{}}";
        dfnative::Actions a(f,30000);
        Result r(a.Execute(std::string(action)=="describe"?"native.app.describe":"native.app.call",
            std::string("{\"app\":\"/ext/apps/Tools/test.fap\",\"request_id\":\"123_1\",\"action\":\"")+action+"\",\"arguments\":\"{\\\"path\\\":\\\"/ext/subghz/a.sub\\\"}\"}"));
        assert(r.ok()&&r.flag("stopped")&&!f.app&&!f.busy);
        Result command(f.command);assert(command.text("action")==action);
    }
    for(const char* data : {"not-json", "{\"v\":1,\"ok\":true,\"request_id\":\"old\",\"action\":\"ir.learn\",\"data\":{}}",
                           "{\"v\":2,\"ok\":true,\"request_id\":\"123_1\",\"action\":\"ir.learn\",\"data\":{}}",
                           "{\"v\":1.5,\"ok\":true,\"request_id\":\"123_1\",\"action\":\"ir.learn\",\"data\":{}}",
                           "{\"v\":1,\"ok\":true,\"request_id\":\"123_1\",\"action\":\"other\",\"data\":{}}"}) {
        Fake f;f.file_data=data;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.capture","{\"kind\":\"ir\",\"request_id\":\"123_1\",\"timeout_ms\":500}"));
        assert(!r.ok()&&r.flag("stopped")&&r.text("code")=="invalid_app_result"&&!f.app);
    }
    {
        Fake f;f.old_mailbox=true;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.capture","{\"kind\":\"ir\",\"request_id\":\"123_1\"}"));
        assert(!r.ok()&&r.text("code")=="request_id_reused"&&f.command.empty());
    }
    for(const char* arguments : {"[]", "{} trailing", "null", "{\"a\":[[[[[[[]]]]]]]}"}) {
        Fake f;dfnative::Actions a(f,30000);
        auto args=cJSON_CreateObject();cJSON_AddStringToObject(args,"app","/ext/apps/Tools/test.fap");
        cJSON_AddStringToObject(args,"action","test");cJSON_AddStringToObject(args,"request_id","123_1");
        cJSON_AddStringToObject(args,"arguments",arguments);char* serialized=cJSON_PrintUnformatted(args);
        Result r(a.Execute("native.app.call",serialized));cJSON_free(serialized);cJSON_Delete(args);
        assert(!r.ok()&&r.text("code")=="invalid_arguments"&&f.command.empty());
    }
    {
        Fake f;f.bad_command=true;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.app.describe","{\"app\":\"/ext/apps/Tools/no_sdk.fap\",\"request_id\":\"123_1\"}"));
        assert(!r.ok()&&r.flag("stopped")&&!f.app);
    }
    for(int mode=0;mode<3;++mode) {
        Fake f;f.mailbox_at=10000;
        if(mode==0)f.cancel_at=100;
        if(mode==1)f.lock_at=100;
        dfnative::Actions a(f,mode==2?200:30000);
        Result r(a.Execute("native.app.call","{\"app\":\"/ext/apps/Tools/test.fap\",\"request_id\":\"123_1\",\"action\":\"test\",\"arguments\":\"{}\"}"));
        assert(!r.ok()&&r.flag("stopped")&&!f.app&&!f.busy);
        assert(r.text("code")== (mode==0?"cancelled":mode==1?"locked":"timeout"));
    }
    {
        Fake f;f.file_data="invalid";f.fail_stop=true;dfnative::Actions a(f,30000);
        Result r(a.Execute("native.app.call","{\"app\":\"/ext/apps/Tools/test.fap\",\"request_id\":\"123_1\",\"action\":\"test\",\"arguments\":\"{}\"}"));
        assert(!r.ok()&&r.text("code")=="stop_unconfirmed"&&f.reset);
    }
    std::cout<<"PASS: generic app discovery/actions, request correlation, stale mailbox rejection, strict arguments and unsupported-app cleanup\n";
    {
        Fake f;dfnative::Actions a(f,30000);
        f.file_data="{\"v\":1,\"ok\":true,\"request_id\":\"123_1\",\"action\":\"test.start\",\"keep_open\":true,\"session_supported\":true,\"data\":{}}";
        Result first(a.Execute("native.app.call","{\"app\":\"/ext/apps/Tools/test.fap\",\"request_id\":\"123_1\",\"action\":\"test.start\",\"arguments\":\"{}\",\"keep_open\":true,\"lease_ms\":5000}"));
        assert(first.ok()&&first.flag("app_retained")&&!first.flag("stopped")&&f.busy&&f.app);
        assert(f.retained=="/ext/apps/Tools/test.fap"&&f.retained_deadline>f.now);
        Result conflict(a.Execute("native.app.call","{\"app\":\"/ext/apps/Tools/other.fap\",\"request_id\":\"123_8\",\"action\":\"test\",\"arguments\":\"{}\"}"));
        assert(!conflict.ok()&&conflict.text("code")=="busy"&&f.busy&&f.retained=="/ext/apps/Tools/test.fap");
        Result gpio(a.Execute("native.gpio","{\"pin\":\"PA7\",\"action\":\"write\",\"value\":1}"));
        assert(!gpio.ok()&&gpio.text("code")=="busy"&&f.busy);
        f.calls.clear();
        f.file_data="{\"v\":1,\"ok\":true,\"request_id\":\"123_2\",\"action\":\"test.state\",\"keep_open\":true,\"session_supported\":true,\"data\":{}}";
        Result second(a.Execute("native.app.call","{\"app\":\"/ext/apps/Tools/test.fap\",\"request_id\":\"123_2\",\"action\":\"test.state\",\"arguments\":\"{}\",\"keep_open\":true}"));
        assert(second.ok()&&second.flag("app_retained")&&f.busy);
        assert(std::find(f.calls.begin(),f.calls.end(),16)==f.calls.end());
        assert(std::find(f.calls.begin(),f.calls.end(),47)==f.calls.end());
        f.file_data="{\"v\":1,\"ok\":true,\"request_id\":\"123_3\",\"action\":\"test.stop\",\"keep_open\":false,\"session_supported\":true,\"data\":{}}";
        Result third(a.Execute("native.app.call","{\"app\":\"/ext/apps/Tools/test.fap\",\"request_id\":\"123_3\",\"action\":\"test.stop\",\"arguments\":\"{}\"}"));
        assert(third.ok()&&!third.flag("app_retained")&&third.flag("stopped")&&f.retained.empty()&&!f.app&&!f.busy);
    }
    for(bool stop_failure:{false,true}) {
        Fake f;f.app=f.busy=true;f.fail_stop=stop_failure;f.RetainApp("/ext/apps/Tools/test.fap",1000);
        dfnative::Actions a(f,10000);assert(a.ExpireRetained().empty()&&f.calls.empty());
        f.now=1000;f.locked=true;Result expired(a.ExpireRetained());
        assert(expired.ok()==!stop_failure&&f.retained.empty()&&!f.busy);
        assert(f.reset==stop_failure);
    }
    {
        Fake f;f.app=f.busy=true;f.fail_stop=true;f.RetainApp("/ext/apps/Tools/test.fap",10000);
        dfnative::Actions a(f,30000);Result exit(a.Execute("native.app.exit","{}"));
        assert(!exit.ok()&&!exit.flag("stopped")&&f.reset&&f.retained.empty());
    }
    {
        Fake f;f.file_data="{\"v\":1,\"ok\":true,\"request_id\":\"123_1\",\"action\":\"test\",\"data\":{}}";
        dfnative::Actions a(f,30000);
        Result unsupported(a.Execute("native.app.call","{\"app\":\"/ext/apps/Tools/old.fap\",\"request_id\":\"123_1\",\"action\":\"test\",\"arguments\":\"{}\",\"keep_open\":true}"));
        assert(!unsupported.ok()&&unsupported.text("code")=="session_not_supported"&&unsupported.flag("stopped")&&!f.busy);
    }
    {
        Fake f;f.lose_on_remove=true;
        f.file_data="{\"v\":1,\"ok\":true,\"request_id\":\"123_1\",\"action\":\"test.start\",\"keep_open\":true,\"session_supported\":true,\"data\":{}}";
        dfnative::Actions a(f,30000);
        Result lost(a.Execute("native.app.call","{\"app\":\"/ext/apps/Tools/test.fap\",\"request_id\":\"123_1\",\"action\":\"test.start\",\"arguments\":\"{}\",\"keep_open\":true}"));
        assert(!lost.ok()&&!lost.flag("app_retained")&&lost.text("code")=="app_session_lost"&&f.retained.empty());
    }
    std::cout<<"PASS: retained app start/state/stop, renewal, no relaunch, lease expiry under PIN and failed-cleanup fallback\n";
    std::cout<<"PASS: capture adapter lifecycle, bounded SD reads, clock, GPIO and long input\n";
}
