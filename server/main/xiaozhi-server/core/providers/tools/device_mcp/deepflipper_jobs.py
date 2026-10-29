"""Await actual module jobs without blocking the voice socket or the model loop."""
import asyncio
import json


async def await_flipper_job(result, call, timeout=13.0):
    try:
        job = json.loads(result) if isinstance(result, str) else result
    except (ValueError, TypeError):
        return result
    if not isinstance(job, dict) or job.get("state") != "pending":
        return result
    job_id = job.get("job")
    if type(job_id) is not int or job_id < 1:
        return json.dumps({"ok": False, "code": "module_busy"})
    wait_ms = job.get("wait_ms")
    if type(wait_ms) is int and 0 < wait_ms <= 30000:
        timeout = min(30.0, max(timeout, wait_ms / 1000))
    deadline = asyncio.get_running_loop().time() + timeout
    try:
        while asyncio.get_running_loop().time() < deadline:
            remaining = deadline - asyncio.get_running_loop().time()
            response = await asyncio.wait_for(
                call("flipper_job_get", json.dumps({"job": job_id})), remaining
            )
            status = json.loads(response)
            if not isinstance(status, dict):
                raise ValueError("Invalid module result")
            if status.get("state") != "pending":
                if status.get("state") in ("timeout", "cancelled", "busy", "not_found"):
                    status["ok"] = False
                    status["code"] = status["state"]
                    return json.dumps(status)
                return response
            await asyncio.sleep(0.15)
    except asyncio.CancelledError:
        try:
            await asyncio.wait_for(call("flipper_job_cancel", json.dumps({"job": job_id})), 1)
        finally:
            raise
    except (TimeoutError, ValueError, TypeError):
        pass
    try:
        await asyncio.wait_for(call("flipper_job_cancel", json.dumps({"job": job_id})), 1)
    except Exception:
        pass
    return json.dumps({"ok": False, "code": "timeout", "job": job_id})
