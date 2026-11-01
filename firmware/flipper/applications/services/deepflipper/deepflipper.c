#include "deepflipper.h"
#include "deepflipper_sprite.h"

#include <furi_hal.h>
#include <expansion/expansion.h>
#include <power/power_service/power.h>
#include <mjs/common/frozen/frozen.h>
#include <nfc/nfc.h>
#include <nfc/nfc_scanner.h>
#include <nfc/nfc_device.h>
#include <nfc/protocols/nfc_protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FuriMutex* mutex;
    FuriThread* thread;
    DeepFlipperSnapshot snapshot;
    uint32_t last_seen;
    uint32_t next_session;
    uint32_t next_command;
    uint32_t command_id;
    uint32_t command_deadline;
    char command[512];
    bool nfc_requested;
    bool nfc_running;
    bool nfc_cancel;
    bool nfc_detected;
    uint32_t nfc_timeout;
    uint32_t nfc_deadline;
    char job_state[20];
    char protocol[64];
} DeepFlipper;

static DeepFlipper* service;

static void lock(void) {
    furi_check(furi_mutex_acquire(service->mutex, FuriWaitForever) == FuriStatusOk);
}

static void unlock(void) {
    furi_mutex_release(service->mutex);
}

static void fail(char* out, size_t capacity, const char* code) {
    snprintf(out, capacity, "{\"ok\":false,\"code\":\"%s\"}", code);
}

void deepflipper_snapshot(DeepFlipperSnapshot* snapshot) {
    lock();
    *snapshot = service->snapshot;
    unlock();
}

static void nfc_callback(NfcScannerEvent event, void* context) {
    UNUSED(context);
    if(event.type != NfcScannerEventTypeDetected || !event.data.protocol_num) return;
    lock();
    snprintf(
        service->protocol,
        sizeof(service->protocol),
        "%s",
        nfc_device_get_protocol_name(event.data.protocols[0]));
    service->nfc_detected = true;
    unlock();
}

static int32_t worker(void* context) {
    UNUSED(context);
    Nfc* nfc = NULL;
    NfcScanner* scanner = NULL;
    uint32_t power_tick = 0;
    while(true) {
        lock();
        const uint32_t now = furi_get_tick();
        // Native RPC app/file operations can take several seconds. Physical link
        // loss still closes the Expansion RPC session and calls disconnected().
        if(service->snapshot.connected && now - service->last_seen > furi_ms_to_ticks(10000)) {
            service->snapshot.connected = false;
            service->snapshot.state = DeepFlipperOffline;
            service->snapshot.mouth = 0;
            service->nfc_cancel = true;
            memset(service->command, 0, sizeof(service->command));
            service->command_id = 0;
        }
        if(service->command_id && (int32_t)(now - service->command_deadline) >= 0) {
            service->command_id = 0;
            memset(service->command, 0, sizeof(service->command));
            snprintf(
                service->snapshot.notice,
                sizeof(service->snapshot.notice),
                "Module command timed out");
        }
        if(service->nfc_requested && !service->nfc_running) {
            service->nfc_requested = false;
            if(service->nfc_cancel || service->snapshot.foreground || !service->snapshot.enabled ||
               furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock)) {
                strcpy(service->job_state, "cancelled");
            } else {
                service->nfc_running = true;
                service->nfc_deadline = now + furi_ms_to_ticks(service->nfc_timeout);
                strcpy(service->job_state, "running");
                service->snapshot.state = DeepFlipperExecuting;
            }
        }
        bool start = service->nfc_running && !scanner;
        unlock();
        if(start) {
            // This thread owns NFC for its entire allocation/free lifetime.
            if(furi_hal_nfc_is_hal_ready() == FuriHalNfcErrorNone) {
                nfc = nfc_alloc();
                scanner = nfc_scanner_alloc(nfc);
                nfc_scanner_start(scanner, nfc_callback, NULL);
            } else {
                lock();
                service->nfc_running = false;
                strcpy(service->job_state, "busy");
                service->snapshot.state = DeepFlipperIdle;
                unlock();
            }
        }
        lock();
        bool stop = scanner && (service->nfc_cancel || service->nfc_detected ||
                                (int32_t)(now - service->nfc_deadline) >= 0);
        bool cancelled = service->nfc_cancel;
        bool detected = service->nfc_detected;
        unlock();
        if(stop) {
            nfc_scanner_stop(scanner);
            nfc_scanner_free(scanner);
            nfc_free(nfc);
            scanner = NULL;
            nfc = NULL;
            lock();
            strcpy(service->job_state, cancelled ? "cancelled" : detected ? "done" : "timeout");
            service->nfc_running = false;
            service->snapshot.state = service->snapshot.connected ? DeepFlipperIdle :
                                                                    DeepFlipperOffline;
            unlock();
        }
        DeepFlipperSnapshot snapshot;
        deepflipper_snapshot(&snapshot);
        deepflipper_sprite_update(&snapshot);
        if(furi_get_tick() - power_tick > furi_ms_to_ticks(1000)) {
            Power* power = furi_record_open(RECORD_POWER);
            PowerInfo info;
            power_get_info(power, &info);
            furi_record_close(RECORD_POWER);
            lock();
            service->snapshot.battery = info.charge;
            unlock();
            power_tick = furi_get_tick();
        }
        // Mascot frames run at 120 ms. Only an active NFC worker needs fast polling.
        furi_delay_ms(scanner ? 20 : 120);
    }
    return 0;
}

