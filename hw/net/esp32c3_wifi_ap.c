/**
 * QEMU WLAN access point emulation
 *
 * Copyright (c) 2008 Clemens Kolbitsch
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Modifications:
 *  2008-February-24  Clemens Kolbitsch :
 *                                  New implementation based on ne2000.c
 *  18/01/22 Martin Johnson : Modified for esp32 wifi emulation
 *  08/12/23 lcgamboa@yahoo.com : Modified for esp32c3, espnow and SoftAp wifi emulation
 */


#include "qemu/osdep.h"
#include "net/net.h"
#include "qemu/guest-random.h"
#include "qemu/timer.h"
#include "esp32c3_wifi_trace.h"
#include "esp32c3_wifi_link.h"
#include "esp32c3_wifi_interface.h"

#include "hw/irq.h"
#include "hw/net/esp32c3_wifi.h"
#include "hw/misc/esp32c3_radio.h"
#include "hw/net/esp32c3_wifi_80211.h"
#include "hw/net/esp32c3_wifi_frame.h"

// 50ms between beacons
#define BEACON_TIME 500000000
#define WAIT_ACK_TIMEOUT 10000000
/* Retry virtual peer management exchanges; no guest task or busy-wait delay. */
#define MANAGEMENT_RETRY_NS 1000000000
#define DEBUG 0
#define ENABLE_BEACON 1
#define DEBUG_DUMPFRAMES 0
#define DEBUG_WIRESHARK_IMPORT 0

// color defines
#define BLACK 0
#define RED 1
#define GREEN 2
#define YELLOW 3
#define BLUE 4
#define MAGNETA 5
#define CYAN 6
#define WHITE 7
#define ANSI_DEFAULT() printf("\033[0m")
#define ANSI_COLOR(f, b) printf("\033[%d;%dm", (f) + 30, (b) + 40)
#define ANSI_FG_LCOLOR(f) printf("\033[0;%dm", (f) + 30)
#define ANSI_FG_HCOLOR(f) printf("\033[1;%dm", (f) + 30)

access_point_info access_points[]={
    {"QEMU",1,-25,{0x10,0x01,0x00,0xc4,0x0a,0x56}},
    {"QEMU-2",5,-30,{0x10,0x01,0x00,0xc4,0x0a,0x51}},
    {"QEMU-3",10,-40,{0x10,0x01,0x00,0xc4,0x0a,0x52}}
};

int nb_aps=sizeof(access_points)/sizeof(access_point_info);

typedef struct Esp32C3WifiEthernetPacket {
    struct Esp32C3WifiEthernetPacket *next;
    int64_t delivery_ns;
    size_t size;
    uint8_t interface_id;
    uint8_t data[];
} Esp32C3WifiEthernetPacket;

static void Esp32_WLAN_ethernet_tx_timer(void *opaque)
{
    Esp32C3WifiState *s = opaque;
    Esp32C3WifiEthernetPacket *packet = s->ethernet_tx_queue;

    if (!packet) {
        return;
    }
    s->ethernet_tx_queue = packet->next;
    if (!s->ethernet_tx_queue) {
        s->ethernet_tx_tail = NULL;
    }
    s->ethernet_tx_queue_size--;
    qemu_send_packet(qemu_get_queue(s->nic), packet->data, packet->size);
    g_free(packet);

    if (s->ethernet_tx_queue) {
        timer_mod(s->ethernet_tx_timer,
                  esp32_wifi_next_delivery(s, s->ethernet_tx_queue->delivery_ns,
                                          s->ethernet_tx_queue->size));
    }
}

static void Esp32_WLAN_send_ethernet(Esp32C3WifiState *s,
                                     const uint8_t *data, size_t size)
{
    Esp32C3WifiEthernetPacket *packet;

    if (esp32c2_clock_service(s, data, size)) return;

    if (esp32_wifi_packet_lost(s) ||
        s->ethernet_tx_queue_size >= s->queue_limit) {
        return;
    }

    packet = g_malloc(sizeof(*packet) + size);
    packet->next = NULL;
    packet->size = size;
    packet->interface_id = esp32_wifi_interface_id(s, s->wlan);
    memcpy(packet->data, data, size);
    packet->delivery_ns = esp32_wifi_delivery_time(
        s, size, &s->tx_serial_ready_ns);

    if (s->ethernet_tx_tail) {
        s->ethernet_tx_tail->next = packet;
    } else {
        s->ethernet_tx_queue = packet;
        timer_mod(s->ethernet_tx_timer, esp32_wifi_next_delivery(s,
                  packet->delivery_ns, packet->size));
    }
    s->ethernet_tx_tail = packet;
    s->ethernet_tx_queue_size++;
}

/* Discard one departed peer without disturbing the other radio queues. */
static void discard_interface_traffic(Esp32C3WifiState *s, uint8_t id)
{
    struct mac80211_frame *frame = s->inject_queue, *previous = NULL;
    Esp32C3WifiEthernetPacket **ethernet = &s->ethernet_tx_queue;

    s->inject_queue_size = 0;
    while (frame) {
        struct mac80211_frame *next = frame->next_frame;
        if (frame->interface_id == id) {
            if (previous) {
                previous->next_frame = next;
            } else {
                s->inject_queue = next;
            }
            free(frame);
        } else {
            s->inject_queue_size++;
            previous = frame;
        }
        frame = next;
    }
    s->ethernet_tx_tail = NULL;
    s->ethernet_tx_queue_size = 0;
    while (*ethernet) {
        Esp32C3WifiEthernetPacket *packet = *ethernet;
        if (packet->interface_id == id) {
            *ethernet = packet->next;
            g_free(packet);
        } else {
            s->ethernet_tx_tail = packet;
            s->ethernet_tx_queue_size++;
            ethernet = &packet->next;
        }
    }
    timer_del(s->inject_timer);
    timer_del(s->ethernet_tx_timer);
    s->inject_timer_running = s->inject_queue != NULL;
    if (s->inject_queue) {
        timer_mod(s->inject_timer, esp32_wifi_next_delivery(s,
                  s->inject_queue->delivery_ns, s->inject_queue->frame_length));
    }
    if (s->ethernet_tx_queue) {
        timer_mod(s->ethernet_tx_timer, esp32_wifi_next_delivery(s,
                  s->ethernet_tx_queue->delivery_ns, s->ethernet_tx_queue->size));
    }
}

