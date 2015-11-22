LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := main

SDL_PATH := android/jni/SDL

LOCAL_C_INCLUDES := $(LOCAL_PATH)/$(SDL_PATH)/include

LOCAL_CFLAGS += -std=gnu99 -DX86_32 -DDISABLE_OPENGL

# Add your application source files here...
LOCAL_SRC_FILES := $(SDL_PATH)/src/main/android/SDL_android_main.c \
	68kinst.c debug.c gst.c psg.c z80_to_x86.c backend.c io.c render_sdl.c \
	tern.c backend_x86.c gdb_remote.c m68k_core.c romdb.c m68k_core_x86.c \
	util.c wave.c blastem.c gen.c mem.c vdp.c ym2612.c config.c gen_x86.c \
	terminal.c z80inst.c menu.c arena.c

LOCAL_SHARED_LIBRARIES := SDL2

LOCAL_LDLIBS := -lGLESv1_CM -lGLESv2 -llog

include $(BUILD_SHARED_LIBRARY)
