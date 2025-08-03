#include "ip.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "arp.h"
#include "net.h"
#include "platform.h"
#include "util.h"

struct ip_hdr {
    uint8_t vhl; /* version and header length */
    uint8_t tos;
    uint16_t total;
    uint16_t id;
    uint16_t offset;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t sum;
    ip_addr_t src;
    ip_addr_t dst;
    uint8_t options[];
};

struct ip_protocol {
    struct ip_protocol *next;
    uint8_t type;
    void (*handler)(const uint8_t *data, size_t len, ip_addr_t src,
                    ip_addr_t dst, struct ip_iface *iface);
};

struct ip_route {
    struct ip_route *next;
    ip_addr_t network;
    ip_addr_t netmask;
    ip_addr_t nexthop;
    struct ip_iface *iface;
};

const ip_addr_t IP_ADDR_ANY = 0x00000000;       /* 0.0.0.0 */
const ip_addr_t IP_ADDR_BROADCAST = 0xffffffff; /* 255.255.255.255 */

/* NOTE: if you want to add/delete the entries after net_run(), you need to
 * protect these lists with a mutex. */
static struct ip_iface *ifaces;
static struct ip_protocol *protocols;
static struct ip_route *routes;

int ip_addr_pton(const char *p, ip_addr_t *n) {
    char *sp, *ep;
    int idx;
    long ret;

    sp = (char *)p;
    for (idx = 0; idx < 4; idx++) {
        ret = strtol(sp, &ep, 10);
        if (ret < 0 || ret > 255) {
            return -1;
        }
        if (ep == sp) {
            return -1;
        }
        if ((idx == 3 && *ep != '\0') || (idx != 3 && *ep != '.')) {
            return -1;
        }
        ((uint8_t *)n)[idx] = ret;
        sp = ep + 1;
    }
    return 0;
}

char *ip_addr_ntop(ip_addr_t n, char *p, size_t size) {
    uint8_t *u8;

    u8 = (uint8_t *)&n;
    snprintf(p, size, "%d.%d.%d.%d", u8[0], u8[1], u8[2], u8[3]);
    return p;
}

static void ip_dump(const uint8_t *data, size_t len) {
    struct ip_hdr *hdr;
    uint8_t v, hl, hlen;
    uint16_t total, offset;
    char addr[IP_ADDR_STR_LEN];

    flockfile(stderr);
    hdr = (struct ip_hdr *)data;
    v = (hdr->vhl & 0xf0) >> 4;
    hl = hdr->vhl & 0x0f;
    hlen = hl << 2;
    fprintf(stderr, " vhl: 0x%02x [v: %u, hl: %u (%u)]\n", hdr->vhl, v, hl,
            hlen);
    fprintf(stderr, " tos: 0x%02x\n", hdr->tos);
    total = ntoh16(hdr->total);
    fprintf(stderr, " total: %u (payload: %u)\n", total, total - hlen);
    fprintf(stderr, " id: %u\n", ntoh16(hdr->id));
    offset = ntoh16(hdr->offset);
    fprintf(stderr, " offset: 0x%04x [flags=%x, offset=%u]\n", offset,
            (offset & 0xe000) >> 13, offset & 0x1fff);
    fprintf(stderr, " ttl: %u\n", hdr->ttl);
    fprintf(stderr, " protocol: %u\n", hdr->protocol);
    fprintf(stderr, " sum: 0x%04x\n", ntoh16(hdr->sum));
    fprintf(stderr, " src: %s\n", ip_addr_ntop(hdr->src, addr, sizeof(addr)));
    fprintf(stderr, " dst: %s\n", ip_addr_ntop(hdr->dst, addr, sizeof(addr)));
#ifdef HEXDUMP
    hexdump(stderr, data, len);
#endif
    funlockfile(stderr);
}

/* NOTE: must not be call after net_run() */
static struct ip_route *ip_route_add(ip_addr_t network, ip_addr_t netmask,
                                     ip_addr_t nexthop,
                                     struct ip_iface *iface) {}

static struct ip_route *ip_route_lookup(ip_addr_t dst) {}

/* NOTE: must not be call after net_run() */
int ip_route_set_default_gateway(struct ip_iface *iface, const char *gateway) {}

struct ip_iface *ip_route_get_iface(ip_addr_t dst) {}

struct ip_iface *ip_iface_alloc(const char *unicast, const char *netmask) {
    struct ip_iface *iface;

    iface = memory_alloc(sizeof(*iface));
    if (!iface) {
        errorf("memory_alloc() failure");
        return NULL;
    }
    NET_IFACE(iface)->family = NET_IFACE_FAMILY_IP;

    if (ip_addr_pton(unicast, &iface->unicast) == -1) {
        errorf("invalid unicast address: %s", unicast);
        memory_free(iface);
        return NULL;
    }

