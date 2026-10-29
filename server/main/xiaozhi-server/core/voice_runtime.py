"""Keep the admin page available while voice models start or settings are incomplete."""
import asyncio
import os
from pathlib import Path

from config.config_loader import get_project_dir, load_config
from core.admin_config import configured
from core.websocket_server import WebSocketServer


class VoiceRuntime:
    def __init__(self):
        self.state = 'starting'
        self.error = ''
        self.server = None
        self.voice_task = None
        self.apply_task = None
        self.pending = False
        self.closed = False

    def apply(self):
        self.pending = True
        if not self.apply_task or self.apply_task.done():
            self.apply_task = asyncio.create_task(self._apply())

    async def _stop_voice(self):
        if self.voice_task:
            self.voice_task.cancel()
            try:
                await asyncio.wait_for(self.voice_task, 15)
            except asyncio.CancelledError:
                pass
            except Exception:
                # Drain a stopped/failed listener before applying the next config.
                pass
            self.voice_task = None
        self.server = None

    async def _apply(self):
        while self.pending and not self.closed:
            self.pending = False
            self.state, self.error = 'applying', ''
            await self._stop_voice()
            try:
                config = await load_config(force=True)
                config['server']['auth_key'] = os.environ.get('DEEPFLIPPER_AUTH_KEY') or config['server'].get('auth_key', '')
                llm = config.get('LLM', {}).get(config.get('selected_module', {}).get('LLM'), {})
                if not llm or (llm.get('type', 'openai') == 'openai' and (
                    not configured(llm.get('api_key')) or not configured(llm.get('model_name')))):
                    self.state = 'needs_config'
                    continue
                asr = config.get('ASR', {}).get(config.get('selected_module', {}).get('ASR'), {})
                vad = config.get('VAD', {}).get(config.get('selected_module', {}).get('VAD'), {})
                if asr.get('type') == 'fun_local':
                    model = Path(get_project_dir()) / asr.get('model_dir', 'models/SenseVoiceSmall') / 'model.pt'
                    if not model.is_file():
                        raise FileNotFoundError('Local ASR model missing')
                if vad.get('type') == 'silero':
                    model = Path(get_project_dir()) / vad['model_dir'] / 'src/silero_vad/data/silero_vad.onnx'
                    if not model.is_file():
                        raise FileNotFoundError('Local VAD model missing')
                server = await asyncio.to_thread(WebSocketServer, config)
                if self.closed:
                    break
                self.server = server
                self.voice_task = asyncio.create_task(server.start())
                self.voice_task.add_done_callback(lambda task: server.started.set())
                await asyncio.wait_for(server.started.wait(), 15)
                if self.voice_task.done():
                    self.voice_task.result()
                self.state = 'ready'
                self.voice_task.add_done_callback(self._voice_done)
            except asyncio.CancelledError:
                raise
            except Exception as error:
                self.state = 'error'
                self.error = 'missing_speech_models' if isinstance(error, FileNotFoundError) else type(error).__name__

    def _voice_done(self, task):
        if task.cancelled() or self.closed:
            return
        error = task.exception()
        if error:
            self.state, self.error = 'error', type(error).__name__

    def status(self):
        devices = []
        for handler in list(getattr(self.server, 'connections', ())):
            headers = handler.headers or {}
            client = getattr(handler, 'mcp_client', None)
            devices.append({'device_id': headers.get('device-id', ''),
                            'client_id': headers.get('client-id', ''),
                            'tools': list(getattr(client, 'tools', {}))})
        return {'state': self.state, 'error': self.error, 'devices': devices}

    async def close(self):
        self.closed = True
        self.pending = False
        if self.apply_task and not self.apply_task.done():
            self.apply_task.cancel()
            try:
                await self.apply_task
            except asyncio.CancelledError:
                pass
        await self._stop_voice()
