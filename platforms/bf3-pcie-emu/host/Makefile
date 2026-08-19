obj-m += daqiri_bf3_pcie.o
ccflags-y += -I$(src)/../../../include

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) modules

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) clean
