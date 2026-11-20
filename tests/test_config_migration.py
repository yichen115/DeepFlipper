from pathlib import Path
import sys
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / 'server/main/xiaozhi-server'
sys.path.insert(0, str(SERVER))
from config.config_loader import merge_local_config


def defaults():
    return yaml.safe_load((SERVER / 'config.yaml').read_text(encoding='utf-8'))


class ConfigMigrationTests(unittest.TestCase):
    def test_default_profile_is_the_lightweight_local_stack(self):
        config = merge_local_config(defaults(), {})
        self.assertEqual(config['selected_module'], {
            'VAD': 'SileroVAD', 'ASR': 'SherpaASR', 'LLM': 'DeepSeekLLM',
            'TTS': 'LocalTTS', 'Memory': 'nomem', 'Intent': 'function_call'})
        asr = config['ASR']['SherpaASR']
        self.assertEqual((asr['type'], asr['num_threads'], asr['language']),
                         ('sherpa_onnx_local', 2, 'auto'))
        self.assertEqual(config['Intent']['function_call']['functions'], [])
        self.assertEqual(config['TTS']['EdgeTTS']['type'], 'edge')
        self.assertIn('鲸鱼娘', config['prompt'])
        self.assertEqual(config['wakeup_words'], [])
        self.assertFalse(config['enable_wakeup_words_response_cache'])

    def test_old_funasr_selection_switches_to_sherpa_and_keeps_language(self):
        custom = {'selected_module': {'ASR': 'FunASR'}, 'ASR': {'FunASR': {'language': 'zh'}}}
        config = merge_local_config(defaults(), custom)
        self.assertEqual(config['selected_module']['ASR'], 'SherpaASR')
        self.assertEqual(config['ASR']['SherpaASR']['type'], 'sherpa_onnx_local')
        self.assertEqual(config['ASR']['SherpaASR']['language'], 'zh')

    def test_removed_provider_type_and_missing_entry_fall_back(self):
        for custom in ({'selected_module': {'ASR': 'FunASR'},
                        'ASR': {'FunASR': {'type': 'fun_local', 'model_dir': 'models/SenseVoiceSmall'}}},
                       {'selected_module': {'ASR': 'NoSuchASR'}}):
            with self.subTest(custom=custom):
                config = merge_local_config(defaults(), custom)
                self.assertEqual(config['selected_module']['ASR'], 'SherpaASR')
                self.assertEqual(config['ASR']['SherpaASR']['language'], 'auto')

    def test_explicit_sherpa_language_is_not_overridden(self):
        custom = {'selected_module': {'ASR': 'FunASR'},
                  'ASR': {'FunASR': {'language': 'zh'}, 'SherpaASR': {'language': 'en'}}}
        config = merge_local_config(defaults(), custom)
        self.assertEqual(config['ASR']['SherpaASR']['language'], 'en')

    def test_valid_selection_is_untouched(self):
        custom = {'ASR': {'SherpaASR': {'language': 'zh'}}}
        config = merge_local_config(defaults(), custom)
        self.assertEqual(config['selected_module']['ASR'], 'SherpaASR')
        self.assertEqual(config['ASR']['SherpaASR']['language'], 'zh')


if __name__ == '__main__':
    unittest.main()
