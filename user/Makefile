include ../common.mk

init: initcode.c
	$(CC) $(CFLAGS) -I ../include -march=rv64g -nostdinc -c initcode.c -o initcode.o
	$(LD) $(LDFLAGS) -N -e main -Ttext 0x1000 -o initcode.out initcode.o
	$(OBJCOPY) -S -O binary initcode.out initcode
	xxd -i initcode > ../include/proc/initcode.h
	rm -f initcode initcode.d initcode.o initcode.out