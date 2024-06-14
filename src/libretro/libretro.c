#ifdef WANT_LIBCO
#include <libco.h>
#else
#include <rthreads/rthreads.h>
#endif

#if (HAS_DRZ80 || HAS_CYCLONE)
#include "frontend_list.h"
#endif

#include <stdarg.h>
#include <sys/time.h>
#include <libretro.h>
#include "mame.h"
#include "cpuintrf.h"
#include "osdepend.h"
#include "driver.h"
#include "allegro.h"
#include <file/file_path.h>

#ifndef RETROK_TILDE
#define RETROK_TILDE 178
#endif

#ifdef _WIN32
char slash = '\\';
#else
char slash = '/';
#endif

char *IMAMEBASEPATH = NULL;
char *IMAMESAMPLEPATH = NULL;

const char *retro_save_directory;
const char *retro_system_directory;
const char *retro_content_directory;
char core_save_directory[1024];
char core_sys_directory[1024];

unsigned retro_hook_quit;
volatile static unsigned audio_done;
volatile static unsigned video_done;
volatile static unsigned mame_sleep;
#ifdef WANT_LIBCO
int libco_quit=0;
static cothread_t core_thread;
static cothread_t main_thread;
#else
static sthread_t *run_thread = NULL;
static scond_t   *libretro_cond = NULL;
static slock_t   *libretro_mutex = NULL;
#endif

unsigned frameskip_type                  = 0;
unsigned frameskip_threshold             = 0;
unsigned frameskip_counter               = 0;
unsigned frameskip_interval              = 0;

int retro_audio_buff_active              = false;
unsigned retro_audio_buff_occupancy      = 0;
int retro_audio_buff_underrun            = false;

unsigned retro_audio_latency             = 0;
int update_audio_latency                 = false;

int should_skip_frame                    = 0;

static int sample_rate                   = 22050;
static int stereo_enabled                = true;
static int audio_enabled                 = true;

int game_index = -1;
unsigned short *gp2x_screen15;
int thread_done = 0;
extern int gfx_xoffset;
extern int gfx_yoffset;
extern int gfx_width;
extern int gfx_height;
extern int usestereo;
extern int samples_per_frame;
extern short *samples_buffer;
extern short *conversion_buffer;
extern int joy_pressed[40];
extern int key[KEY_MAX];
extern char *nvdir, *hidir, *cfgdir, *inpdir, *stadir, *memcarddir;
extern char *artworkdir, *screenshotdir, *alternate_name;
extern char *cheatdir;

void decompose_rom_sample_path(char *rompath, char *samplepath);
void init_joy_list(void);

extern UINT32 create_path_recursive(char *path);

#if defined(SF2000)
static retro_log_printf_t log_cb;
#endif

#if defined(_3DS)
void* linearMemAlign(size_t size, size_t alignment);
void linearFree(void* mem);
#endif

void CLIB_DECL logerror(const char *text,...)
{
#ifdef DISABLE_ERROR_LOGGING
#if defined(SF2000)
   //log_cb(RETRO_LOG_DEBUG, text);
#else
   va_list arg;
   va_start(arg,text);
   vprintf(text,arg);
   va_end(arg);
#endif
#endif
}

int global_showinfo = 1;
int emulated_width;
int emulated_height;
int safe_render_path = 1;
unsigned short gp2x_palette[512];
int gp2x_pal_50hz=0;
int global_fps = 1;
int rotate_controls = 0;
int num_of_joys = 2;
int soundcard;
int attenuation = 0;

void gp2x_printf(char* fmt, ...)
{
#if defined(SF2000)
	char buffer[500];

	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	if (log_cb)
		log_cb(RETRO_LOG_INFO, buffer);
#else
    va_list marker;
    va_start(marker, fmt);
    vprintf(fmt, marker);
    va_end(marker);
#endif
}

void gp2x_set_video_mode(int bpp,int width,int height)
{
   (void)bpp;
   (void)width;
   (void)height;
}

void gp2x_video_setpalette(void)
{
}

unsigned long gp2x_joystick_read(int n)
{
   (void)n;
#if defined(SF2000)
   return 0;
#endif
}

int osd_init(void)
{
   return 0;
}

void osd_exit(void)
{
}

int screen_reinit(void)
{
   return 1;
}

static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static bool libretro_supports_bitmasks = false;

unsigned skip_disclaimer = 0;

static void retro_audio_buff_status_cb(
      bool active, unsigned occupancy, bool underrun_likely)
{
   retro_audio_buff_active    = active;
   retro_audio_buff_occupancy = occupancy;
   retro_audio_buff_underrun  = underrun_likely;
}

