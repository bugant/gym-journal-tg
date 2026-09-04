#!/usr/bin/env python3
"""Mock Telegram Bot API server for end-to-end tests.

Serves the small subset of the Telegram Bot API that botlib uses
(getMe, getUpdates, sendMessage, editMessageText, sendPhoto, getFile),
plus control endpoints the test driver can use to enqueue user messages and
observe the bot's replies.

The test driver normally imports this module and runs the server in-process;
the script entry point (`python3 mock_tg.py --portfile ...`) exists for
standalone / manual use.

Pure standard library, no third-party dependencies.
"""

import argparse
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

BOT_USER = {"id": 123456789, "is_bot": True, "first_name": "MockBot",
            "username": "mock_gym_bot"}


class MockTelegram:
    """Shared state behind the mock server (also used directly by the driver
    when the server runs in-process)."""

    def __init__(self, token=None):
        self.token = token              # if set, /bot<token>/<method> enforced
        self.lock = threading.Condition()
        self.updates = []               # full update dicts, update_id ascending
        self.next_update_id = 1
        self.next_message_id = 1000
        self.sent_messages = []         # bot -> "telegram" messages, order
        self.ready = False              # True once getMe was served

    # -- helpers ----------------------------------------------------------

    def _enqueue(self, text, chat_id, from_id, username):
        with self.lock:
            update_id = self.next_update_id
            self.next_update_id += 1
            msg_id = self.next_message_id
            self.next_message_id += 1
            uname = username or "tester"
            update = {
                "update_id": update_id,
                "message": {
                    "message_id": msg_id,
                    "from": {"id": from_id, "is_bot": False,
                             "first_name": "Tester", "username": uname},
                    "chat": {"id": chat_id, "type": "private",
                             "first_name": "Tester", "username": uname},
                    "date": int(time.time()),
                    "text": text,
                },
            }
            self.updates.append(update)
            self.lock.notify_all()
            return update_id

    def _record(self, method, payload):
        with self.lock:
            rec = {"method": method, "seq": len(self.sent_messages) + 1,
                   "ts": int(time.time())}
            rec.update(payload)
            self.sent_messages.append(rec)
            return rec

    def _bump_message_id(self):
        with self.lock:
            self.next_message_id += 1
            return self.next_message_id - 1

    # -- bot-facing API handlers ------------------------------------------

    def getMe(self, query=None):
        self.ready = True
        return {"ok": True, "result": dict(BOT_USER)}

    def getUpdates(self, query):
        offset = int(query.get("offset", ["0"])[0])
        timeout = float(query.get("timeout", ["0"])[0])
        with self.lock:
            pending = [u for u in self.updates if u["update_id"] >= offset]
            if not pending and timeout > 0:
                # Long-poll: block briefly for new arrivals (bot polls t=1s).
                self.lock.wait(timeout)
                pending = [u for u in self.updates if u["update_id"] >= offset]
        return {"ok": True, "result": pending}

    def sendMessage(self, query):
        chat_id = query.get("chat_id", [""])[0]
        text = query.get("text", [""])[0]
        reply_to = query.get("reply_to_message_id", ["0"])[0]
        self._record("sendMessage", {
            "chat_id": chat_id,
            "text": text,
            "reply_to_message_id": int(reply_to) if reply_to.isdigit() else 0,
            "parse_mode": query.get("parse_mode", [""])[0],
        })
        return {"ok": True, "result": {
            "message_id": self._bump_message_id(),
            "chat": {"id": chat_id},
        }}

    def editMessageText(self, query):
        self._record("editMessageText", {
            "chat_id": query.get("chat_id", [""])[0],
            "message_id": query.get("message_id", [""])[0],
            "text": query.get("text", [""])[0],
        })
        return {"ok": True, "result": {
            "message_id": query.get("message_id", ["0"])[0],
            "chat": {"id": query.get("chat_id", ["0"])[0]},
        }}

    def sendPhoto(self, query):
        self._record("sendPhoto", {"chat_id": query.get("chat_id", [""])[0]})
        return {"ok": True, "result": {
            "message_id": self._bump_message_id(),
        }}

    def getFile(self, query):
        self._record("getFile", {"file_id": query.get("file_id", [""])[0]})
        return {"ok": True, "result": {
            "file_id": query.get("file_id", [""])[0],
            "file_path": "mock/files/dummy.dat",
        }}

    # -- control API (driver) ----------------------------------------------

    def enqueue(self, payload):
        """Queue a user message to the bot: {text, chat_id, from_id, username}."""
        return self._enqueue(payload.get("text", ""),
                             payload.get("chat_id", 0),
                             payload.get("from_id", 0),
                             payload.get("username", "tester"))

    def sent_log(self):
        with self.lock:
            return list(self.sent_messages)

    def reset(self):
        # Clear pending updates and the sent log. update/message id counters
        # are deliberately NOT reset: the bot keeps a monotonic getUpdates
        # offset, so re-queued updates must carry ever-increasing ids to be
        # delivered again after a reset.
        with self.lock:
            self.updates = []
            self.sent_messages = []
            self.ready = False
            self.lock.notify_all()


