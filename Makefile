obj-m += it5570_fan.o

KDIR ?= /lib/modules/$(shell uname -r)/build

# CachyOS and other Clang-based kernel builds pass CC/LD on the command line.
# When building against a GCC kernel the variables are simply not set and
# the kernel Kbuild picks up its own defaults, so this works everywhere.

all:
	$(MAKE) -C $(KDIR) M=$(PWD) \
		$(if $(CC),CC=$(CC)) \
		$(if $(LD),LD=$(LD)) \
		$(if $(LLVM),LLVM=$(LLVM)) \
		modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Quick local install (not for DKMS - use 'make dkms-install' for that)
insmod: all
	sudo insmod it5570_fan.ko

rmmod:
	sudo rmmod it5570_fan

# DKMS helpers (run from repo checkout)
DKMS_NAME  := it5570-fan
DKMS_VER   := $(shell git describe --tags --abbrev=0 2>/dev/null || echo "0.1.0")
DKMS_SRC   := /usr/src/$(DKMS_NAME)-$(DKMS_VER)

dkms-install:
	sudo mkdir -p $(DKMS_SRC)
	sudo cp it5570_fan.c Makefile dkms.conf $(DKMS_SRC)/
	sudo sed -i "s/@PKGVER@/$(DKMS_VER)/g" $(DKMS_SRC)/dkms.conf
	sudo dkms add $(DKMS_NAME)/$(DKMS_VER)
	sudo dkms build $(DKMS_NAME)/$(DKMS_VER)
	sudo dkms install $(DKMS_NAME)/$(DKMS_VER)

dkms-remove:
	sudo dkms remove $(DKMS_NAME)/$(DKMS_VER) --all
	sudo rm -rf $(DKMS_SRC)

.PHONY: all clean insmod rmmod dkms-install dkms-remove
