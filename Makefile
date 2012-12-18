LIBS=sdl

all : dis trans stateview blastem

blastem : blastem.o 68kinst.o gen_x86.o m68k_to_x86.o runtime.o mem.o vdp.o render_sdl.o
	$(CC) -o blastem blastem.o 68kinst.o gen_x86.o m68k_to_x86.o runtime.o mem.o vdp.o render_sdl.o `pkg-config --libs $(LIBS)`

dis : dis.o 68kinst.o
	$(CC) -o dis dis.o 68kinst.o
	
trans : trans.o 68kinst.o gen_x86.o m68k_to_x86.o runtime.o mem.o
	$(CC) -o trans trans.o 68kinst.o gen_x86.o m68k_to_x86.o runtime.o mem.o

stateview : stateview.o vdp.o render_sdl.o
	$(CC) -o stateview stateview.o vdp.o render_sdl.o `pkg-config --libs $(LIBS)`

test_x86 : test_x86.o gen_x86.o
	$(CC) -o test_x86 test_x86.o gen_x86.o

gen_fib : gen_fib.o gen_x86.o mem.o
	$(CC) -o gen_fib gen_fib.o gen_x86.o mem.o
	
%.o : %.S
	$(CC) -c -o $@ $<

%.o : %.c
	$(CC) -ggdb -std=gnu99 `pkg-config --cflags-only-I $(LIBS)` -c -o $@ $<

%.bin : %.s68
	vasmm68k_mot -Fbin -m68000 -spaces -o $@ $<

clean :
	rm -rf dis trans stateview test_x86 gen_fib *.o
