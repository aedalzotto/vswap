obj-m := vswap.o

ifeq ($(KERNELRELEASE),)
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PREFIX ?= /usr/local
CHECKPATCH_IGNORE ?=

all: module tool
module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
tool:
	$(CC) -O2 -Wall -I$(CURDIR) tools/vswap.c -lvulkan -o tools/vswap

install: all
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	install -Dm755 tools/vswap $(DESTDIR)$(PREFIX)/bin/vswap
uninstall:
	rm -f $(INSTALL_MOD_PATH)/lib/modules/$(KVER)/updates/vswap.ko*
	depmod -a $(if $(INSTALL_MOD_PATH),-b $(INSTALL_MOD_PATH)) $(KVER)
	rm -f $(DESTDIR)$(PREFIX)/bin/vswap

# Kernel coding-style check of the module sources; skip rules with CHECKPATCH_IGNORE=RULE1,RULE2
checkpatch:
	$(KDIR)/scripts/checkpatch.pl --no-tree -f --show-types $(if $(CHECKPATCH_IGNORE),--ignore $(CHECKPATCH_IGNORE)) vswap.c vswap_uapi.h

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f tools/vswap
.PHONY: all module tool install uninstall checkpatch clean
endif
