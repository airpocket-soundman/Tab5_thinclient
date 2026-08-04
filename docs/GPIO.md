# Tab5 GPIO API

The embedded MicroPython exposes the Tab5's external connectors through the
globals `Pin`, `ADC`, `I2C`, `SPI`, `UART` and `pins()`. They are available in
the REPL, in `python -c`, and in scripts on the SD card without importing
anything.

```text
python -c print(pins())
python /gpio.py
```

## Which pins can be used

Only pins that are both reachable on an external connector and unclaimed by
on-board hardware are accepted. Anything else raises `ValueError`.

| Pins | Where |
|---|---|
| 2, 3, 4, 5, 16, 18, 19, 45, 47, 48, 51 | M-Bus / ExtPort |
| 17, 52 | Port B (p1, p2) |
| 6, 7 | Port C (TXD, RXD) |
| 53, 54 | Port A Grove (SDA, SCL) |

`pins()` returns the same list at runtime. Analog input additionally requires an
ADC-capable pin: **16, 17, 18, 19** (ADC1) or **51, 52, 53, 54** (ADC2).

Everything else on the SoC is refused on purpose, because it is already wired to
something: the Tab5 Keyboard (0, 1, 50), the ESP32-C6 radio over SDIO (8-15),
the backlight (22), the touch panel (23, 31, 32), the audio codecs (26-30), the
boot straps (35, 36), the console UART (37, 38) and the microSD card (39-44).

## Digital in / out

```python
led = Pin(17, Pin.OUT)
led.on()
led.off()
led.toggle()

button = Pin(52, Pin.IN_PULLUP)
if button.value() == 0:
    print('pressed')
```

Modes are `Pin.IN`, `Pin.OUT`, `Pin.IN_PULLUP` and `Pin.IN_PULLDOWN`.
`value()` reads, `value(level)` writes.

## PWM

```python
led = Pin(17, Pin.OUT)
led.pwm(128)             # 8-bit duty, default 5 kHz
led.pwm(64, freq=1000)
led.release()            # hand the pin back to plain GPIO
```

## Analog in

```python
adc = ADC(16)
print(adc.read())        # raw counts
print(adc.read_mv())     # calibrated millivolts
```

## I2C

Bit-banged on the pins you name, because both hardware I2C controllers are
already taken — one by the Tab5 Keyboard, one by the internal bus that drives
the touch panel, codecs, PMIC and RTC. Expect up to about 100 kHz, which is
enough for the usual Grove sensors. Pull-ups are required; the Grove modules
normally provide them.

```python
i2c = I2C(53, 54, 100000)          # Port A
print(i2c.scan())                   # -> [60, 104]

i2c.write(0x3C, b'\x00\xAE')
data = i2c.read(0x68, 6)
who = i2c.read_reg(0x68, 0x75)      # write register, repeated start, read
i2c.write_reg(0x68, 0x6B, [0x00])
i2c.deinit()
```

## SPI

Uses a spare SPI host, so it does not disturb the microSD card. `cs` is optional
and driven low for the duration of the transfer.

```python
spi = SPI(18, 19, 17, freq=1000000, mode=0)   # sck, miso, mosi
rx = spi.transfer(b'\x9F\x00\x00\x00', cs=5)
spi.write(b'\x01\x02')
spi.deinit()
```

Pass `miso=-1` for write-only devices such as LED strips or displays.

## UART

Port 0 is the diagnostic console and is refused. Ports 1 and 2 are free; Port C
(TXD 6, RXD 7) is the connector meant for serial devices.

```python
uart = UART(1, 9600, rx=7, tx=6)
uart.write(b'AT\r\n')
print(uart.any())
print(uart.read(32, timeout=200))
uart.deinit()
```

`read()` returns as soon as it has the requested number of bytes or the timeout
expires, so a short read is normal rather than an error.

## Errors

| Exception | Meaning |
|---|---|
| `ValueError: pin is reserved by on-board hardware` | The pin is not in the table above |
| `ValueError: argument out of range` | Bad mode, frequency, address or length |
| `OSError: bus not initialised` | `I2C`/`SPI`/`UART` used before construction, or after `deinit()` |
| `OSError: transfer failed` | I2C address not acknowledged, or the transfer aborted |

## Notes and limits

- Scripts run on the main loop, so long busy-waits also stall the UI and the SSH
  session. Keep GPIO loops short, or break them up.
- Bit-banged I2C timing is affected by that same loop; treat it as best-effort
  rather than a precise clock.
- The pin table is a safety policy, not a full description of the hardware.
  Pins that exist on the SoC but reach no connector are deliberately absent.
- Because scripts can be started over SSH (`python /gpio.py`), the server can
  drive the tablet's I/O as well as its screen.