/* Shut down the external AP only; the guest owns reconnect and DHCP policy. */
void Esp32_WLAN_set_station_ap_online(Esp32C3WifiState *s, bool online)
{
    Esp32C3WifiInterface *previous = s->wlan;

    if (s->station_ap_online == online) {
        return;
    }
    s->wlan = &s->station;
    if (!online && s->nic && s->wlan->mode == Esp32_Mode_Station) {
        discard_interface_traffic(s, 0);
        if (s->wlan->ap_state == Esp32_WLAN__STATE_ASSOCIATED) {
            struct mac80211_frame *frame = Esp32_WLAN_create_deauthentication();
            memcpy(frame->destination_address, s->wlan->macaddr, 6);
            memcpy(frame->source_address, s->wlan->associated_ap_macaddr, 6);
            memcpy(frame->bssid_address, s->wlan->associated_ap_macaddr, 6);
            frame->signal_strength = -25;
            frame->sequence_control.sequence_number = s->inject_sequence_number++;
            Esp32_WLAN_insert_frame(s, frame);
        }
        s->wlan->ap_state = Esp32_WLAN__STATE_NOT_AUTHENTICATED;
    }
    s->station_ap_online = online;
    s->wlan = previous;
}

/* Change virtual client presence through real leaving management frames. */
void Esp32_WLAN_set_softap_clients(Esp32C3WifiState *s, uint32_t mask)
{
    Esp32C3WifiInterface *previous = s->wlan;
    assert(!(mask & ~((1u << ESP32_WIFI_SOFTAP_CLIENTS) - 1)));
    for (unsigned i = 0; i < ESP32_WIFI_SOFTAP_CLIENTS; i++) {
        if ((s->softap_client_mask & ~mask) & (1u << i)) {
            Esp32C3WifiInterface *peer = esp32_wifi_softap_client(s, i);
            s->wlan = peer;
            if (s->nic) {
                discard_interface_traffic(s, i + 1);
                if (peer->ssid[0] && peer->ap_state != Esp32_WLAN__STATE_NOT_AUTHENTICATED) {
                    struct mac80211_frame *frame = Esp32_WLAN_create_deauthentication();
                    memcpy(frame->destination_address, peer->softap_macaddr, 6);
                    memcpy(frame->source_address, peer->ap_macaddr, 6);
                    memcpy(frame->bssid_address, peer->softap_macaddr, 6);
                    frame->sequence_control.sequence_number = s->inject_sequence_number++;
                    Esp32_WLAN_insert_frame(s, frame);
                }
            }
            peer->ap_state = Esp32_WLAN__STATE_NOT_AUTHENTICATED;
            memset(peer->offered_ipaddr, 0, sizeof(peer->offered_ipaddr));
            memset(&peer->dhcp, 0, sizeof(peer->dhcp));
            if (s->softap_host_peer == i + 1) s->softap_host_peer = 0;
        }
    }
    s->softap_client_mask = mask;
    s->wlan = previous;
}

static void Esp32_WLAN_beacon_timer(void *opaque)
{
    struct mac80211_frame *frame;
    Esp32C3WifiState *s = (Esp32C3WifiState *)opaque;
    Esp32C3WifiInterface *previous = s->wlan;
    /* The external AP advertises independently of the guest's AP client. */
    s->wlan = &s->station;
    // only send a beacon if we are an access point
    if (ENABLE_BEACON && ((s->wlan->mode == Esp32_Mode_Station && s->station_ap_online) ||
                          (s->wlan->mode == Esp32_Mode_SoftAP && s->softap_client_ssid))) {
      if(s->wlan->ap_state!=Esp32_WLAN__STATE_STA_ASSOCIATED ||
         (s->softap_client_ssid && s->wlan->mode == Esp32_Mode_SoftAP)) {
        for(int i=0;i<nb_aps;i++){
          int ap = (i + s->beacon_ap)%nb_aps;
          if (access_points[ap].channel==esp32c3_wifi_channel(s)) {
              access_point_info info = access_points[ap];
              if (ap == 0) info.ssid = s->station_ssid;
              frame = Esp32_WLAN_create_beacon_frame(&info);
              if (s->wlan->mode == Esp32_Mode_SoftAP) {
                  /* A setup AP may scan other networks while its client stays
                   * associated. Advertise their own BSSID without changing it. */
                  memcpy(frame->source_address, access_points[ap].mac_address, 6);
                  memcpy(frame->bssid_address, access_points[ap].mac_address, 6);
                  frame->sequence_control.sequence_number = s->inject_sequence_number++;
              } else {
                  memcpy(s->wlan->ap_macaddr,access_points[ap].mac_address,6);
                  Esp32_WLAN_init_ap_frame(s, frame);
              }
              Esp32_WLAN_insert_frame(s, frame);
              break;
          }
        }
        s->beacon_ap=(s->beacon_ap+1)%nb_aps;
      }
    }
    s->wlan = previous;
    timer_mod(s->beacon_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + BEACON_TIME);
}

void Esp32_WLAN_rx_ready(Esp32C3WifiState *s)
{
    /* Resume bounded C2 receive work when the guest supplies a DMA buffer.
     * No polling delay is needed: RX_BASE_DESC writes are the wakeup event. */
    if (s->c2_layout && s->rx_desc && s->inject_queue &&
        !s->inject_timer_running) {
        s->inject_timer_running = true;
        timer_mod(s->inject_timer,
                  esp32_wifi_next_delivery(s, s->inject_queue->delivery_ns,
                                          s->inject_queue->frame_length));
    }
}

static void Esp32_WLAN_inject_timer(void *opaque)
{
    Esp32C3WifiState *s = (Esp32C3WifiState *)opaque;
    struct mac80211_frame *frame;

    frame = s->inject_queue;
    if (frame && s->c2_layout && !s->rx_desc) {
        /* A burst can exhaust C2's small DMA chain before its RX ISR refills
         * it. Retain this frame and its order in the existing bounded queue;
         * silently discarding it forced SLIRP to wait for a TCP SYN retry.
         * The guest's next RX_BASE_DESC write resumes delivery. Legacy C3
         * delivery is unchanged, and queue-limit/loss controls still apply. */
        s->inject_timer_running = false;
        return;
    }
    if (frame) {
        // remove from queue
        s->inject_queue_size--;
        s->inject_queue = frame->next_frame;
        Esp32C3WifiInterface *previous = s->wlan;
        s->wlan = esp32_wifi_interface_by_id(s, frame->interface_id);
        Esp32_sendFrame(s, (void *)frame, frame->frame_length,frame->signal_strength);
        s->wlan = previous;
        free(frame);
    }
    if (s->inject_queue_size > 0) {
        // there are more packets... schedule
        // the timer for sending them as well
        timer_mod(s->inject_timer,
                  esp32_wifi_next_delivery(s, s->inject_queue->delivery_ns,
                                          s->inject_queue->frame_length));
    } else {
        // we wait until a new packet schedules
        // us again
        s->inject_timer_running = 0;
    }

}

