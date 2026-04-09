"""Python API backend for the mixed fixture project."""

from typing import List


class Server:
    def __init__(self, port: int) -> None:
        self.port = port

    def start(self) -> None:
        print(f"server listening on :{self.port}")

    def routes(self) -> List[str]:
        return ["/api/users", "/api/health"]


def bootstrap() -> Server:
    server = Server(port=8080)
    server.start()
    return server
