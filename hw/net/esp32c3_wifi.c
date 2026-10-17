/*
 * ESP32-C3 Wi-Fi MAC register and DMA model
 *
 * The register subset is based on the independently reverse-engineered
 * ESP32-C3 Open MAC driver and the PICSimLab ESP32-C3 QEMU model.
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/net/esp32c3_wifi.h"
#include "hw/net/esp32c3_wifi_80211.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "qemu/log.h"
#include "qemu/guest-random.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "trace.h"
#include "esp32c3_wifi_trace.h"

/* C2 ROM uses RX 0x64/0x68 and TX PLCP0 0xce8; C3 uses 0x84/0x88
 * and 0xd08. Keep the shared 802.11 and DMA implementation. */
static hwaddr wifi_register(Esp32C3WifiState *s, hwaddr addr)
{
    if (!s->c2_layout) return addr;
    switch (addr) {
    case 0x064: return ESP32C3_WIFI_RX_CTRL;
    case 0x068: return ESP32C3_WIFI_RX_BASE_DESC;
    case 0x06c: return ESP32C3_WIFI_RX_NEXT_DESC;
    case 0x070: return ESP32C3_WIFI_RX_LAST_DESC;
    case 0xcf8: return addr;
    case 0xcf4: return ESP32C3_WIFI_TX_DMA_STATUS;
    default:
        if (addr >= 0xcc8 && addr <= 0xce8 && !(addr & 7)) return addr + 0x20;
        if ((addr >= 0x084 && addr <= 0x090) ||
            (addr >= 0xce8 && addr <= 0xd14)) return addr + 0x1000;
        return addr;
    }
}

static int esp32c3_wifi_match_mac(const uint8_t *actual,
                                  const uint8_t *configured)
{
    static const uint8_t broadcast[6] = { [0 ... 5] = 0xff };

    return !memcmp(actual, configured, sizeof(broadcast)) ||
           !memcmp(actual, broadcast, sizeof(broadcast));
}

static void esp32c3_wifi_update_irq(Esp32C3WifiState *s)
{
    qemu_set_irq(s->irq, s->interrupt_status != 0);
}

int esp32c3_wifi_channel(Esp32C3WifiState *s)
{
    return s->radio ? s->radio->channel : s->channel;
}

void esp32c3_wifi_set_packet_status(Esp32C3WifiState *s, uint32_t status)
{
    if (s->radio) {
        esp32c3_radio_set_packet_status(s->radio, status);
    }
}

void Esp32_WLAN_frame_delivered(Esp32C3WifiState *s)
{
    s->interrupt_status |= ESP32C3_WIFI_INT_TX_DONE;
    esp32c3_wifi_update_irq(s);
}

static int esp32c3_wifi_tx_slot(hwaddr addr)
{
    hwaddr distance;

    if (addr > ESP32C3_WIFI_TX_PLCP0_BASE) {
        return -1;
    }

    distance = ESP32C3_WIFI_TX_PLCP0_BASE - addr;
    if (distance % ESP32C3_WIFI_TX_SLOT_STRIDE) {
        return -1;
    }

    distance /= ESP32C3_WIFI_TX_SLOT_STRIDE;
    return distance < ESP32C3_WIFI_TX_SLOT_COUNT ? distance : -1;
}

static void esp32c3_wifi_complete_tx(Esp32C3WifiState *s, int slot,
                                     uint32_t value)
{
    Esp32C3WifiDmaDesc desc;
    mac80211_frame frame = { 0 };
    hwaddr desc_addr;
    MemTxResult result;

    desc_addr = 0x3fc00000u | (value & ESP32C3_WIFI_DMA_ADDR_MASK);
    result = address_space_read(&address_space_memory, desc_addr,
                                MEMTXATTRS_UNSPECIFIED, &desc,
                                sizeof(desc));
    if (result != MEMTX_OK || desc.length > sizeof(frame.data_and_fcs) +
                                                 IEEE80211_HEADER_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32c3-wifi: failed to read TX descriptor at "
                      "0x%08" HWADDR_PRIx "\n", desc_addr);
        s->tx_error |= 1u << slot;
    } else {
        result = address_space_read(&address_space_memory, desc.address,
                                    MEMTXATTRS_UNSPECIFIED, &frame,
                                    desc.length);
        if (result == MEMTX_OK) {
            frame.frame_length = desc.length;
            Esp32_WLAN_handle_frame(s, &frame);
            s->tx_done |= 1u << (slot + (s->c2_layout ? 16 : 0));
        } else {
            s->tx_error |= 1u << slot;
        }
    }

    s->interrupt_status |= ESP32C3_WIFI_INT_TX_DONE;
    esp32c3_wifi_update_irq(s);
}

