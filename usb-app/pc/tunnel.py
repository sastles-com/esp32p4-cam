"""The PC end of the serial tunnel (usb-app/firmware/main/tunnel.c).

The board's console UART carries frames that each belong to a numbered channel;
a channel is one TCP connection on the board to its own 127.0.0.1 (:80 the API
and page, :81 MJPEG). Everything on the UART that is not a frame is the board's
log, and is printed as such.

    A5 5A | type | ch | len (u16 LE) | payload | CRC-32 (LE, zlib, over type..payload)

    HELLO 1  both ways. Ours resets the board's channels; the board's (also
             sent when it boots) means every channel we had is gone.
    OPEN  2  payload = port (u16 LE). The board answers CLOSE if it cannot connect.
    DATA  3  bytes for the channel.
    CLOSE 4  the channel is finished. The board answers our CLOSE with one of
             its own, after any DATA it still had queued for the channel; only
             then is the number free again.

The serial port is read in a thread (pyserial blocks) and frames are handed to
the asyncio loop. If the port disappears - USB unplugged, board reset by a
flasher - the thread reopens it once a second.
"""
from __future__ import annotations

import asyncio
import struct
import threading
import time
import zlib

import serial

T_HELLO, T_OPEN, T_DATA, T_CLOSE = 1, 2, 3, 4
MAGIC = b"\xa5\x5a"
CHANNELS = 8          # as tunnel.c
MAX_PAYLOAD = 4096    # the board accepts up to this per frame
MAX_FROM_BOARD = 4096  # anything longer is a false start
# The board's HELLO payload; the number is the protocol version. A board with
# another (an old build, say) is refused rather than half-working: version 1
# did not answer CLOSE, and channels never came free.
PROTOCOL = "usb-app tunnel 2"


def frame(ftype: int, ch: int, payload: bytes = b"") -> bytes:
    body = struct.pack("<BBH", ftype, ch, len(payload)) + payload
    return MAGIC + body + struct.pack("<I", zlib.crc32(body))


class Channel:
    """One connection to a port on the board. recv() returns b"" when it ends."""

    def __init__(self, tunnel: "Tunnel", ch: int):
        self._tunnel = tunnel
        self.ch = ch
        self._q: asyncio.Queue[bytes] = asyncio.Queue()
        self.closed = False        # no more data will come
        self._close_sent = False   # our CLOSE is out; the board's answer frees the number

    async def recv(self) -> bytes:
        if self.closed and self._q.empty():
            return b""
        return await self._q.get()

    def send(self, data: bytes) -> None:
        if self.closed:
            raise ConnectionError("channel closed")
        for i in range(0, len(data), MAX_PAYLOAD):
            self._tunnel._write(frame(T_DATA, self.ch, data[i:i + MAX_PAYLOAD]))

    def close(self) -> None:
        """Tell the board. The number comes back when the board answers."""
        self._ended()
        if self._close_sent:
            return
        self._close_sent = True
        try:
            self._tunnel._write(frame(T_CLOSE, self.ch))
        except ConnectionError:
            self._tunnel._release(self)   # no board to answer; nothing can be in flight

    # called on the loop
    def _data(self, data: bytes) -> None:
        if not self.closed:
            self._q.put_nowait(data)

    def _ended(self) -> None:
        if not self.closed:
            self.closed = True
            self._q.put_nowait(b"")


