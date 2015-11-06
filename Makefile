ifndef OS
OS:=$(shell uname -s)
endif
FIXUP:=true

ifeq ($(OS),Windows)
ifndef SDL2_PREFIX
SDL2_PREFIX:="C:/MinGW/usr"
endif
ifndef GLEW32S_LIB
GLEW32S_LIB=glew32s.lib
endif

MEM:=mem_win.o
TERMINAL:=terminal_win.o
EXE:=.exe
CC:=wine gcc.exe
CFLAGS:=-std=gnu99 -Wreturn-type -Werror=return-type -Werror=implicit-function-declaration -I"$(SDL2_PREFIX)/include/SDL2" -DGLEW_STATIC
LDFLAGS:= $(GLEW32S_LIB) -L"$(SDL2_PREFIX)/lib" -lm -lmingw32 -lSDL2main -lSDL2 -lws2_32 -lopengl32 -lglu32 -mwindows
CPU:=i686

else

MEM:=mem.o
TERMINAL:=terminal.o
EXE:=

ifeq ($(OS),Darwin)
LIBS=sdl2 glew
else
LIBS=sdl2 glew gl
endif #Darwin

HAS_PROC:=$(shell if [ -d /proc ]; then /bin/echo -e -DHAS_PROC; fi)
CFLAGS:=-std=gnu99 -Wreturn-type -Werror=return-type -Werror=implicit-function-declaration -Wno-unused-value -Wno-logical-op-parentheses $(HAS_PROC)
ifdef PORTABLE
CFLAGS+= -DGLEW_STATIC -Iglew/include
LDFLAGS:=-lm glew/lib/libGLEW.a

ifeq ($(OS),Darwin)
CFLAGS+= -IFrameworks/SDL2.framework/Headers
LDFLAGS+= -FFrameworks -framework SDL2 -framework OpenGL
FIXUP:=install_name_tool -change @rpath/SDL2.framework/Versions/A/SDL2 @executable_path/Frameworks/SDL2.framework/Versions/A/SDL2
else
CFLAGS+= -Isdl/include
LDFLAGS+= -Wl,-rpath='$$ORIGIN/lib' -Llib -lSDL2 $(shell pkg-config --libs gl)
endif #Darwin

else
CFLAGS:=$(shell pkg-config --cflags-only-I $(LIBS)) $(CFLAGS)
LDFLAGS:=-lm $(shell pkg-config --libs $(LIBS))

ifeq ($(OS),Darwin)
LDFLAGS+= -framework OpenGL
endif

endif #PORTABLE
endif #Windows

ifdef DEBUG
CFLAGS:=-ggdb $(CFLAGS)
LDFLAGS:=-ggdb $(LDFLAGS)
else
CFLAGS:=-O2 -flto $(CFLAGS)
LDFLAGS:=-O2 -flto $(LDFLAGS)
endif #DEBUG

ifdef Z80_LOG_ADDRESS
CFLAGS+= -DZ80_LOG_ADDRESS
endif

ifdef PROFILE
CFLAGS+= -pg
LDFLAGS+= -pg
endif
ifdef NOGL
CFLAGS+= -DDISABLE_OPENGL
endif

ifdef M68030
CFLAGS+= -DM68030
endif
ifdef M68020
CFLAGS+= -DM68020
endif
ifdef M68010
CFLAGS+= -DM68010
endif

ifndef CPU
CPU:=$(shell uname -m)
endif

TRANSOBJS=gen.o backend.o $(MEM)
M68KOBJS=68kinst.o m68k_core.o
ifeq ($(CPU),x86_64)
M68KOBJS+= m68k_core_x86.o
TRANSOBJS+= gen_x86.o backend_x86.o
else
ifeq ($(CPU),i686)
M68KOBJS+= m68k_core_x86.o
TRANSOBJS+= gen_x86.o backend_x86.o
endif
endif

Z80OBJS=z80inst.o z80_to_x86.o
AUDIOOBJS=ym2612.o psg.o wave.o
CONFIGOBJS=config.o tern.o util.o