static void esp32c3_wifi_trace_rx_drop(Esp32C3WifiState *s,
                                        mac80211_frame *frame, int length,
                                        const char *reason)
{
    /* Observe model-side losses without changing delivery or guest state. */
    trace_esp32c3_wifi_rx_drop(qemu_clock_get_us(QEMU_CLOCK_VIRTUAL),
                             s->rx_desc, frame->frame_control.type,
                             frame->frame_control.sub_type,
                             frame->sequence_control.sequence_number,
                             length, reason);
}

void Esp32_sendFrame(Esp32C3WifiState *s, mac80211_frame *frame, int length,
                     int signal_strength)
{
    Esp32C3WifiDmaDesc desc;
    wifi_pkt_rx_ctrl_c3_t metadata = { 0 };
    uint8_t *packet;
    uint32_t jitter;
    size_t packet_length;
    MemTxResult result;

    if (length <= 0) {
        return;
    }
    if (!s->rx_desc) {
        esp32c3_wifi_trace_rx_drop(s, frame, length, "no-descriptor");
        return;
    }

    result = address_space_read(&address_space_memory, s->rx_desc,
                                MEMTXATTRS_UNSPECIFIED, &desc, sizeof(desc));
    if (result != MEMTX_OK) {
        esp32c3_wifi_trace_rx_drop(s, frame, length, "descriptor-read");
        return;
    }

    packet_length = sizeof(metadata) + length;
    if (packet_length > desc.size) {
        esp32c3_wifi_trace_rx_drop(s, frame, length, "buffer-size");
        qemu_log_mask(LOG_GUEST_ERROR,
                      "esp32c3-wifi: RX packet (%zu bytes) exceeds DMA "
                      "buffer (%u bytes)\n", packet_length, desc.size);
        return;
    }

    qemu_guest_getrandom_nofail(&jitter, sizeof(jitter));
    metadata.rssi = signal_strength + (jitter % 10) + (s->c2_layout ? 0 : 96);
    metadata.rate = 11;
    metadata.sig_len = length;
    metadata.sig_len_copy = length;
    metadata.legacy_length = length;
    metadata.noise_floor = -97;
    metadata.channel = esp32c3_wifi_channel(s);
    metadata.timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000;

    if (esp32c3_wifi_match_mac(frame->destination_address,
                               (uint8_t *)s->regs + (s->c2_layout ? 0x30 : 0x40))) {
        metadata.damatch0 = 1;
    }
    if (esp32c3_wifi_match_mac(frame->destination_address,
                               (uint8_t *)s->regs + (s->c2_layout ? 0x38 : 0x48))) {
        metadata.damatch1 = 1;
    }
    if (esp32c3_wifi_match_mac(frame->bssid_address,
                               (uint8_t *)s->regs + (s->c2_layout ? 0x00 : 0x40))) {
        metadata.bssidmatch0 = 1;
    }
    if (esp32c3_wifi_match_mac(frame->bssid_address,
                               (uint8_t *)s->regs + (s->c2_layout ? 0x08 : 0x48))) {
        metadata.bssidmatch1 = 1;
    }

    if (s->c2_layout && s->softap_client_ssid &&
        s->mode == Esp32_Mode_SoftAP &&
        frame->frame_control.type == IEEE80211_TYPE_MGT &&
        (frame->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_BEACON ||
         frame->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_PROBE_RESP) &&
        memcmp(frame->bssid_address, s->softap_macaddr, 6)) {
        /* External scan results belong to the unassociated STA interface,
         * never to the guest AP's peer table. SDK initialization order may
         * place STA in either hardware MAC slot. Its BSSID filter is wildcard
         * during scan; the old exact-MAC model discarded every result. */
        bool station_first = memcmp((uint8_t *)s->regs + 0x30,
                                    s->softap_macaddr, 6) != 0;
        metadata.damatch0 = metadata.bssidmatch0 = station_first;
        metadata.damatch1 = metadata.bssidmatch1 = !station_first;
    }

    packet = g_malloc(packet_length);
    memcpy(packet, &metadata, sizeof(metadata));
    memcpy(packet + sizeof(metadata), frame, length);
    result = address_space_write(&address_space_memory, desc.address,
                                 MEMTXATTRS_UNSPECIFIED, packet,
                                 packet_length);
    g_free(packet);
    if (result != MEMTX_OK) {
        esp32c3_wifi_trace_rx_drop(s, frame, length, "packet-write");
        return;
    }

    desc.length = packet_length;
    desc.eof = 1;
    address_space_write(&address_space_memory, s->rx_desc,
                        MEMTXATTRS_UNSPECIFIED, &desc, sizeof(uint32_t));
    /* C2's wDev_AppendRxBlocks reads the completed tail when NEXT becomes
     * zero. Returning NEXT as LAST fabricates 0x3fc00000 and faults on +8. */
    esp32_wifi_trace_packet(s, frame, "dma", s->rx_desc, desc.next);
    s->rx_last_desc = s->rx_desc;
    s->rx_desc = desc.next;
    s->interrupt_status |= ESP32C3_WIFI_INT_RX_DONE;
    esp32c3_wifi_update_irq(s);
}