static void retro_set_audio_buff_status_cb(void)
{
   if (frameskip_type > 0)
   {
      struct retro_audio_buffer_status_callback buf_status_cb;

      buf_status_cb.callback = retro_audio_buff_status_cb;
      if (!environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK,
            &buf_status_cb))
      {
         retro_audio_buff_active    = false;
         retro_audio_buff_occupancy = 0;
         retro_audio_buff_underrun  = false;
         retro_audio_latency        = 0;
      }
      else
      {
         /* Frameskip is enabled - increase frontend
          * audio latency to minimise potential
          * buffer underruns */
         uint32_t frame_time_usec = 1000000.0 / Machine->drv->frames_per_second;

         /* Set latency to 6x current frame time... */
         retro_audio_latency = (unsigned)(6 * frame_time_usec / 1000);

         /* ...then round up to nearest multiple of 32 */
         retro_audio_latency = (retro_audio_latency + 0x1F) & ~0x1F;
      }
   }
   else
   {
      environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
      retro_audio_latency = 0;
   }

   update_audio_latency = true;
}

static void update_variables(bool first_run)
{
    struct retro_variable var;
    bool prev_frameskip_type;

    var.key = "mame2000-frameskip";
    var.value = NULL;

    prev_frameskip_type = frameskip_type;
    frameskip_type      = 0;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
       if (strcmp(var.value, "auto") == 0)
          frameskip_type = 1;
       if (strcmp(var.value, "threshold") == 0)
          frameskip_type = 2;
    }

    var.key = "mame2000-frameskip_threshold";
    var.value = NULL;

    frameskip_threshold = 30;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
       frameskip_threshold = strtol(var.value, NULL, 10);

    var.key = "mame2000-frameskip_interval";
    var.value = NULL;

    frameskip_interval = 1;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
       frameskip_interval = strtol(var.value, NULL, 10);

    var.value = NULL;
    var.key = "mame2000-skip_disclaimer";
    
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if(strcmp(var.value, "enabled") == 0)
            skip_disclaimer = 1;
        else
            skip_disclaimer = 0;
    }
    else
        skip_disclaimer = 0;
    
    var.value = NULL;
    var.key = "mame2000-show_gameinfo";
    
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if(strcmp(var.value, "enabled") == 0)
            global_showinfo = 1;
        else
            global_showinfo = 0;
    }
    else
        global_showinfo = 0;

    var.value = NULL;
    var.key = "mame2000-sample_rate";

    sample_rate = 22050;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
       sample_rate = strtol(var.value, NULL, 10);

    var.value = NULL;
    var.key = "mame2000-stereo";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if(strcmp(var.value, "enabled") == 0)
            stereo_enabled = true;
        else
            stereo_enabled = false;
    }
    else
        stereo_enabled = true;

	var.value = NULL;
    var.key = "mame2000-audio";
    
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if(strcmp(var.value, "enabled") == 0)
            audio_enabled = true;
        else
            audio_enabled = false;
    }
    else
        audio_enabled = true;
	
   /* Reinitialise frameskipping, if required */
   if (!first_run &&
       ((frameskip_type     != prev_frameskip_type)))
      retro_set_audio_buff_status_cb();
}

void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_variable vars[] = {
      { "mame2000-frameskip", "Frameskip ; disabled|auto|threshold" },
      { "mame2000-frameskip_threshold", "Frameskip Threshold (%); 30|40|50|60" },
      { "mame2000-frameskip_interval", "Frameskip Interval; 1|2|3|4|5|6|7|8|9" },
      { "mame2000-skip_disclaimer", "Skip Disclaimer; enabled|disabled" },
      { "mame2000-show_gameinfo", "Show Game Information; disabled|enabled" },
      { "mame2000-sample_rate", "Audio Rate (Restart); 22050|11025|22050|32000|44100" },
      { "mame2000-stereo", "Stereo (Restart); enabled|disabled" },
	  { "mame2000-audio", "Sound (Restart); enabled|disabled" },
      { NULL, NULL },
   };
   environ_cb = cb;
    
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
   
#if defined(SF2000)
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;
#endif
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_reset(void)
{
   machine_reset();
}

