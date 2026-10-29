"""Exercise real executor routing without importing the voice provider stack."""
import importlib.util
import json
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import AsyncMock, patch

ROOT = Path(__file__).resolve().parents[1]
DEVICE = ROOT / 'server/main/xiaozhi-server/core/providers/tools/device_mcp'


def load_executor(call):
    prefix = 'core.providers.tools.device_mcp'
    base = types.ModuleType('core.providers.tools.base')
    base.ToolType = base.ToolDefinition = base.ToolExecutor = object
    register = types.ModuleType('plugins_func.register')
    register.Action = types.SimpleNamespace(ERROR='error', NOTFOUND='not_found', REQLLM='llm')
    register.ActionResponse = lambda **kwargs: types.SimpleNamespace(**kwargs)
    handler = types.ModuleType(prefix + '.mcp_handler')
    handler.call_mcp_tool = call
    jobs_spec = importlib.util.spec_from_file_location(prefix + '.deepflipper_jobs', DEVICE / 'deepflipper_jobs.py')
    jobs = importlib.util.module_from_spec(jobs_spec)
    jobs_spec.loader.exec_module(jobs)
    spec = importlib.util.spec_from_file_location(prefix + '.tested_executor', DEVICE / 'mcp_executor.py')
    module = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, {'core.providers.tools.base': base,
                                 'plugins_func.register': register,
                                 prefix + '.mcp_handler': handler,
                                 prefix + '.deepflipper_jobs': jobs}):
        spec.loader.exec_module(module)
    return module.DeviceMCPExecutor


class ExecutorTests(unittest.IsolatedAsyncioTestCase):
    async def test_new_native_tools_wait_for_final_device_result(self):
        final = {'ok': True, 'state': 'done', 'stopped': True, 'target_state_verified': False}
        for tool in ('flipper_run_saved', 'flipper_app_open', 'flipper_list_files', 'flipper_system_info'):
            with self.subTest(tool=tool):
                calls = []
                async def call(conn, client, name, args):
                    calls.append((name, json.loads(args)))
                    return json.dumps({'job': 12, 'state': 'pending', 'wait_ms': 30000}
                                      if len(calls) == 1 else final)
                executor = load_executor(call)
                conn = types.SimpleNamespace(mcp_client=types.SimpleNamespace(is_ready=AsyncMock(return_value=True)))
                result = await executor(conn).execute(conn, tool, {'path': '/ext/infrared/test.ir'})
                self.assertEqual(json.loads(result.result), final)
                self.assertEqual(calls[1], ('flipper_job_get', {'job': 12}))
                self.assertEqual(len(calls), 2)

    async def test_job_management_does_not_recursively_poll(self):
        for tool in ('flipper_job_get', 'flipper_job_cancel'):
            call = AsyncMock(return_value='{"job":12,"state":"pending"}')
            executor = load_executor(call)
            conn = types.SimpleNamespace(mcp_client=types.SimpleNamespace(is_ready=AsyncMock(return_value=True)))
            result = await executor(conn).execute(conn, tool, {'job': 12})
            self.assertEqual(json.loads(result.result)['state'], 'pending')
            call.assert_awaited_once()