static void Esp32_WLAN_Wait_ACk_timer(void *opaque)
{
    Esp32C3WifiState *s = (Esp32C3WifiState *)opaque;

    Esp32_WLAN_frame_delivered(s);
}

static void macprint(const uint8_t *p, const char * name) {
    printf("%s: %02x:%02x:%02x:%02x:%02x:%02x\n",name, p[0],p[1],p[2],p[3],p[4],p[5]);
}

static void buffer_print(uint8_t * b, unsigned size){
    for(int i=0;i < size ;i++) {
        if((i%16)==0) printf("%04x: ",i);
        printf("%02x ",b[i]);
        if((i%16)==15) printf("\n");
    }
    if((size%16)!=0)printf("\n");
}

static void infoprint(struct mac80211_frame *frame) {
    if(DEBUG_DUMPFRAMES) {
#if  (DEBUG_WIRESHARK_IMPORT == 0 )
        printf("\nFrame Info type:%d subtype:%d flags:%d duration:%d length:%d\n",frame->frame_control.type,
           frame->frame_control.sub_type,frame->frame_control.flags,frame->duration_id, frame->frame_length);

        printf("protocol_version: %i\n",frame->frame_control.protocol_version);
        printf("type: %i ",frame->frame_control.type );
        switch (frame->frame_control.type)
        {
        case IEEE80211_TYPE_MGT:
            printf("MGT\n");
            break;
        case IEEE80211_TYPE_CTL:
            printf("CTL\n");
            break;
       case IEEE80211_TYPE_DATA:
            printf("DATA\n");
            break;
        }
        printf("subtype: %i  ",frame->frame_control.sub_type);
        switch (frame->frame_control.type)
        {
        case IEEE80211_TYPE_MGT:
            switch (frame->frame_control.sub_type)
            {
            case IEEE80211_TYPE_MGT_SUBTYPE_BEACON:
                printf("BEACON\n");
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_ACTION:
                printf("ACTION\n");
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_PROBE_REQ:
                printf("PROBE_REQ\n");
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_PROBE_RESP:
                printf("PROBE_RESP\n");
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION:
                printf("AUTHENTICATION\n");
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_DEAUTHENTICATION:
                printf("DEAUTHENTICATION\n");
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_REQ:
                printf("ASSOCIATION_REQ\n");
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_RESP:
                printf("ASSOCIATION_RESP\n");
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_DISASSOCIATION:
                printf("DISASSOCIATION\n");
                break;
            default:
                printf("---\n");
                break;
            }
            break;
        case IEEE80211_TYPE_CTL:
           switch (frame->frame_control.sub_type)
            {
            case IEEE80211_TYPE_CTL_SUBTYPE_ACK:
                printf("ACK\n");
                break;
            default:
                printf("---\n");
                break;
            }
            break;
       case IEEE80211_TYPE_DATA:
            switch (frame->frame_control.sub_type)
            {
            case IEEE80211_TYPE_DATA_SUBTYPE_DATA:
                printf("DATA\n");
                break;
            default:
                printf("---\n");
                break;
            }
            break;
        }

        printf("flags: %i\n",frame->frame_control.flags);
        printf("duration_id: %i\n",frame->duration_id);
        macprint(frame->destination_address,"destination");
        macprint(frame->source_address,"source");
        macprint(frame->bssid_address,"bssid");
        printf("fragment_number: %u\n",frame->sequence_control.fragment_number);
        printf("sequence_number : %u\n",frame->sequence_control.sequence_number);

        switch (frame->frame_control.type)
        {
          case IEEE80211_TYPE_MGT:
            switch (frame->frame_control.sub_type)
            {
              case IEEE80211_TYPE_MGT_SUBTYPE_BEACON:
                printf("beacon timestamp : %lu\n",(unsigned long) frame->beacon_info.timestamp);
                printf("beacon interval : %u\n",frame->beacon_info.interval);
                printf("beacon capability : %u\n",frame->beacon_info.capability);
                break;
              default:
                if(frame->frame_length  > IEEE80211_HEADER_SIZE)
                   buffer_print((uint8_t *)frame->data_and_fcs, frame->frame_length - IEEE80211_HEADER_SIZE);
                break;
            }
            break;
          case IEEE80211_TYPE_CTL:
            switch (frame->frame_control.sub_type)
            {
              default:
                if(frame->frame_length  > IEEE80211_HEADER_SIZE)
                   buffer_print((uint8_t *)frame->data_and_fcs, frame->frame_length - IEEE80211_HEADER_SIZE);
                break;
            }
            break;
            case IEEE80211_TYPE_DATA:
            switch (frame->frame_control.sub_type)
            {
              default:
                printf("EtherType 0x%02X%02X\n",frame->data_and_fcs[6],frame->data_and_fcs[7]);
                if( frame->data_and_fcs[6]==8 && frame->data_and_fcs[7]==0x06) {
                    printf("ARP\n");
                    arp_header_t *header=(arp_header_t *)&frame->data_and_fcs[8];
                    printf("Operation %i\n", header->operation>>8);
                }
                if( frame->data_and_fcs[6]==8 && frame->data_and_fcs[7]==0x00) {
                    printf("IPV4\n");
                    ip_header_t *header=(ip_header_t *)&frame->data_and_fcs[8];
                    printf("Protocol: 0x%04X\n",header->protocol);
                    switch (header->protocol)
                    {
                    case 0x11:
                        printf("UDP\n");
                        break;
                    case 0x06:
                        printf("TCP\n");
                        break;
                    }
                }
                if( frame->data_and_fcs[6]==8 && frame->data_and_fcs[7]==0xdd) {
                    printf("IPV6\n");
                }
                if(frame->frame_length  > IEEE80211_HEADER_SIZE)
                   buffer_print((uint8_t *)frame->data_and_fcs, frame->frame_length - IEEE80211_HEADER_SIZE);
                break;
            }
            break;
        }
        printf("frame_length : %u\n", frame->frame_length);
#else
        uint8_t *b=(uint8_t *)frame;
        for(int i=0;i<frame->frame_length;i++) {
            if((i%16)==0) printf("\n%04x: ",i);
            printf("%02x ",b[i]);
        }
        printf("\n");
#endif
    }
}

