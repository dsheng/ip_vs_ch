#
# Makefile for the IPVS consistent hashing scheduler
#
#ln -s /usr/src/kernels/`uname -r`/ /lib/modules/`uname -r`/build
# if make modules_install does not work, please install it as follow:
# open install comment and make install

# IPVS consisntent hashing sheduler objs
ip_vs_ch-objs := ip_vs_ch_core.o md5.o util_rbtree.o		   	   \
		conhash_util.o conhash_inter.o conhash.o

KERNEL_SOURCE_DIR=/lib/modules/`uname -r`/build
KERNEL_MODULE_DIR=/lib/modules/`uname -r`

# IPVS ch scheduler
obj-m += ip_vs_ch.o

modules:
	$(MAKE) -C $(KERNEL_SOURCE_DIR) M=`pwd` modules

modules_install:
	$(MAKE) -C $(KERNEL_SOURCE_DIR) M=`pwd` modules_install

#install:
#	-cp ip_vs_ch.ko /lib/modules/`uname -r`/kernel/net/netfilter/ipvs/ 
#	-depmod -A

clean:
	$(MAKE) -C $(KERNEL_SOURCE_DIR) M=`pwd` clean

.PHONY: modules modules_install clean
.DEFAULT: modules
