LIBS=sdl
ifdef DEBUG
CFLAGS=-ggdb -std=gnu99 `pkg-config --cflags-only-I $(LIBS)` -Wreturn-type -Werror=return-type
else
CFLAGS=-O2 -std=gnu99 `pkg-config --cflags-only-I $(LIBS)` -Wreturn-type -Werror=return-type
endif

all : dis trans stateview blastem

blastem : blastem.o 68kinst.o gen_x86.o m68k_to_x86.o z80inst.o z80_to_x86.o x86_backend.o runtime.o zruntime.o mem.o vdp.o ym2612.o psg.o render_sdl.o
	$(CC) -o blastem blastem.o 68kinst.o gen_x86.o m68k_to_x86.o z80inst.o z80_to_x86.o x86_backend.o runtime.o zruntime.o mem.o vdp.o ym2612.o psg.o render_sdl.o `pkg-config --libs $(LIBS)`

dis : dis.o 68kinst.o
	$(CC) -o dis dis.o 68kinst.o

zdis : zdis.o z80inst.o
	$(CC) -o zdis zdis.o z80inst.o
	
libemu68k.a : 68kinst.o gen_x86.o m68k_to_x86.o x86_backend.o runtime.o mem.o
	ar rcs libemu68k.a 68kinst.o gen_x86.o m68k_to_x86.o x86_backend.o runtime.o mem.o
	
trans : trans.o 68kinst.o gen_x86.o m68k_to_x86.o x86_backend.o runtime.o mem.o
	$(CC) -o trans trans.o 68kinst.o gen_x86.o m68k_to_x86.o x86_backend.o runtime.o mem.o

transz80 : transz80.o z80inst.o gen_x86.o z80_to_x86.o x86_backend.o zruntime.o mem.o
	$(CC) -o transz80 transz80.o z80inst.o gen_x86.o z80_to_x86.o x86_backend.o zruntime.o mem.o

ztestrun : ztestrun.o z80inst.o gen_x86.o z80_to_x86.o x86_backend.o zruntime.o mem.o
	$(CC) -o ztestrun ztestrun.o z80inst.o gen_x86.o z80_to_x86.o x86_backend.o zruntime.o mem.o

ztestgen : ztestgen.o z80inst.o
	$(CC) -o ztestgen ztestgen.o z80inst.o

stateview : stateview.o vdp.o render_sdl.o
	$(CC) -o stateview stateview.o vdp.o render_sdl.o `pkg-config --libs $(LIBS)`

test_x86 : test_x86.o gen_x86.o
	$(CC) -o test_x86 test_x86.o gen_x86.o

gen_fib : gen_fib.o gen_x86.o mem.o
	$(CC) -o gen_fib gen_fib.o gen_x86.o mem.o

offsets : offsets.c z80_to_x86.h m68k_to_x86.h
	$(CC) -o offsets offsets.c
	
%.o : %.S
	$(CC) -c -o $@ $<

%.o : %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.bin : %.s68
	vasmm68k_mot -Fbin -m68000 -no-opt -spaces -o $@ $<

%.bin : %.sz8
	vasmz80_mot -Fbin -spaces -o $@ $<

clean :
	rm -rf dis trans stateview test_x86 gen_fib *.o
