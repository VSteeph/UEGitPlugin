"""In-memory Git LFS locking API server for the isolated test host.

Implements the subset of https://github.com/git-lfs/git-lfs/blob/main/docs/api/locking.md used by `git lfs lock`,
`git lfs unlock` and `git lfs locks`. Requests are attributed to the user named by the `X-Lfs-User` header, so a test
acts as a colleague with `git -c http.extraHeader="X-Lfs-User: Colleague" lfs lock <file>`.
"""

from __future__ import annotations

import itertools
import json
import threading
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

DEFAULT_USER = "Designer"
USER_HEADER = "X-Lfs-User"
CONTENT_TYPE = "application/vnd.git-lfs+json"


class LockStore:
    def __init__(self) -> None:
        self._locks: dict[str, dict] = {}
        self._ids = itertools.count(1)
        self._mutex = threading.Lock()

    def create(self, path: str, owner: str) -> tuple[int, dict]:
        with self._mutex:
            existing = next((lock for lock in self._locks.values() if lock["path"] == path), None)
            if existing:
                return 409, {"lock": existing, "message": "already created lock"}
            lock = {
                "id": str(next(self._ids)),
                "path": path,
                "locked_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "owner": {"name": owner},
            }
            self._locks[lock["id"]] = lock
            return 201, {"lock": lock}

    def list(self, path: str | None, lock_id: str | None) -> list[dict]:
        with self._mutex:
            return [lock for lock in self._locks.values()
                    if (path is None or lock["path"] == path) and (lock_id is None or lock["id"] == lock_id)]

    def delete(self, lock_id: str, user: str, force: bool) -> tuple[int, dict]:
        with self._mutex:
            lock = self._locks.get(lock_id)
            if lock is None:
                return 404, {"message": "lock not found"}
            if lock["owner"]["name"] != user and not force:
                return 403, {"message": f"lock is owned by {lock['owner']['name']}"}
            del self._locks[lock_id]
            return 200, {"lock": lock}


def make_handler(store: LockStore) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format: str, *args) -> None:
            pass

        def _user(self) -> str:
            return self.headers.get(USER_HEADER, DEFAULT_USER).strip() or DEFAULT_USER

        def _body(self) -> dict:
            length = int(self.headers.get("Content-Length") or 0)
            return json.loads(self.rfile.read(length) or b"{}") if length else {}

        def _reply(self, status: int, payload: dict) -> None:
            data = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", CONTENT_TYPE)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self) -> None:
            url = urlparse(self.path)
            if not url.path.endswith("/locks"):
                self._reply(404, {"message": "not found"})
                return
            query = parse_qs(url.query)
            locks = store.list(query.get("path", [None])[0], query.get("id", [None])[0])
            self._reply(200, {"locks": locks, "next_cursor": ""})

        def do_POST(self) -> None:
            path = urlparse(self.path).path
            body = self._body()
            user = self._user()
            if path.endswith("/locks/verify"):
                locks = store.list(None, None)
                self._reply(200, {
                    "ours": [lock for lock in locks if lock["owner"]["name"] == user],
                    "theirs": [lock for lock in locks if lock["owner"]["name"] != user],
                    "next_cursor": "",
                })
            elif path.endswith("/unlock"):
                lock_id = path.rstrip("/").split("/")[-2]
                self._reply(*store.delete(lock_id, user, bool(body.get("force"))))
            elif path.endswith("/locks"):
                self._reply(*store.create(body.get("path", ""), user))
            else:
                self._reply(404, {"message": "not found"})

    return Handler


class LfsLockServer:
    """Serves the locking API on 127.0.0.1 from a background thread for the lifetime of a `with` block."""

    def __init__(self) -> None:
        self._server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(LockStore()))
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    @property
    def url(self) -> str:
        host, port = self._server.server_address[:2]
        return f"http://{host}:{port}"

    def __enter__(self) -> "LfsLockServer":
        self._thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self._server.shutdown()
        self._server.server_close()


if __name__ == "__main__":
    with LfsLockServer() as server:
        print(server.url, flush=True)
        threading.Event().wait()
