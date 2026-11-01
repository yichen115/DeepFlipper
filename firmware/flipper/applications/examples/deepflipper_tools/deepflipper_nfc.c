#include "deepflipper_nfc.h"
#include <nfc/nfc_device.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_poller.h>
#include <nfc/protocols/iso14443_3b/iso14443_3b_poller.h>
#include <nfc/protocols/iso15693_3/iso15693_3_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <nfc/protocols/mf_classic/mf_classic_poller.h>
#include "../../main/nfc/helpers/mf_classic_key_cache.h"
#include <mjs/common/frozen/frozen.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

struct DeepFlipperNfc {
    NfcPoller* poller;
    NfcDevice* device;
    NfcProtocol protocol;
    atomic_bool done;
    bool success;
    MfClassicKeyCache* keys;
    bool keys_loaded;
};

static NfcCommand read_callback(NfcGenericEvent event, void* context) {
    DeepFlipperNfc* read = context;
    bool ready = false;
    if(event.protocol == NfcProtocolMfUltralight) {
        const MfUltralightPollerEvent* e = event.event_data;
        if(e->type == MfUltralightPollerEventTypeRequestMode)
            e->data->poller_mode = MfUltralightPollerModeRead;
        else if(e->type == MfUltralightPollerEventTypeAuthRequest)
            e->data->auth_context.skip_auth = true;
        ready = e->type == MfUltralightPollerEventTypeReadSuccess;
        if(e->type == MfUltralightPollerEventTypeReadFailed) {
            atomic_store(&read->done, true);
            return NfcCommandStop;
        }
    } else if(event.protocol == NfcProtocolMfClassic) {
        const MfClassicPollerEvent* e = event.event_data;
        if(e->type == MfClassicPollerEventTypeRequestMode) {
            nfc_device_set_data(read->device, read->protocol, nfc_poller_get_data(read->poller));
            size_t uid_size = 0;
            const uint8_t* uid = nfc_device_get_uid(read->device, &uid_size);
            mf_classic_key_cache_reset(read->keys);
            read->keys_loaded = mf_classic_key_cache_load(read->keys, uid, uid_size);
            e->data->poller_mode.mode = MfClassicPollerModeRead;
            ready = !read->keys_loaded;
        } else if(e->type == MfClassicPollerEventTypeRequestReadSector) {
            MfClassicPollerEventDataReadSectorRequest* r = &e->data->read_sector_request_data;
            r->key_provided = mf_classic_key_cache_get_next_key(
                read->keys, &r->sector_num, &r->key, &r->key_type);
        } else
            ready = e->type == MfClassicPollerEventTypeSuccess;
    } else if(event.protocol == NfcProtocolIso14443_3a) {
        ready = ((Iso14443_3aPollerEvent*)event.event_data)->type ==
                Iso14443_3aPollerEventTypeReady;
    } else if(event.protocol == NfcProtocolIso14443_3b) {
        ready = ((Iso14443_3bPollerEvent*)event.event_data)->type ==
                Iso14443_3bPollerEventTypeReady;
    } else if(event.protocol == NfcProtocolIso15693_3) {
        ready = ((Iso15693_3PollerEvent*)event.event_data)->type == Iso15693_3PollerEventTypeReady;
    }
    if(!ready) return NfcCommandContinue;
    nfc_device_set_data(read->device, read->protocol, nfc_poller_get_data(read->poller));
    read->success = true;
    atomic_store(&read->done, true);
    return NfcCommandStop;
}

DeepFlipperNfc* deepflipper_nfc_alloc(Nfc* nfc, NfcProtocol detected) {
    NfcProtocol protocol = detected;
    if(protocol != NfcProtocolMfUltralight && protocol != NfcProtocolMfClassic &&
       protocol != NfcProtocolIso14443_3a && protocol != NfcProtocolIso14443_3b &&
       protocol != NfcProtocolIso15693_3) {
        protocol = nfc_protocol_get_parent(protocol);
        while(protocol != NfcProtocolInvalid && protocol != NfcProtocolIso14443_3a &&
              protocol != NfcProtocolIso14443_3b && protocol != NfcProtocolIso15693_3)
            protocol = nfc_protocol_get_parent(protocol);
    }
    if(protocol == NfcProtocolInvalid) return NULL;
    DeepFlipperNfc* read = calloc(1, sizeof(DeepFlipperNfc));
    read->protocol = protocol;
    read->device = nfc_device_alloc();
    read->keys = mf_classic_key_cache_alloc();
    read->poller = nfc_poller_alloc(nfc, protocol);
    nfc_poller_start(read->poller, read_callback, read);
    return read;
}

bool deepflipper_nfc_done(DeepFlipperNfc* read) {
    return atomic_load(&read->done);
}

void deepflipper_nfc_result(DeepFlipperNfc* read, char* output, size_t capacity) {
    if(!read->success) {
        snprintf(output, capacity, "{\"read_ok\":false,\"code\":\"card_read_failed\"}");
        return;
    }
    size_t uid_size = 0;
    const uint8_t* uid = nfc_device_get_uid(read->device, &uid_size);
    char uid_hex[33] = {0}, data_hex[257] = {0};
    for(size_t i = 0; i < uid_size && i < 16; ++i)
        snprintf(uid_hex + i * 2, 3, "%02X", uid[i]);
    unsigned bytes_read = 0, bytes_total = 0;
    const char* scope = "identification_only";
    if(read->protocol == NfcProtocolMfUltralight) {
        const MfUltralightData* data = nfc_device_get_data(read->device, read->protocol);
        bytes_read = data->pages_read * MF_ULTRALIGHT_PAGE_SIZE;
        bytes_total = data->pages_total * MF_ULTRALIGHT_PAGE_SIZE;
        scope = "public_pages";
        for(size_t i = 0; i < bytes_read && i < 128; ++i)
            snprintf(data_hex + i * 2, 3, "%02X", ((const uint8_t*)data->page)[i]);
    } else if(read->protocol == NfcProtocolMfClassic) {
        const MfClassicData* data = nfc_device_get_data(read->device, read->protocol);
        bytes_total = mf_classic_get_total_block_num(data->type) * 16;
        for(unsigned i = 0; i < bytes_total / 16; ++i)
            if(mf_classic_is_block_read(data, i)) bytes_read += 16;
        scope = read->keys_loaded ? "cached_keys_only" : "identification_only_no_cached_keys";
    }
    struct json_out out = JSON_OUT_BUF(output, capacity);
    json_printf(
        &out,
        "{read_ok:%B,uid:%Q,card_type:%Q,scope:%Q,bytes_read:%u,bytes_total:%u,preview_hex:%Q,memory_complete:%B}",
        true,
        uid_hex,
        nfc_device_get_name(read->device, NfcDeviceNameTypeFull),
        scope,
        bytes_read,
        bytes_total,
        data_hex,
        bytes_total > 0 && bytes_read == bytes_total);
}

void deepflipper_nfc_free(DeepFlipperNfc* read) {
    nfc_poller_stop(read->poller);
    nfc_poller_free(read->poller);
    nfc_device_free(read->device);
    mf_classic_key_cache_free(read->keys);
    free(read);
}

bool deepflipper_nfc_save(DeepFlipperNfc* read, const char* path) {
    return read->success && nfc_device_save(read->device, path);
}
