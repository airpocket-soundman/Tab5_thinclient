// MicroPython bindings for the Tab5 GPIO bridge.
//
// These are registered as globals at VM start (see PythonRunner::ensureVm) using
// runtime interned strings, so the embedded MicroPython does not need its QSTR
// tables regenerated. The friendly Pin/ADC/I2C/SPI/UART classes are layered on
// top of these primitives by the Python prelude in PythonRunner.cpp.

#include <string.h>

#include "py/runtime.h"
#include "py/objlist.h"
#include "py/objstr.h"

#include "tab5_gpio_bridge.h"

#define TAB5_MAX_XFER 4096

static NORETURN void tab5_raise(int rc)
{
    if (rc == TAB5_GPIO_ERR_PIN) {
        mp_raise_ValueError(MP_ERROR_TEXT("pin is reserved by on-board hardware"));
    }
    if (rc == TAB5_GPIO_ERR_RANGE) {
        mp_raise_ValueError(MP_ERROR_TEXT("argument out of range"));
    }
    if (rc == TAB5_GPIO_ERR_STATE) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("bus not initialised"));
    }
    if (rc == TAB5_GPIO_ERR_BUSY) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("resource busy"));
    }
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("transfer failed"));
}

static int tab5_check(int rc)
{
    if (rc < 0) {
        tab5_raise(rc);
    }
    return rc;
}

static int tab5_length_arg(mp_obj_t obj)
{
    mp_int_t len = mp_obj_get_int(obj);
    if (len < 0 || len > TAB5_MAX_XFER) {
        mp_raise_ValueError(MP_ERROR_TEXT("length out of range"));
    }
    return (int)len;
}

static void tab5_read_buffer(mp_obj_t obj, mp_buffer_info_t *info)
{
    mp_get_buffer_raise(obj, info, MP_BUFFER_READ);
    if (info->len > TAB5_MAX_XFER) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer too large"));
    }
}

// ---------------------------------------------------------------- digital I/O

