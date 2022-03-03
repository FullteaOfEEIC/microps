#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"
#include "net.h"

#define DUMMY_MTU UINT16_MAX

static int dummy_transmit(struct net_device *dev, uint16_t type, const uint8_t *data, size_t len, const void *dst){
	debugf("dev=%s, type=0x%04x, len=%zu", dev->name, type, len);
	debugdump(data, len);
	/*DROP DATA*/
	return 0;
}

static struct net_device_ops dummy_ops = {
	.transmit = dummy_transmit,
};

struct net_device* dummy_init(void){
	struct net_device *dev;
	dev = net_device_alloc();
	if(!dev){
		errorf("net_device_alloc() failure");
		return NULL;
	}
	debugf("here");
	dev->mtu = DUMMY_MTU;
	dev->type = NET_DEVICE_TYPE_DUMMY;
	debugf("here1");
	//dev->mtu = DUMMY_MTU;
	debugf("here3.3");
	dev->hlen = 0;
	dev->alen = 0;
	debugf("here3.5");
	dev->ops = &dummy_ops;
	debugf("here3");
	if(net_device_register(dev) == -1){
		errorf("net_device_register() failure");
		return NULL;
	}
	debugf("initialized, dev=%s", dev->name);
	return dev;
}

