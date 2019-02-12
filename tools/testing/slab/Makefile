obj-m += slub_defrag.o slub_defrag_xarray.o

KTREE=../../..

all:
	make -C ${KTREE} M=$(PWD) modules

clean:
	make -C ${KTREE} M=$(PWD) clean

