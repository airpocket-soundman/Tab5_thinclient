// Arduino side of the MicroPython GPIO bindings.
//
// Pin policy: only pins that are reachable on an external connector AND unused
// by on-board hardware are exposed. Values come from the Tab5 tables in
// M5Unified (M5Unified.cpp: internal/external I2C, Port B/C, SD, M-Bus) and the
// firmware's own reservations; see docs/GPIO.md for the derivation.
//
// I2C is bit-banged on purpose: both HP I2C controllers are already claimed
// (I2C0 by the Tab5 Keyboard on GPIO 0/1, I2C1 by M5Unified's internal bus on
// GPIO 31/32), so a hardware peripheral cannot be handed to user code safely.

#include <Arduino.h>
#include <SPI.h>

#include <new>

#include "tab5_gpio_bridge.h"

namespace {

// Reachable on M-Bus / Port A / Port B / Port C and claimed by nothing.
constexpr int8_t UserPins[] = {
    2, 3, 4, 5,      // M-Bus 21, 19, 20, 11
    6, 7,            // Port C TXD / RXD
    16, 18, 19,      // M-Bus 2, 7, 9
    17, 52,          // Port B p1 / p2
    45, 47, 48, 51,  // M-Bus 8, 23, 22, 26
    53, 54,          // Port A SDA / SCL (Grove)
};

// ESP32-P4 ADC1 covers 16-23, ADC2 covers 49-54 (soc/adc_channel.h).
constexpr int8_t AdcPins[] = {16, 17, 18, 19, 51, 52, 53, 54};

constexpr int PwmResolutionBits = 8;

bool inList(const int8_t* list, size_t count, int pin)
{
    for (size_t i = 0; i < count; ++i) {
        if (list[i] == pin) {
            return true;
        }
    }
    return false;
}

// Which pins currently hold an LEDC channel, and at what frequency.
struct PwmSlot {
    int8_t pin{-1};
    int freq{0};
};
PwmSlot g_pwm[sizeof(UserPins) / sizeof(UserPins[0])];

bool pwmAttached(int pin, int freq)
{
    for (const PwmSlot& slot : g_pwm) {
        if (slot.pin == pin) {
            return slot.freq == freq;
        }
    }
    return false;
}

void rememberPwm(int pin, int freq)
{
    for (PwmSlot& slot : g_pwm) {
        if (slot.pin == pin || slot.pin < 0) {
            slot.pin = (int8_t)pin;
            slot.freq = freq;
            return;
        }
    }
}

void forgetPwm(int pin)
{
    for (PwmSlot& slot : g_pwm) {
        if (slot.pin == pin) {
            slot.pin = -1;
            slot.freq = 0;
            return;
        }
    }
}

// ------------------------------------------------------------- software I2C

struct SoftI2c {
    int sda{-1};
    int scl{-1};
    uint32_t halfPeriodUs{5};
    bool ready{false};
};

SoftI2c g_i2c;

void sclHigh()
{
    pinMode(g_i2c.scl, INPUT_PULLUP);
    // Honour clock stretching, but never hang the UI loop.
    uint32_t guard = 0;
    while (digitalRead(g_i2c.scl) == LOW && guard < 10000) {
        ++guard;
        delayMicroseconds(1);
    }
}

void sclLow()
{
    pinMode(g_i2c.scl, OUTPUT);
    digitalWrite(g_i2c.scl, LOW);
}

void sdaHigh()
{
    pinMode(g_i2c.sda, INPUT_PULLUP);
}

void sdaLow()
{
    pinMode(g_i2c.sda, OUTPUT);
    digitalWrite(g_i2c.sda, LOW);
}

void i2cDelay()
{
    delayMicroseconds(g_i2c.halfPeriodUs);
}

void i2cStart()
{
    sdaHigh();
    sclHigh();
    i2cDelay();
    sdaLow();
    i2cDelay();
    sclLow();
    i2cDelay();
}

void i2cStop()
{
    sdaLow();
    i2cDelay();
    sclHigh();
    i2cDelay();
    sdaHigh();
    i2cDelay();
}

// Returns true when the slave acknowledged.
bool i2cWriteByte(uint8_t value)
{
    for (int bit = 7; bit >= 0; --bit) {
        if (value & (1 << bit)) {
            sdaHigh();
        } else {
            sdaLow();
        }
        i2cDelay();
        sclHigh();
        i2cDelay();
        sclLow();
        i2cDelay();
    }
    sdaHigh();
    i2cDelay();
    sclHigh();
    i2cDelay();
    bool ack = digitalRead(g_i2c.sda) == LOW;
    sclLow();
    i2cDelay();
    return ack;
}

uint8_t i2cReadByte(bool ack)
{
    uint8_t value = 0;
    sdaHigh();
    for (int bit = 7; bit >= 0; --bit) {
        i2cDelay();
        sclHigh();
        i2cDelay();
        if (digitalRead(g_i2c.sda) == HIGH) {
            value |= (1 << bit);
        }
        sclLow();
    }
    if (ack) {
        sdaLow();
    } else {
        sdaHigh();
    }
    i2cDelay();
    sclHigh();
    i2cDelay();
    sclLow();
    sdaHigh();
    i2cDelay();
    return value;
}

// ---------------------------------------------------------------------- SPI

#if defined(HSPI)
constexpr int UserSpiBus = HSPI;
#else
constexpr int UserSpiBus = 2;
#endif

SPIClass* g_spi = nullptr;
uint32_t g_spiFreq = 1000000;
uint8_t g_spiMode = SPI_MODE0;

uint8_t spiModeFor(int mode)
{
    switch (mode) {
    case 1: return SPI_MODE1;
    case 2: return SPI_MODE2;
    case 3: return SPI_MODE3;
    default: return SPI_MODE0;
    }
}

// --------------------------------------------------------------------- UART

HardwareSerial* uartFor(int port)
{
#if SOC_UART_NUM > 2
    if (port == 2) {
        return &Serial2;
    }
#endif
    if (port == 1) {
        return &Serial1;
    }
    return nullptr;  // port 0 is the diagnostic console
}

bool g_uartStarted[3] = {false, false, false};

}  // namespace

