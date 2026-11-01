#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <deepflipper/deepflipper.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ViewDispatcher* dispatcher;
    Submenu* menu;
    Widget* help;
    bool menu_ready;
    uint32_t help_kind;
    char last_help[220];
    uint32_t feedback_until;
} AssistantSettings;

static void rebuild(AssistantSettings* app);
static void show_help(AssistantSettings* app, const char* title, const char* text) {
    if(strcmp(app->last_help, text) == 0) return;
    snprintf(app->last_help, sizeof(app->last_help), "%s", text);
    widget_reset(app->help);
    widget_add_string_element(app->help, 64, 1, AlignCenter, AlignTop, FontPrimary, title);
    // Long status strings, UUIDs and setup details must wrap and remain reachable.
    widget_add_text_scroll_element(app->help, 2, 14, 124, 50, text);
    view_dispatcher_switch_to_view(app->dispatcher, 1);
}
static bool send_command(AssistantSettings* app, const char* field, const char* value) {
    bool accepted = deepflipper_module_command(field, value);
    app->feedback_until = accepted ? 0 : furi_get_tick() + furi_ms_to_ticks(6000);
    return accepted;
}
static uint32_t exit_view(void* context) { UNUSED(context); return VIEW_NONE; }
static bool back(void* context) {
    AssistantSettings* app = context;
    app->help_kind = 0;
    app->last_help[0] = 0;
    view_dispatcher_switch_to_view(app->dispatcher, 0);
    return true;
}
static void select_item(void* context, uint32_t index) {
    AssistantSettings* app = context;
    DeepFlipperSnapshot s;
    deepflipper_snapshot(&s);
    char text[220];
    if(index == 0) {
        deepflipper_set_enabled(!s.enabled);
    } else if(index == 2) {
        app->help_kind = 2;
        if(!s.connected) {
            show_help(app, "Setup web page", "Turn AI session ON.\nCheck module power,\nTX/RX and common GND.\nWait for UART connection.");
        } else {
            if(!s.setup_key[0]) {
                show_help(app, "Setup web page", "Waiting for setup info.\nIf this persists, update\nESP32 firmware too.");
            } else {
                snprintf(text, sizeof(text), "LAN: %s\nAP: %s\n%s\nPass: %s\nLogin: %s",
                         s.ip[0] ? s.ip : "not connected", s.ap ? s.ap_ip : "OFF",
                         s.ap ? s.ap_ssid : "Back to desktop for AI", s.setup_key,
                         s.web_changed ? "custom password" : "same as AP pass");
                show_help(app, "Open http://", text);
            }
        }
        return;
    } else if(index == 9) {
        snprintf(text, sizeof(text), "Device:\n%s\nClient:\n%.18s\n%s\nUART: %lu baud",
                 s.device_id[0] ? s.device_id : "waiting", s.client_id,
                 s.client_id + (strlen(s.client_id) > 18 ? 18 : strlen(s.client_id)),
                 (unsigned long)(s.connected ? s.uart_baud : 0));
        show_help(app, "Module identity", text);
        return;
    } else if(index == 1) {
        app->help_kind = 1;
        snprintf(text, sizeof(text), "UART: %s\nWi-Fi: %s\nIP: %s\nAP: %s\nServer: %s",
                 s.connected ? "connected" : "offline", s.connected ? s.wifi : "unknown",
                 s.connected && s.ip[0] ? s.ip : "none", s.connected && s.ap ? "ON" : "OFF",
                 s.connected ? s.server_state : "unknown");
        show_help(app, "Network status", text);
        return;
    } else if(index == 3) {
        send_command(app, "test_connection", "start");
    } else if(index == 4) {
        send_command(app, "wifi_portal", "start");
    } else if(index == 5) {
        char volume[8];
        snprintf(volume, sizeof(volume), "%u", s.volume >= 100 ? 0 : (s.volume / 10 + 1) * 10);
        send_command(app, "volume", volume);
    } else if(index == 6) {
        send_command(app, "mute", "toggle");
    } else if(index == 7) {
        send_command(app, "microphone", "toggle");
    } else if(index == 8) {
        send_command(app, "led_brightness", "cycle");
    } else if(index == 10) {
        send_command(app,"power_mode",strcmp(s.wifi,"sleeping")==0?"awake":"standby");
    }
    rebuild(app);
}
static void rebuild(AssistantSettings* app) {
    DeepFlipperSnapshot s;
    deepflipper_snapshot(&s);
    if(!app->menu_ready) {
        static const char* const labels[] = {"AI session", "Wi-Fi", "Setup web page", "Test server",
            "Start setup hotspot", "Volume", "Voice mute", "Microphone", "LED brightness", "Device info", "ESP32 standby"};
        for(uint32_t i = 0; i < COUNT_OF(labels); i++) submenu_add_item(app->menu, labels[i], i, select_item, app);
        app->menu_ready = true;
    }
    if(app->feedback_until && (int32_t)(app->feedback_until - furi_get_tick()) > 0)
        submenu_set_header(app->menu, "Module offline or busy");
    else { app->feedback_until = 0; submenu_set_header(app->menu, s.connected ? "UART: connected" : "UART: offline"); }
    char label[64];
    snprintf(label, sizeof(label), "AI session: %s", s.enabled ? "ON" : "OFF");
    submenu_change_item_label(app->menu, 0, label);
    snprintf(label, sizeof(label), "Wi-Fi: %s", s.connected && s.wifi[0] ? s.wifi : "unknown");
    submenu_change_item_label(app->menu, 1, label);
    const char* ip = s.ip[0] ? s.ip : s.ap ? s.ap_ip : "";
    snprintf(label, sizeof(label), "Setup: %s", !s.connected ? "module offline" : ip[0] ? ip : "waiting for IP");
    submenu_change_item_label(app->menu, 2, label);
    snprintf(label, sizeof(label), "Server: %s", s.connected && s.server_state[0] ? s.server_state : "unknown");
    submenu_change_item_label(app->menu, 3, label);
    snprintf(label, sizeof(label), "Volume: %u", s.volume);
    submenu_change_item_label(app->menu, 5, label);
    snprintf(label, sizeof(label), "Voice mute: %s", s.muted ? "ON" : "OFF");
    submenu_change_item_label(app->menu, 6, label);
    snprintf(label, sizeof(label), "Microphone: %s", s.microphone ? "ON" : "OFF");
    submenu_change_item_label(app->menu, 7, label);
    snprintf(label,sizeof(label),"ESP32 standby: %s",strcmp(s.wifi,"sleeping")==0?"ON":"OFF");
    submenu_change_item_label(app->menu,10,label);
}
static void tick(void* context) {
    AssistantSettings* app = context;
    rebuild(app);
    if(app->help_kind) select_item(app, app->help_kind);
}
int32_t deepflipper_settings_app(void* context) {
    UNUSED(context);
    AssistantSettings* app = calloc(1, sizeof(AssistantSettings));
    app->dispatcher = view_dispatcher_alloc();
    app->menu = submenu_alloc(); app->help = widget_alloc();
    view_dispatcher_set_event_callback_context(app->dispatcher, app);
    view_dispatcher_set_tick_event_callback(app->dispatcher, tick, 1000);
    view_set_previous_callback(submenu_get_view(app->menu), exit_view);
    view_dispatcher_add_view(app->dispatcher, 0, submenu_get_view(app->menu));
    view_dispatcher_add_view(app->dispatcher, 1, widget_get_view(app->help));
    view_dispatcher_set_navigation_event_callback(app->dispatcher, back);
    Gui* gui = furi_record_open(RECORD_GUI);
    view_dispatcher_attach_to_gui(app->dispatcher, gui, ViewDispatcherTypeFullscreen);
    rebuild(app); view_dispatcher_switch_to_view(app->dispatcher, 0);
    view_dispatcher_run(app->dispatcher);
    view_dispatcher_remove_view(app->dispatcher, 0); view_dispatcher_remove_view(app->dispatcher, 1);
    submenu_free(app->menu); widget_free(app->help); view_dispatcher_free(app->dispatcher);
    furi_record_close(RECORD_GUI); free(app);
    return 0;
}