#if defined(SF2000)
void update_geometry()
{
	const bool rotated = ((Machine->orientation == ROT90) || (Machine->orientation == ROT270));
	struct retro_system_av_info system_av_info;
	system_av_info.geometry.base_width   = emulated_width;
	system_av_info.geometry.base_height  = emulated_height;
	system_av_info.geometry.aspect_ratio = (rotated) ? ( (float) 3 / (float) 4) : ( (float) 4/ (float) 3);
	environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &system_av_info);
}
#endif
static void update_input(void)
{
#if !defined(SF2000)
#define RK(port,key)     input_state_cb(port, RETRO_DEVICE_KEYBOARD, 0,RETROK_##key)
#define JS(port, button) joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_##button)
#else
#define RK(port,key)     (input_state_cb(port, RETRO_DEVICE_KEYBOARD, 0,RETROK_##key))
#define JS(port, button) (joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_##button))
#endif
	int i, j, c = 0;
	input_poll_cb();
	
	key[KEY_TAB] = 0;
	key[KEY_TILDE] = 0;
	key[KEY_P] = 0;
	key[KEY_F3] = 0;
	key[KEY_TAB] = 0;
	key[KEY_ESC] = 0;
	
	for (i = 0; i < 4; i++)
	{
		key[KEY_1 + i] = 0;
		key[KEY_5 + i] = 0;
	}
	
	for (i = 0; i < 4; i++)
	{
		int16_t joypad_bits;
		
		if (libretro_supports_bitmasks)
			joypad_bits = input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
		else
		{
			joypad_bits = 0;
			for (j = 0; j < (RETRO_DEVICE_ID_JOYPAD_R3+1); j++)
				joypad_bits |= input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, j) ? (1 << j) : 0;
		}

		key[KEY_1 + i]   |= JS(i, START);
		key[KEY_5 + i]   |= JS(i, SELECT);
		joy_pressed[c++] = JS(i, LEFT);
		joy_pressed[c++] = JS(i, RIGHT);
		joy_pressed[c++] = JS(i, UP);
		joy_pressed[c++] = JS(i, DOWN);
		joy_pressed[c++] = JS(i, B);
		joy_pressed[c++] = JS(i, A);
		joy_pressed[c++] = JS(i, Y);
		joy_pressed[c++] = JS(i, X);
		joy_pressed[c++] = JS(i, L);
		joy_pressed[c++] = JS(i, R);

#if defined(SF2000)
		key[KEY_TAB] |= (JS(i, L) > 0) && (JS(i, START) > 0);	// config menu: L + START
		key[KEY_TILDE] |= (JS(i, R) > 0) && (JS(i, START) > 0);	//    OSD menu: R + START
		key[KEY_P] |= (JS(i, R) > 0) && (JS(i, L) > 0);			//       pause: R + L
		key[KEY_F3] |= (JS(i, R) > 0) && (JS(i, SELECT) > 0);	//  reset game: R + SELECT
		key[KEY_ESC]  |= (JS(i, A) > 0);						// menu cancel: A
#else
		key[KEY_TAB] |= JS(i, R2);
#endif
	}

	key[KEY_A] =RK(0, a);
	key[KEY_B] =RK(0, b);
	key[KEY_C] =RK(0, c);
	key[KEY_D] =RK(0, d);
	key[KEY_E] =RK(0, e);
	key[KEY_F] =RK(0, f);
	key[KEY_G] =RK(0, g);
	key[KEY_H] =RK(0, h);
	key[KEY_I] =RK(0, i);
	key[KEY_J] =RK(0, j);
	key[KEY_K] =RK(0, k);
	key[KEY_L] =RK(0, l);
	key[KEY_M] =RK(0, m);
	key[KEY_N] =RK(0, n);
	key[KEY_O] =RK(0, o);
	key[KEY_P] |=RK(0, p);
	key[KEY_Q] =RK(0, q);
	key[KEY_R] =RK(0, r);
	key[KEY_S] =RK(0, s);
	key[KEY_T] =RK(0, t);
	key[KEY_U] =RK(0, u);
	key[KEY_V] =RK(0, v);
	key[KEY_W] =RK(0, w);
	key[KEY_X] =RK(0, x);
	key[KEY_Y] =RK(0, y);
	key[KEY_Z] =RK(0, z);
	key[KEY_0] =RK(0, 0);
	key[KEY_1] |=RK(0, 1);
	key[KEY_2] |=RK(0, 2);
	key[KEY_3] |=RK(0, 3);
	key[KEY_4] |=RK(0, 4);
	key[KEY_5] |=RK(0, 5);
	key[KEY_6] |=RK(0, 6);
	key[KEY_7] |=RK(0, 7);
	key[KEY_8] |=RK(0, 8);
	key[KEY_9] =RK(0, 9);
	key[KEY_0_PAD] =RK(0, KP0);
	key[KEY_1_PAD] =RK(0, KP1);
	key[KEY_2_PAD] =RK(0, KP2);
	key[KEY_3_PAD] =RK(0, KP3);
	key[KEY_4_PAD] =RK(0, KP4);
	key[KEY_5_PAD] =RK(0, KP5);
	key[KEY_6_PAD] =RK(0, KP6);
	key[KEY_7_PAD] =RK(0, KP7);
	key[KEY_8_PAD] =RK(0, KP8);
	key[KEY_9_PAD] =RK(0, KP9);
	key[KEY_F1] =RK(0, F1);
	key[KEY_F2] =RK(0, F2);
	key[KEY_F3] |=RK(0, F3);
	key[KEY_F4] =RK(0, F4);
	key[KEY_F5] =RK(0, F5);
	key[KEY_F6] =RK(0, F6);
	key[KEY_F7] =RK(0, F7);
	key[KEY_F8] =RK(0, F8);
	key[KEY_F9] =RK(0, F9);
	key[KEY_F10] =RK(0, F10);
	key[KEY_F11] =RK(0, F11);
	key[KEY_F12] =RK(0, F12);
	key[KEY_ESC] |=RK(0, ESCAPE);
	key[KEY_TILDE] |=RK(0, TILDE);
	key[KEY_MINUS] =RK(0, MINUS);
	key[KEY_EQUALS] =RK(0, EQUALS);
	key[KEY_BACKSPACE] =RK(0, BACKSPACE);
	key[KEY_TAB] |=RK(0, TAB);
	key[KEY_OPENBRACE] =RK(0, LEFTBRACKET);
	key[KEY_CLOSEBRACE] =RK(0, RIGHTBRACKET);
	key[KEY_ENTER] =RK(0, RETURN);
	key[KEY_COLON] =RK(0, COLON);
	key[KEY_QUOTE] =RK(0, QUOTE);
	key[KEY_BACKSLASH] =RK(0, BACKSLASH);
	key[KEY_BACKSLASH2] =RK(0, LESS);
	key[KEY_COMMA] =RK(0, COMMA);
	key[KEY_STOP] =RK(0, PERIOD);
	key[KEY_SLASH] =RK(0, SLASH);
	key[KEY_SPACE] =RK(0, SPACE);
	key[KEY_INSERT] =RK(0, INSERT);
	key[KEY_DEL] =RK(0, DELETE);
	key[KEY_HOME] =RK(0, HOME);
	key[KEY_END] =RK(0, END);
	key[KEY_PGUP] =RK(0, PAGEUP);
	key[KEY_PGDN] =RK(0, PAGEDOWN);
	key[KEY_LEFT] =RK(0, LEFT);
	key[KEY_RIGHT] =RK(0, RIGHT);
	key[KEY_UP] =RK(0, UP);
	key[KEY_DOWN] =RK(0, DOWN);
	key[KEY_SLASH_PAD] =RK(0, KP_DIVIDE);
	key[KEY_ASTERISK] =RK(0, ASTERISK);
	key[KEY_MINUS_PAD] =RK(0, KP_MINUS);
	key[KEY_PLUS_PAD] =RK(0, KP_PLUS);
	key[KEY_DEL_PAD] =RK(0, KP_PERIOD);
	key[KEY_ENTER_PAD] =RK(0, KP_ENTER);
	key[KEY_PRTSCR] =RK(0, PRINT);
	key[KEY_PAUSE] =RK(0, PAUSE);
	key[KEY_LSHIFT] =RK(0, LSHIFT);
	key[KEY_RSHIFT] =RK(0, RSHIFT);
	key[KEY_LCONTROL] =RK(0, LCTRL);
	key[KEY_RCONTROL] =RK(0, RCTRL);
	key[KEY_ALT] =RK(0, LALT);
	key[KEY_ALTGR] =RK(0, RALT);
	key[KEY_LWIN] =RK(0, LMETA);
	key[KEY_RWIN] =RK(0, RMETA);
	key[KEY_MENU] =RK(0, MENU);
	key[KEY_SCRLOCK] =RK(0, SCROLLOCK);
	key[KEY_NUMLOCK] =RK(0, NUMLOCK);
	key[KEY_CAPSLOCK] =RK(0, CAPSLOCK);

