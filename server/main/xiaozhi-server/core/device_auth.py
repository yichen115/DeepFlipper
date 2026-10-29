"""Shared device authentication for voice and configuration endpoints."""
from core.auth import AuthenticationError


def authorize_device(auth, headers, allowed_devices=(), revoked_devices=()):
    headers = {str(k).lower(): str(v) for k, v in headers.items()}
    device = headers.get("device-id", "")
    client = headers.get("client-id", "")
    authorization = headers.get("authorization", "")
    if not device or not client or not authorization.startswith("Bearer "):
        raise AuthenticationError("Device credentials required")
    if allowed_devices and device not in allowed_devices:
        raise AuthenticationError("Device is not enrolled")
    if device in revoked_devices:
        raise AuthenticationError("Device credential revoked")
    if not auth.verify_token(authorization[7:], client_id=client, username=device):
        raise AuthenticationError("Invalid or expired device credential")
    return device, client
