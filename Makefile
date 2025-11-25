MODULE = perftop

obj-m += $(MODULE).o
$(MODULE)-y := perftop_main.o perftop_probe.o perftop_data.o perftop_utils.o

# Optional: Enable debug mode
# CFLAGS_perftop_main.o += -DCONFIG_PERFTOP_DEBUG
# CFLAGS_perftop_probe.o += -DCONFIG_PERFTOP_DEBUG
# CFLAGS_perftop_data.o += -DCONFIG_PERFTOP_DEBUG
# CFLAGS_perftop_utils.o += -DCONFIG_PERFTOP_DEBUG

KERNELDIR ?= /lib/modules/$(shell uname -r)/build

PWD := $(shell pwd)

all: $(MODULE)

$(MODULE):
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