#undef RK
#undef JS
#undef _B
}

#ifdef WANT_LIBCO
void hook_audio_done(void)
{
}

void hook_video_done(void)
{
   co_switch(main_thread);
}

void run_thread_proc(void)
{
   run_game(game_index);
#if !defined(SF2000)
   hook_audio_done();
   hook_video_done();
#else
   // never exit a co-thread
   while (true)
      co_switch(main_thread);
#endif
}
#else
static void hook_check(void)
{
   if (video_done && audio_done)
   {
      scond_signal(libretro_cond);
      if (mame_sleep && !retro_hook_quit)
         scond_wait(libretro_cond, libretro_mutex);
      mame_sleep = 1;
   }
}

void hook_audio_done(void)
{
   slock_lock(libretro_mutex);
   audio_done = 1;
   hook_check();
   slock_unlock(libretro_mutex);
}

void hook_video_done(void)
{
   slock_lock(libretro_mutex);
   if (video_done) // Audio doesn't seem to be running atm, so fake it ...
      audio_done = 1;
   video_done = 1;
   hook_check();
   slock_unlock(libretro_mutex);
}

#if !defined(SF2000)
#ifdef WANT_LIBCO
void *run_thread_proc(void *v)
{
   (void)v;

   run_game(game_index);
   thread_done = 1;
   hook_audio_done();
   hook_video_done();

   return NULL;
}
#else
void run_thread_proc(void *v)
{
   run_game(game_index);
   thread_done = 1;
   hook_audio_done();
   hook_video_done();
}
#endif
#else
void run_thread_proc(void *v)
{
   run_game(game_index);
   thread_done = 1;
   hook_audio_done();
   hook_video_done();
}
#endif