static uint64_t esp32c3_wifi_read(void *opaque, hwaddr addr,
                                  unsigned size)
{
    Esp32C3WifiState *s = ESP32C3_WIFI(opaque);

    hwaddr physical_addr = addr;
    addr = wifi_register(s, addr);
    switch (addr) {
    case 0xcf8:
        if (s->c2_layout) return 1; /* MAC reset done */
        return s->regs[addr / 4];
    case ESP32C3_WIFI_INT_STATUS:
        return s->interrupt_status;
    case ESP32C3_WIFI_TX_ERROR:
        return s->tx_error;
    case ESP32C3_WIFI_TX_DONE:
        return 1 | s->tx_done;
    case ESP32C3_WIFI_TX_DMA_STATUS:
        return 1;
    case ESP32C3_WIFI_RX_NEXT_DESC:
        return s->rx_desc;
    case ESP32C3_WIFI_RX_LAST_DESC:
        return s->c2_layout ? s->rx_last_desc : s->rx_desc;
    default:
        return s->regs[physical_addr / sizeof(uint32_t)];
    }
}

static void esp32c3_wifi_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    Esp32C3WifiState *s = ESP32C3_WIFI(opaque);
    int slot;

    hwaddr physical_addr = addr;
    addr = wifi_register(s, addr);
    switch (addr) {
    case ESP32C3_WIFI_RX_BASE_DESC:
        s->rx_desc = value;
        Esp32_WLAN_rx_ready(s);
        break;
    case ESP32C3_WIFI_RX_CTRL:
        /* Hardware clears the RX descriptor update request immediately. */
        value &= ~1u;
        break;
    case ESP32C3_WIFI_INT_CLEAR:
        s->interrupt_status &= ~value;
        esp32c3_wifi_update_irq(s);
        break;
    case ESP32C3_WIFI_TX_ERROR_CLEAR:
        s->tx_error &= ~value;
        break;
    case ESP32C3_WIFI_TX_DONE_CLEAR:
        s->tx_done &= ~value;
        break;
    default:
        slot = esp32c3_wifi_tx_slot(addr);
        if (slot >= 0 && (value & ESP32C3_WIFI_TX_TRIGGER_MASK)) {
            esp32c3_wifi_complete_tx(s, slot, value);
        }
        break;
    }

    s->regs[physical_addr / sizeof(uint32_t)] = value;
    if (addr == ESP32C3_WIFI_RX_BASE_DESC || addr == ESP32C3_WIFI_RX_CTRL ||
        addr == ESP32C3_WIFI_INT_CLEAR) {
        trace_esp32c3_wifi_rx_state(qemu_clock_get_us(QEMU_CLOCK_VIRTUAL),
            physical_addr, value, s->rx_desc, s->rx_last_desc,
            s->interrupt_status, s->inject_queue_size);
    }
}

