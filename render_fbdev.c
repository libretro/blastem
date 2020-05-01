/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <alsa/asoundlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include "render.h"
#include "blastem.h"
#include "genesis.h"
#include "bindings.h"
#include "util.h"
#include "paths.h"
#include "ppm.h"
#include "png.h"
#include "config.h"
#include "controller_info.h"

#ifndef DISABLE_OPENGL
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#ifdef USE_MALI
//Mali GLES headers don't seem to define GLchar for some reason
typedef char GLchar;
#endif
#endif

#define MAX_EVENT_POLL_PER_FRAME 2

static EGLContext main_context;

static int main_width, main_height, windowed_width, windowed_height, is_fullscreen;

static uint8_t render_gl = 1;
static uint8_t scanlines = 0;

static uint32_t last_frame = 0;
static snd_pcm_uframes_t buffer_samples;
static size_t buffer_bytes;
static unsigned int output_channels, sample_rate;


static uint8_t quitting = 0;


static void render_close_audio()
{

}

static snd_pcm_t *audio_handle;
static void *output_buffer;
void render_do_audio_ready(audio_source *src)
{
	if (src->front_populated) {
		fatal_error("Audio source filled up a buffer a second time before other sources finished their first\n");
	}
	int16_t *tmp = src->front;
	src->front = src->back;
	src->back = tmp;
	src->front_populated = 1;
	src->buffer_pos = 0;
	
	if (!all_sources_ready()) {
		return;
	}
	mix_and_convert(output_buffer, buffer_bytes, NULL);

	int frames = snd_pcm_writei(audio_handle, output_buffer, buffer_samples);
	if (frames < 0) {
		frames = snd_pcm_recover(audio_handle, frames, 0);
	}
	if (frames < 0) {
		fprintf(stderr, "Failed to write samples: %s\n", snd_strerror(frames));
	}
}

int render_width()
{
	return main_width;
}

int render_height()
{
	return main_height;
}

int render_fullscreen()
{
	return 1;
}

uint32_t red_shift, blue_shift, green_shift;
uint32_t render_map_color(uint8_t r, uint8_t g, uint8_t b)
{
	return r << red_shift | g << green_shift | b << blue_shift;
}

#ifndef DISABLE_OPENGL
static GLuint textures[3], buffers[2], vshader, fshader, program, un_textures[2], un_width, un_height, at_pos;

static GLfloat vertex_data_default[] = {
	-1.0f, -1.0f,
	 1.0f, -1.0f,
	-1.0f,  1.0f,
	 1.0f,  1.0f
};

static GLfloat vertex_data[8];

static const GLushort element_data[] = {0, 1, 2, 3};

static const GLchar shader_prefix[] =
#ifdef USE_GLES
	"#version 100\n";
#else
	"#version 110\n"
	"#define lowp\n"
	"#define mediump\n"
	"#define highp\n";
#endif

static GLuint load_shader(char * fname, GLenum shader_type)
{
	char const * parts[] = {get_home_dir(), "/.config/blastem/shaders/", fname};
	char * shader_path = alloc_concat_m(3, parts);
	FILE * f = fopen(shader_path, "rb");
	free(shader_path);
	GLchar * text;
	long fsize;
	if (f) {
		fsize = file_size(f);
		text = malloc(fsize);
		if (fread(text, 1, fsize, f) != fsize) {
			warning("Error reading from shader file %s\n", fname);
			free(text);
			return 0;
		}
	} else {
		shader_path = path_append("shaders", fname);
		uint32_t fsize32;
		text = read_bundled_file(shader_path, &fsize32);
		free(shader_path);
		if (!text) {
			warning("Failed to open shader file %s for reading\n", fname);
			return 0;
		}
		fsize = fsize32;
	}
	
	if (strncmp(text, "#version", strlen("#version"))) {
		GLchar *tmp = text;
		text = alloc_concat(shader_prefix, tmp);
		free(tmp);
		fsize += strlen(shader_prefix);
	}
	GLuint ret = glCreateShader(shader_type);
	glShaderSource(ret, 1, (const GLchar **)&text, (const GLint *)&fsize);
	free(text);
	glCompileShader(ret);
	GLint compile_status, loglen;
	glGetShaderiv(ret, GL_COMPILE_STATUS, &compile_status);
	if (!compile_status) {
		glGetShaderiv(ret, GL_INFO_LOG_LENGTH, &loglen);
		text = malloc(loglen);
		glGetShaderInfoLog(ret, loglen, NULL, text);
		warning("Shader %s failed to compile:\n%s\n", fname, text);
		free(text);
		glDeleteShader(ret);
		return 0;
	}
	return ret;
}
#endif