static void lock_mame(void)
{
   slock_lock(libretro_mutex);
   while (!audio_done || !video_done)
      scond_wait(libretro_cond, libretro_mutex);
   slock_unlock(libretro_mutex);
}

static void unlock_mame(void)
{
   slock_lock(libretro_mutex);
   mame_sleep = 0;
   scond_signal(libretro_cond);
   slock_unlock(libretro_mutex);
}
#endif

void retro_init(void)
{
#ifdef _3DS
   gp2x_screen15 = (unsigned short *) linearMemAlign(640 * 480 * 2, 0x80);
#else
   gp2x_screen15 = (unsigned short *) malloc(640 * 480 * 2);
#endif
#ifndef WANT_LIBCO
   libretro_cond  = scond_new();
   libretro_mutex = slock_new();
#endif
   init_joy_list();
   update_variables(true);

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;
}

void retro_deinit(void)
{
   free(IMAMEBASEPATH);
   free(IMAMESAMPLEPATH);
#ifdef _3DS
   linearFree(gp2x_screen15);
#else
   free(gp2x_screen15);
#endif
#ifndef WANT_LIBCO
   scond_free(libretro_cond);
   slock_free(libretro_mutex);
#endif

   libretro_supports_bitmasks = false;
   frameskip_type             = 0;
   frameskip_threshold        = 0;
   frameskip_counter          = 0;
   retro_audio_buff_active    = false;
   retro_audio_buff_occupancy = 0;
   retro_audio_buff_underrun  = false;
   retro_audio_latency        = 0;
   update_audio_latency       = false;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_get_system_info(struct retro_system_info *info)
{
   info->library_name     = "MAME 2000";
#ifdef GIT_VERSION
   info->library_version  = "0.37b5" GIT_VERSION;
#else
   info->library_version  = build_version;
#endif
   info->need_fullpath    = true;
   info->valid_extensions = "zip|ZIP";
   info->block_extract    = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   float aspect_ratio = Machine->orientation & ORIENTATION_SWAP_XY? ( (float) 3 / (float) 4) : ( (float) 4/ (float) 3);
#ifndef WANT_LIBCO
   lock_mame();
#endif
   struct retro_game_geometry g = {
     emulated_width,
      emulated_height,
      emulated_width,
      emulated_height,
      aspect_ratio
   };
   struct retro_system_timing t = {
      Machine->drv->frames_per_second,
      (double)Machine->sample_rate
   };
   info->timing = t;
   info->geometry = g;
}

void retro_run(void)
{
   int i, j;
#ifdef WANT_LIBCO
   if(libco_quit==0)co_switch(core_thread);
   else  printf("running dead emulator");
#else
   lock_mame();

//   if (thread_done)
//      environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
#endif

   bool updated = false;
    
   update_input();

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables(false);

   if (should_skip_frame)
      video_cb(NULL, gfx_width, gfx_height, gfx_width * 2);
   else
      video_cb(gp2x_screen15, gfx_width, gfx_height, gfx_width * 2);

   if (audio_enabled)
   {
      if (samples_per_frame)
      {
         if (usestereo)
            audio_batch_cb(samples_buffer, samples_per_frame);
         else
         {
            for (i = 0, j = 0; i < samples_per_frame; i++)
            {
               conversion_buffer[j++] = samples_buffer[i];
               conversion_buffer[j++] = samples_buffer[i];
            }
            audio_batch_cb(conversion_buffer, samples_per_frame);
         }
      }
   }

#ifndef WANT_LIBCO
   audio_done = 0;
   video_done = 0;

   unlock_mame();
#endif

   /* If frameskip/timing settings have changed,
    * update frontend audio latency
    * > Can do this before or after the frameskip
    *   check, but doing it after means we at least
    *   retain the current frame's audio output */
   if (update_audio_latency)
   {
      environ_cb(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY,
                 &retro_audio_latency);
      update_audio_latency = false;
   }

}

bool retro_load_game(const struct retro_game_info *info)
{
   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Button 1" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Button 2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Button 4" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Button 3" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Button 5" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Button 6" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Coins" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "OSD Menu" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Button 1" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Button 2" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Button 4" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Button 3" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Button 5" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Button 6" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Coins" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "OSD Menu" },

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Button 1" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Button 2" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Button 4" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Button 3" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Button 5" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Button 6" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Coins" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "OSD Menu" },

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Button 1" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Button 2" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Button 4" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Button 3" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Button 5" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Button 6" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Coins" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "OSD Menu" },

      { 0, 0, 0, 0, NULL },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
#if !defined(SF2000)
      fprintf(stderr, "[libretro]: RGB565 is not supported.\n");
#else
      logerror("[libretro]: RGB565 is not supported.\n");
#endif
      return false;
   }

  retro_content_directory = strdup(info->path);
  path_basedir(retro_content_directory);

  printf("CONTENT_DIRECTORY: %s\n", retro_content_directory);

  /* Get system directory from frontend */
  retro_system_directory = NULL;
  environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY,&retro_system_directory);
  if (retro_system_directory == NULL || retro_system_directory[0] == '\0')
  {
      printf("libretro system path not set by frontend, using content path\n");
      retro_system_directory = retro_content_directory;
  }
   printf("SYSTEM_DIRECTORY: %s\n", retro_system_directory);


  /* Get save directory from frontend */
  retro_save_directory = NULL;
  environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY,&retro_save_directory);
  if (retro_save_directory == NULL || retro_save_directory[0] == '\0')
  {
      printf("libretro save path not set by frontent, using content path\n");
      retro_save_directory = retro_content_directory;
  }
   printf("SAVE_DIRECTORY: %s\n", retro_save_directory);