void Esp32_WLAN_insert_frame(Esp32C3WifiState *s, struct mac80211_frame *frame)
{
    struct mac80211_frame *i_frame;

    frame->interface_id = esp32_wifi_interface_id(s, s->wlan);
    insertCRC(frame);
    if (esp32_wifi_packet_lost(s) ||
        s->inject_queue_size >= s->queue_limit) {
        esp32_wifi_trace_packet(s, frame, "enqueue-drop", s->rx_desc, 0);
        free(frame);
        return;
    }
    frame->delivery_ns = esp32_wifi_delivery_time(
        s, frame->frame_length, &s->rx_serial_ready_ns);
    if(DEBUG) {
        ANSI_FG_HCOLOR(GREEN);
        printf("---------------\n>IN Send Frame type:%d subtype:%d channel:%d ap_state:%d  time:%ld \n",frame->frame_control.type,frame->frame_control.sub_type,esp32c3_wifi_channel(s),s->wlan->ap_state,(unsigned long) qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        ANSI_DEFAULT();
    }
    infoprint(frame);
    s->inject_queue_size++;
    i_frame = s->inject_queue;
    if (!i_frame) {
        s->inject_queue = frame;
    } else {
        while (i_frame->next_frame) {
            i_frame = i_frame->next_frame;
        }
        i_frame->next_frame = frame;
    }

    esp32_wifi_trace_packet(s, frame, "enqueue", s->rx_desc, 0);
    if (!s->inject_timer_running) {
        // if the injection timer is not
        // running currently, let's schedule
        // one run...
        s->inject_timer_running = 1;
        timer_mod(s->inject_timer, esp32_wifi_next_delivery(s,
                  frame->delivery_ns, frame->frame_length));
    }

}

static _Bool Esp32_WLAN_can_receive(NetClientState *ncs)
{
    Esp32C3WifiState *s = qemu_get_nic_opaque(ncs);
    /*
    if (s->wlan->ap_state != Esp32_WLAN__STATE_ASSOCIATED  && s->wlan->ap_state != Esp32_WLAN__STATE_STA_ASSOCIATED) {
        // we are currently not connected
        // to the access point
        return 0;
    }
    */
    if (s->inject_queue_size >= s->queue_limit) {
        // overload, please give me some time...
        return 0;
    }

    return 1;
}

/* Announce the bridge's new L2 next hop with an ordinary ARP request.
 * The host Ethernet backend is behind one selected virtual Wi-Fi peer. After
 * that peer changes, the guest must learn the new source MAC before replying
 * to an IPv4 frame; otherwise its ARP entry still targets a departed client.
 * This goes through normal radio/DMA and guest ARP processing, not RAM edits.
 */
static void refresh_bridge_neighbor(Esp32C3WifiState *s,
                                    const uint8_t *data, size_t size)
{
    uint8_t id = esp32_wifi_interface_id(s, s->wlan);
    if (!s->softap_client_ssid || !id || size < 34 ||
        data[12] != 8 || data[13] != 0 || data[14] >> 4 != 4 ||
        (s->softap_host_peer == id && !memcmp(s->softap_host_ip, data + 26, 4))) {
        return;
    }
    uint8_t arp[42] = {
        255, 255, 255, 255, 255, 255, 0, 0, 0, 0, 0, 0, 8, 6,
        0, 1, 8, 0, 6, 4, 0, 1,
    };
    memcpy(arp + 6, s->wlan->ap_macaddr, 6);
    memcpy(arp + 22, s->wlan->ap_macaddr, 6);
    memcpy(arp + 28, data + 26, 4);
    memcpy(arp + 38, data + 30, 4);
    struct mac80211_frame *request = Esp32_WLAN_create_data_packet(s, arp, sizeof(arp));
    memcpy(request->destination_address, s->wlan->softap_macaddr, 6);
    request->frame_control.flags = 1;
    Esp32_WLAN_init_ap_frame(s, request);
    Esp32_WLAN_insert_frame(s, request);
    s->softap_host_peer = id;
    memcpy(s->softap_host_ip, data + 26, 4);
}

static ssize_t Esp32_WLAN_receive(NetClientState *ncs,
                                    const uint8_t *buf, size_t size)
{
    Esp32C3WifiState *s = qemu_get_nic_opaque(ncs);
    struct mac80211_frame *frame;
    if (!Esp32_WLAN_can_receive(ncs)) {
        // this should not happen, but in
        // case it does, let's simply drop
        // the packet
        return -1;
    }

    if (!s) {
        return -1;
    }
    if (size < 14) {
        return size;
    }
    esp32_wifi_select_ethernet_interface(s, buf, size);
    if (!s->station_ap_online && s->wlan->mode == Esp32_Mode_Station) {
        return size;
    }
    if (!esp32_wifi_client_online(s, s->wlan)) return size;
    if (s->softap_client_ssid && s->wlan->mode == Esp32_Mode_SoftAP &&
        s->wlan->ap_state != Esp32_WLAN__STATE_STA_ASSOCIATED) return size;
    refresh_bridge_neighbor(s, buf, size);
    /*
     * A 802.3 packet comes from the qemu network. The
     * access points turns it into a 802.11 frame and
     * forwards it to the wireless device
     */
    uint8_t local_dhcp[1518];
    size_t local_size = esp32c2_clock_prepare_dhcp(s, buf, size,
                                                local_dhcp, sizeof(local_dhcp));
    if (local_size) {
        buf = local_dhcp;
        size = local_size;
    }
    frame = Esp32_WLAN_create_data_packet(s, buf, size);
    if (frame) {
        if(s->wlan->mode == Esp32_Mode_Station){
             memcpy(s->wlan->ap_macaddr,s->wlan->associated_ap_macaddr,6);

            if(s->wlan->ap_state==Esp32_WLAN__STATE_STA_ASSOCIATED) {
                frame->frame_control.flags=1;
                // if it's an arp request put the correct reply mac address in the packet
                if( frame->data_and_fcs[6]==8 && frame->data_and_fcs[7]==6) {
                    memcpy(frame->data_and_fcs+16,s->wlan->macaddr,6);
                }
            }
        }
        else{
            memcpy(frame->destination_address, s->wlan->softap_macaddr,6);
            if(s->wlan->ap_state==Esp32_WLAN__STATE_STA_ASSOCIATED) {
                frame->frame_control.flags=1;
                // if it's an arp request put the correct reply mac address in the packet
                if( frame->data_and_fcs[6]==8 && frame->data_and_fcs[7]==6) {
                    memcpy(frame->data_and_fcs+16,s->wlan->ap_macaddr,6);
                }
            }
        }


        if(((buf[12] & 0xE0) == IEEE80211_ENCAPSULATED)&&(buf[13] == ((IEEE80211_TYPE_MGT << 4)|IEEE80211_TYPE_MGT_SUBTYPE_ACTION)))
        {
          unsigned long ethernet_frame_size;
          unsigned char ethernet_frame[1518];
          struct mac80211_frame *reply = NULL;

          //discard packages from others channels
          if((buf[12] & 0x0F) != esp32c3_wifi_channel(s)) return -1;
          //discard packages originated from the same mac address
          if(!memcmp(&buf[6],s->wlan->macaddr,6)) return -1;
          //check destination
          if((memcmp(&buf[0],BROADCAST,6))&&(memcmp(&buf[0],s->wlan->macaddr,6))) return -1;

          //action frame
          Esp32_WLAN_init_ap_frame(s, frame);
          memcpy(frame->data_and_fcs, buf+14, size-14);
          memcpy(frame->destination_address, &buf[0], 6);
          memcpy(frame->source_address, &buf[6], 6);
          memset(frame->bssid_address, 0xff, 6);
          frame->frame_control.type= IEEE80211_TYPE_MGT;
          frame->frame_control.sub_type= IEEE80211_TYPE_MGT_SUBTYPE_ACTION;
          frame->frame_control.flags=  ((buf[12] & 0xF0) == IEEE80211_ENCAPSULATED_PROTECTED) ? 0x40 : 00;
          frame->duration_id= 0 ;
          frame->frame_length-=12;
          Esp32_WLAN_insert_frame(s, frame);


           //if destination is broadcast, don´t send ack
          if(!memcmp(&buf[0],BROADCAST,6)) return size;

          //Send Ack
          reply = Esp32_WLAN_create_ack();
          memcpy(reply->destination_address, &buf[6], 6);
          memcpy(reply->source_address, s->wlan->macaddr, 6);
          memset(reply->bssid_address, 0xff, 6);
          reply->duration_id=0;

          if(DEBUG) {
            ANSI_FG_HCOLOR(RED);
            macprint(reply->destination_address,"---------------\n<OUT ACK send: dest ");
            ANSI_DEFAULT();
          }
          infoprint(reply);
           /*
            * The access point uses the 802.11 frame
            * and sends a 802.3 frame into the network...
            * This packet is then understandable by
            * qemu-slirp
            *
            * If we ever want the access point to offer
            * some services, it can be added here!!
            */
          // ethernet header type
          ethernet_frame[12] = IEEE80211_ENCAPSULATED | esp32c3_wifi_channel(s);
          ethernet_frame[13] = ((IEEE80211_TYPE_CTL << 4)|IEEE80211_TYPE_CTL_SUBTYPE_ACK);

          memcpy(&ethernet_frame[0], reply->destination_address, 6);
          memcpy(&ethernet_frame[6], reply->source_address, 6);

          // add size of ethernet header
          ethernet_frame_size = 14;
          /*
           * Send 802.3 frame
           */
          Esp32_WLAN_send_ethernet(s, ethernet_frame, ethernet_frame_size);

          return size;
        }

        if(((buf[12] & 0xF0)== IEEE80211_ENCAPSULATED)&&(buf[13] == ((IEEE80211_TYPE_CTL << 4)|IEEE80211_TYPE_CTL_SUBTYPE_ACK)))
        {

          //discard packages from others channels
          if((buf[12] & 0x0F) != esp32c3_wifi_channel(s)) return -1;
          //discard packages originated from the same mac address
          if(!memcmp(&buf[6],s->wlan->macaddr,6)) return -1;
          //check destination
          if(memcmp(&buf[0],s->wlan->macaddr,6)) return -1;

          esp32c3_wifi_set_packet_status(s, ESP32C3_RADIO_PHYA_ACK);
          timer_mod_anticipate(s->wait_ack_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

          Esp32_WLAN_init_ap_frame(s, frame);
          memcpy(frame->destination_address, &buf[0], 6);
          memcpy(frame->source_address, &buf[6], 6);
          memset(frame->bssid_address, 0xff, 6);
          frame->frame_control.type= IEEE80211_TYPE_CTL;
          frame->frame_control.sub_type= IEEE80211_TYPE_CTL_SUBTYPE_ACK;
          frame->frame_control.flags= 0;
          frame->duration_id= 0 ;
          frame->frame_length = 10;

          if(DEBUG) {
            ANSI_FG_HCOLOR(GREEN);
            macprint(&buf[6],"---------------\n>IN ACK received: source");
            ANSI_DEFAULT();
          }
          infoprint(frame);
          //ACK is not inserted in DMA
          return size;
        }
        Esp32_WLAN_init_ap_frame(s, frame);
        Esp32_WLAN_insert_frame(s, frame);
    }
    return size;
}
static void Esp32_WLAN_cleanup(NetClientState *ncs) { }

static NetClientInfo net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = Esp32_WLAN_can_receive,
    .receive = Esp32_WLAN_receive,
    .cleanup = Esp32_WLAN_cleanup,
};

void Esp32_WLAN_reset_ap(Esp32C3WifiState *s) {
    struct mac80211_frame *frame;
    Esp32C3WifiEthernetPacket *packet;

    while ((frame = s->inject_queue)) {
        s->inject_queue = frame->next_frame;
        free(frame);
    }
    while ((packet = s->ethernet_tx_queue)) {
        s->ethernet_tx_queue = packet->next;
        g_free(packet);
    }
    if (s->inject_timer) {
        timer_del(s->inject_timer);
    }
    if (s->ethernet_tx_timer) {
        timer_del(s->ethernet_tx_timer);
    }
    s->channel = 0;
    esp32_wifi_reset_interfaces(s);
    s->beacon_ap = 0;

    s->inject_timer_running = 0;
    s->inject_sequence_number = 0;

    s->inject_queue = NULL;
    s->inject_queue_size = 0;
    s->ethernet_tx_queue = NULL;
    s->ethernet_tx_tail = NULL;
    s->ethernet_tx_queue_size = 0;
    s->rx_serial_ready_ns = 0;
    s->tx_serial_ready_ns = 0;
}

void Esp32_WLAN_setup_ap(DeviceState *dev,Esp32C3WifiState *s) {

    Esp32_WLAN_reset_ap(s);

    s->beacon_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, Esp32_WLAN_beacon_timer, s);
    timer_mod(s->beacon_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)+BEACON_TIME);

    // setup the timer but only schedule
    // it when necessary...
    s->inject_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, Esp32_WLAN_inject_timer, s);

    s->wait_ack_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, Esp32_WLAN_Wait_ACk_timer, s);

    s->ethernet_tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        Esp32_WLAN_ethernet_tx_timer, s);

    s->nic = qemu_new_nic(&net_info, &s->conf, object_get_typename(OBJECT(s)), dev->id, &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->wlan->macaddr);
}