#define MAX_FB_LINES 590
static uint32_t texture_buf[MAX_FB_LINES * LINEBUF_SIZE * 2];
#ifdef DISABLE_OPENGL
#define RENDER_FORMAT SDL_PIXELFORMAT_ARGB8888
#else
#ifdef USE_GLES
#define INTERNAL_FORMAT GL_RGBA
#define SRC_FORMAT GL_RGBA
#define RENDER_FORMAT SDL_PIXELFORMAT_ABGR8888
#else
#define INTERNAL_FORMAT GL_RGBA8
#define SRC_FORMAT GL_BGRA
#define RENDER_FORMAT SDL_PIXELFORMAT_ARGB8888
#endif
static void gl_setup()
{
	tern_val def = {.ptrval = "linear"};
	char *scaling = tern_find_path_default(config, "video\0scaling\0", def, TVAL_PTR).ptrval;
	GLint filter = strcmp(scaling, "linear") ? GL_NEAREST : GL_LINEAR;
	glGenTextures(3, textures);
	for (int i = 0; i < 3; i++)
	{
		glBindTexture(GL_TEXTURE_2D, textures[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		if (i < 2) {
			//TODO: Fixme for PAL + invalid display mode
			glTexImage2D(GL_TEXTURE_2D, 0, INTERNAL_FORMAT, 512, 512, 0, SRC_FORMAT, GL_UNSIGNED_BYTE, texture_buf);
		} else {
			uint32_t blank = 255 << 24;
			glTexImage2D(GL_TEXTURE_2D, 0, INTERNAL_FORMAT, 1, 1, 0, SRC_FORMAT, GL_UNSIGNED_BYTE, &blank);
		}
	}
	glGenBuffers(2, buffers);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(element_data), element_data, GL_STATIC_DRAW);
	def.ptrval = "default.v.glsl";
	vshader = load_shader(tern_find_path_default(config, "video\0vertex_shader\0", def, TVAL_PTR).ptrval, GL_VERTEX_SHADER);
	def.ptrval = "default.f.glsl";
	fshader = load_shader(tern_find_path_default(config, "video\0fragment_shader\0", def, TVAL_PTR).ptrval, GL_FRAGMENT_SHADER);
	program = glCreateProgram();
	glAttachShader(program, vshader);
	glAttachShader(program, fshader);
	glLinkProgram(program);
	GLint link_status;
	glGetProgramiv(program, GL_LINK_STATUS, &link_status);
	if (!link_status) {
		fputs("Failed to link shader program\n", stderr);
		exit(1);
	}
	un_textures[0] = glGetUniformLocation(program, "textures[0]");
	un_textures[1] = glGetUniformLocation(program, "textures[1]");
	un_width = glGetUniformLocation(program, "width");
	un_height = glGetUniformLocation(program, "height");
	at_pos = glGetAttribLocation(program, "pos");
}

static void gl_teardown()
{
	glDeleteProgram(program);
	glDeleteShader(vshader);
	glDeleteShader(fshader);
	glDeleteBuffers(2, buffers);
	glDeleteTextures(3, textures);
}
#endif

static uint8_t texture_init;
static void render_alloc_surfaces()
{
	if (texture_init) {
		return;
	}
	texture_init = 1;
#ifndef DISABLE_OPENGL
	if (render_gl) {
		gl_setup();
	}
#endif
}

static void free_surfaces(void)
{
	texture_init = 0;
}

static char * caption = NULL;
static char * fps_caption = NULL;

static void render_quit()
{
	render_close_audio();
	free_surfaces();
#ifndef DISABLE_OPENGL
	if (render_gl) {
		gl_teardown();
		//FIXME: replace with EGL equivalent
		//SDL_GL_DeleteContext(main_context);
	}
#endif
}

static float config_aspect()
{
	static float aspect = 0.0f;
	if (aspect == 0.0f) {
		char *config_aspect = tern_find_path_default(config, "video\0aspect\0", (tern_val){.ptrval = "4:3"}, TVAL_PTR).ptrval;
		if (strcmp("stretch", config_aspect)) {
			aspect = 4.0f/3.0f;
			char *end;
			float aspect_numerator = strtof(config_aspect, &end);
			if (aspect_numerator > 0.0f && *end == ':') {
				float aspect_denominator = strtof(end+1, &end);
				if (aspect_denominator > 0.0f && !*end) {
					aspect = aspect_numerator / aspect_denominator;
				}
			}
		} else {
			aspect = -1.0f;
		}
	}
	return aspect;
}

static void update_aspect()
{
	//reset default values
#ifndef DISABLE_OPENGL
	memcpy(vertex_data, vertex_data_default, sizeof(vertex_data));
#endif
	if (config_aspect() > 0.0f) {
		float aspect = (float)main_width / main_height;
		if (fabs(aspect - config_aspect()) < 0.01f) {
			//close enough for government work
			return;
		}
#ifndef DISABLE_OPENGL
		if (render_gl) {
			for (int i = 0; i < 4; i++)
			{
				if (aspect > config_aspect()) {
					vertex_data[i*2] *= config_aspect()/aspect;
				} else {
					vertex_data[i*2+1] *= aspect/config_aspect();
				}
			}
		} else {
#endif
		//TODO: Maybe do some stuff for non-integer scaling in raw fbdev copy
#ifndef DISABLE_OPENGL
		}
#endif
	}
}

static uint8_t scancode_map[128] = {
	[KEY_A] = 0x1C,
	[KEY_B] = 0x32,
	[KEY_C] = 0x21,
	[KEY_D] = 0x23,
	[KEY_E] = 0x24,
	[KEY_F] = 0x2B,
	[KEY_G] = 0x34,
	[KEY_H] = 0x33,
	[KEY_I] = 0x43,
	[KEY_J] = 0x3B,
	[KEY_K] = 0x42,
	[KEY_L] = 0x4B,
	[KEY_M] = 0x3A,
	[KEY_N] = 0x31,
	[KEY_O] = 0x44,
	[KEY_P] = 0x4D,
	[KEY_Q] = 0x15,
	[KEY_R] = 0x2D,
	[KEY_S] = 0x1B,
	[KEY_T] = 0x2C,
	[KEY_U] = 0x3C,
	[KEY_V] = 0x2A,
	[KEY_W] = 0x1D,
	[KEY_X] = 0x22,
	[KEY_Y] = 0x35,
	[KEY_Z] = 0x1A,
	[KEY_1] = 0x16,
	[KEY_2] = 0x1E,
	[KEY_3] = 0x26,
	[KEY_4] = 0x25,
	[KEY_5] = 0x2E,
	[KEY_6] = 0x36,
	[KEY_7] = 0x3D,
	[KEY_8] = 0x3E,
	[KEY_9] = 0x46,
	[KEY_0] = 0x45,
	[KEY_ENTER] = 0x5A,
	[KEY_ESC] = 0x76,
	[KEY_SPACE] = 0x29,
	[KEY_TAB] = 0x0D,
	[KEY_BACKSPACE] = 0x66,
	[KEY_MINUS] = 0x4E,
	[KEY_EQUAL] = 0x55,
	[KEY_LEFTBRACE] = 0x54,
	[KEY_RIGHTBRACE] = 0x5B,
	[KEY_BACKSLASH] = 0x5D,
	[KEY_SEMICOLON] = 0x4C,
	[KEY_APOSTROPHE] = 0x52,
	[KEY_GRAVE] = 0x0E,
	[KEY_COMMA] = 0x41,
	[KEY_DOT] = 0x49,
	[KEY_SLASH] = 0x4A,
	[KEY_CAPSLOCK] = 0x58,
	[KEY_F1] = 0x05,
	[KEY_F2] = 0x06,
	[KEY_F3] = 0x04,
	[KEY_F4] = 0x0C,
	[KEY_F5] = 0x03,
	[KEY_F6] = 0x0B,
	[KEY_F7] = 0x83,
	[KEY_F8] = 0x0A,
	[KEY_F9] = 0x01,
	[KEY_F10] = 0x09,
	[KEY_F11] = 0x78,
	[KEY_F12] = 0x07,
	[KEY_LEFTCTRL] = 0x14,
	[KEY_LEFTSHIFT] = 0x12,
	[KEY_LEFTALT] = 0x11,
	[KEY_RIGHTCTRL] = 0x18,
	[KEY_RIGHTSHIFT] = 0x59,
	[KEY_RIGHTALT] = 0x17,
	[KEY_INSERT] = 0x81,
	[KEY_PAUSE] = 0x82,
	[KEY_SYSRQ] = 0x84,
	[KEY_SCROLLLOCK] = 0x7E,
	[KEY_DELETE] = 0x85,
	[KEY_LEFT] = 0x86,
	[KEY_HOME] = 0x87,
	[KEY_END] = 0x88,
	[KEY_UP] = 0x89,
	[KEY_DOWN] = 0x8A,
	[KEY_PAGEUP] = 0x8B,
	[KEY_PAGEDOWN] = 0x8C,
	[KEY_RIGHT] = 0x8D,
	[KEY_NUMLOCK] = 0x77,
	[KEY_KPSLASH] = 0x80,
	[KEY_KPASTERISK] = 0x7C,
	[KEY_KPMINUS] = 0x7B,
	[KEY_KPPLUS] = 0x79,
	[KEY_KPENTER] = 0x19,
	[KEY_KP1] = 0x69,
	[KEY_KP2] = 0x72,
	[KEY_KP3] = 0x7A,
	[KEY_KP4] = 0x6B,
	[KEY_KP5] = 0x73,
	[KEY_KP6] = 0x74,
	[KEY_KP7] = 0x6C,
	[KEY_KP8] = 0x75,
	[KEY_KP9] = 0x7D,
	[KEY_KP0] = 0x70,
	[KEY_KPDOT] = 0x71,
};

#include "special_keys_evdev.h"
static uint8_t sym_map[128] = {
	[KEY_A] = 'a',
	[KEY_B] = 'b',
	[KEY_C] = 'c',
	[KEY_D] = 'd',
	[KEY_E] = 'e',
	[KEY_F] = 'f',
	[KEY_G] = 'g',
	[KEY_H] = 'h',
	[KEY_I] = 'i',
	[KEY_J] = 'j',
	[KEY_K] = 'k',
	[KEY_L] = 'l',
	[KEY_M] = 'm',
	[KEY_N] = 'n',
	[KEY_O] = 'o',
	[KEY_P] = 'p',
	[KEY_Q] = 'q',
	[KEY_R] = 'r',
	[KEY_S] = 's',
	[KEY_T] = 't',
	[KEY_U] = 'u',
	[KEY_V] = 'v',
	[KEY_W] = 'w',
	[KEY_X] = 'x',
	[KEY_Y] = 'y',
	[KEY_Z] = 'z',
	[KEY_1] = '1',
	[KEY_2] = '2',
	[KEY_3] = '3',
	[KEY_4] = '4',
	[KEY_5] = '5',
	[KEY_6] = '6',
	[KEY_7] = '7',
	[KEY_8] = '8',
	[KEY_9] = '9',
	[KEY_0] = '0',
	[KEY_ENTER] = '\r',
	[KEY_SPACE] = ' ',
	[KEY_TAB] = '\t',
	[KEY_BACKSPACE] = '\b',
	[KEY_MINUS] = '-',
	[KEY_EQUAL] = '=',
	[KEY_LEFTBRACE] = '[',
	[KEY_RIGHTBRACE] = ']',
	[KEY_BACKSLASH] = '\\',
	[KEY_SEMICOLON] = ';',
	[KEY_APOSTROPHE] = '\'',
	[KEY_GRAVE] = '`',
	[KEY_COMMA] = ',',
	[KEY_DOT] = '.',
	[KEY_SLASH] = '/',
	[KEY_ESC] = RENDERKEY_ESC,
	[KEY_F1] = RENDERKEY_F1,
	[KEY_F2] = RENDERKEY_F2,
	[KEY_F3] = RENDERKEY_F3,
	[KEY_F4] = RENDERKEY_F4,
	[KEY_F5] = RENDERKEY_F5,
	[KEY_F6] = RENDERKEY_F6,
	[KEY_F7] = RENDERKEY_F7,
	[KEY_F8] = RENDERKEY_F8,
	[KEY_F9] = RENDERKEY_F9,
	[KEY_F10] = RENDERKEY_F10,
	[KEY_F11] = RENDERKEY_F11,
	[KEY_F12] = RENDERKEY_F12,
	[KEY_LEFTCTRL] = RENDERKEY_LCTRL,
	[KEY_LEFTSHIFT] = RENDERKEY_LSHIFT,
	[KEY_LEFTALT] = RENDERKEY_LALT,
	[KEY_RIGHTCTRL] = RENDERKEY_RCTRL,
	[KEY_RIGHTSHIFT] = RENDERKEY_RSHIFT,
	[KEY_RIGHTALT] = RENDERKEY_RALT,
	[KEY_DELETE] = RENDERKEY_DEL,
	[KEY_LEFT] = RENDERKEY_LEFT,
	[KEY_HOME] = RENDERKEY_HOME,
	[KEY_END] = RENDERKEY_END,
	[KEY_UP] = RENDERKEY_UP,
	[KEY_DOWN] = RENDERKEY_DOWN,
	[KEY_PAGEUP] = RENDERKEY_PAGEUP,
	[KEY_PAGEDOWN] = RENDERKEY_PAGEDOWN,
	[KEY_RIGHT] = RENDERKEY_RIGHT,
	[KEY_KPSLASH] = 0x80,
	[KEY_KPASTERISK] = 0x7C,
	[KEY_KPMINUS] = 0x7B,
	[KEY_KPPLUS] = 0x79,
	[KEY_KPENTER] = 0x19,
	[KEY_KP1] = 0x69,
	[KEY_KP2] = 0x72,
	[KEY_KP3] = 0x7A,
	[KEY_KP4] = 0x6B,
	[KEY_KP5] = 0x73,
	[KEY_KP6] = 0x74,
	[KEY_KP7] = 0x6C,
	[KEY_KP8] = 0x75,
	[KEY_KP9] = 0x7D,
	[KEY_KP0] = 0x70,
	[KEY_KPDOT] = 0x71,
};

static drop_handler drag_drop_handler;
void render_set_drag_drop_handler(drop_handler handler)
{
	drag_drop_handler = handler;
}

char* render_joystick_type_id(int index)
{
	return strdup("");
}

static uint32_t overscan_top[NUM_VID_STD] = {2, 21};
static uint32_t overscan_bot[NUM_VID_STD] = {1, 17};
static uint32_t overscan_left[NUM_VID_STD] = {13, 13};
static uint32_t overscan_right[NUM_VID_STD] = {14, 14};
static vid_std video_standard = VID_NTSC;

typedef enum {
	DEV_NONE,
	DEV_KEYBOARD,
	DEV_MOUSE,
	DEV_GAMEPAD
} device_type;

static int32_t mouse_x, mouse_y, mouse_accum_x, mouse_accum_y;
static int32_t handle_event(device_type dtype, int device_index, struct input_event *event)
{
	switch (event->type) {
	case EV_KEY:
		//code is key, value is 1 for keydown, 0 for keyup
		if (dtype == DEV_KEYBOARD && event->code < 128) {
			//keyboard key that we might have a mapping for
			if (event->value) {
				handle_keydown(sym_map[event->code], scancode_map[event->code]);
			} else {
				handle_keyup(sym_map[event->code], scancode_map[event->code]);
			}
		} else if (dtype == DEV_MOUSE && event->code >= BTN_MOUSE && event->code < BTN_JOYSTICK) {
			//mosue button
			if (event->value) {
				handle_mousedown(device_index, event->code - BTN_LEFT);
			} else {
				handle_mouseup(device_index, event->code - BTN_LEFT);
			}
		} else if (dtype == DEV_GAMEPAD && event->code >= BTN_GAMEPAD && event->code < BTN_DIGI) {
			//gamepad button
			if (event->value) {
				handle_joydown(device_index, event->code - BTN_SOUTH);
			} else {
				handle_joyup(device_index, event->code - BTN_SOUTH);
			}
		}
		break;
	case EV_REL:
		if (dtype == DEV_MOUSE) {
			switch(event->code)
			{
			case REL_X:
				mouse_accum_x += event->value;
				break;
			case REL_Y:
				mouse_accum_y += event->value;
				break;
			}
		}
		break;
	case EV_ABS:
		//TODO: Handle joystick axis/hat motion, absolute mouse movement
		break;
	case EV_SYN:
		if (dtype == DEV_MOUSE && (mouse_accum_x || mouse_accum_y)) {
			mouse_x += mouse_accum_x;
			mouse_y += mouse_accum_y;
			if (mouse_x < 0) {
				mouse_x = 0;
			} else if (mouse_x >= main_width) {
				mouse_x = main_width - 1;
			}
			if (mouse_y < 0) {
				mouse_y = 0;
			} else if (mouse_y >= main_height) {
				mouse_y = main_height - 1;
			}
			handle_mouse_moved(device_index, mouse_x, mouse_y, mouse_accum_x, mouse_accum_y);
			mouse_accum_x = mouse_accum_y = 0;
		}
		break;
	/*
	case SDL_JOYHATMOTION:
		handle_joy_dpad(find_joystick_index(event->jhat.which), event->jhat.hat, event->jhat.value);
		break;
	case SDL_JOYAXISMOTION:
		handle_joy_axis(find_joystick_index(event->jaxis.which), event->jaxis.axis, event->jaxis.value);
		break;*/
	}
	return 0;
}

#define MAX_DEVICES 16
static int device_fds[MAX_DEVICES];
static device_type device_types[MAX_DEVICES];
static int cur_devices;

static void drain_events()
{
	struct input_event events[64];
	int index_by_type[3] = {0,0,0};
	for (int i = 0; i < cur_devices; i++)
	{
		int bytes = sizeof(events);
		int device_index = index_by_type[device_types[i]-1]++;
		while (bytes == sizeof(events))
		{
			bytes = read(device_fds[i], events, sizeof(events));
			if (bytes > 0) {
				int num_events = bytes / sizeof(events[0]);
				for (int j = 0; j < num_events; j++)
				{
					handle_event(device_types[i], device_index, events + j);
				}
			} else if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				perror("Failed to read evdev events");
			}
		}
	}
}