#if !defined(SF2000)
   sprintf(core_sys_directory,"%s%cmame2000\0",retro_system_directory,slash);
   sprintf(core_save_directory,"%s%cmame2000\0",retro_save_directory,slash);
#else
   sprintf(core_sys_directory,"%s%cmame2000",retro_system_directory,slash);
   sprintf(core_save_directory,"%s%cmame2000",retro_save_directory,slash);
#endif

   printf("MAME2000_SYS_DIRECTORY: %s\n", core_sys_directory);
   printf("MAME2000_SAVE_DIRECTORY: %s\n", core_save_directory);

   IMAMEBASEPATH = (char *) malloc(1024);
   IMAMESAMPLEPATH = (char *) malloc(1024);


   int i;
   memcpy(IMAMEBASEPATH, info->path, strlen(info->path) + 1);
   if (strrchr(IMAMEBASEPATH, slash)) *(strrchr(IMAMEBASEPATH, slash)) = 0;
   else { IMAMEBASEPATH[0] = '.'; IMAMEBASEPATH[1] = 0; }
   char baseName[1024];
   const char *romName = info->path;
   if (strrchr(info->path, slash)) romName = strrchr(info->path, slash) + 1;
   memcpy(baseName, romName, strlen(romName) + 1);
   if (strrchr(baseName, '.')) *(strrchr(baseName, '.')) = 0;

   strcpy(IMAMESAMPLEPATH, core_sys_directory);
   strcat(IMAMESAMPLEPATH, "/samples");

   /* do we have a driver for this? */
#if defined(SF2000)
   game_index = -1;
#endif
   for (i = 0; drivers[i] && (game_index == -1); i++)
   {
	   if (strcasecmp(baseName,drivers[i]->name) == 0)
	   {
		   game_index = i;
		   break;
	   }
   }

   if (game_index == -1)
   {
	   printf("Game \"%s\" not supported\n", baseName);
	   return false;
   }
#if defined(SF2000)
   printf("game_index=%d driver_name=%s\n", game_index, drivers[game_index]->name);
