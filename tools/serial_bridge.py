#!/usr/bin/env python3
"""Serial <-> WebSocket bridge for the Widscan58 web UI. GPL-3.0-or-later.

Relays the device byte stream unchanged in both directions, so the web UI
(open it with ?bridge=ws://HOST:PORT) runs against real hardware without Web
Serial: headless browser tests, or viewing the scanner from another machine.
One client at a time owns the port; a new client replaces the old one.

  serial_bridge.py [-p /dev/ttyACM0] [--host 127.0.0.1] [--ws-port 5809]
"""
import argparse
import asyncio
import signal

import serial
import websockets

from ttyfix import keep_vmin1


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    ap.add_argument("--host", default="127.0.0.1", help="bind address (0.0.0.0 to expose on the network)")
    ap.add_argument("--ws-port", type=int, default=5809)
    args = ap.parse_args()

    ser = serial.Serial(args.port, 921600, timeout=0)  # the host UART's rate; USB ignores it
    keep_vmin1(ser)
    loop = asyncio.get_running_loop()
    clients: set = set()
    stats = {"rx": 0, "tx": 0}

    async def pump_serial():
        """Read the port and fan out to the (single) client."""
        while True:
            try:
                data = await loop.run_in_executor(None, lambda: ser.read(ser.in_waiting or 1))
            except serial.SerialException as e:
                print(f"serial error: {e}; reopening")
                await asyncio.sleep(1)
                try:
                    ser.close()
                    ser.open()
                    keep_vmin1(ser)
                except serial.SerialException:
                    pass
                continue
            if not data:
                await asyncio.sleep(0.002)
                continue
            stats["rx"] += len(data)
            for ws in list(clients):
                try:
                    await ws.send(data)
                except websockets.ConnectionClosed:
                    clients.discard(ws)

    async def handler(ws):
        for old in list(clients):
            await old.close(code=4000, reason="replaced by a new client")
        clients.clear()
        clients.add(ws)
        print(f"client connected: {ws.remote_address}")
        ser.reset_input_buffer()  # start the client on fresh data
        try:
            async for msg in ws:
                data = msg.encode() if isinstance(msg, str) else msg
                stats["tx"] += len(data)
                ser.write(data)
        except websockets.ConnectionClosed:
            pass
        finally:
            clients.discard(ws)
            print("client disconnected")

    reader = asyncio.ensure_future(pump_serial())
    stop = loop.create_future()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set_result, None)
    async with websockets.serve(handler, args.host, args.ws_port, max_size=None, compression=None):
        print(f"bridging {args.port} <-> ws://{args.host}:{args.ws_port}")
        await stop
    reader.cancel()
    ser.close()
    print(f"stopped; {stats['rx']} bytes from device, {stats['tx']} to device")


if __name__ == "__main__":
    asyncio.run(main())