// ------------------------------------------------------------- pin policy

extern "C" int tab5_gpio_pin_allowed(int pin)
{
    return inList(UserPins, sizeof(UserPins) / sizeof(UserPins[0]), pin) ? 1 : 0;
}

extern "C" int tab5_gpio_pin_list(char* out, int out_len)
{
    if (!out || out_len <= 0) {
        return TAB5_GPIO_ERR_RANGE;
    }
    int written = 0;
    for (size_t i = 0; i < sizeof(UserPins) / sizeof(UserPins[0]); ++i) {
        int n = snprintf(out + written, (size_t)(out_len - written), "%s%d",
                         written ? "," : "", (int)UserPins[i]);
        if (n < 0 || written + n >= out_len) {
            break;
        }
        written += n;
    }
    out[written] = '\0';
    return written;
}

// ------------------------------------------------------------- digital I/O

extern "C" int tab5_gpio_pin_mode(int pin, int mode)
{
    if (!tab5_gpio_pin_allowed(pin)) {
        return TAB5_GPIO_ERR_PIN;
    }
    switch (mode) {
    case TAB5_PIN_INPUT: pinMode(pin, INPUT); break;
    case TAB5_PIN_OUTPUT: pinMode(pin, OUTPUT); break;
    case TAB5_PIN_INPUT_PULLUP: pinMode(pin, INPUT_PULLUP); break;
    case TAB5_PIN_INPUT_PULLDOWN: pinMode(pin, INPUT_PULLDOWN); break;
    default: return TAB5_GPIO_ERR_RANGE;
    }
    return TAB5_GPIO_OK;
}

extern "C" int tab5_gpio_write(int pin, int value)
{
    if (!tab5_gpio_pin_allowed(pin)) {
        return TAB5_GPIO_ERR_PIN;
    }
    digitalWrite(pin, value ? HIGH : LOW);
    return TAB5_GPIO_OK;
}

extern "C" int tab5_gpio_read(int pin)
{
    if (!tab5_gpio_pin_allowed(pin)) {
        return TAB5_GPIO_ERR_PIN;
    }
    return digitalRead(pin) == HIGH ? 1 : 0;
}

extern "C" int tab5_gpio_adc_read(int pin)
{
    if (!tab5_gpio_pin_allowed(pin) ||
        !inList(AdcPins, sizeof(AdcPins) / sizeof(AdcPins[0]), pin)) {
        return TAB5_GPIO_ERR_PIN;
    }
    return (int)analogRead(pin);
}

extern "C" int tab5_gpio_adc_mv(int pin)
{
    if (!tab5_gpio_pin_allowed(pin) ||
        !inList(AdcPins, sizeof(AdcPins) / sizeof(AdcPins[0]), pin)) {
        return TAB5_GPIO_ERR_PIN;
    }
    return (int)analogReadMilliVolts(pin);
}

