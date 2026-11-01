#include "firmware/esp32/main/boards/deepflipper-s3/portal_config.h"
#include "firmware/esp32/main/boards/deepflipper-s3/setup_flow.h"
#include "firmware/esp32/main/protocols/voice_hello.h"
#include "firmware/esp32/main/boards/deepflipper-s3/companion_json.h"
#include <cassert>
#include <iostream>

int main() {
    using namespace dfsetup;
    const std::string old = R"({"ssid":"old-network","wifi_password":"old-password","url":"ws://192.168.1.10:8000/xiaozhi/v1/","token":"old-secret","volume":20,"revision":4})";
    std::string error;
    auto input = Parse(R"({"volume":30,"token":"","admin_password":"ignored","ssid":"ignored"})");
    auto merged = Merge(old, input.get(), error);
    assert(merged && Number(merged.get(), "volume") == 30 && Text(merged.get(), "token") == "old-secret");
    assert(Text(merged.get(), "ssid") == "old-network" && !cJSON_GetObjectItem(merged.get(), "admin_password"));
    input = Parse(R"({"clear_token":true})"); merged = Merge(old, input.get(), error);
    assert(merged && Text(merged.get(), "token").empty());
    for (auto text : {R"({"volume":20.1})", R"({"volume":101})", R"({"microphone":"true"})",
                      R"({"token":"secret\r\nHeader: bad"})", R"({"token":"new","clear_token":true})",
                      R"({"url":"https://wrong.example"})", R"({"url":"ws://user:pass@host/"})"}) {
        input = Parse(text); assert(!Merge(old, input.get(), error));
    }
    input = Parse(R"({"token":"new-secret"})"); merged = Merge(old, input.get(), error);
    assert(merged && Text(merged.get(), "token") == "new-secret");
    assert(Text(Parse(old).get(), "token") == "old-secret");
    input = Parse(R"({"ssid":"new-network","wifi_password":"new-password"})");
    assert(Wifi(input.get(), error));
    assert(!ConfirmWifi(old, "new-network", "new-password", "unrelated-network"));
    assert(Text(Parse(old).get(), "ssid") == "old-network");
    merged = ConfirmWifi(old, "new-network", "new-password", "new-network");
    assert(merged && Text(merged.get(), "ssid") == "new-network" && Text(merged.get(), "token") == "old-secret");
    for (auto text : {R"({"ssid":"","wifi_password":"valid-password"})", R"({"ssid":"abc","wifi_password":"short"})", R"({"ssid":"abc"})"}) {
        input = Parse(text); assert(!Wifi(input.get(), error));
    }
    input = Parse(R"({"ssid":"open-network","wifi_password":""})"); assert(Wifi(input.get(), error));
    input = Parse(R"({"format":"deepflipper.connection","version":1,"device_id":"device-a","client_id":"client-a","server_url":"ws://192.168.1.10:8000/xiaozhi/v1/","token":"test-secret"})");
    assert(Connection(input.get(), "device-a", "client-a", error));
    assert(!Connection(input.get(), "device-b", "client-a", error));
    assert(!Connection(input.get(), "device-a", "other-client", error));
    cJSON_GetObjectItem(input.get(), "version")->valuedouble = 1.5;
    assert(!Connection(input.get(), "device-a", "client-a", error));
    assert(!Parse(R"({"token":"a\u0000hidden"})") && !Parse("{} trailing"));
    assert(!Parse(std::string(100, '[') + "0" + std::string(100, ']')));
    assert(!Parse(std::string("{}\0hidden", 9)));
    WebSession session;
    assert(!session.Valid("", 0));
    session.Open("cookie-one", "csrf-one", 100);
    assert(session.Valid("cookie-one", 101) && session.Write("cookie-one", "csrf-one", 102));
    assert(!session.Write("cookie-one", "bad-csrf", 102) && !session.Valid("wrong-cookie", 102));
    assert(!session.Valid("cookie-one", 1800100));
    session.Open("cookie-two", "csrf-two", 200);
    assert(!session.Valid("cookie-one", 201) && session.Valid("cookie-two", 201));
    session.Close(); assert(!session.Valid("cookie-two", 202));
    {
        WifiTrial trial;
        assert(trial.Begin("new-network", "new-password", "request-a"));
        assert(!trial.CanConfirm("new-network", 100)); // An old IP before the trial starts.
        assert(!trial.Begin("other-network", "other-password", "request-b"));
        trial.Start(1000);
        assert(!trial.CanConfirm("old-network", 2000));
        assert(!trial.CanConfirm("new-network", 61000));
        trial.Fail("dhcp_timeout");
        assert(trial.Password().empty() && trial.State()=="failed");
        // A restored old link is live, but the requested new network still failed.
        assert(!trial.Succeed("old-network", 62000, 4, 5));
        assert(trial.State()=="failed" && trial.Request()=="request-a");
        assert(trial.Begin("correct-network", "correct-password", "request-b"));
        trial.Start(70000);
        assert(trial.Succeed("correct-network", 70100, 4, 5));
        assert(trial.Revision()==5 && trial.BeforeRevision()==4 && trial.Password().empty());
        trial.Fail("late_disconnect");
        assert(trial.State()=="saved");
    }
    assert(std::string(SpeakerError(true,true,20,false))=="speaker_muted");
    assert(std::string(SpeakerError(true,false,0,false))=="volume_zero");
    assert(!SpeakerError(true,false,20,false)); // Works before Wi-Fi provisioning too.
    assert(std::string(TalkError(true,true,false,true,false,true))=="needs_config");
    assert(std::string(TalkError(true,true,true,false,false,true))=="microphone_disabled");
    assert(!TalkError(true,true,true,true,false,true));
    assert(DecideTalk("toggle",false,true)==TalkAction::Start);
    assert(DecideTalk("toggle",true,false)==TalkAction::Stop);
    assert(DecideTalk("start",true,true)==TalkAction::Reject);
    assert(DecideTalk("stop",false,true)==TalkAction::Reject);
    assert(DecideTalk("toggle",false,false)==TalkAction::Reject);
    assert(DecideTalk("cancel",true,false)==TalkAction::Cancel);
    assert(DecideTalk("cancel",false,true,true)==TalkAction::Cancel);
    assert(DecideTalk("cancel",false,true,false)==TalkAction::Reject);
    assert(DecideTalk("unknown",false,true)==TalkAction::Reject);
    assert(VisualState(true,true,true,false,"auth_failed",3)==8);
    assert(VisualState(true,true,true,false,"connect_timeout",3)==8);
    assert(VisualState(true,true,true,false,"connecting",3)==2);
    assert(VisualState(true,true,false,true,"wifi_offline",3)==1);
    assert(VisualState(true,true,true,false,"authenticated",4)==4);
    for(auto text : {R"({"type":"hello","transport":"websocket"})",
                    R"({"type":"hello","transport":"websocket","session_id":""})",
                    R"({"type":"hello","transport":"websocket","session_id":"s","audio_params":{"sample_rate":1}})"}) {
        input=Parse(text); assert(!voicehello::Valid(input.get()));
    }
    input=Parse(R"({"type":"hello","transport":"websocket","session_id":"session-a","audio_params":{"sample_rate":24000,"frame_duration":60}})");
    assert(voicehello::Valid(input.get()));
    for (uint32_t session_id : {1U, 2147483647U, 2147483648U, 3000000000U, UINT32_MAX}) {
        auto hello = Parse("{\"session\":" + std::to_string(session_id) + "}");
        assert(dfcompanion::UInt32(hello.get(), "session") == session_id);
        auto request = Parse("{}");
        cJSON_AddNumberToObject(request.get(), "session", dfcompanion::UInt32(hello.get(), "session"));
        auto roundtrip = Parse(Encode(request.get()));
        assert(dfcompanion::UInt32(roundtrip.get(), "session") == session_id);
    }
    for (auto invalid : {"-1", "1.5", "4294967296", "\"3000000000\"", "null"}) {
        input = Parse(std::string("{\"session\":") + invalid + "}");
        assert(dfcompanion::UInt32(input.get(), "session") == 0);
    }
    std::cout << "PASS: device portal credentials, atomic Wi-Fi confirmation, profile validation, identity binding and session/CSRF expiry\n";
    std::cout << "PASS: setup workflow trials, rollback outcomes, late IP events, audio prerequisites and authenticated hello\n";
    std::cout << "PASS: full unsigned 32-bit companion session IDs and JSON roundtrip\n";
}