    if (ip_addr_pton(netmask, &iface->netmask) == -1) {
        errorf("invalid netmask address: %s", netmask);
        memory_free(iface);
        return NULL;
    }

    iface->broadcast = iface->unicast | ~iface->netmask;

    return iface;
}

/* NOTE: must not be call after net_run() */
int ip_iface_register(struct net_device *dev, struct ip_iface *iface) {
    char addr1[IP_ADDR_STR_LEN];
    char addr2[IP_ADDR_STR_LEN];
    char addr3[IP_ADDR_STR_LEN];

    // Register the interface to the device
    if (net_device_add_iface(dev, NET_IFACE(iface)) == -1) {
        errorf("net_device_add_iface() failure, dev=%s, family=%d", dev->name,
               NET_IFACE(iface)->family);
        memory_free(iface);
        return -1;
    }

    // Add the interface to the global list
    iface->next = ifaces;
    ifaces = iface;

    infof("registered: dev=%s, unicast=%s, netmask=%s, broadcast=%s", dev->name,
          ip_addr_ntop(iface->unicast, addr1, sizeof(addr1)),
          ip_addr_ntop(iface->netmask, addr2, sizeof(addr2)),
          ip_addr_ntop(iface->broadcast, addr3, sizeof(addr3)));
    return 0;
}

struct ip_iface *ip_iface_select(ip_addr_t addr) {
    struct ip_iface *iface = ifaces;

    while (iface != NULL) {
        if ((iface->unicast & iface->netmask) == (addr & iface->netmask)) {
            return iface;
        }
        iface = iface->next;
    }

    return NULL;
}

/* NOTE: must not be call after net_run() */
int ip_protocol_register(uint8_t type,
                         void (*handler)(const uint8_t *data, size_t len,
                                         ip_addr_t src, ip_addr_t dst,
                                         struct ip_iface *iface)) {
    struct ip_protocol *entry;

    // Check if the protocol is already registered
    for (entry = protocols; entry; entry = entry->next) {
        if (type == entry->type) {
            errorf("already registered, type=0x%02x", type);
            return -1;
        }
    }

    // Register the protocol
    entry = memory_alloc(sizeof(*entry));
    if (!entry) {
        errorf("memory_alloc() failure");
        return -1;
    }

    entry->type = type;
    entry->handler = handler;
    entry->next = protocols;
    protocols = entry;

    infof("registered, type=%u", entry->type);
    return 0;
}

static void ip_input(const uint8_t *data, size_t len, struct net_device *dev) {
    struct ip_hdr *hdr;
    uint8_t v;
    uint16_t hlen, total, offset;
    struct ip_iface *iface;
    char addr[IP_ADDR_STR_LEN];

    if (len < IP_HDR_SIZE_MIN) {
        errorf("too short");
        return;
    }
    hdr = (struct ip_hdr *)data;

    v = (hdr->vhl & 0xf0) >> 4;
    if (v != IP_VERSION_IPV4) {
        errorf("unsupported version: %u", v);
        return;
    }

    hlen = (hdr->vhl & 0x0f) << 2;
    if (len < hlen) {
        errorf("too short for header length: %u", hlen);
        return;
    }

    total = ntoh16(hdr->total);
    if (len < total) {
        errorf("too short for total length: %u", total);
        return;
    }

    uint16_t sum = cksum16((uint16_t *)data, hlen, 0);
    if (sum != 0) {
        errorf("invalid checksum: hdr->sum=0x%04x, calc=0x%04x",
               ntoh16(hdr->sum), sum);
        return;
    }

    offset = ntoh16(hdr->offset);
    if (offset & 0x2000 || offset & 0x1fff) {
        errorf("fragments does not support");
        return;
    }

    // Filter interfaces by destination address
    iface = ip_iface_select(hdr->dst);
    if (!iface) {
        errorf("no matching interface for destination address: %s",
               ip_addr_ntop(hdr->dst, addr, sizeof(addr)));
        return;
    }
    // Check if the interface is for this device
    if (iface->unicast != hdr->dst && hdr->dst != 0xffffffff &&
        iface->broadcast != hdr->dst) {
        errorf(
            "not for this interface: dev=%s, unicast=%s, broadcast=%s, dst=%s",
            dev->name, ip_addr_ntop(iface->unicast, addr, sizeof(addr)),
            ip_addr_ntop(iface->broadcast, addr, sizeof(addr)),
            ip_addr_ntop(hdr->dst, addr, sizeof(addr)));
        return;
    }

    debugf("dev=%s, iface=%s, protocol=%u, total=%u", dev->name,
           ip_addr_ntop(iface->unicast, addr, sizeof(addr)), hdr->protocol,
           total);
    ip_dump(data, total);

    // Call the registered protocol handler
    for (struct ip_protocol *proto = protocols; proto; proto = proto->next) {
        if (proto->type == hdr->protocol) {
            proto->handler(data + hlen, total - hlen, hdr->src, hdr->dst,
                           iface);
            return;
        }
    }
    /* unsupported protocol */
}

