#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"
#include "util.h"
#include "net.h"

static struct net_device *devices;
static struct net_protocol *protocols;

struct net_protocol{
	struct net_protocol *next;
	uint16_t type;
	struct queue_head queue; /*input queue*/
	void (*handler)(const uint8_t *data, size_t len, struct net_device *dev);
};

struct net_protocol_queue_entry{
	struct net_device *dev;
	size_t len;
	uint8_t data[];
};

struct net_device* net_device_alloc(void){
	struct net_device *dev;
	size_t sizeofdev = sizeof(*dev);
	dev = memory_alloc(sizeofdev);
	if(!dev){
		errorf("memory_alloc() failure");
		return NULL;
	}
	return dev;
}

int net_protocol_register(uint16_t type, void(*handler)(const uint8_t *data, size_t len, struct net_device *dev)){

}

int net_device_register(struct net_device *dev){
	static unsigned int index = 0;
	dev->index = index++;
	snprintf(dev->name, sizeof(dev->name), "net%d", dev->index);
	dev->next = devices;
	devices = dev;
	infof("registered, dev=%s, type=0x%04x", dev->name, dev->type);
	return 0;
}

static int net_device_open(struct net_device *dev){
	if(NET_DEVICE_IS_UP(dev)){
		errorf("already opend net device, dev=%s", dev->name);
		return -1;
	}
	
	if(dev->ops->open){
		if(dev->ops->open(dev) == -1){
			errorf("failure opening net device, dev=%s", dev->name);
			return -1;
		}
	}
	
	dev->flags |= NET_DEVICE_FLAG_UP;
	infof("dev=%s, state=%s", dev->name, NET_DEVICE_STATE(dev));
	return 0;
}

static int net_device_close(struct net_device *dev){
	if(!NET_DEVICE_IS_UP(dev)){
		errorf("device is not opened. cant close, dev=%s", dev->name);
		return -1;
	}
	if(dev->ops->close){
		if(dev->ops->close(dev) == -1){
			errorf("failure closing net device, dev=%s", dev->name);
			return -1;
		}
	}
	dev->flags &= ~NET_DEVICE_FLAG_UP;
	infof("dev=%s, state=%s", dev->name, NET_DEVICE_STATE(dev));
	return 0;
}

int net_device_output(struct net_device *dev, uint16_t type, const uint8_t *data, size_t len, const void *dst){
	if(!NET_DEVICE_IS_UP(dev)){
		errorf("device not opened, dev=%s", dev->name);
		return -1;
	}
	if(len > dev->mtu){
		errorf("too longer than mtu, dev=%s, mtu=%u, len=%zu", dev->name, dev->mtu, len);
		return -1;
	}
	debugf("dev=%s, type=0x%04x, len=%zu", dev->name, type, len);
	debugdump(data, len);
	if(dev->ops->transmit(dev, type, data, len, dst)==-1){
		errorf("device transmit failure, dev=%s, len=%zu", dev->name, len);
		return -1;
	}
	return 0;
}

int net_input_handler(uint16_t type, const uint8_t *data, size_t len, struct net_device *dev){
	return 0;
}

int net_run(void){
	struct net_device *dev;

	if(intr_run()==-1){
		errorf("intr_run() failure");
		return -1;
	}

	debugf("open all devices...");
	for(dev = devices; dev; dev=dev->next){
		net_device_open(dev);
	}
	debugf("running...");
	return 0;
}

void net_shutdown(void){
	struct net_device *dev;
	intr_shutdown();
	debugf("close all devices...");
	for(dev = devices; dev; dev=dev->next){
		net_device_close(dev);
	}
	debugf("shut down");
}

int net_init(void){
	if(intr_init()==-1){
		errorf("intr_init() failure");
		return -1;
	}
	infof("initialized");
	return 0;
}