static const MemoryRegionOps esp32c3_wifi_ops = {
    .read = esp32c3_wifi_read,
    .write = esp32c3_wifi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void esp32c3_wifi_reset(DeviceState *dev)
{
    Esp32C3WifiState *s = ESP32C3_WIFI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->interrupt_status = 0;
    s->tx_done = 0;
    s->tx_error = 0;
    s->rx_desc = 0;
    s->rx_last_desc = 0;
    Esp32_WLAN_reset_ap(s);
    esp32c3_wifi_update_irq(s);
}

static bool wifi_valid_service_host(const char *host)
{
    const char *label;

    if (!host || !*host || strlen(host) > 253) {
        return false;
    }
    label = host;
    for (const char *p = host; ; p++) {
        if (*p == '.' || !*p) {
            size_t length = p - label;

            if (!length || length > 63 || *label == '-' || p[-1] == '-') {
                return false;
            }
            if (!*p) {
                return true;
            }
            label = p + 1;
        } else if (!g_ascii_isalnum(*p) && *p != '-') {
            return false;
        }
    }
}

static bool wifi_parse_hosts(const char *value, char ***out, size_t limit,
                             const char *name, Error **errp)
{
    g_auto(GStrv) hosts = value && *value ? g_strsplit(value, ";", -1) :
                                          g_new0(char *, 1);
    size_t count = g_strv_length(hosts);

    if (count > limit) {
        error_setg(errp, "%s contains too many hostnames", name);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (!wifi_valid_service_host(hosts[i])) {
            error_setg(errp, "%s must contain valid ASCII DNS hostnames", name);
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (!g_ascii_strcasecmp(hosts[i], hosts[j])) {
                error_setg(errp, "%s must contain distinct hostnames", name);
                return false;
            }
        }
    }
    *out = g_steal_pointer(&hosts);
    return true;
}

static void esp32c3_wifi_realize(DeviceState *dev, Error **errp)
{
    Esp32C3WifiState *s = ESP32C3_WIFI(dev);

    if (!wifi_parse_hosts(s->service_hosts, &s->service_hostnames, 155,
                          "service-hosts", errp) ||
        !wifi_parse_hosts(s->ntp_hosts, &s->ntp_hostnames, 3,
                          "ntp-hosts", errp)) {
        return;
    }
    for (size_t i = 0; s->service_hostnames[i]; i++) {
        for (size_t j = 0; s->ntp_hostnames[j]; j++) {
            if (!g_ascii_strcasecmp(s->service_hostnames[i], s->ntp_hostnames[j])) {
                error_setg(errp, "HTTP and NTP service hostnames must be distinct");
                return;
            }
        }
    }
    if (!s->station_ssid || !s->station_ssid[0] || strlen(s->station_ssid) > 32) {
        error_setg(errp, "station-ssid must contain 1..32 bytes");
        return;
    }
    if (s->softap_client_ssid && (!s->softap_client_ssid[0] ||
                                strlen(s->softap_client_ssid) > 32)) {
        error_setg(errp, "softap-client-ssid must contain 1..32 bytes");
        return;
    }
    Esp32_WLAN_setup_ap(dev, s);
}


static void wifi_get_ntp_seconds(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    /* Observe the local fixture epoch; -1 keeps the normal host clock. */
    visit_type_int64(v, name, &ESP32C3_WIFI(obj)->ntp_unix_seconds, errp);
}

static void wifi_set_ntp_seconds(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    /* Let QMP change the service response without touching any guest clock/RAM. */
    int64_t value;
    if (!visit_type_int64(v, name, &value, errp)) return;
    if (value < -1) {
        error_setg(errp, "ntp-unix-seconds must be -1 or a nonnegative epoch");
        return;
    }
    ESP32C3_WIFI(obj)->ntp_unix_seconds = value;
}

static void wifi_get_ntp_drop_mask(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    /* Read the three-server failure mask; default zero preserves normal replies. */
    visit_type_uint32(v, name, &ESP32C3_WIFI(obj)->ntp_drop_mask, errp);
}

static void wifi_set_ntp_drop_mask(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    /* Drop only selected synthetic NTP servers, never unrelated network traffic. */
    uint32_t value;
    if (!visit_type_uint32(v, name, &value, errp)) return;
    if (value > 7) {
        error_setg(errp, "ntp-drop-mask must use only bits 0..2");
        return;
    }
    ESP32C3_WIFI(obj)->ntp_drop_mask = value;
}

static bool wifi_get_ntp_separate(Object *obj, Error **errp)
{
    /* Expose the optional per-server addresses for retry/fallback experiments. */
    return ESP32C3_WIFI(obj)->ntp_separate_servers;
}

static void wifi_set_ntp_separate(Object *obj, bool value, Error **errp)
{
    /* Keep default DNS aliases, or opt into .2/.4/.5 for the three NTP servers. */
    ESP32C3_WIFI(obj)->ntp_separate_servers = value;
}

static bool wifi_get_station_ap_online(Object *obj, Error **errp)
{
    /* Read the external station AP state, independently of the guest's SoftAP. */
    return ESP32C3_WIFI(obj)->station_ap_online;
}

static void wifi_set_station_ap_online(Object *obj, bool value, Error **errp)
{
    /* Model a graceful AP shutdown through a real 802.11 management frame. */
    Esp32_WLAN_set_station_ap_online(ESP32C3_WIFI(obj), value);
}

static void esp32c3_wifi_init(Object *obj)
{
    Esp32C3WifiState *s = ESP32C3_WIFI(obj);

    s->ntp_unix_seconds = -1;
    s->station_ap_online = true;
    s->station_ssid = g_strdup("QEMU");
    object_property_add(obj, "ntp-unix-seconds", "int", wifi_get_ntp_seconds,
                        wifi_set_ntp_seconds, NULL, NULL);
    object_property_add(obj, "ntp-drop-mask", "uint32", wifi_get_ntp_drop_mask,
                        wifi_set_ntp_drop_mask, NULL, NULL);
    object_property_add_bool(obj, "ntp-separate-servers", wifi_get_ntp_separate,
                             wifi_set_ntp_separate);
    object_property_add_bool(obj, "station-ap-online", wifi_get_station_ap_online,
                             wifi_set_station_ap_online);

    memory_region_init_io(&s->iomem, obj, &esp32c3_wifi_ops, s,
                          TYPE_ESP32C3_WIFI, ESP32C3_WIFI_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void esp32c3_wifi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    static const Property properties[] = {
        DEFINE_PROP_BOOL("clock-services", Esp32C3WifiState, clock_services, false),
        DEFINE_PROP_BOOL("weather-services", Esp32C3WifiState, weather_services, false),
        DEFINE_PROP_STRING("service-hosts", Esp32C3WifiState, service_hosts),
        DEFINE_PROP_STRING("ntp-hosts", Esp32C3WifiState, ntp_hosts),
        DEFINE_PROP_STRING("station-ssid", Esp32C3WifiState, station_ssid),
        DEFINE_PROP_STRING("softap-client-ssid", Esp32C3WifiState, softap_client_ssid),
        DEFINE_PROP_BOOL("c2-layout", Esp32C3WifiState, c2_layout, false),
        DEFINE_NIC_PROPERTIES(Esp32C3WifiState, conf),
        DEFINE_PROP_UINT64("bandwidth-bps", Esp32C3WifiState,
                           bandwidth_bps, 4000000),
        DEFINE_PROP_UINT32("latency-ms", Esp32C3WifiState, latency_ms, 20),
        DEFINE_PROP_UINT32("jitter-ms", Esp32C3WifiState, jitter_ms, 5),
        DEFINE_PROP_UINT32("loss-ppm", Esp32C3WifiState, loss_ppm, 0),
        DEFINE_PROP_UINT32("queue-limit", Esp32C3WifiState, queue_limit, 64),
        DEFINE_PROP_BOOL("link-down", Esp32C3WifiState, link_down, false),
        DEFINE_PROP_END_OF_LIST(),
    };

    dc->desc = "ESP32-C3 Wi-Fi MAC";
    dc->realize = esp32c3_wifi_realize;
    device_class_set_props(dc, properties);
    device_class_set_legacy_reset(dc, esp32c3_wifi_reset);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static void esp32c3_wifi_finalize(Object *obj)
{
    Esp32C3WifiState *s = ESP32C3_WIFI(obj);

    g_strfreev(s->service_hostnames);
    g_strfreev(s->ntp_hostnames);
}

static const TypeInfo esp32c3_wifi_info = {
    .name = TYPE_ESP32C3_WIFI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32C3WifiState),
    .instance_init = esp32c3_wifi_init,
    .instance_finalize = esp32c3_wifi_finalize,
    .class_init = esp32c3_wifi_class_init,
};

static void esp32c3_wifi_register_types(void)
{
    type_register_static(&esp32c3_wifi_info);
}

type_init(esp32c3_wifi_register_types)
