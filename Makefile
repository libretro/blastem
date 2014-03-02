ifdef NOGL
LIBS=sdl
else
LIBS=sdl glew gl
endif
LDFLAGS:=-lm $(shell pkg-config --libs $(LIBS))
ifdef DEBUG
CFLAGS:=-ggdb -std=gnu99 $(shell pkg-config --cflags-only-I $(LIBS)) -Wreturn-type -Werror=return-type
else
CFLAGS:=-O2 -std=gnu99 $(shell pkg-config  --cflags-only-I $(LIBS)) -Wreturn-type -Werror=return-type
endif

ifdef PROFILE
CFLAGS+= -pg
LDFLAGS+= -pg
endif
ifdef NOGL
CFLAGS+= -DDISABLE_OPENGL
endif

ifndef CPU
CPU:=$(shell uname -m)
endif



TRANSOBJS=gen.o backend.o mem.o
M68KOBJS=68kinst.o m68k_to_x86.o
ifeq ($(CPU),x86_64)
M68KOBJS+= runtime.o
TRANSOBJS+= gen_x86.o
else
ifeq ($(CPU),i686)
M68KOBJS+= runtime_32.o
TRANSOBJS+= gen_x86.o
NOZ80:=1
endif
endif

Z80OBJS=z80inst.o z80_to_x86.o zruntime.o
AUDIOOBJS=ym2612.o psg.o wave.o
CONFIGOBJS=config.o tern.o util.o

MAINOBJS=blastem.o debug.o gdb_remote.o vdp.o render_sdl.o io.o $(CONFIGOBJS) gst.o $(M68KOBJS) $(TRANSOBJS) $(AUDIOOBJS)

ifeq ($(CPU),x86_64)
CFLAGS+=-DX86_64
else
ifeq ($(CPU),i686)
CFLAGS+=-DX86_32
endif
endif

ifdef NOZ80
CFLAGS+=-DNO_Z80
else
MAINOBJS+= $(Z80OBJS)
endif


all : dis zdis stateview vgmplay blastem

blastem : $(MAINOBJS)
	$(CC) -ggdb -o blastem $(MAINOBJS) $(LDFLAGS)

dis : dis.o 68kinst.o
	$(CC) -o dis dis.o 68kinst.o

zdis : zdis.o z80inst.o
	$(CC) -o zdis zdis.o z80inst.o

libemu68k.a : $(M68KOBJS) $(TRANSOBJS)
	ar rcs libemu68k.a $(M68KOBJS) $(TRANSOBJS)

trans : trans.o $(M68KOBJS) $(TRANSOBJS)
	$(CC) -o trans trans.o $(M68KOBJS) $(TRANSOBJS)

transz80 : transz80.o $(Z80OBJS) $(TRANSOBJS)
	$(CC) -o transz80 transz80.o $(Z80OBJS) $(TRANSOBJS)

ztestrun : ztestrun.o $(Z80OBJS) $(TRANSOBJS)
	$(CC) -o ztestrun ztestrun.o $(Z80OBJS) $(TRANSOBJS)

ztestgen : ztestgen.o z80inst.o
	$(CC) -ggdb -o ztestgen ztestgen.o z80inst.o

stateview : stateview.o vdp.o render_sdl.o $(CONFIGOBJS) gst.o
	$(CC) -o stateview stateview.o vdp.o render_sdl.o $(CONFIGOBJS) gst.o $(LDFLAGS)

vgmplay : vgmplay.o render_sdl.o $(CONFIGOBJS) $(AUDIOOBJS)
	$(CC) -o vgmplay vgmplay.o render_sdl.o $(CONFIGOBJS) $(AUDIOOBJS) $(LDFLAGS)

testgst : testgst.o gst.o
	$(CC) -o testgst testgst.o gst.o

test_x86 : test_x86.o gen_x86.o gen.o
	$(CC) -o test_x86 test_x86.o gen_x86.o gen.o

test_arm : test_arm.o gen_arm.o mem.o gen.o
	$(CC) -o test_arm test_arm.o gen_arm.o mem.o gen.o

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
