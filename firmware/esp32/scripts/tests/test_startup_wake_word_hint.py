"""Run the real startup chat hint against host-side application dependencies."""

import os
import shlex
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def definition(source, signature, occurrence=0):
    start = source.index(signature)
    for _ in range(occurrence):
        start = source.index(signature, start + len(signature))
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class StartupWakeWordHintTests(unittest.TestCase):
    def compile_and_run(self, driver):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("requires a host C++23 compiler")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            source = path / "startup_wake_word_hint.cc"
            source.write_text(driver, encoding="utf-8")
            executable = path / "startup_wake_word_hint"
            compiled = subprocess.run(
                compiler + ["-std=c++23", str(source), "-o", str(executable)],
                capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


    def test_expiry_removes_only_the_hint_on_oled_classic_lcd_and_wechat_lcd(self):
        platform = r'''
            #include <algorithm>
            #include <cassert>
            #include <cstdint>
            #include <cstring>
            #include <string>
            #include <vector>
            struct lv_obj_t {
                std::string text;
                const char* role = nullptr;
                lv_obj_t* parent = nullptr;
                std::vector<lv_obj_t*> children;
                bool hidden = false;
                bool deleted = false;
            };
            constexpr int LV_OBJ_FLAG_HIDDEN = 1;
            struct DisplayLockGuard { explicit DisplayLockGuard(void*) {} };
            const char* lv_label_get_text(lv_obj_t* obj) { return obj->text.c_str(); }
            void lv_label_set_text(lv_obj_t* obj, const char* text) { obj->text = text; }
            void lv_anim_delete(lv_obj_t*, void*) {}
            void lv_obj_add_flag(lv_obj_t* obj, int) { obj->hidden = true; }
            void lv_obj_remove_flag(lv_obj_t* obj, int) { obj->hidden = false; }
            uint32_t lv_obj_get_child_cnt(lv_obj_t* obj) { return obj->children.size(); }
            lv_obj_t* lv_obj_get_child(lv_obj_t* obj, uint32_t i) { return obj->children.at(i); }
            void* lv_obj_get_user_data(lv_obj_t* obj) { return const_cast<char*>(obj->role); }
            void lv_obj_del(lv_obj_t* obj) {
                obj->deleted = true;
                auto& siblings = obj->parent->children;
                siblings.erase(std::find(siblings.begin(), siblings.end(), obj));
            }
            class TestDisplay {
            public:
                lv_obj_t* chat_message_label_ = nullptr;
                lv_obj_t* content_right_ = nullptr;
                lv_obj_t* bottom_bar_ = nullptr;
                lv_obj_t* content_ = nullptr;
                lv_obj_t* emoji_label_ = nullptr;
                void RemoveChatMessage(const char* role, const char* content);
            };
        '''
        single_slot_checks = r'''
            int main() {
                TestDisplay display;
                display.RemoveChatMessage("assistant", "Hint");
                lv_obj_t label, bar;
                display.chat_message_label_ = &label;
                display.content_right_ = &bar;
                display.bottom_bar_ = &bar;
                label.text = "New reply";
                display.RemoveChatMessage("assistant", "Hint");
                assert(label.text == "New reply");
                assert(!bar.hidden);
                label.text = "Hint";
                display.RemoveChatMessage("assistant", "Hint");
                assert(label.text.empty());
                assert(bar.hidden);
                display.content_right_ = nullptr;
                display.bottom_bar_ = nullptr;
                label.text = "Hint";
                display.RemoveChatMessage("assistant", "Hint");
                assert(label.text.empty());
                label.text = "Network error";
                display.RemoveChatMessage("assistant", "Hint");
                assert(label.text == "Network error");
            }
        '''
        wechat_checks = r'''
            int main() {
                TestDisplay display;
                display.RemoveChatMessage("assistant", "Hint");
                lv_obj_t content, hint, hint_label, latest, latest_label, user, user_label, emoji;
                hint.role = "assistant";
                hint_label.text = "Hint";
                hint.children = {&hint_label};
                hint.parent = &content;
                latest.role = "assistant";
                latest_label.text = "New reply";
                latest.children = {&latest_label};
                latest.parent = &content;
                user.role = "user";
                user_label.text = "Hint";
                user.children = {&user_label};
                user.parent = &content;
                content.children = {&user, &hint, &latest};
                emoji.hidden = true;
                display.content_ = &content;
                display.emoji_label_ = &emoji;
                display.chat_message_label_ = &latest_label;
                display.RemoveChatMessage("assistant", "Hint");
                assert(hint.deleted);
                assert(!user.deleted && !latest.deleted);
                assert(content.children.size() == 2);
                assert(display.chat_message_label_ == &latest_label);
                assert(emoji.hidden);
                display.RemoveChatMessage("assistant", "Hint");
                assert(content.children.size() == 2);

                // Removing the last bubble restores the idle logo.
                content.children = {&hint};
                hint.deleted = false;
                display.chat_message_label_ = &hint_label;
                display.RemoveChatMessage("assistant", "Hint");
                assert(content.children.empty());
                assert(display.chat_message_label_ == nullptr);
                assert(!emoji.hidden);
            }
        '''
        for name, file, occurrence, checks in (
            ("oled", "oled_display.cc", 0, single_slot_checks),
            ("classic_lcd", "lcd_display.cc", 1, single_slot_checks),
            ("wechat_lcd", "lcd_display.cc", 0, wechat_checks),
        ):
            with self.subTest(display=name):
                source = (ROOT / "main/display" / file).read_text()
                signature = "void " + ("OledDisplay" if name == "oled" else "LcdDisplay") + "::RemoveChatMessage("
                method = definition(source, signature, occurrence)
                method = method.replace("OledDisplay::", "TestDisplay::").replace("LcdDisplay::", "TestDisplay::")
                self.compile_and_run(textwrap.dedent(platform) + method + textwrap.dedent(checks))