#endif

   /* parse generic (os-independent) options */
   //parse_cmdline (argc, argv, game_index);

   //Set default path
   nvdir=(char *) malloc(1024);sprintf(nvdir,"%s%c%s\0",core_save_directory,slash,"nvram");
   i=create_path_recursive(nvdir);
   if(i!=0)printf("error %d creating nvram \"%s\"\n", i,nvdir);

   hidir=(char *) malloc(1024);sprintf(hidir,"%s%c%s\0",core_save_directory,slash,"hi");
   i=create_path_recursive(hidir);
   if(i!=0)printf("error %d creating hi \"%s\"\n", i,hidir);

   cfgdir=(char *) malloc(1024);sprintf(cfgdir,"%s%c%s\0",core_save_directory,slash,"cfg");
   i=create_path_recursive(cfgdir);
   if(i!=0)printf("error %d creating cfg \"%s\"\n", i,cfgdir);

   screenshotdir=(char *) malloc(1024);sprintf(screenshotdir,"%s%c%s\0",core_save_directory,slash,"snap");
   i=create_path_recursive(screenshotdir);
   if(i!=0)printf("error %d creating snap \"%s\"\n", i,screenshotdir);

   memcarddir=(char *) malloc(1024);sprintf(memcarddir,"%s%c%s\0",core_save_directory,slash,"memcard");
   i=create_path_recursive(memcarddir);
   if(i!=0)printf("error %d creating memcard \"%s\"\n", i,memcarddir);

   stadir=(char *) malloc(1024);sprintf(stadir,"%s%c%s\0",core_sys_directory,slash,"sta");
   i=create_path_recursive(stadir);
   if(i!=0)printf("error %d creating sta \"%s\"\n", i,stadir);

   artworkdir=(char *) malloc(1024);sprintf(artworkdir,"%s%c%s\0",core_sys_directory,slash,"artwork");
   i=create_path_recursive(artworkdir);
   if(i!=0)printf("error %d creating artwork \"%s\"\n", i,artworkdir);

   cheatdir=(char *) malloc(1024);sprintf(cheatdir,"%s%c%s\0",core_sys_directory,slash,"cheat");
   i=create_path_recursive(cheatdir);
   if(i!=0)printf("error %d creating cheat \"%s\"\n", i,cheatdir);

   Machine->sample_rate = sample_rate;
   options.samplerate = sample_rate;
   usestereo = stereo_enabled;

#if defined(SF2000)
   printf("sample_rate=%d\n", sample_rate);
#endif

   /* This is needed so emulated YM3526/YM3812 chips are used instead on physical ones. */
   options.use_emulated_ym3812 = 1;

   /* enable samples - should be stored in "sample" subdirectory from roms */
   options.use_samples = 1;

   /* skip disclaimer - skips 'nag screen' */
   options.skip_disclaimer = skip_disclaimer;

#if (HAS_CYCLONE || HAS_DRZ80)
   int use_cyclone = 1;
   int use_drz80 = 1;
   int use_drz80_snd = 1;

	for (i=0;i<NUMGAMES;i++)
 	{
		if (strcmp(drivers[game_index]->name,fe_drivers[i].name)==0)
		{
			/* ASM cores: 0=None,1=Cyclone,2=DrZ80,3=Cyclone+DrZ80,4=DrZ80(snd),5=Cyclone+DrZ80(snd) */
         switch (fe_drivers[i].cores)
         {
         case 0:
            use_cyclone = 0;
				use_drz80_snd = 0;
				use_drz80 = 0;
            break;
         case 1:
				use_drz80_snd = 0;
				use_drz80 = 0;
            break;
         case 2:
            use_cyclone = 0;
            break;
         case 4:
            use_cyclone = 0;
				use_drz80 = 0;
            break;
         case 5:
				use_drz80 = 0;
            break;
         default:
            break;
         }
			
         break;
		}
	}

   /* Replace M68000 by CYCLONE */
#if (HAS_CYCLONE)
   if (use_cyclone)
   {
	   for (i=0;i<MAX_CPU;i++)
	   {
		   int *type=(int*)&(drivers[game_index]->drv->cpu[i].cpu_type);
#ifdef NEOMAME
		   if (((*type)&0xff)==CPU_M68000)
#else
			   if (((*type)&0xff)==CPU_M68000 || ((*type)&0xff)==CPU_M68010 )
#endif
			   {
				   *type=((*type)&(~0xff))|CPU_CYCLONE;
			   }
	   }
   }
#endif

#if (HAS_DRZ80)
	/* Replace Z80 by DRZ80 */
	if (use_drz80)
	{
		for (i=0;i<MAX_CPU;i++)
		{
			int *type=(int*)&(drivers[game_index]->drv->cpu[i].cpu_type);
			if (((*type)&0xff)==CPU_Z80)
			{
				*type=((*type)&(~0xff))|CPU_DRZ80;
			}
		}
	}

	/* Replace Z80 with DRZ80 only for sound CPUs */
	if (use_drz80_snd)
	{
		for (i=0;i<MAX_CPU;i++)
		{
			int *type=(int*)&(drivers[game_index]->drv->cpu[i].cpu_type);
			if ((((*type)&0xff)==CPU_Z80) && ((*type)&CPU_AUDIO_CPU))
			{
				*type=((*type)&(~0xff))|CPU_DRZ80;
			}
		}
	}
#endif