MAINOBJS=blastem.o debug.o gdb_remote.o vdp.o render_sdl.o io.o romdb.o menu.o $(TERMINAL) $(CONFIGOBJS) gst.o $(M68KOBJS) $(TRANSOBJS) $(AUDIOOBJS)

ifeq ($(CPU),x86_64)
CFLAGS+=-DX86_64 -m64
LDFLAGS+=-m64
else
ifeq ($(CPU),i686)
CFLAGS+=-DX86_32 -m32
LDFLAGS+=-m32
endif
endif

ifdef NOZ80
CFLAGS+=-DNO_Z80
else
MAINOBJS+= $(Z80OBJS)
endif

ALL=dis$(EXE) zdis$(EXE) stateview$(EXE) vgmplay$(EXE) blastem$(EXE)
ifneq ($(OS),Windows)
ALL+= termhelper
endif

all : $(ALL)

blastem$(EXE) : $(MAINOBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	$(FIXUP) ./$@

dis$(EXE) : dis.o 68kinst.o tern.o vos_program_module.o
	$(CC) -o $@ $^

zdis$(EXE) : zdis.o z80inst.o
	$(CC) -o $@ $^

libemu68k.a : $(M68KOBJS) $(TRANSOBJS)
	ar rcs libemu68k.a $(M68KOBJS) $(TRANSOBJS)

trans : trans.o $(M68KOBJS) $(TRANSOBJS) util.o
	$(CC) -o trans trans.o $(M68KOBJS) $(TRANSOBJS) util.o

transz80 : transz80.o $(Z80OBJS) $(TRANSOBJS)
	$(CC) -o transz80 transz80.o $(Z80OBJS) $(TRANSOBJS)

ztestrun : ztestrun.o $(Z80OBJS) $(TRANSOBJS)
	$(CC) -o ztestrun ztestrun.o $(Z80OBJS) $(TRANSOBJS)

ztestgen : ztestgen.o z80inst.o
	$(CC) -ggdb -o ztestgen ztestgen.o z80inst.o

stateview$(EXE) : stateview.o vdp.o render_sdl.o $(CONFIGOBJS) gst.o
	$(CC) -o $@ $^ $(LDFLAGS)
	$(FIXUP) ./$@

vgmplay$(EXE) : vgmplay.o render_sdl.o $(CONFIGOBJS) $(AUDIOOBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	$(FIXUP) ./$@

blastcpm : blastcpm.o util.o $(Z80OBJS) $(TRANSOBJS)
	$(CC) -o $@ $^

test : test.o vdp.o
	$(CC) -o test test.o vdp.o

testgst : testgst.o gst.o
	$(CC) -o testgst testgst.o gst.o

test_x86 : test_x86.o gen_x86.o gen.o
	$(CC) -o test_x86 test_x86.o gen_x86.o gen.o

test_arm : test_arm.o gen_arm.o mem.o gen.o
	$(CC) -o test_arm test_arm.o gen_arm.o mem.o gen.o

gen_fib : gen_fib.o gen_x86.o mem.o
	$(CC) -o gen_fib gen_fib.o gen_x86.o mem.o

offsets : offsets.c z80_to_x86.h m68k_core.h
	$(CC) -o offsets offsets.c

vos_prog_info : vos_prog_info.o vos_program_module.o
	$(CC) -o vos_prog_info vos_prog_info.o vos_program_module.o

%.o : %.S
	$(CC) -c -o $@ $<

%.o : %.c
	$(CC) $(CFLAGS) -c -o $@ $<
%.png : %.xcf
	xcf2png $< > $@

%.tiles : %.spec
	./img2tiles.py -s $< $@

%.bin : %.s68
	vasmm68k_mot -Fbin -m68000 -no-opt -spaces -o $@ -L $@.list $<

%.bin : %.sz8
	vasmz80_mot -Fbin -spaces -o $@ $<

arrow.tiles : arrow.png
font_interlace_variable.tiles : font_interlace_variable.png
menu.bin : font_interlace_variable.tiles arrow.tiles

clean :
	rm -rf $(ALL) trans ztestrun ztestgen *.o
