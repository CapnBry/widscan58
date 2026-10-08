"""Leave the tty in a state Chrome's Web Serial can open. GPL-3.0-or-later.

termios settings outlive a close: they belong to the tty node until the USB
device re-enumerates. pyserial (and so esptool) sets VMIN=0/VTIME=0, and with
that a non-blocking read() returns 0 when no data is waiting. Chrome (at least
up to 153) doesn't reset VMIN on open and reports that 0 as "The device has
been lost". VMIN=1 makes such a read return EAGAIN instead, which pyserial
ignores, so set it right after opening: it then survives even a killed
process. pyserial resets it on any reconfiguration (baud rate, timeout), so
call this again after one.
"""
import termios


def keep_vmin1(ser):
    attrs = termios.tcgetattr(ser.fd)
    attrs[6][termios.VMIN] = 1
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(ser.fd, termios.TCSANOW, attrs)
