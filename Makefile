#
# Makefile 
# Desgin of Netlink
#

MODULE_NAME :=training-netlink
obj-m :=$(MODULE_NAME).o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
    $(MAKE) -C $(KERNELDIR) M=$(PWD)

clean:
    $(MAKE) -C $(KERNELDIR) M=$(PWD) clean

#obj-$(CONFIG_HOPEN_TRAINING)	+=  training-netlink.o