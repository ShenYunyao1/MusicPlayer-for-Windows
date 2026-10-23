"""Check the native client's HTTP/TLS wire protocol without real credentials."""

import argparse
import json
import socket
import subprocess
import tempfile
import threading


def check_protocol(executable, scheme):
    with socket.socket() as listener, tempfile.TemporaryDirectory(prefix="musicplayer2-tls-") as cache:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(15)
        received = []
        failures = []

        def serve():
            try:
                with listener.accept()[0] as connection:
                    connection.settimeout(10)
                    data = connection.recv(16384)
                    received.append(data)
                    if data.startswith(b"GET "):
                        while b"\r\n\r\n" not in data:
                            part = connection.recv(16384)
                            if not part:
                                break
                            data += part
                        received[0] = data
                        connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n[]")
            except Exception as error:
                failures.append(error)

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        request = {"url": f"{scheme}://127.0.0.1:{listener.getsockname()[1]}",
                   "key": "tls-fixture-key", "cache": cache}
        result = subprocess.run([executable, "--check"], input=json.dumps(request),
                                capture_output=True, text=True, encoding="utf-8", timeout=20)
        thread.join(timeout=2)
        assert not thread.is_alive(), "protocol listener did not stop"
        assert not failures, failures
        assert result.returncode == 0, result.stderr
        assert received, "client did not send a request"
        response = json.loads(result.stdout)
        if scheme == "https":
            assert received[0].startswith(b"\x16\x03"), "HTTPS sent plaintext instead of a TLS ClientHello"
            assert b"tls-fixture-key" not in received[0], "HTTPS exposed the API Key in plaintext"
            assert not response["success"], "incomplete TLS handshake was accepted"
        else:
            assert received[0].startswith(b"GET /api/v2/playlists HTTP/1.1\r\n"), "unexpected HTTP request"
            assert b"X-API-Key: tls-fixture-key\r\n" in received[0], "HTTP fixture authentication missing"
            assert response["success"], response
        print(f"PASS {scheme.upper()} wire protocol", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    args = parser.parse_args()
    check_protocol(args.exe, "http")
    check_protocol(args.exe, "https")


if __name__ == "__main__":
    main()
