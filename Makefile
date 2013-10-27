LIBS=sdl gl
LDFLAGS=-lm `pkg-config --libs $(LIBS)`
ifdef DEBUG
CFLAGS=-ggdb -std=gnu99 `pkg-config --cflags-only-I $(LIBS)` -Wreturn-type -Werror=return-type
else
CFLAGS=-O2 -std=gnu99 `pkg-config --cflags-only-I $(LIBS)` -Wreturn-type -Werror=return-type
endif

ifdef PROFILE
CFLAGS+= -pg
LDFLAGS+= -pg
endif

TRANSOBJS=gen_x86.o x86_backend.o mem.o
M68KOBJS=68kinst.o m68k_to_x86.o runtime.o
Z80OBJS=z80inst.o z80_to_x86.o zruntime.o
AUDIOOBJS=ym2612.o psg.o wave.o

all : dis zdis stateview vgmplay blastem

blastem : blastem.o vdp.o render_sdl.o io.o config.o tern.o gst.o $(M68KOBJS) $(Z80OBJS) $(TRANSOBJS) $(AUDIOOBJS)
	$(CC) -ggdb -o blastem  blastem.o vdp.o render_sdl.o io.o config.o tern.o gst.o $(M68KOBJS) $(Z80OBJS) $(TRANSOBJS) $(AUDIOOBJS) $(LDFLAGS)

dis : dis.o 68kinst.o
	$(CC) -o dis dis.o 68kinst.o

zdis : zdis.o z80inst.o
	$(CC) -o zdis zdis.o z80inst.o

libemu68k.a : $(M68KOBJS) $(TRANSOBJS)
	ar rcs libemu68k.a $(M68KOBJS) $(TRANSOBJS)

trans : trans.o $(M68KOBJS) $(TRANSOBJS)
	$(CC) -o trans trans.o $(M68KOBJS) $(TRANSOBJS)

transz80 : transz80.o $(Z80OBJS) $(TRANSOBJS)
	$(CC) -o transz80 $(Z80OBJS) $(TRANSOBJS)

ztestrun : ztestrun.o $(Z80OBJS) $(TRANSOBJS)
	$(CC) -o ztestrun $(Z80OBJS) $(TRANSOBJS)

ztestgen : ztestgen.o z80inst.o
	$(CC) -o ztestgen ztestgen.o z80inst.o

stateview : stateview.o vdp.o render_sdl.o config.o tern.o gst.o
	$(CC) -o stateview stateview.o vdp.o render_sdl.o config.o tern.o gst.o `pkg-config --libs $(LIBS)`

vgmplay : vgmplay.o render_sdl.o config.o tern.o $(AUDIOOBJS)
	$(CC) -o vgmplay vgmplay.o render_sdl.o config.o tern.o $(AUDIOOBJS) $(LDFLAGS)

testgst : testgst.o gst.o
	$(CC) -o testgst testgst.o gst.o

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