static char *vid_std_names[NUM_VID_STD] = {"ntsc", "pal"};

static void init_audio()
{
	char *device_name = tern_find_path_default(config, "audio\0alsa_device\0", (tern_val){.ptrval="default"}, TVAL_PTR).ptrval;
	int res = snd_pcm_open(&audio_handle, device_name, SND_PCM_STREAM_PLAYBACK, 0);
	if (res < 0) {
		fatal_error("Failed to open ALSA device: %s\n", snd_strerror(res));
	}
	
	snd_pcm_hw_params_t *params;
	snd_pcm_hw_params_alloca(&params);
	res = snd_pcm_hw_params_any(audio_handle, params);
	if (res < 0) {
		fatal_error("No playback configurations available: %s\n", snd_strerror(res));
	}
	res = snd_pcm_hw_params_set_access(audio_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (res < 0) {
		fatal_error("Failed to set access type: %s\n", snd_strerror(res));
	}
	res = snd_pcm_hw_params_set_format(audio_handle, params, SND_PCM_FORMAT_S16_LE);
	if (res < 0) {
		//failed to set, signed 16-bit integer, try floating point
		res = snd_pcm_hw_params_set_format(audio_handle, params, SND_PCM_FORMAT_FLOAT_LE);
		if (res < 0) {
			fatal_error("Failed to set an acceptable format: %s\n", snd_strerror(res));
		}
		mix = mix_f32;
	} else {
		mix = mix_s16;
	}

    char * rate_str = tern_find_path(config, "audio\0rate\0", TVAL_PTR).ptrval;
   	sample_rate = rate_str ? atoi(rate_str) : 0;
   	if (!sample_rate) {
   		sample_rate = 48000;
   	}
    snd_pcm_hw_params_set_rate_near(audio_handle, params, &sample_rate, NULL);
	output_channels = 2;
	snd_pcm_hw_params_set_channels_near(audio_handle, params, &output_channels);

    char * samples_str = tern_find_path(config, "audio\0buffer\0", TVAL_PTR).ptrval;
   	buffer_samples = samples_str ? atoi(samples_str) : 0;
   	if (!buffer_samples) {
   		buffer_samples = 512;
   	}
	snd_pcm_hw_params_set_period_size_near(audio_handle, params, &buffer_samples, NULL);
	
	int dir = 1;
	unsigned int periods = 2;
	snd_pcm_hw_params_set_periods_near(audio_handle, params, &periods, &dir);

	res = snd_pcm_hw_params(audio_handle, params);
	if (res < 0) {
		fatal_error("Failed to set ALSA hardware params: %s\n", snd_strerror(res));
	}
	
	printf("Initialized audio at frequency %d with a %d sample buffer, ", (int)sample_rate, (int)buffer_samples);
	if (mix == mix_s16) {
		puts("signed 16-bit int format");
	} else {
		puts("32-bit float format");
	}
}

int fbfd;
uint32_t *framebuffer;
uint32_t fb_stride;
#ifndef DISABLE_OPENGL
EGLDisplay egl_display;
EGLSurface main_surface;
uint8_t egl_setup(void)
{
	//Mesa wants the fbdev file descriptor as the display
	egl_display = eglGetDisplay((EGLNativeDisplayType)fbfd);
	if (egl_display == EGL_NO_DISPLAY) {
		//Mali (and possibly others) seems to just want EGL_DEFAULT_DISPLAY
		egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		if (egl_display == EGL_NO_DISPLAY) {
			warning("eglGetDisplay failed with error %X\n", eglGetError());
			return 0;
		}
	}
	EGLint major, minor;
	if (!eglInitialize(egl_display, &major, &minor)) {
		warning("eglInitialize failed with error %X\n", eglGetError());
		return 0;
	}
	printf("EGL version %d.%d\n", major, minor);
	EGLint num_configs;
	EGLConfig config;
	EGLint const config_attribs[] = {
		EGL_RED_SIZE, 5,
		EGL_GREEN_SIZE, 5,
		EGL_BLUE_SIZE, 5,
		EGL_CONFORMANT, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};
	if (!eglChooseConfig(egl_display, config_attribs, &config, 1, &num_configs)) {
		num_configs = 0;
		warning("eglChooseConfig failed with error %X\n", eglGetError());
	}
	if (!num_configs) {
		warning("Failed to choose an EGL config\n");
		goto error;
	}
	EGLint const context_attribs[] = {
#ifdef EGL_CONTEXT_MAJOR_VERSION
		EGL_CONTEXT_MAJOR_VERSION, 2,
#endif
		EGL_NONE
	};
	main_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attribs);
	if (main_context == EGL_NO_CONTEXT) {
		warning("Failed to create EGL context %X\n", eglGetError());
		goto error;
	}
#ifdef USE_MALI
	struct mali_native_window native_window = {
		.width = main_width,
		.height = main_height
	};
	main_surface = eglCreateWindowSurface(egl_display, config, &native_window, NULL);
#else
	main_surface = eglCreateWindowSurface(egl_display, config, (EGLNativeWindowType)NULL, NULL);
#endif
	if (main_surface == EGL_NO_SURFACE) {
		warning("Failed to create EGL surface %X\n", eglGetError());
		goto post_context_error;
	}
	if (eglMakeCurrent(egl_display, main_surface, main_surface, main_context)) {
		return 1;
	}
	eglDestroySurface(egl_display, main_surface);
post_context_error:
	eglDestroyContext(egl_display, main_context);
error:
	eglTerminate(egl_display);
	return 0;
}
#endif
static pthread_mutex_t buffer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t buffer_cond = PTHREAD_COND_INITIALIZER;
static uint8_t buffer_ready;
static uint32_t *copy_buffer;
static uint32_t last_width, last_width_scale, last_height, last_height_scale;
static uint32_t max_multiple;

static uint32_t mix_pixel(uint32_t last, uint32_t cur, float ratio)
{
	float a,b,c,d;
	a = (last & 255) * ratio;
	b = (last >> 8 & 255) * ratio;
	c = (last >> 16 & 255) * ratio;
	d = (last >> 24 & 255) * ratio;
	ratio = 1.0f - ratio;
	a += (cur & 255) * ratio;
	b += (cur >> 8 & 255) * ratio;
	c += (cur >> 16 & 255) * ratio;
	d += (cur >> 24 & 255) * ratio;
	return ((int)d) << 24 | ((int)c) << 16 | ((int)b) << 8 | ((int)a);
}
static void do_buffer_copy(void)
{
	uint32_t width_multiple = main_width / last_width_scale;
	uint32_t height_multiple = main_height / last_height_scale;
	uint32_t multiple = width_multiple < height_multiple ? width_multiple : height_multiple;
	if (max_multiple && multiple > max_multiple) {
		multiple = max_multiple;
	}
	height_multiple = last_height_scale * multiple / last_height;
	uint32_t *cur_line = framebuffer + (main_width - last_width_scale * multiple)/2;
	cur_line += fb_stride * (main_height - last_height_scale * multiple) / (2 * sizeof(uint32_t));
	uint32_t *src_line = copy_buffer;
	if (height_multiple * last_height == multiple * last_height_scale) {
		if (last_width == last_width_scale) {
			for (uint32_t y = 0; y < last_height; y++)
			{
				for (uint32_t i = 0; i < height_multiple; i++)
				{
					uint32_t *cur = cur_line;
					uint32_t *src = src_line;
					for (uint32_t x = 0; x < last_width	; x++)
					{
						uint32_t pixel = *(src++);
						for (uint32_t j = 0; j < multiple; j++)
						{
							*(cur++) = pixel;
						}
					}
					
					cur_line += fb_stride / sizeof(uint32_t);
				}
				src_line += LINEBUF_SIZE;
			}
		} else {
			float scale_multiple = ((float)(last_width_scale * multiple)) / (float)last_width;
			float remaining = 0.0f;
			uint32_t last_pixel = 0;
			for (uint32_t y = 0; y < last_height; y++)
			{
				for (uint32_t i = 0; i < height_multiple; i++)
				{
					uint32_t *cur = cur_line;
					uint32_t *src = src_line;
					for (uint32_t x = 0; x < last_width	; x++)
					{
						uint32_t pixel = *(src++);
						float count = scale_multiple;
						if (remaining > 0.0f) {
							*(cur++) = mix_pixel(last_pixel, pixel, remaining);
							count -= 1.0f - remaining;
						}
						for (; count >= 1; count -= 1.0f)
						{
							*(cur++) = pixel;
						}
						remaining = count;
						last_pixel = pixel;
					}
					
					cur_line += fb_stride / sizeof(uint32_t);
				}
				src_line += LINEBUF_SIZE;
			}
		}
	} else {
		float height_scale = ((float)(last_height_scale * multiple)) / (float)last_height;
		float height_remaining = 0.0f;
		uint32_t *last_line;
		if (last_width == last_width_scale) {
			for (uint32_t y = 0; y < last_height; y++)
			{
				float hcount = height_scale;
				if (height_remaining > 0.0f) {
					uint32_t *cur = cur_line;
					uint32_t *src = src_line;
					uint32_t *last = last_line;
					for (uint32_t x = 0; x < last_width	; x++)
					{
						uint32_t mixed = mix_pixel(*(last++), *(src++), height_remaining);
						for (uint32_t j = 0; j < multiple; j++)
						{
							*(cur++) = mixed;
						}
					}
					hcount -= 1.0f - height_remaining;
					cur_line += fb_stride / sizeof(uint32_t);
				}
				for(; hcount >= 1; hcount -= 1.0f)
				{
					uint32_t *cur = cur_line;
					uint32_t *src = src_line;
					for (uint32_t x = 0; x < last_width	; x++)
					{
						uint32_t pixel = *(src++);
						for (uint32_t j = 0; j < multiple; j++)
						{
							*(cur++) = pixel;
						}
					}
					
					cur_line += fb_stride / sizeof(uint32_t);
				}
				height_remaining = hcount;
				last_line = src_line;
				src_line += LINEBUF_SIZE;
			}
		} else {
			float scale_multiple = ((float)(last_width_scale * multiple)) / (float)last_width;
			float remaining = 0.0f;
			uint32_t last_pixel = 0;
			for (uint32_t y = 0; y < last_height; y++)
			{
				float hcount = height_scale;
				if (height_remaining > 0.0f) {
					uint32_t *cur = cur_line;
					uint32_t *src = src_line;
					uint32_t *last = last_line;
					
					for (uint32_t x = 0; x < last_width; x++)
					{
						uint32_t pixel = mix_pixel(*(last++), *(src++), height_remaining);
						float count = scale_multiple;
						if (remaining > 0.0f) {
							*(cur++) = mix_pixel(last_pixel, pixel, remaining);
							count -= 1.0f - remaining;
						}
						for (; count >= 1.0f; count -= 1.0f)
						{
							*(cur++) = pixel;
						}
						remaining = count;
						last_pixel = pixel;
					}
					hcount -= 1.0f - height_remaining;
					cur_line += fb_stride / sizeof(uint32_t);
				}
				
				for (; hcount >= 1.0f; hcount -= 1.0f)
				{
					uint32_t *cur = cur_line;
					uint32_t *src = src_line;
					for (uint32_t x = 0; x < last_width	; x++)
					{
						uint32_t pixel = *(src++);
						float count = scale_multiple;
						if (remaining > 0.0f) {
							*(cur++) = mix_pixel(last_pixel, pixel, remaining);
							count -= 1.0f - remaining;
						}
						for (; count >= 1; count -= 1.0f)
						{
							*(cur++) = pixel;
						}
						remaining = count;
						last_pixel = pixel;
					}
					
					cur_line += fb_stride / sizeof(uint32_t);
				}
				height_remaining = hcount;
				last_line = src_line;
				src_line += LINEBUF_SIZE;
			}
		}
	}
}
static void *buffer_copy(void *data)
{
	pthread_mutex_lock(&buffer_lock);
	for(;;)
	{
		while (!buffer_ready)
		{
			pthread_cond_wait(&buffer_cond, &buffer_lock);
		}
		buffer_ready = 0;
		do_buffer_copy();
	}
	return 0;
}

static pthread_t buffer_copy_handle;
static uint8_t copy_use_thread;
void window_setup(void)
{
	fbfd = open("/dev/fb0", O_RDWR);
	struct fb_fix_screeninfo fixInfo;
	struct fb_var_screeninfo varInfo;
	ioctl(fbfd, FBIOGET_FSCREENINFO, &fixInfo);
	ioctl(fbfd, FBIOGET_VSCREENINFO, &varInfo);
	printf("Resolution: %d x %d\n", varInfo.xres, varInfo.yres);
	printf("Framebuffer size: %d, line stride: %d\n", fixInfo.smem_len, fixInfo.line_length);
	main_width = varInfo.xres;
	main_height = varInfo.yres;
	fb_stride = fixInfo.line_length;
	tern_val def = {.ptrval = "audio"};
	char *sync_src = tern_find_path_default(config, "system\0sync_source\0", def, TVAL_PTR).ptrval;
		
	const char *vsync;
	def.ptrval = "off";
	vsync = tern_find_path_default(config, "video\0vsync\0", def, TVAL_PTR).ptrval;
	
	
	tern_node *video = tern_find_node(config, "video");
	if (video)
	{
		for (int i = 0; i < NUM_VID_STD; i++)
		{
			tern_node *std_settings = tern_find_node(video, vid_std_names[i]);
			if (std_settings) {
				char *val = tern_find_path_default(std_settings, "overscan\0top\0", (tern_val){.ptrval = NULL}, TVAL_PTR).ptrval;
				if (val) {
					overscan_top[i] = atoi(val);
				}
				val = tern_find_path_default(std_settings, "overscan\0bottom\0", (tern_val){.ptrval = NULL}, TVAL_PTR).ptrval;
				if (val) {
					overscan_bot[i] = atoi(val);
				}
				val = tern_find_path_default(std_settings, "overscan\0left\0", (tern_val){.ptrval = NULL}, TVAL_PTR).ptrval;
				if (val) {
					overscan_left[i] = atoi(val);
				}
				val = tern_find_path_default(std_settings, "overscan\0right\0", (tern_val){.ptrval = NULL}, TVAL_PTR).ptrval;
				if (val) {
					overscan_right[i] = atoi(val);
				}
			}
		}
	}
	render_gl = 0;
#ifndef DISABLE_OPENGL
	char *gl_enabled_str = tern_find_path_default(config, "video\0gl\0", def, TVAL_PTR).ptrval;
	uint8_t gl_enabled = strcmp(gl_enabled_str, "off") != 0;
	if (gl_enabled)
	{
		render_gl = egl_setup();
		blue_shift = 16;
		green_shift = 8;
		red_shift = 0;
	}
	if (!render_gl) {
#endif
	framebuffer = mmap(NULL, fixInfo.smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, fbfd, 0);
	red_shift = varInfo.red.offset;
	green_shift = varInfo.green.offset;
	blue_shift = varInfo.blue.offset;
	def.ptrval = "0";
	max_multiple = atoi(tern_find_path_default(config, "video\0fbdev\0max_multiple\0", def, TVAL_PTR).ptrval);
	def.ptrval = "true";
	copy_use_thread = strcmp(tern_find_path_default(config, "video\0fbdev\0use_thread\0", def, TVAL_PTR).ptrval, "false");
	if (copy_use_thread) {
		pthread_create(&buffer_copy_handle, NULL, buffer_copy, NULL);
	}
#ifndef DISABLE_OPENGL
	}
#endif
	
	update_aspect();
	render_alloc_surfaces();
	def.ptrval = "off";
	scanlines = !strcmp(tern_find_path_default(config, "video\0scanlines\0", def, TVAL_PTR).ptrval, "on");
}

void restore_tty(void)
{
	ioctl(STDIN_FILENO, KDSETMODE, KD_TEXT);
	for (int i = 0; i < cur_devices; i++)
	{
		if (device_types[i] == DEV_KEYBOARD) {
			ioctl(device_fds[i], EVIOCGRAB, 0);
		}
	}
}

void render_init(int width, int height, char * title, uint8_t fullscreen)
{
	if (height <= 0) {
		float aspect = config_aspect() > 0.0f ? config_aspect() : 4.0f/3.0f;
		height = ((float)width / aspect) + 0.5f;
	}
	printf("width: %d, height: %d\n", width, height);
	windowed_width = width;
	windowed_height = height;
	
	main_width = width;
	main_height = height;
	
	caption = title;
	
	if (isatty(STDIN_FILENO)) {
		ioctl(STDIN_FILENO, KDSETMODE, KD_GRAPHICS);
		atexit(restore_tty);
	}
	
	window_setup();
	
	init_audio();
	
	render_set_video_standard(VID_NTSC);
	
	DIR *d = opendir("/dev/input");
	struct dirent* entry;
	int joystick_counter = 0;
	while ((entry = readdir(d)) && cur_devices < MAX_DEVICES)
	{
		if (!strncmp("event", entry->d_name, strlen("event"))) {
			char *filename = alloc_concat("/dev/input/", entry->d_name);
			int fd = open(filename, O_RDONLY);
			if (fd == -1) {
				int errnum = errno;
				warning("Failed to open evdev device %s for reading: %s\n", filename, strerror(errnum));
				free(filename);
				continue;
			}
			
			unsigned long bits;
			if (-1 == ioctl(fd, EVIOCGBIT(0, sizeof(bits)), &bits)) {
				int errnum = errno;
				warning("Failed get capability bits from evdev device %s: %s\n", filename, strerror(errnum));
				free(filename);
				close(fd);
				continue;
			}
			if (!(1 & bits >> EV_KEY)) {
				//if it doesn't support key events we don't care about it
				free(filename);
				close(fd);
				continue;
			}
			unsigned long button_bits[(BTN_THUMBR+8*sizeof(long))/(8*sizeof(long))];
			int res = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(button_bits)), button_bits);
			if (-1 == res) {
				int errnum = errno;
				warning("Failed get capability bits from evdev device %s: %s\n", filename, strerror(errnum));
				free(filename);
				close(fd);
				continue;
			}
			int to_check[] = {KEY_ENTER, BTN_MOUSE, BTN_GAMEPAD};
			device_type dtype = DEV_NONE;
			for (int i = 0; i < 3; i++)
			{
				if (1 & button_bits[to_check[i]/(8*sizeof(button_bits[0]))] >> to_check[i]%(8*sizeof(button_bits[0]))) {
					dtype = i + 1;
				}
			}
			if (dtype == DEV_NONE) {
				close(fd);
			} else {
				device_fds[cur_devices] = fd;
				device_types[cur_devices] = dtype;
				char name[1024];
				char *names[] = {"Keyboard", "Mouse", "Gamepad"};
				ioctl(fd, EVIOCGNAME(sizeof(name)), name);
				printf("%s is a %s\n%s\n", filename, names[dtype - 1], name);
				
				if (dtype == DEV_GAMEPAD) {
					handle_joy_added(joystick_counter++);
				} else if (dtype == DEV_KEYBOARD && isatty(STDIN_FILENO)) {
					ioctl(fd, EVIOCGRAB, 1);
				}
				
				//set FD to non-blocking mode for event polling
				fcntl(fd, F_SETFL, O_NONBLOCK);
				cur_devices++;
			}
			free(filename);
		}
	}

	atexit(render_quit);
}
#include<unistd.h>
static int in_toggle;
static void update_source(audio_source *src, double rc, uint8_t sync_changed)
{
	double alpha = src->dt / (src->dt + rc);
	int32_t lowpass_alpha = (int32_t)(((double)0x10000) * alpha);
	src->lowpass_alpha = lowpass_alpha;
}

void render_config_updated(void)
{
	
	free_surfaces();
#ifndef DISABLE_OPENGL
	if (render_gl) {
		/*if (on_context_destroyed) {
			on_context_destroyed();
		}*/
		gl_teardown();
		//FIXME: EGL equivalent
		//SDL_GL_DeleteContext(main_context);
	} else {
#endif
#ifndef DISABLE_OPENGL
	}
#endif
	//FIXME: EGL equivalent
	//SDL_DestroyWindow(main_window);
	drain_events();
	
	char *config_width = tern_find_path(config, "video\0width\0", TVAL_PTR).ptrval;
	if (config_width) {
		windowed_width = atoi(config_width);
	}
	char *config_height = tern_find_path(config, "video\0height\0", TVAL_PTR).ptrval;
	if (config_height) {
		windowed_height = atoi(config_height);
	} else {
		float aspect = config_aspect() > 0.0f ? config_aspect() : 4.0f/3.0f;
		windowed_height = ((float)windowed_width / aspect) + 0.5f;
	}
	
	window_setup();
	update_aspect();
#ifndef DISABLE_OPENGL
	//need to check render_gl again after window_setup as render option could have changed
	/*if (render_gl && on_context_created) {
		on_context_created();
	}*/
#endif

	render_close_audio();
	quitting = 0;
	init_audio();
	render_set_video_standard(video_standard);
	
	double lowpass_cutoff = get_lowpass_cutoff(config);
	double rc = (1.0 / lowpass_cutoff) / (2.0 * M_PI);
	for (uint8_t i = 0; i < num_audio_sources; i++)
	{
		update_source(audio_sources[i], rc, 0);
	}
	for (uint8_t i = 0; i < num_inactive_audio_sources; i++)
	{
		update_source(inactive_audio_sources[i], rc, 0);
	}
	drain_events();
}

void render_set_video_standard(vid_std std)
{
	video_standard = std;
}

void render_update_caption(char *title)
{
	caption = title;
	free(fps_caption);
	fps_caption = NULL;
}

static char *screenshot_path;
void render_save_screenshot(char *path)
{
	if (screenshot_path) {
		free(screenshot_path);
	}
	screenshot_path = path;
}

uint8_t render_create_window(char *caption, uint32_t width, uint32_t height, window_close_handler close_handler)
{
	//not supported under fbdev
	return 0;
}

void render_destroy_window(uint8_t which)
{
	//not supported under fbdev
}

static uint8_t last_fb;
static uint32_t texture_off;
uint32_t *render_get_framebuffer(uint8_t which, int *pitch)
{
	if (max_multiple == 1 && !render_gl) {
		if (last_fb != which) {
			*pitch = fb_stride * 2;
			return framebuffer + (which == FRAMEBUFFER_EVEN ? fb_stride / sizeof(uint32_t) : 0);
		}
		*pitch = fb_stride;
		return framebuffer;
	}
	if (!render_gl && last_fb != which) {
		*pitch = LINEBUF_SIZE * sizeof(uint32_t) * 2;
		return texture_buf + texture_off + (which == FRAMEBUFFER_EVEN ? LINEBUF_SIZE : 0);
	}
	*pitch = LINEBUF_SIZE * sizeof(uint32_t);
	return texture_buf + texture_off;
}

uint8_t events_processed;
#ifdef __ANDROID__
#define FPS_INTERVAL 10000
#else
#define FPS_INTERVAL 1000
#endif

static uint8_t interlaced;
void render_update_display();
void render_framebuffer_updated(uint8_t which, int width)
{
	uint32_t height = which <= FRAMEBUFFER_EVEN 
		? (video_standard == VID_NTSC ? 243 : 294) - (overscan_top[video_standard] + overscan_bot[video_standard])
		: 240;
	width -= overscan_left[video_standard] + overscan_right[video_standard];
#ifndef DISABLE_OPENGL
	if (render_gl && which <= FRAMEBUFFER_EVEN) {
		last_width = width;
		glBindTexture(GL_TEXTURE_2D, textures[which]);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, LINEBUF_SIZE, height, SRC_FORMAT, GL_UNSIGNED_BYTE, texture_buf + overscan_left[video_standard] + LINEBUF_SIZE * overscan_top[video_standard]);
		render_update_display();
		last_height = height;
	} else {
#endif
	if (max_multiple != 1) {
		if (copy_use_thread) {
			pthread_mutex_lock(&buffer_lock);
				buffer_ready = 1;
				last_width = width;
				last_width_scale = LINEBUF_SIZE - (overscan_left[video_standard] + overscan_right[video_standard]);
				last_height = last_height_scale = height;
				copy_buffer = texture_buf + texture_off + overscan_left[video_standard] + LINEBUF_SIZE * overscan_top[video_standard];
				if (which != last_fb) {
					last_height *= 2;
					copy_buffer += LINEBUF_SIZE * overscan_top[video_standard];
					uint32_t *src = texture_buf + (texture_off ? 0 : LINEBUF_SIZE * MAX_FB_LINES) + overscan_left[video_standard] + LINEBUF_SIZE * overscan_top[video_standard] + LINEBUF_SIZE * overscan_top[video_standard];
					uint32_t *dst = copy_buffer;
					if (which == FRAMEBUFFER_ODD) {
						src += LINEBUF_SIZE;
						dst += LINEBUF_SIZE;
					}
					for (int i = 0; i < height; i++)
					{
						memcpy(dst, src, width * sizeof(uint32_t));
						src += LINEBUF_SIZE * 2;
						dst += LINEBUF_SIZE * 2;
					}
				}
				texture_off = texture_off ? 0 : LINEBUF_SIZE * MAX_FB_LINES;
				pthread_cond_signal(&buffer_cond);
			pthread_mutex_unlock(&buffer_lock);
		} else {
			last_width = width;
			last_width_scale = LINEBUF_SIZE - (overscan_left[video_standard] + overscan_right[video_standard]);
			last_height = last_height_scale = height;
			copy_buffer = texture_buf + texture_off + overscan_left[video_standard] + LINEBUF_SIZE * overscan_top[video_standard];
			if (which != last_fb) {
				last_height *= 2;
				copy_buffer += LINEBUF_SIZE * overscan_top[video_standard];
			}
			do_buffer_copy();
		}
	}
	last_fb = which;
	if (!events_processed) {
		process_events();
	}
	events_processed = 0;
#ifndef DISABLE_OPENGL
	}
#endif
}

void render_update_display()
{
#ifndef DISABLE_OPENGL
	if (render_gl) {
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		glUseProgram(program);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, textures[0]);
		glUniform1i(un_textures[0], 0);

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, textures[interlaced ? 1 : scanlines ? 2 : 0]);
		glUniform1i(un_textures[1], 1);

		glUniform1f(un_width, render_emulated_width());
		glUniform1f(un_height, last_height);

		glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
		glVertexAttribPointer(at_pos, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat[2]), (void *)0);
		glEnableVertexAttribArray(at_pos);

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
		glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, (void *)0);

		glDisableVertexAttribArray(at_pos);
		
		/*if (render_ui) {
			render_ui();
		}*/

		eglSwapBuffers(egl_display, main_surface);
	}
#endif
	if (!events_processed) {
		process_events();
	}
	events_processed = 0;
}

uint32_t render_emulated_width()
{
	return last_width - overscan_left[video_standard] - overscan_right[video_standard];
}

uint32_t render_emulated_height()
{
	return (video_standard == VID_NTSC ? 243 : 294) - overscan_top[video_standard] - overscan_bot[video_standard];
}

uint32_t render_overscan_left()
{
	return overscan_left[video_standard];
}

uint32_t render_overscan_top()
{
	return overscan_top[video_standard];
}

void render_wait_quit(vdp_context * context)
{
	for(;;)
	{
		drain_events();
		sleep(1);
	}
}

int render_lookup_button(char *name)
{
	static tern_node *button_lookup;
	if (!button_lookup) {
		//xbox/sdl style names
		button_lookup = tern_insert_int(button_lookup, "a", BTN_SOUTH);
		button_lookup = tern_insert_int(button_lookup, "b", BTN_EAST);
		button_lookup = tern_insert_int(button_lookup, "x", BTN_WEST);
		button_lookup = tern_insert_int(button_lookup, "y", BTN_NORTH);
		button_lookup = tern_insert_int(button_lookup, "back", BTN_SELECT);
		button_lookup = tern_insert_int(button_lookup, "start", BTN_START);
		button_lookup = tern_insert_int(button_lookup, "guid", BTN_MODE);
		button_lookup = tern_insert_int(button_lookup, "leftshoulder", BTN_TL);
		button_lookup = tern_insert_int(button_lookup, "rightshoulder", BTN_TR);
		button_lookup = tern_insert_int(button_lookup, "leftstick", BTN_THUMBL);
		button_lookup = tern_insert_int(button_lookup, "rightstick", BTN_THUMBR);
		//alternative Playstation-style names
		button_lookup = tern_insert_int(button_lookup, "cross", BTN_SOUTH);
		button_lookup = tern_insert_int(button_lookup, "circle", BTN_EAST);
		button_lookup = tern_insert_int(button_lookup, "square", BTN_WEST);
		button_lookup = tern_insert_int(button_lookup, "triangle", BTN_NORTH);
		button_lookup = tern_insert_int(button_lookup, "share", BTN_SELECT);
		button_lookup = tern_insert_int(button_lookup, "select", BTN_SELECT);
		button_lookup = tern_insert_int(button_lookup, "options", BTN_START);
		button_lookup = tern_insert_int(button_lookup, "l1", BTN_TL);
		button_lookup = tern_insert_int(button_lookup, "r1", BTN_TR);
		button_lookup = tern_insert_int(button_lookup, "l3", BTN_THUMBL);
		button_lookup = tern_insert_int(button_lookup, "r3", BTN_THUMBR);
	}
	return (int)tern_find_int(button_lookup, name, KEY_CNT);
}

int render_lookup_axis(char *name)
{
	static tern_node *axis_lookup;
	if (!axis_lookup) {
		//xbox/sdl style names
		axis_lookup = tern_insert_int(axis_lookup, "leftx", ABS_X);
		axis_lookup = tern_insert_int(axis_lookup, "lefty", ABS_Y);
		axis_lookup = tern_insert_int(axis_lookup, "lefttrigger", ABS_Z);
		axis_lookup = tern_insert_int(axis_lookup, "rightx", ABS_RX);
		axis_lookup = tern_insert_int(axis_lookup, "righty", ABS_RY);
		axis_lookup = tern_insert_int(axis_lookup, "righttrigger", ABS_RZ);
		//alternative Playstation-style names
		axis_lookup = tern_insert_int(axis_lookup, "l2", ABS_Z);
		axis_lookup = tern_insert_int(axis_lookup, "r2", ABS_RZ);
	}
	return (int)tern_find_int(axis_lookup, name, ABS_CNT);
}

int32_t render_translate_input_name(int32_t controller, char *name, uint8_t is_axis)
{
	tern_node *button_lookup, *axis_lookup;
	if (is_axis) {
		int axis = render_lookup_axis(name);
		if (axis == ABS_CNT) {
			return RENDER_INVALID_NAME;
		}
		return RENDER_AXIS_BIT | axis;
	} else {
		int button = render_lookup_button(name);
		if (button != KEY_CNT) {
			return button;
		}
		if (!strcmp("dpup", name)) {
			return RENDER_DPAD_BIT | 1;
		}
		if (!strcmp("dpdown", name)) {
			return RENDER_DPAD_BIT | 4;
		}
		if (!strcmp("dpdleft", name)) {
			return RENDER_DPAD_BIT | 8;
		}
		if (!strcmp("dpright", name)) {
			return RENDER_DPAD_BIT | 2;
		}
		return RENDER_INVALID_NAME;
	}
}

int32_t render_dpad_part(int32_t input)
{
	return input >> 4 & 0xFFFFFF;
}

uint8_t render_direction_part(int32_t input)
{
	return input & 0xF;
}

int32_t render_axis_part(int32_t input)
{
	return input & 0xFFFFFFF;
}

void process_events()
{
	if (events_processed > MAX_EVENT_POLL_PER_FRAME) {
		return;
	}
	drain_events();
	events_processed++;
}

#define TOGGLE_MIN_DELAY 250
void render_toggle_fullscreen()
{
	//always fullscreen in fbdev
}

uint32_t render_audio_buffer()
{
	return buffer_samples;
}

uint32_t render_sample_rate()
{
	return sample_rate;
}

void render_errorbox(char *title, char *message)
{
	
}

void render_warnbox(char *title, char *message)
{
	
}

void render_infobox(char *title, char *message)
{
	
}

uint8_t render_has_gl(void)
{
	return render_gl;
}

uint8_t render_get_active_framebuffer(void)
{
	return FRAMEBUFFER_ODD;
}