void deepflipper_on_system_start(void* context) {
    UNUSED(context);
    service = calloc(1, sizeof(DeepFlipper));
    service->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    service->snapshot.volume = 20;
    service->snapshot.microphone = true;
    service->next_session = furi_hal_random_get();
    strcpy(service->job_state, "idle");
    deepflipper_sprite_init();
    service->thread = furi_thread_alloc_ex("WhaleTools", 3072, worker, NULL);
    furi_record_create(RECORD_DEEPFLIPPER, service);
    furi_thread_start(service->thread);
}

void deepflipper_set_enabled(bool enabled) {
    lock();
    service->snapshot.enabled = enabled;
    service->snapshot.connected = false;
    service->snapshot.mouth = 0;
    service->snapshot.state = enabled ? DeepFlipperConnecting : DeepFlipperOffline;
    service->nfc_cancel = true;
    service->command_id = 0;
    memset(service->command, 0, sizeof(service->command));
    unlock();
    Expansion* expansion = furi_record_open(RECORD_EXPANSION);
    if(enabled) {
        Power* power = furi_record_open(RECORD_POWER);
        power_enable_otg(power, true);
        furi_record_close(RECORD_POWER);
        expansion_set_listen_serial(expansion, FuriHalSerialIdUsart);
    } else {
        expansion_disable(expansion);
        // Do not disable shared 5 V: the module has its own physical switch.
    }
    furi_record_close(RECORD_EXPANSION);
}

bool deepflipper_prepare_app_start(void) {
    lock();
    service->snapshot.foreground = true;
    service->nfc_cancel = true;
    if(service->nfc_requested) strcpy(service->job_state, "cancelled");
    service->nfc_requested = false;
    unlock();
    for(size_t i = 0; i < 100; i++) {
        lock();
        bool running = service->nfc_running;
        unlock();
        if(!running) return true;
        furi_delay_ms(10);
    }
    return false;
}

void deepflipper_app_finished(void) {
    lock();
    service->snapshot.foreground = false;
    unlock();
}