static void send_single_frame(Esp32C3WifiState *s, struct mac80211_frame *frame, struct mac80211_frame *reply) {
    reply->sequence_control.sequence_number = s->inject_sequence_number++ +0x730;
    reply->signal_strength=-10;

    if(frame) {
        memcpy(reply->destination_address, frame->source_address, 6);
        if(s->wlan->mode == Esp32_Mode_Station){
           memcpy(reply->source_address, s->wlan->macaddr, 6);
        }
        else{
           memcpy(reply->source_address, s->wlan->ap_macaddr, 6);
        }
        memcpy(reply->bssid_address, frame->source_address, 6);
    }

    Esp32_WLAN_insert_frame(s, reply);
}

/* A guest AP beacon starts each present client with its own immutable MAC. */
static bool start_softap_clients(Esp32C3WifiState *s, struct mac80211_frame *frame)
{
    char ssid[33];
    if (!s->softap_client_ssid || frame->frame_control.type != IEEE80211_TYPE_MGT ||
        frame->frame_control.sub_type != IEEE80211_TYPE_MGT_SUBTYPE_BEACON ||
        !esp32_wifi_read_ssid(frame, ssid, sizeof(ssid)) || strcmp(ssid, s->softap_client_ssid)) return false;
    for (unsigned i = 0; i < ESP32_WIFI_SOFTAP_CLIENTS; i++) {
        Esp32C3WifiInterface *peer = esp32_wifi_softap_client(s, i);
        s->wlan = peer;
        g_strlcpy(peer->ssid, ssid, sizeof(peer->ssid));
        memcpy(peer->softap_macaddr, frame->bssid_address, 6);
        memcpy(peer->macaddr, frame->source_address, 6);
        if (!esp32_wifi_client_online(s, peer)) continue;
        bool begin = peer->ap_state == Esp32_WLAN__STATE_NOT_AUTHENTICATED ||
                     peer->ap_state == Esp32_WLAN__STATE_AUTHENTICATED;
        if (begin || qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= peer->management_retry_ns) {
            access_point_info info = { .ssid = peer->ssid,
                .channel = esp32c3_wifi_channel(s), .sigstrength = -23 };
            memcpy(info.mac_address, peer->ap_macaddr, 6);
            struct mac80211_frame *request = NULL;
            if (begin || peer->ap_state == Esp32_WLAN__STATE_STA_NOT_AUTHENTICATED) {
                peer->ap_state = Esp32_WLAN__STATE_STA_NOT_AUTHENTICATED;
                request = Esp32_WLAN_create_probe_request(&info);
            } else if (peer->ap_state == Esp32_WLAN__STATE_STA_AUTHENTICATED) {
                request = Esp32_WLAN_create_authentication_request();
            } else if (peer->ap_state == Esp32_WLAN__STATE_STA_ASSOCIATING) {
                request = Esp32_WLAN_create_association_request(&info);
            }
            if (request) {
                peer->management_retry_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MANAGEMENT_RETRY_NS;
                send_single_frame(s, frame, request);
            }
        }
    }
    esp32c3_wifi_set_packet_status(s, ESP32C3_RADIO_PHYA_ACK);
    Esp32_WLAN_frame_delivered(s);
    return true;
}

