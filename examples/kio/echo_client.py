import argparse
import socket
import sys


def check_echo(connection, payload):
    connection.sendall(payload)
    reply = bytearray()
    while len(reply) < len(payload):
        chunk = connection.recv(min(4096, len(payload) - len(reply)))
        if not chunk:
            raise RuntimeError(
                f"Connection closed after {len(reply)} of {len(payload)} bytes"
            )
        reply.extend(chunk)
    if reply != payload:
        raise RuntimeError("Echo does not match the sent payload")


def main():
    parser = argparse.ArgumentParser(description="Test the kio TCP echo server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=5.0)
    args = parser.parse_args()
    if args.repeat < 1 or args.timeout <= 0:
        parser.error("--repeat and --timeout must be positive")

    payloads = (b"Hello, kio!\n", bytes(range(256)) * 64)
    try:
        with socket.create_connection((args.host, args.port), args.timeout) as connection:
            for iteration in range(args.repeat):
                for payload in payloads:
                    check_echo(connection, payload)
                    print(f"Round {iteration + 1}: OK ({len(payload)} bytes)")
            connection.shutdown(socket.SHUT_WR)
            if connection.recv(1):
                raise RuntimeError("Unexpected bytes after the final echo")
    except (OSError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    print("PASS: all echoes match; server closed after EOF")
    return 0


if __name__ == "__main__":
    sys.exit(main())
