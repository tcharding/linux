obj-m += slub_defrag.o slub_defrag_xarray.o

KTREE=../../..

all:
	make -C ${KTREE} M=$(PWD) modules

install:
	make -C ${KTREE} M=$(PWD) modules_install

clean:
	make -C ${KTREE} M=$(PWD) clean

