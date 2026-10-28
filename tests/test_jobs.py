import asyncio
import importlib.util
import json
from pathlib import Path
import unittest

p=Path(__file__).resolve().parents[1]/'server/main/xiaozhi-server/core/providers/tools/device_mcp/deepflipper_jobs.py'
spec=importlib.util.spec_from_file_location('deepflipper_jobs',p)
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class JobTests(unittest.IsolatedAsyncioTestCase):
    async def test_waits_for_actual_result(self):
        results=iter(['{"state":"pending"}','{"ok":true,"state":"done","protocol":"ISO14443A"}'])
        async def call(name,args):
            self.assertEqual(name,'flipper_job_get')
            return next(results)
        result=await module.await_flipper_job('{"job":3,"state":"pending"}',call)
        self.assertEqual(json.loads(result)['protocol'],'ISO14443A')

    async def test_no_free_module_job(self):
        async def call(*args):
            self.fail('busy job must not be polled')
        result=await module.await_flipper_job('{"job":0,"state":"pending"}',call)
        self.assertEqual(json.loads(result)['code'],'module_busy')

    async def test_timeout_requests_cancellation(self):
        calls=[]
        async def call(name,args):
            calls.append(name)
            return '{"state":"pending"}'
        result=await module.await_flipper_job('{"job":3,"state":"pending"}',call,timeout=0.01)
        self.assertEqual(json.loads(result)['code'],'timeout')
        self.assertIn('flipper_job_cancel',calls)

    async def test_completed_errors_are_not_replaced_by_success(self):
        async def call(*args):
            return '{"ok":false,"code":"busy"}'
        result=await module.await_flipper_job('{"job":1,"state":"pending"}',call)
        self.assertFalse(json.loads(result)['ok'])

    async def test_rpc_query_success_does_not_mean_hardware_success(self):
        async def call(*args):
            return '{"ok":true,"state":"timeout","job":1}'
        result=await module.await_flipper_job('{"job":1,"state":"pending"}',call)
        self.assertEqual(json.loads(result)['code'],'timeout')
        self.assertFalse(json.loads(result)['ok'])


if __name__=='__main__':
    unittest.main()