class Tunnel:
    def __init__(self, port: str, baud: int, log=print, debug: bool = False):
        self.port = port
        self.baud = baud
        self.log = log
        self.debug = debug
        self.board_hello: str | None = None   # set once a board of our version has answered
        self._ser: serial.Serial | None = None
        self._wlock = threading.Lock()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._free: asyncio.Queue[int] | None = None
        self._open: dict[int, Channel] = {}
        self._stop = False

    # ---- lifecycle ----------------------------------------------------------

    def start(self) -> None:
        self._loop = asyncio.get_running_loop()
        self._free = asyncio.Queue()
        for ch in range(CHANNELS):
            self._free.put_nowait(ch)
        threading.Thread(target=self._reader, name="tunnel-reader", daemon=True).start()

    def stop(self) -> None:
        self._stop = True

    @property
    def connected(self) -> bool:
        return self._ser is not None and self.board_hello is not None

    # ---- channels -----------------------------------------------------------

    async def open(self, port: int, timeout: float = 10.0) -> Channel:
        if not self.connected:
            raise ConnectionError("board not connected")
        ch = await asyncio.wait_for(self._free.get(), timeout)
        c = Channel(self, ch)
        self._open[ch] = c
        if self.debug:
            self.log(f"[tunnel] ch {ch}: open :{port} ({len(self._open)} in use)")
        try:
            self._write(frame(T_OPEN, ch, struct.pack("<H", port)))
        except ConnectionError:
            self._release(c)
            raise
        return c

    def _release(self, c: Channel) -> None:
        if self._open.get(c.ch) is c:
            del self._open[c.ch]
            self._free.put_nowait(c.ch)
            if self.debug:
                self.log(f"[tunnel] ch {c.ch}: free ({len(self._open)} in use)")

    def _drop_all(self) -> None:
        """Every channel ends and every number is free (board rebooted or port
        lost: nothing from before can arrive any more)."""
        for c in list(self._open.values()):
            c._ended()
            # No CLOSE for these later: the board has no such channel now, and
            # its answer would end a new channel that reused the number.
            c._close_sent = True
            self._release(c)

    # ---- serial -------------------------------------------------------------

    def _write(self, data: bytes) -> None:
        with self._wlock:
            ser = self._ser
            if ser is None:
                raise ConnectionError("serial port not open")
            try:
                ser.write(data)
            except (serial.SerialException, OSError) as e:
                raise ConnectionError(str(e)) from e

    def _open_port(self) -> serial.Serial:
        # DTR/RTS low before opening: on this board they drive EN and the boot
        # strap through the CH343, and the default (asserted) resets the board.
        s = serial.Serial()
        s.port = self.port
        s.baudrate = self.baud
        s.timeout = 0.05
        s.dtr = False
        s.rts = False
        s.open()
        return s

    def _reader(self) -> None:
        buf = bytearray()
        text = bytearray()
        while not self._stop:
            try:
                ser = self._open_port()
            except (serial.SerialException, OSError) as e:
                self.log(f"[tunnel] {self.port}: {e} - retrying")
                time.sleep(1)
                continue
            with self._wlock:
                self._ser = ser
            self.log(f"[tunnel] {self.port} open at {self.baud} baud")
            try:
                self._write(frame(T_HELLO, 0))
                while not self._stop:
                    data = ser.read(ser.in_waiting or 1)
                    if not data:
                        continue
                    buf += data
                    self._parse(buf, text)
            except (serial.SerialException, OSError, ConnectionError) as e:
                self.log(f"[tunnel] {self.port} lost: {e}")
            with self._wlock:
                self._ser = None
            try:
                ser.close()
            except Exception:
                pass
            self.board_hello = None
            self._loop.call_soon_threadsafe(self._drop_all)
            time.sleep(1)

    def _parse(self, buf: bytearray, text: bytearray) -> None:
        """Take frames off the front of buf; the rest of the bytes are log text."""
        i = 0
        n = len(buf)
        while i < n:
            if buf[i] != 0xA5:
                text.append(buf[i])
                i += 1
                continue
            if n - i < 6:
                break                                   # maybe a frame; wait
            if buf[i + 1] != 0x5A:
                text.append(buf[i])
                i += 1
                continue
            ftype, ch, ln = struct.unpack_from("<BBH", buf, i + 2)
            if ln > MAX_FROM_BOARD:
                text.append(buf[i])
                i += 1
                continue
            if n - i < 6 + ln + 4:
                break                                   # wait for the rest
            body = bytes(buf[i + 2:i + 6 + ln])
            (crc,) = struct.unpack_from("<I", buf, i + 6 + ln)
            if crc != zlib.crc32(body):
                text.append(buf[i])
                i += 1
                continue
            self._loop.call_soon_threadsafe(self._on_frame, ftype, ch, body[4:])
            i += 6 + ln + 4
        del buf[:i]
        self._flush_text(text)

    def _flush_text(self, text: bytearray) -> None:
        while True:
            j = text.find(b"\n")
            if j < 0:
                break
            line = text[:j].decode(errors="replace").rstrip("\r")
            del text[:j + 1]
            if line:
                self.log(f"[board] {line}")
        if len(text) > 4096:                            # no newline for a long time
            self.log(f"[board] {text.decode(errors='replace')}")
            text.clear()

    # ---- frames, on the loop ------------------------------------------------

    def _on_frame(self, ftype: int, ch: int, payload: bytes) -> None:
        if ftype == T_HELLO:
            hello = payload.decode(errors="replace")
            if self.board_hello is not None or self._open:
                self.log("[tunnel] board restarted - dropping open channels")
            self._drop_all()
            if hello != PROTOCOL:
                self.board_hello = None
                self.log(f'[tunnel] board says "{hello}", this server speaks "{PROTOCOL}" - '
                         "flash usb-app/firmware from this checkout")
                return
            self.board_hello = hello
            self.log(f"[tunnel] board: {hello}")
            return
        c = self._open.get(ch)
        if c is None:
            return
        if ftype == T_DATA:
            c._data(payload)
        elif ftype == T_CLOSE:
            c._ended()
            if c._close_sent:
                self._release(c)   # the answer to ours: nothing more for this number