#endif

   // Remove the mouse usage for certain games
   if ( (strcasecmp(drivers[game_index]->name,"hbarrel")==0) || (strcasecmp(drivers[game_index]->name,"hbarrelw")==0) ||
		   (strcasecmp(drivers[game_index]->name,"midres")==0) || (strcasecmp(drivers[game_index]->name,"midresu")==0) ||
		   (strcasecmp(drivers[game_index]->name,"midresj")==0) || (strcasecmp(drivers[game_index]->name,"tnk3")==0) ||
		   (strcasecmp(drivers[game_index]->name,"tnk3j")==0) || (strcasecmp(drivers[game_index]->name,"ikari")==0) ||
		   (strcasecmp(drivers[game_index]->name,"ikarijp")==0) || (strcasecmp(drivers[game_index]->name,"ikarijpb")==0) ||
		   (strcasecmp(drivers[game_index]->name,"victroad")==0) || (strcasecmp(drivers[game_index]->name,"dogosoke")==0) ||
		   (strcasecmp(drivers[game_index]->name,"gwar")==0) || (strcasecmp(drivers[game_index]->name,"gwarj")==0) ||
		   (strcasecmp(drivers[game_index]->name,"gwara")==0) || (strcasecmp(drivers[game_index]->name,"gwarb")==0) ||
		   (strcasecmp(drivers[game_index]->name,"bermudat")==0) || (strcasecmp(drivers[game_index]->name,"bermudaj")==0) ||
		   (strcasecmp(drivers[game_index]->name,"bermudaa")==0) || (strcasecmp(drivers[game_index]->name,"mplanets")==0) ||
		   (strcasecmp(drivers[game_index]->name,"forgottn")==0) || (strcasecmp(drivers[game_index]->name,"lostwrld")==0) ||
		   (strcasecmp(drivers[game_index]->name,"gondo")==0) || (strcasecmp(drivers[game_index]->name,"makyosen")==0) ||
		   (strcasecmp(drivers[game_index]->name,"topgunr")==0) || (strcasecmp(drivers[game_index]->name,"topgunbl")==0) ||
		   (strcasecmp(drivers[game_index]->name,"tron")==0) || (strcasecmp(drivers[game_index]->name,"tron2")==0) ||
		   (strcasecmp(drivers[game_index]->name,"kroozr")==0) ||(strcasecmp(drivers[game_index]->name,"crater")==0) ||
		   (strcasecmp(drivers[game_index]->name,"dotron")==0) || (strcasecmp(drivers[game_index]->name,"dotrone")==0) ||
		   (strcasecmp(drivers[game_index]->name,"zwackery")==0) || (strcasecmp(drivers[game_index]->name,"ikari3")==0) ||
		   (strcasecmp(drivers[game_index]->name,"searchar")==0) || (strcasecmp(drivers[game_index]->name,"sercharu")==0) ||
		   (strcasecmp(drivers[game_index]->name,"timesold")==0) || (strcasecmp(drivers[game_index]->name,"timesol1")==0) ||
		   (strcasecmp(drivers[game_index]->name,"btlfield")==0) || (strcasecmp(drivers[game_index]->name,"aztarac")==0))
   {
	   extern int use_mouse;
	   use_mouse=0;
   }

   decompose_rom_sample_path(IMAMEBASEPATH, IMAMESAMPLEPATH);

#if !defined(SF2000)
   mame_sleep = 1;
#endif

#ifdef WANT_LIBCO
   main_thread = co_active();
   core_thread = co_create(0x10000, run_thread_proc);
   co_switch(core_thread);
#if defined(SF2000)
   extern int bailing;
   if (bailing)
       return false;
#endif   
#else
#if defined(SF2000)
   mame_sleep = 1;
#endif   
   run_thread = sthread_create(run_thread_proc, NULL);
#endif

   retro_set_audio_buff_status_cb();
   return true;
}

void retro_unload_game(void)
{
#ifdef WANT_LIBCO
if(libco_quit==0){
   printf("ask for quit!\n");
   libco_quit=1;
   co_switch(core_thread);
}
else printf("esc pressed quit!\n");
   co_delete(core_thread);
#else
   slock_lock(libretro_mutex);
   // make sure we escape the copyright warning and game warning loops
   key[KEY_ESC] = 1;
   retro_hook_quit = 1;
   mame_sleep = 0;
   scond_signal(libretro_cond);
   slock_unlock(libretro_mutex);

   if (run_thread)
      sthread_join(run_thread);

   run_thread      = NULL;
   retro_hook_quit = 0;
#endif
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data_, size_t size)
{
   (void)data_;
   (void)size;
   return false;
}

bool retro_unserialize(const void *data_, size_t size)
{
   (void)data_;
   (void)size;
   return false;
}

void *retro_get_memory_data(unsigned id)
{
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}
