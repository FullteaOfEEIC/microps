#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>

#include "util.h"
#include "net.h"
#include "ip.h"
#include "platform.h"

static struct ip_iface *ifaces;
static struct ip_protocol *protocols;

struct ip_hdr{
    uint8_t vhl;
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

struct ip_protocol{
    struct ip_protocol *next;
    uint8_t type;
    void (*handler)(const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst, struct ip_iface *iface);
};


const ip_addr_t IP_ADDR_ANY =    0x00000000; /* 0.0.0.0 */
const ip_addr_t IP_ADDR_BROADCAST = 0xffffffff; /* 255.255.255.255 */

int ip_addr_pton(const char *p, ip_addr_t *n){
    char *sp, *ep;
    long ret;

    sp = (char*)p;
    for(int idx=0;idx<4;idx++){
        ret = strtol(sp, &ep, 10);
        if (ret<0 || ret>255){
            return -1;
        }
        else if(ep==sp){
            return -1;
        }
        else if((idx==3 && *ep != '\0') || (idx != 3 && *ep != '.')){
            return -1;
        }
        ((uint8_t *)n)[idx] = ret;
        sp = ep+1;
    }
    return 0;
}

char* ip_addr_ntop(ip_addr_t n, char* p, size_t size){
    uint8_t *u8;
    u8 = (uint8_t *)&n;
    snprintf(p, size, "%d.%d.%d.%d", u8[0], u8[1], u8[2], u8[3]);
    return p;
}

static void ip_dump(const uint8_t *data, size_t len){
    struct ip_hdr *hdr;
    uint8_t v, hl, hlen;
    uint16_t total, offset;
    char addr[IP_ADDR_STR_LEN];

    flockfile(stderr);
    hdr = (struct ip_hdr *)data;
    v = (hdr->vhl & 0xf0) >> 4;
    hl = hdr->vhl & 0x0f;
    hlen = hl << 2;
    fprintf(stderr, "\tvhl:\t0x%02x [v:%u, hl:%u (%u)\n", hdr->vhl, v, hl, hlen);
    fprintf(stderr, "\ttos:\t0x%02x\n", hdr->tos);
    total = ntoh16(hdr->total);
    fprintf(stderr, "\ttotal\t%u (payload: %u)\n", total, total-hlen);
    fprintf(stderr, "\tid:\t%u\n", ntoh16(hdr->id));
    offset = ntoh16(hdr->offset);
    fprintf(stderr, "\toffset:\t0x%04x [flags=%x, offset=%u]\n", offset, (offset & 0xe000) >> 13, offset & 0x1fff);
    fprintf(stderr, "\tttl:\t%u\n", hdr->ttl);
    fprintf(stderr, "\tprotocol:\t%u\n", hdr->protocol);
    fprintf(stderr, "\tsum:\t0x%04x\n", ntoh16(hdr->sum));
    fprintf(stderr, "\tsrc:\t%s\n", ip_addr_ntop(hdr->src, addr, sizeof(addr)));
    fprintf(stderr, "\tdst:\t%s\n", ip_addr_ntop(hdr->dst, addr, sizeof(addr)));

#ifdef HEXDUMP
    hexdump(stderr, data, len)
#endif //HEXDUMP
    funlockfile(stderr);
}

struct ip_iface *ip_iface_alloc(const char *unicast, const char *netmask){
    struct ip_iface *iface;
    iface = memory_alloc(sizeof(*iface));
    if(!iface){
        errorf("memory_alloc() failure");
        return NULL;
    }
    NET_IFACE(iface)->family = NET_IFACE_FAMILY_IP;
    if(ip_addr_pton(unicast, &(iface->unicast))==-1){
        errorf("failure to change unicast address from string to binary");
        memory_free(iface);
        return NULL;
    }
    if(ip_addr_pton(netmask, &(iface->netmask))==-1){
        errorf("failure to change netmask address from string to binary");
        memory_free(iface);
        return NULL;
    }
    iface->broadcast = (iface->unicast & iface->netmask) | (~iface->netmask);
    return iface;
}

int ip_iface_register(struct net_device *dev, struct ip_iface *iface){
    char addr1[IP_ADDR_STR_LEN];
    char addr2[IP_ADDR_STR_LEN];
    char addr3[IP_ADDR_STR_LEN];
    if(net_device_add_iface(dev, NET_IFACE(iface))==-1){
        errorf("net_device_add_iface() failure");
        return -1;
    }

    iface->next = ifaces;
    ifaces = iface;

    infof("registered: dev=%s, unicast=%s, netmask=%s, broadcast=%s", dev->name, ip_addr_ntop(iface->unicast, addr1, sizeof(addr1)),ip_addr_ntop(iface->netmask, addr2, sizeof(addr2)), ip_addr_ntop(iface->broadcast, addr3, sizeof(addr3)));
    return 0;
}

struct ip_iface *ip_iface_select(ip_addr_t addr){
    struct ip_iface *entry;
    for(entry = ifaces;entry;entry=entry->next){
        if(entry->unicast == addr){
            return entry;
        }
    }
    return NULL;
}

int ip_protocol_register(uint8_t type, void (*handler)(const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst, struct ip_iface *iface)){
    struct ip_protocol *entry;
    for(entry = protocols;entry;entry=entry->next){
        if(entry->type == type){
            errorf("already registered protocol");
            return -1;
        }
    }
    entry = memory_alloc(sizeof(*entry));
    if(!entry){
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

static void ip_input(const uint8_t *data, size_t len, struct net_device *dev){
    struct ip_hdr *hdr;
    uint8_t v;
    uint16_t hlen, total, offset;
    struct ip_iface *iface;
    char addr[IP_ADDR_STR_LEN];

    if(len<IP_HDR_SIZE_MIN){
        errorf("too short (shorter than IP_HDR_SIZE_MIN)");
        return;
    }
    hdr = (struct ip_hdr *)data;
    v = (hdr->vhl & 0xf0) >> 4;
    hlen = hdr->vhl & 0x0f;
    total = ntoh16(hdr->total);
    if(v!=IP_VERSION_IPV4){
        errorf("Not IPv4");
        return;
    }
    else if(len<hlen){
        errorf("too short (shorter than ip_header)");
        return;
    }
    else if(total<len){
        errorf("too short (shorter than ip packet)");
        return;
    }

    uint16_t sum = cksum16((uint16_t *)data, len, 0);
    if(sum!=0){
        errorf("invalid checksum");
        return;
    }

    offset = ntoh16(hdr->offset);
    if(offset & 0x2000 || offset & 0x1fff){
        errorf("fragments does not supported. Sorry!");
        return;
    }

    iface = (struct ip_iface*)net_device_get_iface(dev, NET_IFACE_FAMILY_IP);
    if(!iface){
        errorf("can't get ip interface");
        return;
    }

    if(hdr->dst!=iface->unicast && hdr->dst!=iface->broadcast && hdr->dst!=iface->netmask){
        // for other host;
        return;
    }

    debugf("dev=%s, iface=%s, protocol=%u, total=%u", dev->name, ip_addr_ntop(iface->unicast, addr, sizeof(addr)), hdr->protocol, total);
    ip_dump(data, total);
    struct ip_protocol* entry;
    for(entry = protocols; entry; entry=entry->next){
        debugf("searching"); 
        if(entry->type==hdr->protocol){
            debugf("find");
            entry->handler((uint8_t*)hdr+hlen, total-hlen, hdr->src, hdr->dst, iface); //事故りそうで怖いような、そうでもないような(ポインタだからいいのか)
        }
    }
}

static int ip_output_device(struct ip_iface *iface, const uint8_t *data, size_t len, ip_addr_t dst){
    uint8_t hwaddr[NET_DEVICE_ADDR_LEN];
    if(NET_IFACE(iface)->dev->flags & NET_DEVICE_FLAG_NEED_ARP){
        if(dst==iface->broadcast || dst==IP_ADDR_BROADCAST){
            memcpy(hwaddr, NET_IFACE(iface)->dev->broadcast, NET_IFACE(iface)->dev->alen);
        }
        else{
            errorf("arp doesn't support now. Sorry!");
            return -1;
        }
    }
    return 0;
}

static ssize_t ip_output_core(struct ip_iface *iface, uint8_t protocol, const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst, uint16_t id, uint16_t offset){
    uint8_t buf[IP_TOTAL_SIZE_MAX];
    struct ip_hdr *hdr;
    uint16_t hlen, total;
    char addr[IP_ADDR_STR_LEN];

    hdr = (struct ip_hdr *)buf;
    hlen = IP_HDR_SIZE_MIN;
    hdr->vhl = (IP_VERSION_IPV4<<4)|(hlen >> 2);
    hdr->tos = 0;
    total = hlen+len;
    hdr->total = hton16(total);
    hdr->ttl = 255;
    hdr->protocol = protocol;
    hdr->src = src;
    hdr->dst = dst;
    hdr->sum = 0;
    hdr->sum = cksum16((uint16_t *)data, len, 0);
    memcpy(hdr+1, data, len);
    debugf("dev=%s, dst=%s, protocol=%u, len=%u", NET_IFACE(iface)->dev->name, ip_addr_ntop(dst, addr, sizeof(addr)), protocol, total);
    ip_dump(buf, total);
    return ip_output_device(iface, buf, total, dst);
}

static uint16_t ip_generate_id(void){ //本当はランダムな数値を返すほうが...
    static mutex_t mutex = MUTEX_INITIALIZER;
    static uint16_t id = 128;
    uint16_t ret;

    mutex_lock(&mutex);
    ret = id++;
    mutex_unlock(&mutex);
    return ret;
}

ssize_t ip_output(uint8_t protocol, const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst){
    struct ip_iface *iface;
    char addr[IP_ADDR_STR_LEN];
    uint16_t ipid;

    if(src == IP_ADDR_ANY){
        errorf("ip routing does not support now. Sorry!");
        return -1;
    }
    else{
        iface = ip_iface_select(src);
        if(!iface){
            errorf("inter face with specified src addr doesn't found, src=%s", ip_addr_ntop(src,addr,sizeof(addr)));
            return -1;
        }
        if(dst == iface->unicast){
            ;
        }
        else if(dst==IP_ADDR_BROADCAST){
            ;
        }
        else if((dst & iface->netmask) == (iface->unicast & iface->netmask)){
            ;
        }
        else{
            errorf("destination unreachable, dst=%s", ip_addr_ntop(src,addr,sizeof(addr)));
            return -1;
        }
    }

    if(NET_IFACE(iface)->dev->mtu < IP_HDR_SIZE_MIN+len){
        errorf("too long. we don't support flagment, dev=%s, mtu=%u < %zu", NET_IFACE(iface)->dev->name, NET_IFACE(iface)->dev->mtu, IP_HDR_SIZE_MIN+len);
        return -1;
    }
    ipid = ip_generate_id();
    if(ip_output_core(iface, protocol, data, len, iface->unicast, dst, ipid, 0)==-1){
        errorf("ip_output_core() failure");
        return -1;
    }
    return len;
}

int ip_init(void){
    if(net_protocol_register(NET_PROTOCOL_TYPE_IP, ip_input)==-1){
        errorf("net_protocol_register() failure");
        return -1;
    }
    return 0;
}