extern "C" int tab5_gpio_pwm_write(int pin, int duty, int freq)
{
    if (!tab5_gpio_pin_allowed(pin)) {
        return TAB5_GPIO_ERR_PIN;
    }
    if (duty < 0 || duty > 255 || freq <= 0 || freq > 40000000) {
        return TAB5_GPIO_ERR_RANGE;
    }
    // ledcAttach() fails when the pin is already attached, so only attach on the
    // first use or when the requested frequency changed.
    if (!pwmAttached(pin, freq) && !ledcAttach(pin, (uint32_t)freq, PwmResolutionBits)) {
        return TAB5_GPIO_ERR_IO;
    }
    rememberPwm(pin, freq);
    if (!ledcWrite(pin, (uint32_t)duty)) {
        return TAB5_GPIO_ERR_IO;
    }
    return TAB5_GPIO_OK;
}

extern "C" int tab5_gpio_pwm_release(int pin)
{
    if (!tab5_gpio_pin_allowed(pin)) {
        return TAB5_GPIO_ERR_PIN;
    }
    ledcDetach(pin);
    forgetPwm(pin);
    pinMode(pin, INPUT);
    return TAB5_GPIO_OK;
}

// ----------------------------------------------------------------------- I2C

extern "C" int tab5_i2c_init(int sda, int scl, int freq)
{
    if (!tab5_gpio_pin_allowed(sda) || !tab5_gpio_pin_allowed(scl)) {
        return TAB5_GPIO_ERR_PIN;
    }
    if (sda == scl || freq < 1000 || freq > 400000) {
        return TAB5_GPIO_ERR_RANGE;
    }
    g_i2c.sda = sda;
    g_i2c.scl = scl;
    g_i2c.halfPeriodUs = 500000u / (uint32_t)freq;
    if (g_i2c.halfPeriodUs == 0) {
        g_i2c.halfPeriodUs = 1;
    }
    g_i2c.ready = true;
    sdaHigh();
    sclHigh();
    return TAB5_GPIO_OK;
}

extern "C" int tab5_i2c_deinit(void)
{
    if (g_i2c.ready) {
        pinMode(g_i2c.sda, INPUT);
        pinMode(g_i2c.scl, INPUT);
    }
    g_i2c.ready = false;
    return TAB5_GPIO_OK;
}

extern "C" int tab5_i2c_scan(uint8_t* out, int max_len)
{
    if (!g_i2c.ready) {
        return TAB5_GPIO_ERR_STATE;
    }
    if (!out || max_len <= 0) {
        return TAB5_GPIO_ERR_RANGE;
    }
    int count = 0;
    for (int addr = 0x08; addr <= 0x77 && count < max_len; ++addr) {
        i2cStart();
        bool ack = i2cWriteByte((uint8_t)(addr << 1));
        i2cStop();
        if (ack) {
            out[count++] = (uint8_t)addr;
        }
    }
    return count;
}

extern "C" int tab5_i2c_write(int addr, const uint8_t* data, int len, int stop)
{
    if (!g_i2c.ready) {
        return TAB5_GPIO_ERR_STATE;
    }
    if (addr < 0 || addr > 0x7F || len < 0 || (len && !data)) {
        return TAB5_GPIO_ERR_RANGE;
    }
    i2cStart();
    if (!i2cWriteByte((uint8_t)(addr << 1))) {
        i2cStop();
        return TAB5_GPIO_ERR_IO;
    }
    for (int i = 0; i < len; ++i) {
        if (!i2cWriteByte(data[i])) {
            i2cStop();
            return TAB5_GPIO_ERR_IO;
        }
    }
    if (stop) {
        i2cStop();
    }
    return len;
}

extern "C" int tab5_i2c_read(int addr, uint8_t* out, int len)
{
    if (!g_i2c.ready) {
        return TAB5_GPIO_ERR_STATE;
    }
    if (addr < 0 || addr > 0x7F || len < 0 || (len && !out)) {
        return TAB5_GPIO_ERR_RANGE;
    }
    i2cStart();
    if (!i2cWriteByte((uint8_t)((addr << 1) | 1))) {
        i2cStop();
        return TAB5_GPIO_ERR_IO;
    }
    for (int i = 0; i < len; ++i) {
        out[i] = i2cReadByte(i + 1 < len);
    }
    i2cStop();
    return len;
}

extern "C" int tab5_i2c_write_read(int addr, const uint8_t* tx, int tx_len,
                                   uint8_t* rx, int rx_len)
{
    int rc = tab5_i2c_write(addr, tx, tx_len, 0);
    if (rc < 0) {
        return rc;
    }
    return tab5_i2c_read(addr, rx, rx_len);
}

// ----------------------------------------------------------------------- SPI

