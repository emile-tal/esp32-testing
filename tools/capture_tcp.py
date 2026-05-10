"""Connect to the ESP32 mic over TCP, read one framed PCM blob, write WAV."""

import socket
import sys
import wave

DEFAULT_PORT = 3333
PCM_OUT = "recording.pcm"
WAV_OUT = "recording.wav"


def read_until_newline(sock):
    buf = bytearray()
    while True:
        chunk = sock.recv(1)
        if not chunk:
            raise RuntimeError("socket closed before header newline")
        if chunk == b"\n":
            return buf.decode("ascii")
        buf += chunk
        if len(buf) > 128:
            raise RuntimeError(f"header too long: {buf!r}")


def recv_exact(sock, n):
    buf = bytearray(n)
    view = memoryview(buf)
    got = 0
    while got < n:
        n_read = sock.recv_into(view[got:])
        if n_read == 0:
            raise RuntimeError(f"socket closed after {got}/{n} bytes")
        got += n_read
    return bytes(buf)


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <esp_ip> [port]", file=sys.stderr)
        sys.exit(1)

    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    print(f"Connecting to {host}:{port} ...")
    with socket.create_connection((host, port), timeout=30) as sock:
        header = read_until_newline(sock)
        print(f"Got header: {header}")

        parts = header.split()
        if len(parts) != 4 or parts[0] != "PCM16":
            raise RuntimeError(f"unexpected header: {header!r}")
        sample_rate = int(parts[1])
        channels = int(parts[2])
        samples = int(parts[3])
        byte_count = samples * 2 * channels

        pcm = recv_exact(sock, byte_count)
        print(f"Wrote {len(pcm)} bytes to {PCM_OUT}")

    with open(PCM_OUT, "wb") as f:
        f.write(pcm)

    with wave.open(WAV_OUT, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm)
    print(f"Wrote {WAV_OUT} ({sample_rate} Hz, {channels} ch, {samples} samples)")


if __name__ == "__main__":
    main()
