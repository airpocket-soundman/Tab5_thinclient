# gpio.py - exercise the Tab5 GPIO API from MicroPython.
#
# Wiring assumed by the defaults (all on the Grove ports):
#   Port B p1 (GPIO 17) -> LED through a resistor
#   Port B p2 (GPIO 52) -> button to GND
#   Port A     (53/54)  -> I2C device
# Nothing has to be connected: every step reports what it finds and moves on.

LED = 17
BUTTON = 52
ADC_PIN = 16
I2C_SDA = 53
I2C_SCL = 54

if len(argv) > 1:
    LED = int(argv[1])
if len(argv) > 2:
    BUTTON = int(argv[2])

print('usable pins:', pins())

print('--- digital out: blinking pin', LED)
led = Pin(LED, Pin.OUT)
for _ in range(6):
    led.toggle()

print('--- pwm fade on pin', LED)
for duty in range(0, 256, 32):
    led.pwm(duty)
led.release()

print('--- digital in: pin', BUTTON)
button = Pin(BUTTON, Pin.IN_PULLUP)
print('level =', button.value(), '(1 = released with the internal pull-up)')

print('--- analog in: pin', ADC_PIN)
try:
    adc = ADC(ADC_PIN)
    print('raw =', adc.read(), ' mV =', adc.read_mv())
except ValueError as e:
    print('adc unavailable:', e)

print('--- i2c scan on', I2C_SDA, '/', I2C_SCL)
i2c = I2C(I2C_SDA, I2C_SCL, 100000)
found = i2c.scan()
if found:
    for addr in found:
        print('  device at 0x%02x' % addr)
else:
    print('  no devices responded')
i2c.deinit()

print('done')