bool deepflipper_module_command(const char* field, const char* value) {
    bool success = false;
    lock();
    if(service->snapshot.enabled && service->snapshot.connected && !service->command_id) {
        struct json_out out = JSON_OUT_BUF(service->command, sizeof(service->command));
        uint32_t id = ++service->next_command;
        int size = json_printf(&out, "{id:%u,field:%Q,value:%Q}", id, field, value);
        if(size > 0 && (size_t)size < sizeof(service->command)) {
            service->command_id = id;
            service->command_deadline = furi_get_tick() + furi_ms_to_ticks(15000);
            strcpy(service->snapshot.notice, "Waiting for module");
            success = true;
        } else {
            memset(service->command, 0, sizeof(service->command));
        }
    }
    unlock();
    return success;
}

void deepflipper_disconnected(uint32_t session) {
    lock();
    if(session && session == service->snapshot.session) {
        service->snapshot.connected = false;
        service->snapshot.state = DeepFlipperOffline;
        service->snapshot.mouth = 0;
        service->nfc_cancel = true;
        service->command_id = 0;
        memset(service->command, 0, sizeof(service->command));
    }
    unlock();
}

void deepflipper_request(const char* json, size_t size, char* result, size_t capacity) {
    char* op = NULL;
    int version = 0;
    unsigned session = 0;
    if(size >= DEEPFLIPPER_JSON_SIZE ||
       json_scanf(json, size, "{v:%d,op:%Q}", &version, &op) != 2 || version != 1) {
        free(op);
        fail(result, capacity, "invalid_request");
        return;
    }
    lock();
    struct json_out out = JSON_OUT_BUF(result, capacity);
    if(strcmp(op, "hello") == 0) {
        if(!service->snapshot.enabled) {
            fail(result, capacity, "disabled");
        } else {
            service->snapshot.session = ++service->next_session;
            service->snapshot.connected = true;
            service->last_seen = furi_get_tick();
            json_printf(&out, "{ok:%B,v:1,session:%u,native_rpc:1}", true, service->snapshot.session);
        }
    } else if(
        json_scanf(json, size, "{session:%u}", &session) != 1 || !service->snapshot.enabled ||
        !service->snapshot.connected || session != service->snapshot.session) {
        fail(result, capacity, "invalid_session");
    } else {
        service->last_seen = furi_get_tick();
        if(strcmp(op, "status") == 0 || strcmp(op, "capabilities") == 0) {
            json_printf(
                &out,
                "{ok:%B,state:%d,foreground:%B,locked:%B,volume:%d,tools:[%Q,%Q]}",
                true,
                service->snapshot.state,
                service->snapshot.foreground,
                furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock),
                service->snapshot.volume,
                "flipper_get_status",
                "flipper_nfc_scan");
        } else if(strcmp(op, "ui") == 0) {
            int state = -1, mouth = -1, emotion = -1;
            json_scanf(json, size, "{state:%d,mouth:%d,emotion:%d}", &state, &mouth, &emotion);
            if(state >= DeepFlipperOffline && state <= DeepFlipperError && !service->nfc_running)
                service->snapshot.state = state;
            if(mouth >= 0 && mouth <= 2) service->snapshot.mouth = mouth;
            if(emotion >= 0 && emotion < 12) service->snapshot.emotion = emotion;
            snprintf(result, capacity, "{\"ok\":true}");
        } else if(strcmp(op, "poll") == 0) {
            snprintf(
                result,
                capacity,
                "{\"ok\":true,\"command\":%s}",
                service->command_id ? service->command : "null");
        } else if(strcmp(op, "module.result") == 0) {
            unsigned id = 0;
            int success = 0, volume = -1;
            json_scanf(
                json,
                size,
                "{id:%u,ok:%B,volume:%d}",
                &id,
                &success,
                &volume);
            if(volume >= 0 && volume <= 100) service->snapshot.volume = volume;
            int token_set = 0, muted = 0, microphone = 1;
            json_scanf(
                json,
                size,
                "{token_set:%B,muted:%B,microphone:%B}",
                &token_set,
                &muted,
                &microphone);
            service->snapshot.token_set = token_set;
            service->snapshot.muted = muted;
            service->snapshot.microphone = microphone;
            char *device = NULL, *client = NULL;
            json_scanf(json, size, "{device_id:%Q,client_id:%Q}", &device, &client);
            if(device)
                snprintf(
                    service->snapshot.device_id, sizeof(service->snapshot.device_id), "%s", device);
            if(client)
                snprintf(
                    service->snapshot.client_id, sizeof(service->snapshot.client_id), "%s", client);
            free(device);
            free(client);
            char *wifi = NULL, *ip = NULL, *ap_ip = NULL, *ap_ssid = NULL, *key = NULL, *server = NULL;
            int ap = 0, changed = 0;
            unsigned baud = 0;
            json_scanf(json, size,
                "{wifi:%Q,ip:%Q,ap_ip:%Q,ap_ssid:%Q,setup_key:%Q,server_state:%Q,ap:%B,web_changed:%B,uart_baud:%u}",
                &wifi, &ip, &ap_ip, &ap_ssid, &key, &server, &ap, &changed, &baud);
            if(wifi) snprintf(service->snapshot.wifi, sizeof(service->snapshot.wifi), "%s", wifi);
            if(ip) snprintf(service->snapshot.ip, sizeof(service->snapshot.ip), "%s", ip);
            if(ap_ip) snprintf(service->snapshot.ap_ip, sizeof(service->snapshot.ap_ip), "%s", ap_ip);
            if(ap_ssid) snprintf(service->snapshot.ap_ssid, sizeof(service->snapshot.ap_ssid), "%s", ap_ssid);
            if(key) snprintf(service->snapshot.setup_key, sizeof(service->snapshot.setup_key), "%s", key);
            if(server) snprintf(service->snapshot.server_state, sizeof(service->snapshot.server_state), "%s", server);
            service->snapshot.ap = ap; service->snapshot.web_changed = changed; service->snapshot.uart_baud = baud;
            free(wifi); free(ip); free(ap_ip); free(ap_ssid); free(key); free(server);
            if(id && id == service->command_id) {
                service->command_id = 0;
                memset(service->command, 0, sizeof(service->command));
                snprintf(
                    service->snapshot.notice,
                    sizeof(service->snapshot.notice),
                    "%s",
                    success ? "Action accepted" : "Module rejected action");
            }
            snprintf(result, capacity, "{\"ok\":true}");
        } else if(strcmp(op, "tool.nfc.scan") == 0) {
            int timeout = 5000;
            json_scanf(json, size, "{timeout_ms:%d}", &timeout);
            if(timeout < 250 || timeout > 10000) {
                fail(result, capacity, "invalid_args");
            } else if(
                service->snapshot.foreground || service->nfc_requested || service->nfc_running ||
                furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock)) {
                fail(result, capacity, "busy");
            } else {
                service->nfc_cancel = false;
                service->nfc_detected = false;
                service->protocol[0] = 0;
                service->nfc_timeout = timeout;
                service->nfc_requested = true;
                service->snapshot.job++;
                strcpy(service->job_state, "accepted");
                json_printf(
                    &out, "{ok:%B,job:%u,state:%Q}", true, service->snapshot.job, "accepted");
            }
        } else if(strcmp(op, "job.get") == 0 || strcmp(op, "job.cancel") == 0) {
            unsigned job = 0;
            json_scanf(json, size, "{job:%u}", &job);
            if(!job || job != service->snapshot.job) {
                fail(result, capacity, "not_found");
            } else {
                if(strcmp(op, "job.cancel") == 0) service->nfc_cancel = true;
                json_printf(
                    &out,
                    "{ok:%B,job:%u,state:%Q,protocol:%Q}",
                    true,
                    job,
                    service->job_state,
                    service->protocol);
            }
        } else {
            fail(result, capacity, "unsupported");
        }
    }
    unlock();
    free(op);
}
