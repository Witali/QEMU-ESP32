/* Composable ESP SPI/I2C sockets with firmware-controlled GPIO routing.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "hw/i2c/i2c.h"
#include "hw/gpio/esp32_gpio.h"
#include "hw/sd/sd.h"
#include "hw/irq.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qjson.h"
#include "qom/qom-qobject.h"
#include "sysemu/block-backend.h"

static bool module_properties(DeviceState *dev, const char *json, Error **errp)
{
    QObject *obj;
    QDict *dict;
    const QDictEntry *entry;
    if (!json || !*json) return true;
    obj = qobject_from_json(json,errp);
    if (!obj) return false;
    dict = qobject_to(QDict,obj);
    if (!dict) {
        error_setg(errp,"Module properties must be a JSON object");
        qobject_unref(obj); return false;
    }
    for (entry = qdict_first(dict); entry; entry = qdict_next(dict,entry)) {
        if (!object_property_set_qobject(OBJECT(dev),qdict_entry_key(entry),
                                        qdict_entry_value(entry),errp)) {
            qobject_unref(obj); return false;
        }
    }
    qobject_unref(obj); return true;
}

static bool module_pin(Esp32GpioState *gpio, int pin, Error **errp)
{
    if (pin < -1 || (pin >= 0 && (unsigned)pin >= gpio->pin_count)) {
        error_setg(errp,"GPIO %d is outside this SoC's range",pin); return false;
    }
    return true;
}

#define TYPE_ESP_SPI_MODULE "esp-spi-module"
OBJECT_DECLARE_SIMPLE_TYPE(EspSpiModule, ESP_SPI_MODULE)
struct EspSpiModule {
    SSIPeripheral parent;
    Esp32GpioState *gpio;
    SSIBus *socket;
    DeviceState *controller;
    char *model, *properties, *drive;
    uint32_t host;
    int32_t sck, mosi, miso, cs, dc, reset, backlight;
    bool backlight_low;
    uint64_t wired_bytes, dropped_bytes;
};

static unsigned module_signal(EspSpiModule *s, unsigned role)
{
    static const unsigned hspi[] = {8,10,9};
    static const unsigned fspi[] = {63,65,64};
    return !s->gpio->matrix_c3 && s->host == 2 ? hspi[role] : fspi[role];
}

static bool spi_module_chip_selected(SSIPeripheral *dev)
{
    EspSpiModule *s = ESP_SPI_MODULE(dev);
    return s->cs < 0 || (esp_gpio_output_enabled(s->gpio,s->cs) && !SSI_PERIPHERAL(s->controller)->cs);
}

static bool spi_module_selected(EspSpiModule *s)
{
    if (!esp_gpio_has_signal(s->gpio,s->sck,module_signal(s,0),false) ||
        !esp_gpio_has_signal(s->gpio,s->mosi,module_signal(s,1),false)) return false;
    if (s->cs >= 0) {
        if (!esp_gpio_output_enabled(s->gpio,s->cs)) return false;
        if (SSI_PERIPHERAL(s->controller)->cs) return false;
    }
    return true;
}

static uint32_t spi_module_transfer(SSIPeripheral *dev, uint32_t value)
{
    EspSpiModule *s = ESP_SPI_MODULE(dev);
    if (!spi_module_selected(s)) { s->dropped_bytes++; return 0; }
    s->wired_bytes++;
    value = ssi_transfer(s->socket,value);
    if (s->miso < 0 || !esp_gpio_has_signal(s->gpio,s->miso,module_signal(s,2),true)) return 0;
    return value;
}

static void spi_module_transfer_buf(SSIPeripheral *dev, const uint8_t *tx,
                                   uint8_t *rx, size_t len)
{
    EspSpiModule *s = ESP_SPI_MODULE(dev);
    bool read = rx && s->miso >= 0 &&
                esp_gpio_has_signal(s->gpio,s->miso,module_signal(s,2),true);
    if (!spi_module_selected(s)) {
        s->dropped_bytes += len;
        if (rx) memset(rx,0,len);
        return;
    }
    s->wired_bytes += len;
    ssi_transfer_buf(s->socket,tx,read ? rx : NULL,len);
    if (rx && !read) memset(rx,0,len);
}

static void spi_module_brightness(void *opaque, int n, int duty)
{
    EspSpiModule *s = opaque;
    qemu_set_irq(qdev_get_gpio_in_named(s->controller,"brightness",0),
                 s->backlight_low ? 1000-duty : duty);
}

static bool spi_module_gpio(EspSpiModule *s, int pin, const char *name, Error **errp)
{
    NamedGPIOList *group;
    if (pin < 0) return true;
    QLIST_FOREACH(group,&s->controller->gpios,node) {
        if (group->name && !strcmp(group->name,name) && group->num_in > 0) {
            esp_gpio_connect(s->gpio,pin,qdev_get_gpio_in_named(s->controller,name,0),false);
            return true;
        }
    }
    error_setg(errp,"%s has no input '%s'",s->model,name); return false;
}

static void spi_module_realize(SSIPeripheral *dev, Error **errp)
{
    EspSpiModule *s = ESP_SPI_MODULE(dev);
    bool sd = s->model && !strcmp(s->model,"sd");
    ObjectClass *klass;
    if (!s->gpio || !s->model) { error_setg(errp,"Module requires gpio link and model"); return; }
    if (s->host < 2 || s->host > (s->gpio->matrix_c3 ? 2 : 3)) {
        error_setg(errp,"SPI%u is unavailable for external modules on this SoC",s->host); return;
    }
    const char *master = object_get_canonical_path_component(OBJECT(qdev_get_parent_bus(DEVICE(s))->parent));
    g_autofree char *expected = g_strdup_printf("spi%u",s->host);
    if (strcmp(master,expected)) { error_setg(errp,"Module SPI host does not match its parent bus"); return; }
    int pins[] = {s->sck,s->mosi,s->miso,s->cs,s->dc,s->reset,s->backlight};
    for (unsigned i=0;i<G_N_ELEMENTS(pins);i++) if (!module_pin(s->gpio,pins[i],errp)) return;
    if (s->sck < 0 || s->mosi < 0 || (sd && s->miso < 0)) {
        error_setg(errp,"SPI module needs SCK/MOSI, SD also needs MISO"); return;
    }
    const char *type = sd ? "ssi-sd" : s->model;
    klass = object_class_by_name(type);
    if (!klass || !object_class_dynamic_cast(klass,TYPE_SSI_PERIPHERAL)) {
        error_setg(errp,"'%s' is not a compiled SSI peripheral",type); return;
    }
    s->socket = ssi_create_bus(DEVICE(s),"socket");
    ssi_set_idle_value(s->socket,0xff);
    s->controller = qdev_new(type);
    if (DEVICE(s)->id) s->controller->id = g_strdup_printf("%s-display",DEVICE(s)->id);
    object_property_add_child(OBJECT(s),"controller",OBJECT(s->controller));
    if (!module_properties(s->controller,s->properties,errp) ||
        !ssi_realize_and_unref(s->controller,s->socket,errp)) return;
    qemu_set_irq(qdev_get_gpio_in_named(s->controller,SSI_GPIO_CS,0),s->cs >= 0);
    if (sd) {
        BlockBackend *blk = s->drive ? blk_by_name(s->drive) : NULL;
        if (!blk) { error_setg(errp,"SD module requires an existing -drive if=none,id=... backend"); return; }
        DeviceState *card = qdev_new(TYPE_SD_CARD_SPI);
        qdev_prop_set_drive_err(card,"drive",blk,errp);
        if (*errp) { object_unref(OBJECT(card)); return; }
        if (!qdev_realize_and_unref(card,qdev_get_child_bus(s->controller,"sd-bus"),errp)) return;
    }
    if (!spi_module_gpio(s,s->cs,SSI_GPIO_CS,errp) ||
        !spi_module_gpio(s,s->dc,"dc",errp) ||
        !spi_module_gpio(s,s->reset,"reset-pin",errp)) return;
    if (!strcmp(type,"st7789")) {
        qemu_set_irq(qdev_get_gpio_in_named(s->controller,"backlight",0),1);
        if (s->backlight >= 0) esp_gpio_connect(s->gpio,s->backlight,
            qemu_allocate_irq(spi_module_brightness,s,0),true);
    } else if (s->backlight >= 0) {
        error_setg(errp,"PWM backlight is currently available for st7789 only"); return;
    }
    object_property_add_uint64_ptr(OBJECT(s),"wired-bytes",&s->wired_bytes,OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s),"dropped-bytes",&s->dropped_bytes,OBJ_PROP_FLAG_READ);
}

static const Property spi_module_properties[] = {
    DEFINE_PROP_LINK("gpio",EspSpiModule,gpio,TYPE_ESP32_GPIO,Esp32GpioState *),
    DEFINE_PROP_STRING("model",EspSpiModule,model),
    DEFINE_PROP_STRING("properties",EspSpiModule,properties),
    DEFINE_PROP_STRING("drive",EspSpiModule,drive),
    DEFINE_PROP_UINT32("spi-host",EspSpiModule,host,2),
    DEFINE_PROP_INT32("sck",EspSpiModule,sck,-1),
    DEFINE_PROP_INT32("mosi",EspSpiModule,mosi,-1),
    DEFINE_PROP_INT32("miso",EspSpiModule,miso,-1),
    DEFINE_PROP_INT32("cs-gpio",EspSpiModule,cs,-1),
    DEFINE_PROP_INT32("dc-gpio",EspSpiModule,dc,-1),
    DEFINE_PROP_INT32("reset-gpio",EspSpiModule,reset,-1),
    DEFINE_PROP_INT32("backlight-gpio",EspSpiModule,backlight,-1),
    DEFINE_PROP_BOOL("backlight-active-low",EspSpiModule,backlight_low,false),
    DEFINE_PROP_END_OF_LIST(),
};
static void spi_module_class_init(ObjectClass *klass, void *data)
{
    SSIPeripheralClass *ssi = SSI_PERIPHERAL_CLASS(klass);
    ssi->realize = spi_module_realize;
    ssi->transfer = spi_module_transfer;
    ssi->transfer_buf = spi_module_transfer_buf;
    ssi->cs_polarity = SSI_CS_NONE;
    ssi->is_selected = spi_module_chip_selected;
    DEVICE_CLASS(klass)->hotpluggable = false;
    device_class_set_props(DEVICE_CLASS(klass),spi_module_properties);
}

#define TYPE_ESP_I2C_MODULE "esp-i2c-module"
OBJECT_DECLARE_SIMPLE_TYPE(EspI2cModule, ESP_I2C_MODULE)
struct EspI2cModule {
    I2CSlave parent;
    Esp32GpioState *gpio;
    I2CBus *socket;
    char *model, *properties;
    int32_t scl, sda;
    uint32_t scl_signal, sda_signal;
};
static bool i2c_module_wired(EspI2cModule *s)
{
    return esp_gpio_has_signal(s->gpio,s->scl,s->scl_signal,false) &&
           esp_gpio_has_signal(s->gpio,s->sda,s->sda_signal,false);
}
static int i2c_module_event(I2CSlave *dev, enum i2c_event event)
{
    EspI2cModule *s = ESP_I2C_MODULE(dev);
    switch (event) {
    case I2C_START_SEND: case I2C_START_RECV:
        return i2c_module_wired(s) ? i2c_start_transfer(s->socket,dev->address,event==I2C_START_RECV) : -1;
    case I2C_FINISH: i2c_end_transfer(s->socket); break;
    case I2C_NACK: i2c_nack(s->socket); break;
    default: return -1;
    }
    return 0;
}
static int i2c_module_send(I2CSlave *dev, uint8_t value)
{
    EspI2cModule *s = ESP_I2C_MODULE(dev);
    return i2c_module_wired(s) ? i2c_send(s->socket,value) : -1;
}
static uint8_t i2c_module_recv(I2CSlave *dev)
{
    EspI2cModule *s = ESP_I2C_MODULE(dev);
    return i2c_module_wired(s) ? i2c_recv(s->socket) : 0xff;
}
static void i2c_module_realize(DeviceState *dev, Error **errp)
{
    EspI2cModule *s = ESP_I2C_MODULE(dev);
    ObjectClass *klass = s->model ? object_class_by_name(s->model) : NULL;
    if (!s->gpio || !klass || !object_class_dynamic_cast(klass,TYPE_I2C_SLAVE)) {
        error_setg(errp,"I2C module requires gpio link and compiled I2C slave model"); return;
    }
    if (!module_pin(s->gpio,s->scl,errp) || !module_pin(s->gpio,s->sda,errp)) return;
    if (s->scl < 0 || s->sda < 0) { error_setg(errp,"I2C module needs SCL and SDA GPIOs"); return; }
    s->socket = i2c_init_bus(dev,"socket");
    DeviceState *child = DEVICE(i2c_slave_new(s->model,I2C_SLAVE(s)->address));
    if (dev->id) child->id = g_strdup_printf("%s-display",dev->id);
    object_property_add_child(OBJECT(s),"controller",OBJECT(child));
    if (!module_properties(child,s->properties,errp)) return;
    i2c_slave_realize_and_unref(I2C_SLAVE(child),s->socket,errp);
}
static const Property i2c_module_properties[] = {
    DEFINE_PROP_LINK("gpio",EspI2cModule,gpio,TYPE_ESP32_GPIO,Esp32GpioState *),
    DEFINE_PROP_STRING("model",EspI2cModule,model),
    DEFINE_PROP_STRING("properties",EspI2cModule,properties),
    DEFINE_PROP_INT32("scl",EspI2cModule,scl,-1),
    DEFINE_PROP_INT32("sda",EspI2cModule,sda,-1),
    DEFINE_PROP_UINT32("scl-signal",EspI2cModule,scl_signal,53),
    DEFINE_PROP_UINT32("sda-signal",EspI2cModule,sda_signal,54),
    DEFINE_PROP_END_OF_LIST(),
};
static void i2c_module_class_init(ObjectClass *klass, void *data)
{
    I2CSlaveClass *i2c = I2C_SLAVE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = i2c_module_realize; dc->hotpluggable = false;
    i2c->event = i2c_module_event; i2c->send = i2c_module_send; i2c->recv = i2c_module_recv;
    device_class_set_props(dc,i2c_module_properties);
}
static const TypeInfo module_types[] = {
    {.name=TYPE_ESP_SPI_MODULE,.parent=TYPE_SSI_PERIPHERAL,.instance_size=sizeof(EspSpiModule),.class_init=spi_module_class_init},
    {.name=TYPE_ESP_I2C_MODULE,.parent=TYPE_I2C_SLAVE,.instance_size=sizeof(EspI2cModule),.class_init=i2c_module_class_init},
};
DEFINE_TYPES(module_types)
