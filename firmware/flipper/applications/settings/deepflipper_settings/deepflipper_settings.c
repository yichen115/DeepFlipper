#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/widget.h>
#include <deepflipper/deepflipper.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ViewDispatcher* dispatcher;
    Submenu* menu;
    TextInput* input;
    Widget* help;
    char buffer[240];
    const char* field;
    bool menu_ready;
    uint32_t feedback_until;
} AssistantSettings;

static void rebuild(AssistantSettings* app);

static void show_help(AssistantSettings* app, const char* title, const char* text) {
    widget_reset(app->help);
    widget_add_string_element(app->help, 64, 2, AlignCenter, AlignTop, FontPrimary, title);
    widget_add_string_multiline_element(
        app->help, 4, 18, AlignLeft, AlignTop, FontSecondary, text);
    view_dispatcher_switch_to_view(app->dispatcher, 2);
}

static bool send_command(AssistantSettings* app, const char* field, const char* value) {
    bool accepted = deepflipper_module_command(field, value);
    app->feedback_until = accepted ? 0 : furi_get_tick() + furi_ms_to_ticks(6000);
    return accepted;
}

static uint32_t exit_view(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static bool input_back(void* context) {
    AssistantSettings* app = context;
    memset(app->buffer, 0, sizeof(app->buffer));
    view_dispatcher_switch_to_view(app->dispatcher, 0);
    return true;
}

static void input_done(void* context) {
    AssistantSettings* app = context;
    send_command(app, app->field, app->buffer);
    memset(app->buffer, 0, sizeof(app->buffer));
    rebuild(app);
    view_dispatcher_switch_to_view(app->dispatcher, 0);
}

static void select_item(void* context, uint32_t index) {
    AssistantSettings* app = context;
    if(index == 11 || index > 13) return;
    DeepFlipperSnapshot s;
    deepflipper_snapshot(&s);
    if(index == 12 || index == 13) {
        char identity[120];
        snprintf(identity, sizeof(identity), "Device: %s\nClient ID:\n%.18s\n%s",
                 s.device_id[0] ? s.device_id : "waiting", s.client_id, s.client_id + (strlen(s.client_id) > 18 ? 18 : strlen(s.client_id)));
        show_help(app, "Module identity", identity);
        return;
    }
    if(index == 0) {
        app->feedback_until = 0;
        deepflipper_set_enabled(!s.enabled);
    } else if(index == 1) {
        char volume[8];
        snprintf(volume, sizeof(volume), "%u", s.volume >= 100 ? 0 : (s.volume / 10 + 1) * 10);
        send_command(app, "volume", volume);
    } else if(index == 2) {
        send_command(app, "mute", "toggle");
    } else if(index == 3) {
        send_command(app, "microphone", "toggle");
    } else if(index == 8) {
        if(send_command(app, "wifi_portal", "start")) {
            rebuild(app);
            show_help(app, "Phone Wi-Fi setup",
                      "Join DeepFlipper-XXXX\non your phone, then open\nhttp://192.168.4.1\nSelect a 2.4 GHz Wi-Fi");
            return;
        }
    } else if(index == 9) {
        send_command(app, "led_brightness", "cycle");
    } else if(index == 10) {
        send_command(app, "test_connection", "start");
    } else {
        static const char* const fields[] = {"ssid", "wifi_password", "url", "token"};
        app->field = fields[index - 4];
        memset(app->buffer, 0, sizeof(app->buffer));
        bool secret = index == 5 || index == 7;
        if(index == 4) snprintf(app->buffer, sizeof(app->buffer), "%s", s.ssid);
        if(index == 6) snprintf(app->buffer, sizeof(app->buffer), "%s", s.server);
        text_input_set_header_text(app->input, app->field);
        text_input_set_password(app->input, secret);
        text_input_set_symbols(app->input, true);
        text_input_set_minimum_length(app->input, secret ? 0 : 1);
        text_input_set_result_callback(
            app->input,
            input_done,
            app,
            app->buffer,
            index == 4 ? 33 :
            index == 5 ? 65 :
                         sizeof(app->buffer),
            true);
        view_dispatcher_switch_to_view(app->dispatcher, 1);
        return;
    }
    rebuild(app);
}

static void rebuild(AssistantSettings* app) {
    DeepFlipperSnapshot s;
    deepflipper_snapshot(&s);
    // Build once. Replacing labels preserves both selection and the scroll window.
    if(!app->menu_ready) {
        static const char* const labels[] = {
            "AI session", "Volume", "Voice mute", "Microphone", "Wi-Fi SSID",
            "Wi-Fi password", "Server URL (WS/WSS)", "Device token", "Phone Wi-Fi setup",
            "Cycle LED brightness", "Apply voice config", "Status", "Device", "Client"};
        for(uint32_t i = 0; i < COUNT_OF(labels); i++) {
            submenu_add_item(app->menu, labels[i], i, select_item, app);
        }
        app->menu_ready = true;
    }
    if(app->feedback_until && (int32_t)(app->feedback_until - furi_get_tick()) > 0) {
        submenu_set_header(app->menu, "Module offline or busy");
    } else {
        app->feedback_until = 0;
        submenu_set_header(app->menu, s.connected ? "AI: module connected" : "AI: module offline");
    }
    char label[64];
    snprintf(label, sizeof(label), "AI session: %s", s.enabled ? "ON" : "OFF");
    submenu_change_item_label(app->menu, 0, label);
    snprintf(label, sizeof(label), "Volume: %u (step 10)", s.volume);
    submenu_change_item_label(app->menu, 1, label);
    snprintf(label, sizeof(label), "Voice mute: %s", s.muted ? "ON" : "OFF");
    submenu_change_item_label(app->menu, 2, label);
    snprintf(label, sizeof(label), "Microphone: %s", s.microphone ? "ON" : "OFF");
    submenu_change_item_label(app->menu, 3, label);
    submenu_change_item_label(app->menu, 11, s.notice[0] ? s.notice : "No pending command");
    submenu_change_item_label(app->menu, 12, s.device_id[0] ? s.device_id : "Device ID: waiting");
    submenu_change_item_label(app->menu, 13, s.client_id[0] ? s.client_id : "Client ID: waiting");
}

static void tick(void* context) {
    AssistantSettings* app = context;
    rebuild(app);
}

int32_t deepflipper_settings_app(void* context) {
    UNUSED(context);
    AssistantSettings* app = calloc(1, sizeof(AssistantSettings));
    app->dispatcher = view_dispatcher_alloc();
    app->menu = submenu_alloc();
    app->input = text_input_alloc();
    app->help = widget_alloc();
    widget_add_string_element(
        app->help, 64, 2, AlignCenter, AlignTop, FontPrimary, "Phone Wi-Fi setup");
    widget_add_string_multiline_element(
        app->help, 4, 18, AlignLeft, AlignTop, FontSecondary,
        "Join DeepFlipper-XXXX\non your phone, then open\nhttp://192.168.4.1\nSelect a 2.4 GHz Wi-Fi");
    view_dispatcher_set_event_callback_context(app->dispatcher, app);
    view_dispatcher_set_tick_event_callback(app->dispatcher, tick, 1000);
    view_set_previous_callback(submenu_get_view(app->menu), exit_view);
    view_dispatcher_add_view(app->dispatcher, 0, submenu_get_view(app->menu));
    view_dispatcher_add_view(app->dispatcher, 1, text_input_get_view(app->input));
    view_dispatcher_add_view(app->dispatcher, 2, widget_get_view(app->help));
    view_dispatcher_set_navigation_event_callback(app->dispatcher, input_back);
    view_set_previous_callback(text_input_get_view(app->input), NULL);
    view_set_previous_callback(widget_get_view(app->help), NULL);
    Gui* gui = furi_record_open(RECORD_GUI);
    view_dispatcher_attach_to_gui(app->dispatcher, gui, ViewDispatcherTypeFullscreen);
    rebuild(app);
    view_dispatcher_switch_to_view(app->dispatcher, 0);
    view_dispatcher_run(app->dispatcher);
    view_dispatcher_remove_view(app->dispatcher, 1);
    view_dispatcher_remove_view(app->dispatcher, 2);
    view_dispatcher_remove_view(app->dispatcher, 0);
    text_input_free(app->input);
    widget_free(app->help);
    submenu_free(app->menu);
    view_dispatcher_free(app->dispatcher);
    furi_record_close(RECORD_GUI);
    memset(app->buffer, 0, sizeof(app->buffer));
    free(app);
    return 0;
}
