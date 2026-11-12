"""HTTP/1.1 requests to the board over one tunnel channel each.

h11 does the protocol (the board's server answers with Content-Length or with
chunked transfer encoding - the MJPEG stream is one endless chunked body); this
module only moves its bytes over a Channel.
"""
from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator
from dataclasses import dataclass

import h11

from tunnel import Channel, Tunnel


class BoardError(Exception):
    """The board could not be reached, or broke the connection mid-reply."""


@dataclass
class Reply:
    status: int
    headers: dict[str, str]          # lower-case names
    _ch: Channel
    _conn: h11.Connection

    async def body(self) -> AsyncIterator[bytes]:
        """The body as it arrives. Closes the channel when done or abandoned."""
        try:
            while True:
                ev = self._conn.next_event()
                if ev is h11.NEED_DATA:
                    self._conn.receive_data(await self._ch.recv())
                elif isinstance(ev, h11.Data):
                    yield bytes(ev.data)
                elif isinstance(ev, (h11.EndOfMessage, h11.ConnectionClosed)):
                    return
        except h11.RemoteProtocolError as e:
            raise BoardError(f"reply cut short: {e}") from e
        finally:
            self.close()

    async def read(self) -> bytes:
        return b"".join([chunk async for chunk in self.body()])

    def close(self) -> None:
        self._ch.close()


# Until the status line and headers are in. The board answers in well under a
# second; a still at q100 is ~1.6 MB, but its headers come first.
HEADERS_TIMEOUT = 15.0


async def request(tunnel: Tunnel, port: int, method: str, target: str,
                  body: bytes = b"") -> Reply:
    """Send one request; return once the status and headers are in."""
    try:
        return await asyncio.wait_for(_request(tunnel, port, method, target, body), HEADERS_TIMEOUT)
    except asyncio.TimeoutError as e:
        raise BoardError(f"no reply to {method} {target} within {HEADERS_TIMEOUT:.0f} s") from e


async def _request(tunnel: Tunnel, port: int, method: str, target: str, body: bytes) -> Reply:
    try:
        ch = await tunnel.open(port)
    except ConnectionError as e:
        raise BoardError(str(e)) from e
    except asyncio.TimeoutError as e:
        raise BoardError("all tunnel channels busy") from e

    conn = h11.Connection(h11.CLIENT)
    headers = [("Host", "board"), ("Connection", "close")]
    if body or method in ("POST", "PUT"):
        headers.append(("Content-Length", str(len(body))))
    try:
        out = conn.send(h11.Request(method=method, target=target, headers=headers))
        if body:
            out += conn.send(h11.Data(data=body))
        out += conn.send(h11.EndOfMessage())
        ch.send(out)
        while True:
            ev = conn.next_event()
            if ev is h11.NEED_DATA:
                conn.receive_data(await ch.recv())   # b"" = the board closed it
            elif isinstance(ev, h11.Response):
                hdrs = {k.decode().lower(): v.decode() for k, v in ev.headers}
                return Reply(ev.status_code, hdrs, ch, conn)
            elif isinstance(ev, h11.ConnectionClosed):
                raise BoardError("board closed the connection before replying")
    except (h11.RemoteProtocolError, ConnectionError) as e:
        ch.close()
        raise BoardError(str(e)) from e
    except BaseException:
        ch.close()
        raise