static void handle_interface_frame(Esp32C3WifiState *s, struct mac80211_frame *frame)
{
    struct mac80211_frame *reply = NULL;
    char *ssid = s->wlan->ssid;
    access_point_info dummy_ap = {
        .ssid = ssid, .channel = esp32c3_wifi_channel(s), .sigstrength = -23,
    };
    memcpy(dummy_ap.mac_address, s->wlan->ap_macaddr, 6);
    access_point_info station_ap = access_points[0];
    station_ap.ssid = s->station_ssid;
    char requested_ssid[33] = { 0 };
    unsigned long ethernet_frame_size;
    unsigned char ethernet_frame[1518];


    if(DEBUG){
        if((ENABLE_BEACON)||!((frame->frame_control.type == IEEE80211_TYPE_MGT)&&(frame->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_BEACON))){
            ANSI_FG_HCOLOR(RED);
            printf("-------------------------\n<OUT Handle Frame type:%d subtype:%d channel:%d ap_state:%d time:%ld \n",frame->frame_control.type,
                         frame->frame_control.sub_type,esp32c3_wifi_channel(s),s->wlan->ap_state,(unsigned long)  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
            ANSI_DEFAULT();
        }
    }
    if((ENABLE_BEACON)||!((frame->frame_control.type == IEEE80211_TYPE_MGT)&&(frame->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_BEACON))){
        infoprint(frame);
    }
    access_point_info *ap_info=0;

    if (memcmp(frame->source_address, BROADCAST, sizeof(s->wlan->macaddr))) {
        memcpy(s->wlan->macaddr, frame->source_address, sizeof(s->wlan->macaddr));
    }

    if (!s->station_ap_online && s->wlan->mode == Esp32_Mode_Station &&
        !(frame->frame_control.type == IEEE80211_TYPE_MGT &&
          frame->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_BEACON)) {
        /* The guest may still start its own AP after failed station retries. */
        esp32c3_wifi_set_packet_status(s, ESP32C3_RADIO_PHYA_NACK);
        Esp32_WLAN_frame_delivered(s);
        return;
    }

    if(s->wlan->mode == Esp32_Mode_Station){
        station_ap.channel = esp32c3_wifi_channel(s);
        if (frame->frame_control.type == IEEE80211_TYPE_MGT &&
            frame->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_PROBE_REQ &&
            frame->data_and_fcs[0] == IEEE80211_BEACON_PARAM_SSID &&
            frame->data_and_fcs[1] <= sizeof(requested_ssid) - 1) {
            memcpy(requested_ssid, &frame->data_and_fcs[2],
                   frame->data_and_fcs[1]);
            if (requested_ssid[0]) {
                station_ap.ssid = requested_ssid;
            }
        }
        ap_info = &station_ap;
    }

    /* Active scan on the guest's STA interface may run while its AP client
     * remains associated. Answer as each external AP, without replacing the
     * setup client's MAC/state or waiting for a 500-ms beacon interval. */
    if (s->softap_client_ssid && s->softap.ssid[0] &&
        frame->frame_control.type == IEEE80211_TYPE_MGT &&
        frame->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_PROBE_REQ) {
        for (int i = 0; i < nb_aps; ++i) {
            if (access_points[i].channel != esp32c3_wifi_channel(s)) continue;
            access_point_info info = access_points[i];
            if (i == 0) info.ssid = s->station_ssid;
            reply = Esp32_WLAN_create_probe_response(&info);
            memcpy(reply->destination_address, frame->source_address, 6);
            memcpy(reply->source_address, access_points[i].mac_address, 6);
            memcpy(reply->bssid_address, access_points[i].mac_address, 6);
            reply->signal_strength = access_points[i].sigstrength;
            reply->sequence_control.sequence_number = s->inject_sequence_number++;
            Esp32_WLAN_insert_frame(s, reply);
        }
        Esp32_WLAN_frame_delivered(s);
        return;
    }

    esp32c3_wifi_set_packet_status(s, ESP32C3_RADIO_PHYA_ACK);
    if(frame->frame_control.type == IEEE80211_TYPE_MGT) {
        switch(frame->frame_control.sub_type) {
            case IEEE80211_TYPE_MGT_SUBTYPE_BEACON:
                if(s->wlan->ap_state==Esp32_WLAN__STATE_NOT_AUTHENTICATED || s->wlan->ap_state==Esp32_WLAN__STATE_AUTHENTICATED) {
                    if (!esp32_wifi_read_ssid(frame, ssid, 33)) break;
                    if(DEBUG) printf("beacon from %s\n",ssid);
                    dummy_ap.ssid=ssid;
                    dummy_ap.channel = esp32c3_wifi_channel(s);
                    dummy_ap.sigstrength= -23;
                    /* Pin one virtual client MAC through probe/auth/association.
                     * Otherwise the generic station-AP reply below replaces it,
                     * and association arrives from a different (often zero) MAC. */
                    if (s->softap_client_ssid &&
                        !strcmp(ssid, s->softap_client_ssid)) {
                        memcpy(s->wlan->ap_macaddr, (uint8_t[]){0x52,0x54,0,0x43,0,2}, 6);
                        ap_info = NULL;
                    }
                    memcpy(dummy_ap.mac_address,s->wlan->ap_macaddr,6) ;
                    memcpy(s->wlan->softap_macaddr,frame->bssid_address,6);
                    s->wlan->mode = Esp32_Mode_SoftAP;
                    s->wlan->ap_state=Esp32_WLAN__STATE_STA_NOT_AUTHENTICATED;
                    send_single_frame(s,frame,Esp32_WLAN_create_probe_request(&dummy_ap));
                }
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_ACTION:
                if(DEBUG) printf("action\n");
               /*
                * The access point uses the 802.11 frame
                * and sends a 802.3 frame into the network...
                * This packet is then understandable by
                * qemu-slirp
                *
                * If we ever want the access point to offer
                * some services, it can be added here!!
                */
                // ethernet header type
                ethernet_frame[12] = ((frame->frame_control.flags & 0x40) ? IEEE80211_ENCAPSULATED_PROTECTED : IEEE80211_ENCAPSULATED)| esp32c3_wifi_channel(s);
                ethernet_frame[13] = ((IEEE80211_TYPE_MGT << 4)|IEEE80211_TYPE_MGT_SUBTYPE_ACTION);

                memcpy(&ethernet_frame[0], frame->destination_address, 6);
                memcpy(&ethernet_frame[6], s->wlan->macaddr, 6);

                if (frame->frame_length < IEEE80211_HEADER_SIZE + 8 + 4) {
                Esp32_WLAN_frame_delivered(s);
                return;
            }
            // add packet content
                ethernet_frame_size = frame->frame_length -IEEE80211_HEADER_SIZE;

                if (ethernet_frame_size > sizeof(ethernet_frame)) {
                    ethernet_frame_size = sizeof(ethernet_frame);
                }
                memcpy(&ethernet_frame[14], &frame->data_and_fcs[0], ethernet_frame_size);
                // add size of ethernet header
                ethernet_frame_size += 14;
                /*
                * Send 802.3 frame
                */
                Esp32_WLAN_send_ethernet(s, ethernet_frame,
                                         ethernet_frame_size);

                //if destination is not broadcast wait for ack
                if(memcmp(&ethernet_frame[0],BROADCAST,6)){
                   esp32c3_wifi_set_packet_status(s, ESP32C3_RADIO_PHYA_NACK);
                   timer_mod(s->wait_ack_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + WAIT_ACK_TIMEOUT);
                   return;
                }
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_PROBE_RESP:
                ap_info=&dummy_ap;
                if (!esp32_wifi_read_ssid(frame, ssid, 33)) break;
                if(DEBUG) printf("probe resp from %s\n",ssid);
                dummy_ap.ssid=ssid;
                s->wlan->ap_state=Esp32_WLAN__STATE_STA_NOT_AUTHENTICATED;
                send_single_frame(s,frame,Esp32_WLAN_create_deauthentication());
                send_single_frame(s,frame,Esp32_WLAN_create_authentication_request());
                s->wlan->ap_state = Esp32_WLAN__STATE_STA_AUTHENTICATED;
                s->wlan->management_retry_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MANAGEMENT_RETRY_NS;
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_RESP:
                if(DEBUG) printf("assoc resp\n");
                memset(&s->wlan->dhcp, 0, sizeof(s->wlan->dhcp));
                memset(s->wlan->offered_ipaddr, 0, sizeof(s->wlan->offered_ipaddr));
                mac80211_frame *frame1=Esp32_WLAN_create_dhcp_discover(s);
                memcpy(frame1->bssid_address,BROADCAST,6);
                memcpy(frame1->source_address,frame->destination_address,6);
                memcpy(frame1->destination_address,frame->source_address,6);
                send_single_frame(s,0,frame1);
                s->wlan->ap_state=Esp32_WLAN__STATE_STA_DHCP;
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_DISASSOCIATION:
                DEBUG_PRINT_AP(("Received disassociation!\n"));
                send_single_frame(s,frame,Esp32_WLAN_create_disassociation());
                if (s->wlan->ap_state == Esp32_WLAN__STATE_ASSOCIATED || s->wlan->ap_state == Esp32_WLAN__STATE_STA_ASSOCIATED) {
                    s->wlan->ap_state = Esp32_WLAN__STATE_AUTHENTICATED;
                }
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_DEAUTHENTICATION:
                DEBUG_PRINT_AP(("Received deauthentication!\n"));
                //reply = Esp32_WLAN_create_authentication_response(ap_info);
                if (s->wlan->ap_state == Esp32_WLAN__STATE_AUTHENTICATED) {
                    s->wlan->ap_state = Esp32_WLAN__STATE_NOT_AUTHENTICATED;
                }
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION:
                DEBUG_PRINT_AP(("Received authentication!\n"));
                if(frame->data_and_fcs[2]==2) { // response
                    send_single_frame(s,frame,Esp32_WLAN_create_association_request(&dummy_ap));
                    s->wlan->ap_state = Esp32_WLAN__STATE_STA_ASSOCIATING;
                    s->wlan->management_retry_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MANAGEMENT_RETRY_NS;
                }
                break;
        }
        if(ap_info) {
            memcpy(s->wlan->ap_macaddr,ap_info->mac_address,6);
            switch(frame->frame_control.sub_type) {
                case IEEE80211_TYPE_MGT_SUBTYPE_PROBE_REQ:
                    DEBUG_PRINT_AP(("Received probe request!\n"));
                    reply = Esp32_WLAN_create_probe_response(ap_info);
                    break;
                case IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION:
                    DEBUG_PRINT_AP(("Received authentication req!\n"));
                    if(frame->data_and_fcs[2]==1) { // request
                        reply = Esp32_WLAN_create_authentication_response(ap_info);
                        if (s->wlan->ap_state == Esp32_WLAN__STATE_NOT_AUTHENTICATED) {
                            s->wlan->ap_state = Esp32_WLAN__STATE_AUTHENTICATED;
                        }
                    }
                break;
                case IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_REQ:
                    DEBUG_PRINT_AP(("Received association request!\n"));
                    reply = Esp32_WLAN_create_association_response(ap_info);
                    if (s->wlan->ap_state == Esp32_WLAN__STATE_AUTHENTICATED) {
                        s->wlan->ap_state = Esp32_WLAN__STATE_ASSOCIATED;
                        memcpy(s->wlan->associated_ap_macaddr,s->wlan->ap_macaddr,6);
                    }
                    break;
            }
            if (reply) {
                reply->signal_strength=ap_info->sigstrength;
                memcpy(reply->destination_address, frame->source_address, 6);
                Esp32_WLAN_init_ap_frame(s, reply);
                Esp32_WLAN_insert_frame(s, reply);
            }
        }
    }
    if ((frame->frame_control.type == IEEE80211_TYPE_DATA) &&
        (frame->frame_control.sub_type == IEEE80211_TYPE_DATA_SUBTYPE_DATA)) {
        if (s->wlan->ap_state == Esp32_WLAN__STATE_STA_DHCP ||
            s->wlan->ap_state == Esp32_WLAN__STATE_STA_DHCP_REQUESTING) {
            Esp32WifiDhcpReply dhcp_reply;
            if (frame->frame_length >= IEEE80211_HEADER_SIZE + 8 + 4 &&
                esp32_wifi_dhcp_reply(frame->data_and_fcs,
                    frame->frame_length - IEEE80211_HEADER_SIZE - 4, &dhcp_reply)) {
                enum Esp32WifiDhcpAction action = esp32_wifi_dhcp_accept(
                    &s->wlan->dhcp, &dhcp_reply, s->wlan->ap_macaddr);
                mac80211_frame *request = NULL;
                switch (action) {
                case ESP32_DHCP_SEND_REQUEST:
                    memcpy(s->wlan->offered_ipaddr, s->wlan->dhcp.address, 4);
                    request = Esp32_WLAN_create_dhcp_request(s, s->wlan->dhcp.address);
                    s->wlan->ap_state = Esp32_WLAN__STATE_STA_DHCP_REQUESTING;
                    break;
                case ESP32_DHCP_LEASE_BOUND:
                    memcpy(s->wlan->associated_ap_macaddr, s->wlan->macaddr, 6);
                    s->wlan->ap_state = Esp32_WLAN__STATE_STA_ASSOCIATED;
                    break;
                case ESP32_DHCP_RESTART:
                    memset(s->wlan->offered_ipaddr, 0, sizeof(s->wlan->offered_ipaddr));
                    request = Esp32_WLAN_create_dhcp_discover(s);
                    s->wlan->ap_state = Esp32_WLAN__STATE_STA_DHCP;
                    break;
                case ESP32_DHCP_IGNORE:
                    break;
                }
                if (request) {
                    memcpy(request->bssid_address, BROADCAST, 6);
                    memcpy(request->source_address, s->wlan->ap_macaddr, 6);
                    memcpy(request->destination_address, frame->source_address, 6);
                    send_single_frame(s, NULL, request);
                }
            }
        } else if (s->wlan->ap_state == Esp32_WLAN__STATE_ASSOCIATED || s->wlan->ap_state == Esp32_WLAN__STATE_STA_ASSOCIATED) {
            /*
            * The access point uses the 802.11 frame
            * and sends a 802.3 frame into the network...
            * This packet is then understandable by
            * qemu-slirp
            *
            * If we ever want the access point to offer
            * some services, it can be added here!!
            */
            // ethernet header type
            ethernet_frame[12] = frame->data_and_fcs[6];
            ethernet_frame[13] = frame->data_and_fcs[7];

            // the new originator of the packet is
            // the access point
            if(s->wlan->ap_state == Esp32_WLAN__STATE_ASSOCIATED)
                memcpy(&ethernet_frame[6], s->wlan->ap_macaddr, 6);
            else
                memcpy(&ethernet_frame[6], s->wlan->macaddr, 6);

            if (ethernet_frame[12] == 0x08 && ethernet_frame[13] == 0x06) {
                // for arp request, we use a broadcast
                memset(&ethernet_frame[0], 0xff, 6);
            } else {
                // otherwise we forward the packet to
                // where it really belongs
                memcpy(&ethernet_frame[0], frame->destination_address, 6);
            }

            if (frame->frame_length < IEEE80211_HEADER_SIZE + 8 + 4) {
                Esp32_WLAN_frame_delivered(s);
                return;
            }
            // add packet content
            ethernet_frame_size = frame->frame_length - IEEE80211_HEADER_SIZE - 4 - 8;

            // for some reason, the packet is 22 bytes too small (??)
            if (!s->c2_layout) ethernet_frame_size += 22;
            if (ethernet_frame_size > sizeof(ethernet_frame) - 14) {
                ethernet_frame_size = sizeof(ethernet_frame) - 14;
            }
            memcpy(&ethernet_frame[14], &frame->data_and_fcs[8], ethernet_frame_size);
            // add size of ethernet header
            ethernet_frame_size += 14;
            /*
            * Send 802.3 frame
            */
            Esp32_WLAN_send_ethernet(s, ethernet_frame,
                                     ethernet_frame_size);
        }
    }
    Esp32_WLAN_frame_delivered(s);
}

/* Select the actual transmitting guest interface before changing peer state. */
void Esp32_WLAN_handle_frame(Esp32C3WifiState *s, struct mac80211_frame *frame)
{
    if (start_softap_clients(s, frame)) return;
    if (!esp32_wifi_select_guest_interface(s, frame)) {
        /* An opt-in virtual client ignores other SSIDs and malformed beacons. */
        esp32c3_wifi_set_packet_status(s, ESP32C3_RADIO_PHYA_ACK);
        Esp32_WLAN_frame_delivered(s);
        return;
    }
    esp32_wifi_learn_guest_ip(s, frame);
    handle_interface_frame(s, frame);
}