static mp_obj_t tab5_py_pin_allowed(mp_obj_t pin_in)
{
    return mp_obj_new_bool(tab5_gpio_pin_allowed(mp_obj_get_int(pin_in)) != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(tab5_py_pin_allowed_obj, tab5_py_pin_allowed);

static mp_obj_t tab5_py_pin_list(void)
{
    char buf[256];
    int len = tab5_gpio_pin_list(buf, (int)sizeof(buf));
    if (len < 0) {
        len = 0;
    }
    return mp_obj_new_str(buf, (size_t)len);
}
static MP_DEFINE_CONST_FUN_OBJ_0(tab5_py_pin_list_obj, tab5_py_pin_list);

static mp_obj_t tab5_py_pin_mode(mp_obj_t pin_in, mp_obj_t mode_in)
{
    tab5_check(tab5_gpio_pin_mode(mp_obj_get_int(pin_in), mp_obj_get_int(mode_in)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(tab5_py_pin_mode_obj, tab5_py_pin_mode);

static mp_obj_t tab5_py_pin_write(mp_obj_t pin_in, mp_obj_t value_in)
{
    tab5_check(tab5_gpio_write(mp_obj_get_int(pin_in), mp_obj_is_true(value_in) ? 1 : 0));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(tab5_py_pin_write_obj, tab5_py_pin_write);

static mp_obj_t tab5_py_pin_read(mp_obj_t pin_in)
{
    return MP_OBJ_NEW_SMALL_INT(tab5_check(tab5_gpio_read(mp_obj_get_int(pin_in))));
}
static MP_DEFINE_CONST_FUN_OBJ_1(tab5_py_pin_read_obj, tab5_py_pin_read);

static mp_obj_t tab5_py_adc_read(mp_obj_t pin_in)
{
    return MP_OBJ_NEW_SMALL_INT(tab5_check(tab5_gpio_adc_read(mp_obj_get_int(pin_in))));
}
static MP_DEFINE_CONST_FUN_OBJ_1(tab5_py_adc_read_obj, tab5_py_adc_read);

static mp_obj_t tab5_py_adc_mv(mp_obj_t pin_in)
{
    return MP_OBJ_NEW_SMALL_INT(tab5_check(tab5_gpio_adc_mv(mp_obj_get_int(pin_in))));
}
static MP_DEFINE_CONST_FUN_OBJ_1(tab5_py_adc_mv_obj, tab5_py_adc_mv);

static mp_obj_t tab5_py_pwm_write(size_t n_args, const mp_obj_t *args)
{
    int freq = n_args > 2 ? mp_obj_get_int(args[2]) : 5000;
    tab5_check(tab5_gpio_pwm_write(mp_obj_get_int(args[0]), mp_obj_get_int(args[1]), freq));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(tab5_py_pwm_write_obj, 2, 3, tab5_py_pwm_write);

static mp_obj_t tab5_py_pwm_release(mp_obj_t pin_in)
{
    tab5_check(tab5_gpio_pwm_release(mp_obj_get_int(pin_in)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(tab5_py_pwm_release_obj, tab5_py_pwm_release);

// ----------------------------------------------------------------------- I2C

static mp_obj_t tab5_py_i2c_init(size_t n_args, const mp_obj_t *args)
{
    int freq = n_args > 2 ? mp_obj_get_int(args[2]) : 100000;
    tab5_check(tab5_i2c_init(mp_obj_get_int(args[0]), mp_obj_get_int(args[1]), freq));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(tab5_py_i2c_init_obj, 2, 3, tab5_py_i2c_init);

static mp_obj_t tab5_py_i2c_deinit(void)
{
    tab5_check(tab5_i2c_deinit());
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(tab5_py_i2c_deinit_obj, tab5_py_i2c_deinit);

static mp_obj_t tab5_py_i2c_scan(void)
{
    uint8_t found[128];
    int count = tab5_check(tab5_i2c_scan(found, (int)sizeof(found)));
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < count; ++i) {
        mp_obj_list_append(list, MP_OBJ_NEW_SMALL_INT(found[i]));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(tab5_py_i2c_scan_obj, tab5_py_i2c_scan);

static mp_obj_t tab5_py_i2c_write(size_t n_args, const mp_obj_t *args)
{
    mp_buffer_info_t info;
    tab5_read_buffer(args[1], &info);
    int stop = n_args > 2 ? (mp_obj_is_true(args[2]) ? 1 : 0) : 1;
    int rc = tab5_check(tab5_i2c_write(mp_obj_get_int(args[0]), (const uint8_t *)info.buf,
                                       (int)info.len, stop));
    return MP_OBJ_NEW_SMALL_INT(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(tab5_py_i2c_write_obj, 2, 3, tab5_py_i2c_write);

static mp_obj_t tab5_py_i2c_read(mp_obj_t addr_in, mp_obj_t len_in)
{
    int len = tab5_length_arg(len_in);
    uint8_t *buf = m_new(uint8_t, len ? len : 1);
    int rc = tab5_i2c_read(mp_obj_get_int(addr_in), buf, len);
    if (rc < 0) {
        m_del(uint8_t, buf, len ? len : 1);
        tab5_raise(rc);
    }
    mp_obj_t out = mp_obj_new_bytes(buf, (size_t)rc);
    m_del(uint8_t, buf, len ? len : 1);
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_2(tab5_py_i2c_read_obj, tab5_py_i2c_read);

static mp_obj_t tab5_py_i2c_write_read(mp_obj_t addr_in, mp_obj_t tx_in, mp_obj_t len_in)
{
    mp_buffer_info_t info;
    tab5_read_buffer(tx_in, &info);
    int len = tab5_length_arg(len_in);
    uint8_t *buf = m_new(uint8_t, len ? len : 1);
    int rc = tab5_i2c_write_read(mp_obj_get_int(addr_in), (const uint8_t *)info.buf,
                                 (int)info.len, buf, len);
    if (rc < 0) {
        m_del(uint8_t, buf, len ? len : 1);
        tab5_raise(rc);
    }
    mp_obj_t out = mp_obj_new_bytes(buf, (size_t)rc);
    m_del(uint8_t, buf, len ? len : 1);
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_3(tab5_py_i2c_write_read_obj, tab5_py_i2c_write_read);

// ----------------------------------------------------------------------- SPI

static mp_obj_t tab5_py_spi_init(size_t n_args, const mp_obj_t *args)
{
    int freq = n_args > 3 ? mp_obj_get_int(args[3]) : 1000000;
    int mode = n_args > 4 ? mp_obj_get_int(args[4]) : 0;
    tab5_check(tab5_spi_init(mp_obj_get_int(args[0]), mp_obj_get_int(args[1]),
                             mp_obj_get_int(args[2]), freq, mode));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(tab5_py_spi_init_obj, 3, 5, tab5_py_spi_init);

static mp_obj_t tab5_py_spi_deinit(void)
{
    tab5_check(tab5_spi_deinit());
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(tab5_py_spi_deinit_obj, tab5_py_spi_deinit);

static mp_obj_t tab5_py_spi_transfer(mp_obj_t cs_in, mp_obj_t tx_in)
{
    mp_buffer_info_t info;
    tab5_read_buffer(tx_in, &info);
    int len = (int)info.len;
    uint8_t *rx = m_new(uint8_t, len ? len : 1);
    int rc = tab5_spi_transfer(mp_obj_get_int(cs_in), (const uint8_t *)info.buf, rx, len);
    if (rc < 0) {
        m_del(uint8_t, rx, len ? len : 1);
        tab5_raise(rc);
    }
    mp_obj_t out = mp_obj_new_bytes(rx, (size_t)rc);
    m_del(uint8_t, rx, len ? len : 1);
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_2(tab5_py_spi_transfer_obj, tab5_py_spi_transfer);

// ---------------------------------------------------------------------- UART

static mp_obj_t tab5_py_uart_init(size_t n_args, const mp_obj_t *args)
{
    int rx = n_args > 2 ? mp_obj_get_int(args[2]) : -1;
    int tx = n_args > 3 ? mp_obj_get_int(args[3]) : -1;
    tab5_check(tab5_uart_init(mp_obj_get_int(args[0]), mp_obj_get_int(args[1]), rx, tx));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(tab5_py_uart_init_obj, 2, 4, tab5_py_uart_init);

static mp_obj_t tab5_py_uart_deinit(mp_obj_t port_in)
{
    tab5_check(tab5_uart_deinit(mp_obj_get_int(port_in)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(tab5_py_uart_deinit_obj, tab5_py_uart_deinit);

static mp_obj_t tab5_py_uart_write(mp_obj_t port_in, mp_obj_t data_in)
{
    mp_buffer_info_t info;
    tab5_read_buffer(data_in, &info);
    int rc = tab5_check(tab5_uart_write(mp_obj_get_int(port_in), (const uint8_t *)info.buf,
                                        (int)info.len));
    return MP_OBJ_NEW_SMALL_INT(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_2(tab5_py_uart_write_obj, tab5_py_uart_write);

static mp_obj_t tab5_py_uart_read(size_t n_args, const mp_obj_t *args)
{
    int len = tab5_length_arg(args[1]);
    int timeout = n_args > 2 ? mp_obj_get_int(args[2]) : 100;
    uint8_t *buf = m_new(uint8_t, len ? len : 1);
    int rc = tab5_uart_read(mp_obj_get_int(args[0]), buf, len, timeout);
    if (rc < 0) {
        m_del(uint8_t, buf, len ? len : 1);
        tab5_raise(rc);
    }
    mp_obj_t out = mp_obj_new_bytes(buf, (size_t)rc);
    m_del(uint8_t, buf, len ? len : 1);
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(tab5_py_uart_read_obj, 2, 3, tab5_py_uart_read);

static mp_obj_t tab5_py_uart_any(mp_obj_t port_in)
{
    return MP_OBJ_NEW_SMALL_INT(tab5_check(tab5_uart_any(mp_obj_get_int(port_in))));
}
static MP_DEFINE_CONST_FUN_OBJ_1(tab5_py_uart_any_obj, tab5_py_uart_any);

// ------------------------------------------------------------- registration

static void tab5_store(const char *name, const void *obj)
{
    mp_store_global(qstr_from_str(name), MP_OBJ_FROM_PTR(obj));
}

void tab5_gpio_register_globals(void)
{
    tab5_store("_tab5_pin_allowed", &tab5_py_pin_allowed_obj);
    tab5_store("_tab5_pin_list", &tab5_py_pin_list_obj);
    tab5_store("_tab5_pin_mode", &tab5_py_pin_mode_obj);
    tab5_store("_tab5_pin_write", &tab5_py_pin_write_obj);
    tab5_store("_tab5_pin_read", &tab5_py_pin_read_obj);
    tab5_store("_tab5_adc_read", &tab5_py_adc_read_obj);
    tab5_store("_tab5_adc_mv", &tab5_py_adc_mv_obj);
    tab5_store("_tab5_pwm_write", &tab5_py_pwm_write_obj);
    tab5_store("_tab5_pwm_release", &tab5_py_pwm_release_obj);

    tab5_store("_tab5_i2c_init", &tab5_py_i2c_init_obj);
    tab5_store("_tab5_i2c_deinit", &tab5_py_i2c_deinit_obj);
    tab5_store("_tab5_i2c_scan", &tab5_py_i2c_scan_obj);
    tab5_store("_tab5_i2c_write", &tab5_py_i2c_write_obj);
    tab5_store("_tab5_i2c_read", &tab5_py_i2c_read_obj);
    tab5_store("_tab5_i2c_write_read", &tab5_py_i2c_write_read_obj);

    tab5_store("_tab5_spi_init", &tab5_py_spi_init_obj);
    tab5_store("_tab5_spi_deinit", &tab5_py_spi_deinit_obj);
    tab5_store("_tab5_spi_transfer", &tab5_py_spi_transfer_obj);

    tab5_store("_tab5_uart_init", &tab5_py_uart_init_obj);
    tab5_store("_tab5_uart_deinit", &tab5_py_uart_deinit_obj);
    tab5_store("_tab5_uart_write", &tab5_py_uart_write_obj);
    tab5_store("_tab5_uart_read", &tab5_py_uart_read_obj);
    tab5_store("_tab5_uart_any", &tab5_py_uart_any_obj);
}