static int ip_output_device(struct ip_iface *iface, const uint8_t *data,
                            size_t len, ip_addr_t dst) {
    uint8_t hwaddr[NET_DEVICE_ADDR_LEN] = {};
    int ret;

    if (NET_IFACE(iface)->dev->flags & NET_DEVICE_FLAG_NEED_ARP) {
        if (dst == iface->broadcast || dst == IP_ADDR_BROADCAST) {
            memcpy(hwaddr, NET_IFACE(iface)->dev->broadcast,
                   NET_IFACE(iface)->dev->alen);
        } else {
            ret = arp_resolve(NET_IFACE(iface), dst, hwaddr);
            if (ret == -1) {
                errorf("arp_resolve() failure");
                return -1;
            } else if (ret == 0) {
                errorf("arp resolve failure, dst=%s",
                       ip_addr_ntop(dst, hwaddr, sizeof(hwaddr)));
                return -1;
            }
        }
    }

    return net_device_output(NET_IFACE(iface)->dev, NET_PROTOCOL_TYPE_IP, data,
                             len, hwaddr);
}

static ssize_t ip_output_core(struct ip_iface *iface, uint8_t protocol,
                              const uint8_t *data, size_t len, ip_addr_t src,
                              ip_addr_t dst, uint16_t id, uint16_t offset) {
    uint8_t buf[IP_TOTAL_SIZE_MAX];
    struct ip_hdr *hdr;
    uint16_t hlen, total;
    char addr[IP_ADDR_STR_LEN];

    hdr = (struct ip_hdr *)buf;

    // Generate the IP datagram
    // Header
    hlen = IP_HDR_SIZE_MIN;  // Fixed header length
    total = hlen + len;      // Total length of the IP datagram
    if (total > IP_TOTAL_SIZE_MAX) {
        errorf("too long, total=%u > %u", total, IP_TOTAL_SIZE_MAX);
        return -1;
    }
    hdr->vhl =
        (IP_VERSION_IPV4 << 4) | (hlen >> 2);  // Version and header length
    hdr->tos = 0;                              // Type of service
    hdr->total = ntoh16(total);                // Total length
    hdr->id = ntoh16(id);                      // Identification
    hdr->offset = ntoh16(offset);              // Fragment offset
    hdr->ttl = 255;                            // Time to live
    hdr->protocol = protocol;                  // Protocol
    hdr->sum = 0;    // Checksum (will be calculated later)
    hdr->src = src;  // Source address
    hdr->dst = dst;  // Destination address

    // Calculate the checksum
    hdr->sum = cksum16((uint16_t *)buf, hlen, 0);
    // Validate the checksum
    if (cksum16((uint16_t *)buf, hlen, 0) != 0) {
        errorf("invalid checksum: hdr->sum=0x%04x", ntoh16(hdr->sum));
        return -1;
    }

    // Copy the payload data
    if (len > 0) {
        if (len > IP_PAYLOAD_SIZE_MAX) {
            errorf("too long, len=%zu > %zu", len, IP_PAYLOAD_SIZE_MAX);
            return -1;
        }
    }
    memcpy(buf + hlen, data, len);

    debugf("dev=%s, dst=%s, protocol=%u, len=%u", NET_IFACE(iface)->dev->name,
           ip_addr_ntop(dst, addr, sizeof(addr)), protocol, total);
    ip_dump(buf, total);
    return ip_output_device(iface, buf, total, dst);
}

static uint16_t ip_generate_id(void) {
    static mutex_t mutex = MUTEX_INITIALIZER;
    static uint16_t id = 128;
    uint16_t ret;

    mutex_lock(&mutex);
    ret = id++;
    mutex_unlock(&mutex);
    return ret;
}

ssize_t ip_output(uint8_t protocol, const uint8_t *data, size_t len,
                  ip_addr_t src, ip_addr_t dst) {
    struct ip_iface *iface;
    char addr[IP_ADDR_STR_LEN];
    uint16_t id;
    if (src == IP_ADDR_ANY) {
        errorf("ip routing does not implement");
        return -1;
    }

    if (NET_IFACE(iface)->dev->mtu < IP_HDR_SIZE_MIN + len) {
        errorf("too long, dev=%s, mtu=%u < %zu", NET_IFACE(iface)->dev->name,
               NET_IFACE(iface)->dev->mtu, IP_HDR_SIZE_MIN + len);
        return -1;
    }

    id = ip_generate_id();
    if (ip_output_core(iface, protocol, data, len, iface->unicast, dst, id,
                       0) == -1) {
        errorf("ip_output_core() failure");
        return -1;
    }
    return len;
}

int ip_init(void) {
    if (net_protocol_register(NET_PROTOCOL_TYPE_IP, ip_input) == -1) {
        errorf("net_protocol_register() failure");
        return -1;
    }

    return 0;
}