extern "C" int tab5_spi_init(int sck, int miso, int mosi, int freq, int mode)
{
    if (!tab5_gpio_pin_allowed(sck) || !tab5_gpio_pin_allowed(mosi)) {
        return TAB5_GPIO_ERR_PIN;
    }
    if (miso >= 0 && !tab5_gpio_pin_allowed(miso)) {
        return TAB5_GPIO_ERR_PIN;
    }
    if (freq <= 0 || freq > 40000000 || mode < 0 || mode > 3) {
        return TAB5_GPIO_ERR_RANGE;
    }
    if (!g_spi) {
        g_spi = new (std::nothrow) SPIClass(UserSpiBus);
        if (!g_spi) {
            return TAB5_GPIO_ERR_BUSY;
        }
    } else {
        g_spi->end();
    }
    g_spiFreq = (uint32_t)freq;
    g_spiMode = spiModeFor(mode);
    g_spi->begin(sck, miso, mosi, -1);
    return TAB5_GPIO_OK;
}

extern "C" int tab5_spi_deinit(void)
{
    if (g_spi) {
        g_spi->end();
        delete g_spi;
        g_spi = nullptr;
    }
    return TAB5_GPIO_OK;
}

extern "C" int tab5_spi_transfer(int cs, const uint8_t* tx, uint8_t* rx, int len)
{
    if (!g_spi) {
        return TAB5_GPIO_ERR_STATE;
    }
    if (len < 0 || (len && !tx)) {
        return TAB5_GPIO_ERR_RANGE;
    }
    if (cs >= 0) {
        if (!tab5_gpio_pin_allowed(cs)) {
            return TAB5_GPIO_ERR_PIN;
        }
        pinMode(cs, OUTPUT);
        digitalWrite(cs, LOW);
    }
    g_spi->beginTransaction(SPISettings(g_spiFreq, MSBFIRST, g_spiMode));
    g_spi->transferBytes(tx, rx, (uint32_t)len);
    g_spi->endTransaction();
    if (cs >= 0) {
        digitalWrite(cs, HIGH);
    }
    return len;
}

// ---------------------------------------------------------------------- UART

extern "C" int tab5_uart_init(int port, int baud, int rx_pin, int tx_pin)
{
    HardwareSerial* serial = uartFor(port);
    if (!serial) {
        return TAB5_GPIO_ERR_RANGE;
    }
    if (baud < 300 || baud > 5000000) {
        return TAB5_GPIO_ERR_RANGE;
    }
    if (rx_pin >= 0 && !tab5_gpio_pin_allowed(rx_pin)) {
        return TAB5_GPIO_ERR_PIN;
    }
    if (tx_pin >= 0 && !tab5_gpio_pin_allowed(tx_pin)) {
        return TAB5_GPIO_ERR_PIN;
    }
    serial->begin((unsigned long)baud, SERIAL_8N1, rx_pin, tx_pin);
    g_uartStarted[port] = true;
    return TAB5_GPIO_OK;
}

extern "C" int tab5_uart_deinit(int port)
{
    HardwareSerial* serial = uartFor(port);
    if (!serial) {
        return TAB5_GPIO_ERR_RANGE;
    }
    if (g_uartStarted[port]) {
        serial->end();
        g_uartStarted[port] = false;
    }
    return TAB5_GPIO_OK;
}

extern "C" int tab5_uart_write(int port, const uint8_t* data, int len)
{
    HardwareSerial* serial = uartFor(port);
    if (!serial) {
        return TAB5_GPIO_ERR_RANGE;
    }
    if (!g_uartStarted[port]) {
        return TAB5_GPIO_ERR_STATE;
    }
    if (len < 0 || (len && !data)) {
        return TAB5_GPIO_ERR_RANGE;
    }
    return (int)serial->write(data, (size_t)len);
}

extern "C" int tab5_uart_read(int port, uint8_t* out, int len, int timeout_ms)
{
    HardwareSerial* serial = uartFor(port);
    if (!serial) {
        return TAB5_GPIO_ERR_RANGE;
    }
    if (!g_uartStarted[port]) {
        return TAB5_GPIO_ERR_STATE;
    }
    if (len < 0 || (len && !out) || timeout_ms < 0) {
        return TAB5_GPIO_ERR_RANGE;
    }
    int got = 0;
    uint32_t deadline = millis() + (uint32_t)timeout_ms;
    while (got < len && (int32_t)(millis() - deadline) < 0) {
        int c = serial->read();
        if (c < 0) {
            delay(1);
            continue;
        }
        out[got++] = (uint8_t)c;
    }
    return got;
}

extern "C" int tab5_uart_any(int port)
{
    HardwareSerial* serial = uartFor(port);
    if (!serial) {
        return TAB5_GPIO_ERR_RANGE;
    }
    if (!g_uartStarted[port]) {
        return TAB5_GPIO_ERR_STATE;
    }
    return serial->available();
}
