#pragma once

// Plain C interface between the MicroPython bindings (tab5_gpio_module.c) and
// the Arduino implementation (Tab5GpioBridge.cpp). Keep this header free of
// C++ and Arduino types so the MicroPython glue can include it.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Every call returns >= 0 on success and one of these on failure.
#define TAB5_GPIO_OK 0
#define TAB5_GPIO_ERR_PIN (-1)      // pin is reserved by on-board hardware
#define TAB5_GPIO_ERR_RANGE (-2)    // argument out of range
#define TAB5_GPIO_ERR_STATE (-3)    // bus used before init
#define TAB5_GPIO_ERR_IO (-4)       // transfer failed / NACK
#define TAB5_GPIO_ERR_BUSY (-5)     // resource already claimed elsewhere

// pin modes accepted by tab5_gpio_pin_mode
#define TAB5_PIN_INPUT 0
#define TAB5_PIN_OUTPUT 1
#define TAB5_PIN_INPUT_PULLUP 2
#define TAB5_PIN_INPUT_PULLDOWN 3

// Returns 1 when the pin is safe for user code, 0 when reserved on-board.
int tab5_gpio_pin_allowed(int pin);
// Writes a comma separated list of usable pins, returns the length written.
int tab5_gpio_pin_list(char *out, int out_len);

int tab5_gpio_pin_mode(int pin, int mode);
int tab5_gpio_write(int pin, int value);
int tab5_gpio_read(int pin);
int tab5_gpio_adc_read(int pin);
int tab5_gpio_adc_mv(int pin);
int tab5_gpio_pwm_write(int pin, int duty, int freq);
int tab5_gpio_pwm_release(int pin);

int tab5_i2c_init(int sda, int scl, int freq);
int tab5_i2c_deinit(void);
int tab5_i2c_scan(uint8_t *out, int max_len);
int tab5_i2c_write(int addr, const uint8_t *data, int len, int stop);
int tab5_i2c_read(int addr, uint8_t *out, int len);
int tab5_i2c_write_read(int addr, const uint8_t *tx, int tx_len, uint8_t *rx, int rx_len);

int tab5_spi_init(int sck, int miso, int mosi, int freq, int mode);
int tab5_spi_deinit(void);
// rx may be NULL for write-only transfers; cs < 0 leaves chip select untouched.
int tab5_spi_transfer(int cs, const uint8_t *tx, uint8_t *rx, int len);

int tab5_uart_init(int port, int baud, int rx_pin, int tx_pin);
int tab5_uart_deinit(int port);
int tab5_uart_write(int port, const uint8_t *data, int len);
int tab5_uart_read(int port, uint8_t *out, int len, int timeout_ms);
int tab5_uart_any(int port);

// Implemented in tab5_gpio_module.c: installs the primitives above as globals
// in the running MicroPython VM. Call once after mp_embed_init().
void tab5_gpio_register_globals(void);

#ifdef __cplusplus
}
#endif
