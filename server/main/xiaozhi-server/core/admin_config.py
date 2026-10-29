"""Narrow, atomic edits for the Python admin page. Never return saved secrets."""
import hashlib
import os
from pathlib import Path
import tempfile
from urllib.parse import urlsplit

import yaml

from config.config_loader import merge_local_config
from config.deepflipper_defaults import DEFAULT_PROMPT, TOOL_POLICY, TOOLS


class ConfigConflict(ValueError):
    pass


def configured(value):
    return bool(value) and not str(value).startswith(('REPLACE_', '你的'))


def text(value, name, maximum, required=True):
    if not isinstance(value, str) or len(value) > maximum or any(ord(c) < 32 for c in value):
        raise ValueError(f'{name} 格式或长度不正确')
    value = value.strip()
    if required and not value:
        raise ValueError(f'{name} 不能为空')
    return value


class AdminConfig:
    def __init__(self, root):
        self.root = Path(root)
        self.path = self.root / 'data/.config.yaml'

    def revision(self):
        return hashlib.sha256(self.path.read_bytes() if self.path.exists() else b'').hexdigest()

    def read(self):
        defaults = yaml.safe_load((self.root / 'config.yaml').read_text(encoding='utf-8')) or {}
        custom = yaml.safe_load(self.path.read_text(encoding='utf-8')) if self.path.exists() else {}
        custom = custom or {}
        return custom, merge_local_config(defaults, custom)

    def public(self):
        custom, config = self.read()
        selected = config.get('selected_module', {})
        llm = config.get('LLM', {}).get(selected.get('LLM'), {})
        base = llm.get('base_url', llm.get('url', 'https://api.deepseek.com'))
        try:
            parsed = urlsplit(base)
            if parsed.username or parsed.password or parsed.query or parsed.fragment:
                base = ''
        except ValueError:
            base = ''
        return {
            'revision': self.revision(),
            'base_url': base,
            'model_name': llm.get('model_name', '') if configured(llm.get('model_name')) else '',
            'api_key_set': configured(llm.get('api_key')),
            'prompt': custom.get('prompt', DEFAULT_PROMPT),
            'tool_policy': TOOL_POLICY, 'tools': TOOLS,
            'tool_calling': selected.get('Intent') == 'function_call',
            'asr': selected.get('ASR', ''), 'tts': selected.get('TTS', ''),
            'tts_voice': config.get('TTS', {}).get('EdgeTTS', {}).get('voice', 'zh-CN-XiaoxiaoNeural'),
            'websocket': config.get('server', {}).get('websocket', ''),
        }

    def model(self, payload):
        _, config = self.read()
        old = config.get('LLM', {}).get(config.get('selected_module', {}).get('LLM'), {})
        base = text(payload.get('base_url', old.get('base_url', old.get('url', ''))), 'API 地址', 512)
        parsed = urlsplit(base)
        if parsed.scheme not in ('http', 'https') or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
            raise ValueError('API 地址应为 HTTP/HTTPS Base URL，不包含凭据、查询参数或片段')
        try:
            parsed.port
        except ValueError:
            raise ValueError('API 地址端口无效') from None
        name = text(payload.get('model_name', old.get('model_name', '')), '模型名称', 128, required=False)
        key = text(payload.get('api_key', ''), 'API Key', 4096, required=False)
        if not isinstance(payload.get('clear_api_key', False), bool):
            raise ValueError('清除密钥选项应为布尔值')
        if payload.get('clear_api_key') and key:
            raise ValueError('不能同时填写新密钥并选择清除密钥')
        if not key:
            key = '' if payload.get('clear_api_key') else old.get('api_key', '')
        return {'type': 'openai', 'base_url': base, 'model_name': name, 'api_key': key,
                'max_tokens': old.get('max_tokens', 500)}

    def save(self, payload):
        if payload.get('revision') != self.revision():
            raise ConfigConflict('配置已被其他页面或文件修改，请刷新后重试')
        custom, merged = self.read()
        model = self.model(payload)
        prompt = payload.get('prompt', custom.get('prompt', DEFAULT_PROMPT))
        if not isinstance(prompt, str) or not prompt.strip() or len(prompt) > 16000 or '\0' in prompt:
            raise ValueError('系统提示词不能为空，最多 16000 字符')
        voice = text(payload.get('tts_voice', self.public()['tts_voice']), '音色', 128)
        provider = merged.get('selected_module', {}).get('LLM', 'DeepFlipperLLM')
        old = merged.get('LLM', {}).get(provider, {})
        if old.get('type', 'openai') != 'openai':
            provider, old = 'DeepFlipperLLM', {}
        custom.setdefault('selected_module', {})['LLM'] = provider
        if 'tool_calling' in payload:
            if not isinstance(payload['tool_calling'], bool):
                raise ValueError('工具调用选项应为布尔值')
            custom['selected_module']['Intent'] = 'function_call' if payload['tool_calling'] else 'nointent'
        updated_model = {**old, **model}
        updated_model.pop('url', None)
        custom.setdefault('LLM', {})[provider] = updated_model
        custom['prompt'] = prompt
        custom.setdefault('TTS', {}).setdefault('EdgeTTS', {})['voice'] = voice
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary = tempfile.mkstemp(prefix='.admin-', suffix='.yaml', dir=self.path.parent)
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as out:
                yaml.safe_dump(custom, out, allow_unicode=True, sort_keys=False)
                out.flush()
                os.fsync(out.fileno())
            os.replace(temporary, self.path)
        finally:
            Path(temporary).unlink(missing_ok=True)
        return self.public()
