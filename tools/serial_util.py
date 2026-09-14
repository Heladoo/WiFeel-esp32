"""
Shared serial-port opening for WiFeel tools.

Both boards use the ESP32-C6's native USB-Serial/JTAG port, which maps the
DTR/RTS lines to the chip's reset and boot pins (the same auto-reset trick
esptool uses). pySerial's default open raises DTR and RTS one after the
other, and closing drops them — either transition can reset the board.
That was observed repeatedly on HUB-1: opening a port for a quick `status`
often rebooted it, sometimes twice.

Setting both lines low *before* open() means they never change, which is
what ESP-IDF's own monitor does.
"""
import serial


def open_port(port: str, baud: int = 115200, timeout: float = 0.5) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = timeout
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser
