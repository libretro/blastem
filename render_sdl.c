/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "render.h"
#include "blastem.h"
#include "io.h"
#include "util.h"

#include <GL/glew.h>

SDL_Window *main_window;
SDL_GLContext *main_context;
uint8_t render_dbg = 0;
uint8_t debug_pal = 0;

uint32_t last_frame = 0;

uint32_t min_delay;
uint32_t frame_delay = 1000/60;

int16_t * current_psg = NULL;
int16_t * current_ym = NULL;

uint32_t buffer_samples, sample_rate;
uint32_t missing_count;

SDL_mutex * audio_mutex;
SDL_cond * audio_ready;
SDL_cond * psg_cond;
SDL_cond * ym_cond;
uint8_t quitting = 0;

void audio_callback(void * userdata, uint8_t *byte_stream, int len)
{
	//puts("audio_callback");
	int16_t * stream = (int16_t *)byte_stream;
	int samples = len/(sizeof(int16_t)*2);
	int16_t * psg_buf, * ym_buf;
	uint8_t local_quit;
	SDL_LockMutex(audio_mutex);
		psg_buf = NULL;
		ym_buf = NULL;
		do {
			if (!psg_buf) {
				psg_buf = current_psg;
				current_psg = NULL;
				SDL_CondSignal(psg_cond);
			}
			if (!ym_buf) {
				ym_buf = current_ym;
				current_ym = NULL;
				SDL_CondSignal(ym_cond);
			}
			if (!quitting && (!psg_buf || !ym_buf)) {
				SDL_CondWait(audio_ready, audio_mutex);
			}
		} while(!quitting && (!psg_buf || !ym_buf));

		local_quit = quitting;
	SDL_UnlockMutex(audio_mutex);
	if (!local_quit) {
		for (int i = 0; i < samples; i++) {
			*(stream++) = psg_buf[i] + *(ym_buf++);
			*(stream++) = psg_buf[i] + *(ym_buf++);
		}
	}
}

void render_close_audio()
{
	SDL_LockMutex(audio_mutex);
		quitting = 1;
		SDL_CondSignal(audio_ready);
	SDL_UnlockMutex(audio_mutex);
	SDL_CloseAudio();
}

SDL_Joystick * joysticks[MAX_JOYSTICKS];
int num_joysticks;

int render_num_joysticks()
{
	return num_joysticks;
}

uint32_t render_map_color(uint8_t r, uint8_t g, uint8_t b)
{
	return 255 << 24 | r << 16 | g << 8 | b;
}

GLuint textures[3], buffers[2], vshader, fshader, program, un_textures[2], un_width, at_pos;

GLfloat vertex_data[] = {
	-1.0f, -1.0f,
	 1.0f, -1.0f,
	-1.0f,  1.0f,
	 1.0f,  1.0f
};

const GLushort element_data[] = {0, 1, 2, 3};

GLuint load_shader(char * fname, GLenum shader_type)
{
	char * parts[] = {get_home_dir(), "/.config/blastem/shaders/", fname};
	char * shader_path = alloc_concat_m(3, parts);
	FILE * f = fopen(shader_path, "r");
	free(shader_path);
	if (!f) {
		parts[0] = get_exe_dir();
		parts[1] = "/shaders/";
		shader_path = alloc_concat_m(3, parts);
		f = fopen(shader_path, "r");
		free(shader_path);
		if (!f) {
			fprintf(stderr, "Failed to open shader file %s for reading\n", fname);
			return 0;
		}
	}
	long fsize = file_size(f);
	GLchar * text = malloc(fsize);
	if (fread(text, 1, fsize, f) != fsize) {
		fprintf(stderr, "Error reading from shader file %s\n", fname);
		free(text);
		return 0;
	}
	GLuint ret = glCreateShader(shader_type);
	glShaderSource(ret, 1, (const GLchar **)&text, (const GLint *)&fsize);
	free(text);
	glCompileShader(ret);
	GLint compile_status, loglen;
	glGetShaderiv(ret, GL_COMPILE_STATUS, &compile_status);
	if (!compile_status) {
		fprintf(stderr, "Shader %s failed to compile\n", fname);
		glGetShaderiv(ret, GL_INFO_LOG_LENGTH, &loglen);
		text = malloc(loglen);
		glGetShaderInfoLog(ret, loglen, NULL, text);
		fputs(text, stderr);
		free(text);
		glDeleteShader(ret);
		return 0;
	}
	return ret;
}

void render_alloc_surfaces(vdp_context * context)
{
	context->oddbuf = context->framebuf = malloc(512 * 256 * 4 * 2);
	memset(context->oddbuf, 0, 512 * 256 * 4 * 2);
	context->evenbuf = ((char *)context->oddbuf) + 512 * 256 * 4;
	glGenTextures(3, textures);
	for (int i = 0; i < 3; i++)
	{
		glBindTexture(GL_TEXTURE_2D, textures[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		if (i < 2) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 256, 0, GL_BGRA, GL_UNSIGNED_BYTE, i ? context->evenbuf : context->oddbuf);
		} else {
			uint32_t blank = 255 << 24;
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &blank);
		}
	}
	glGenBuffers(2, buffers);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(element_data), element_data, GL_STATIC_DRAW);
	vshader = load_shader(tern_find_ptr_default(config, "videovertex_shader", "default.v.glsl"), GL_VERTEX_SHADER);
	fshader = load_shader(tern_find_ptr_default(config, "videofragment_shader", "default.f.glsl"), GL_FRAGMENT_SHADER);
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
	at_pos = glGetAttribLocation(program, "pos");
}

char * caption = NULL;

void render_init(int width, int height, char * title, uint32_t fps, uint8_t fullscreen)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
		fprintf(stderr, "Unable to init SDL: %s\n", SDL_GetError());
		exit(1);
	}
	printf("width: %d, height: %d\n", width, height);
	uint32_t flags = SDL_WINDOW_OPENGL;


	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 5);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	if (fullscreen) {
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
		SDL_DisplayMode mode;
		//TODO: Multiple monitor support
		SDL_GetCurrentDisplayMode(0, &mode);
		//the SDL2 migration guide suggests setting width and height to 0 when using SDL_WINDOW_FULLSCREEN_DESKTOP
		//but that doesn't seem to work right when using OpenGL, at least on Linux anyway
		width = mode.w;
		height = mode.h;
	}
	main_window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, flags);
	if (!main_window) {
		fprintf(stderr, "Unable to create SDL window: %s\n", SDL_GetError());
		SDL_Quit();
		exit(1);
	}
	SDL_GetWindowSize(main_window, &width, &height);
	printf("Window created with size: %d x %d\n", width, height);
	main_context = SDL_GL_CreateContext(main_window);
	GLenum res = glewInit();
	if (res != GLEW_OK) {
		fprintf(stderr, "Initialization of GLEW failed with code %d\n", res);
		SDL_Quit();
		exit(1);
	}
	if (!GLEW_VERSION_2_0) {
		fputs("BlastEm requires at least OpenGL 2.0, but it is unavailable\n", stderr);
		SDL_Quit();
		exit(1);
	}
	float aspect = (float)width / height;
	if (fabs(aspect - 4.0/3.0) > 0.01 && strcmp(tern_find_ptr_default(config, "videoaspect", "normal"), "stretch")) {
		for (int i = 0; i < 4; i++)
		{
			if (aspect > 4.0/3.0) {
				vertex_data[i*2] *= (4.0/3.0)/aspect;
			} else {
				vertex_data[i*2+1] *= aspect/(4.0/3.0);
			}
		}
	}
	caption = title;
	min_delay = 0;
	for (int i = 0; i < 100; i++) {
		uint32_t start = SDL_GetTicks();
		SDL_Delay(1);
		uint32_t delay = SDL_GetTicks()-start;
		if (delay > min_delay) {
			min_delay = delay;
		}
	}
	if (!min_delay) {
		min_delay = 1;
	}
	printf("minimum delay: %d\n", min_delay);

	frame_delay = 1000/fps;

	audio_mutex = SDL_CreateMutex();
	psg_cond = SDL_CreateCond();
	ym_cond = SDL_CreateCond();
	audio_ready = SDL_CreateCond();

	SDL_AudioSpec desired, actual;
    char * rate_str = tern_find_ptr(config, "audiorate");
   	int rate = rate_str ? atoi(rate_str) : 0;
   	if (!rate) {
   		rate = 48000;
   	}
    desired.freq = rate;
	desired.format = AUDIO_S16SYS;
	desired.channels = 2;
    char * samples_str = tern_find_ptr(config, "audiobuffer");
   	int samples = samples_str ? atoi(samples_str) : 0;
   	if (!samples) {
   		samples = 512;
   	}
    printf("config says: %d\n", samples);
    desired.samples = samples*2;
	desired.callback = audio_callback;
	desired.userdata = NULL;

	if (SDL_OpenAudio(&desired, &actual) < 0) {
		fprintf(stderr, "Unable to open SDL audio: %s\n", SDL_GetError());
		SDL_Quit();
		exit(1);
	}
	buffer_samples = actual.samples;
	sample_rate = actual.freq;
	printf("Initialized audio at frequency %d with a %d sample buffer\n", actual.freq, actual.samples);
	SDL_PauseAudio(0);
	num_joysticks = SDL_NumJoysticks();
	if (num_joysticks > MAX_JOYSTICKS) {
		num_joysticks = MAX_JOYSTICKS;
	}
	for (int i = 0; i < num_joysticks; i++) {
		SDL_Joystick * joy = joysticks[i] = SDL_JoystickOpen(i);
		printf("Joystick %d: %s\n", i, SDL_JoystickName(joy));
		if (joy) {
			printf("\tNum Axes: %d\n\tNum Buttons: %d\n\tNum Hats: %d\n", SDL_JoystickNumAxes(joy), SDL_JoystickNumButtons(joy), SDL_JoystickNumHats(joy));
		}
	}
	SDL_JoystickEventState(SDL_ENABLE);
	
	atexit(SDL_Quit);
	atexit(render_close_audio);
}

void render_context(vdp_context * context)
{
	last_frame = SDL_GetTicks();
	
	glBindTexture(GL_TEXTURE_2D, textures[context->framebuf == context->oddbuf ? 0 : 1]);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 320, 240, GL_BGRA, GL_UNSIGNED_BYTE, context->framebuf);;

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, textures[0]);
	glUniform1i(un_textures[0], 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, (context->regs[REG_MODE_4] & BIT_INTERLACE) ? textures[1] : textures[2]);
	glUniform1i(un_textures[1], 1);

	glUniform1f(un_width, context->regs[REG_MODE_4] & BIT_H40 ? 320.0f : 256.0f);

	glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
	glVertexAttribPointer(at_pos, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat[2]), (void *)0);
	glEnableVertexAttribArray(at_pos);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
	glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, (void *)0);

	glDisableVertexAttribArray(at_pos);

	SDL_GL_SwapWindow(main_window);
	if (context->regs[REG_MODE_4] & BIT_INTERLACE)
	{
		context->framebuf = context->framebuf == context->oddbuf ? context->evenbuf : context->oddbuf;
	}
}

int render_joystick_num_buttons(int joystick)
{
	if (joystick >= num_joysticks) {
		return 0;
	}
	return SDL_JoystickNumButtons(joysticks[joystick]);
}

int render_joystick_num_hats(int joystick)
{
	if (joystick >= num_joysticks) {
		return 0;
	}
	return SDL_JoystickNumHats(joysticks[joystick]);
}

void render_wait_quit(vdp_context * context)
{
	SDL_Event event;
	while(SDL_WaitEvent(&event)) {
		switch (event.type) {
		case SDL_KEYDOWN:
			if (event.key.keysym.sym == SDLK_LEFTBRACKET) {
				render_dbg++;
				if (render_dbg == 4) {
					render_dbg = 0;
				}
				render_context(context);
			} else if(event.key.keysym.sym ==  SDLK_RIGHTBRACKET) {
				debug_pal++;
				if (debug_pal == 4) {
					debug_pal = 0;
				}
			}
			break;
		case SDL_QUIT:
			return;
		}
	}
}

void render_debug_mode(uint8_t mode)
{
	if (mode < 4) {
		render_dbg = mode;
	}
}

void render_debug_pal(uint8_t pal)
{
	if (pal < 4) {
		debug_pal = pal;
	}
}

int32_t handle_event(SDL_Event *event)
{
	switch (event->type) {
	case SDL_KEYDOWN:
		handle_keydown(event->key.keysym.sym);
		break;
	case SDL_KEYUP:
		handle_keyup(event->key.keysym.sym);
		break;
	case SDL_JOYBUTTONDOWN:
		handle_joydown(event->jbutton.which, event->jbutton.button);
		break;
	case SDL_JOYBUTTONUP:
		handle_joyup(event->jbutton.which, event->jbutton.button);
		break;
	case SDL_JOYHATMOTION:
		handle_joy_dpad(event->jbutton.which, event->jhat.hat, event->jhat.value);
		break;
	case SDL_QUIT:
		puts("");
		exit(0);
	}
	return 0;
}

char * fps_caption = NULL;

uint32_t frame_counter = 0;
uint32_t start = 0;
int wait_render_frame(vdp_context * context, int frame_limit)
{
	SDL_Event event;
	int ret = 0;
	while(SDL_PollEvent(&event)) {
		ret = handle_event(&event);
	}
	if (frame_limit) {
		//TODO: Adjust frame delay so we actually get 60 FPS rather than 62.5 FPS
		uint32_t current = SDL_GetTicks();
		uint32_t desired = last_frame + frame_delay;
		if (current < desired) {
			uint32_t delay = last_frame + frame_delay - current;
			if (delay > min_delay) {
				SDL_Delay((delay/min_delay)*min_delay);
			}
			while ((desired) >= SDL_GetTicks()) {
			}
		}
	}
	render_context(context);

	frame_counter++;
	if ((last_frame - start) > 1000) {
		if (start && (last_frame-start)) {
			if (!fps_caption) {
				fps_caption = malloc(strlen(caption) + strlen(" - 1000.1 fps") + 1);
			}
			sprintf(fps_caption, "%s - %.1f fps", caption, ((float)frame_counter) / (((float)(last_frame-start)) / 1000.0));
			SDL_SetWindowTitle(main_window, fps_caption);
		}
		start = last_frame;
		frame_counter = 0;
	}
	return ret;
}

void process_events()
{
	SDL_Event event;
	while(SDL_PollEvent(&event)) {
		handle_event(&event);
	}
}

void render_wait_psg(psg_context * context)
{
	SDL_LockMutex(audio_mutex);
		while (current_psg != NULL) {
			SDL_CondWait(psg_cond, audio_mutex);
		}
		current_psg = context->audio_buffer;
		SDL_CondSignal(audio_ready);

		context->audio_buffer = context->back_buffer;
		context->back_buffer = current_psg;
	SDL_UnlockMutex(audio_mutex);
	context->buffer_pos = 0;
}

void render_wait_ym(ym2612_context * context)
{
	SDL_LockMutex(audio_mutex);
		while (current_ym != NULL) {
			SDL_CondWait(ym_cond, audio_mutex);
		}
		current_ym = context->audio_buffer;
		SDL_CondSignal(audio_ready);

		context->audio_buffer = context->back_buffer;
		context->back_buffer = current_ym;
	SDL_UnlockMutex(audio_mutex);
	context->buffer_pos = 0;
}

void render_fps(uint32_t fps)
{
	frame_delay = 1000/fps;
}

uint32_t render_audio_buffer()
{
	return buffer_samples;
}

uint32_t render_sample_rate()
{
	return sample_rate;
}


