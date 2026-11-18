"""Minimal client for the psxrecomp TCP debug server (builds with PSX_DEBUG_TOOLS=ON).

The server answers one JSON line per connection, so every call opens its own.

    python3 tools/psx_debug.py frame
    python3 tools/psx_debug.py screenshot path=shot.png   # path is relative to the exe's folder
"""
import json
import socket
import sys


class PsxDebug:
    def __init__(self, port=4370, host="127.0.0.1"):
        self.addr = (host, port)
        self.next_id = 0
        socket.create_connection(self.addr, timeout=5).close()  # fail fast if not listening

    def __call__(self, cmd, **params):
        self.next_id += 1
        params.update(id=self.next_id, cmd=cmd)
        with socket.create_connection(self.addr, timeout=30) as sock:
            sock.sendall((json.dumps(params) + "\n").encode())
            return json.loads(sock.makefile("r").readline())


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    args = dict(a.split("=", 1) for a in sys.argv[2:])
    print(PsxDebug()(sys.argv[1], **args))
