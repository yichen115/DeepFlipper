from typing import Dict, Any

from core.handle.textMessageHandler import TextMessageHandler
from core.handle.textMessageType import TextMessageType

TAG = __name__


class ServerTextMessageHandler(TextMessageHandler):
    """Upstream manager-api control messages (update_config/restart).

    DeepFlipper has no Java manager; configuration changes go through the local
    admin page, so these messages are ignored.
    """

    @property
    def message_type(self) -> TextMessageType:
        return TextMessageType.SERVER

    async def handle(self, conn, msg_json: Dict[str, Any]) -> None:
        conn.logger.bind(tag=TAG).debug("Ignoring server control message")