class Handler(BaseHTTPRequestHandler):
    server_version = "MockTG/1.0"

    def log_message(self, fmt, *args):
        if getattr(self.server, "debug", False):
            sys.stderr.write("mock_tg: %s - %s\n"
                             % (self.address_string(), fmt % args))

    def _send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if not length:
            return {}
        try:
            return json.loads(self.rfile.read(length))
        except ValueError:
            return {}

    def do_GET(self):
        self._handle()

    def do_POST(self):
        self._handle()

    def _handle(self):
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)
        st = self.server.state

        if path == "/__ready":
            self._send_json({"ready": st.ready})
            return
        if path == "/__sent":
            self._send_json({"messages": st.sent_log()})
            return
        if path == "/__reset":
            st.reset()
            self._send_json({"ok": True})
            return
        if path == "/__enqueue" and self.command == "POST":
            uid = st.enqueue(self._read_body())
            self._send_json({"ok": True, "update_id": uid})
            return

        # Telegram Bot API: /bot<token>/<method>?opt=val&...
        if path.startswith("/bot"):
            rest = path[len("/bot"):]
            slash = rest.find("/")
            if slash < 0:
                self._send_json({"ok": False, "error_code": 404,
                                 "description": "Bad path"}, 200)
                return
            token, action = rest[:slash], rest[slash + 1:]
            if st.token and token != st.token:
                self._send_json({"ok": False, "error_code": 401,
                                 "description": "Unauthorized"}, 200)
                return
            handler = getattr(st, action, None)
            if handler is None:
                self._send_json({"ok": False, "error_code": 404,
                                 "description": "Not Found"}, 200)
                return
            self._send_json(handler(query))
            return

        self._send_json({"error": "not found"}, 404)


def make_server(state=None, port=0, token=None, debug=False):
    """Build a ThreadingHTTPServer serving `state` (a MockTelegram). Returns
    (server, state)."""
    if state is None:
        state = MockTelegram(token=token)
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.state = state
    server.debug = debug
    return server, state


def main():
    ap = argparse.ArgumentParser(description="Mock Telegram Bot API server")
    ap.add_argument("--port", type=int, default=0,
                    help="listen port (0 = ephemeral)")
    ap.add_argument("--portfile", default=None,
                    help="write the chosen port to this file")
    ap.add_argument("--token", default=None,
                    help="if set, reject other bot tokens")
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()

    server, state = make_server(port=args.port, token=args.token,
                                debug=args.debug)
    port = server.server_address[1]
    if args.portfile:
        with open(args.portfile, "w") as f:
            f.write(str(port))
    print("mock_tg listening on 127.0.0.1:%d" % port, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()