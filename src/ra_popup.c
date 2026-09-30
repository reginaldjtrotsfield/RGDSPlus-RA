#define _GNU_SOURCE

#include <dlfcn.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define RA_POPUP_QUEUE_CAP       8
#define RA_BADGE_JOB_CAP        16
#define RA_MENU_BADGE_CACHE_CAP 256
#define RA_POPUP_TITLE_MAX      192
#define RA_BADGE_URL_MAX        512
#define RA_POPUP_DURATION_MS    4000
#define RA_POPUP_SLIDE_IN_MS     250
#define RA_POPUP_SLIDE_OUT_MS    250
#define RA_POPUP_PANEL_X         20.0f
#define RA_POPUP_PANEL_OFF_X    -900.0f
#define RA_POPUP_PANEL_Y         20.0f
#define RA_POPUP_PANEL_W         900.0f
#define RA_POPUP_PANEL_H         128.0f
#define RA_BADGE_MAX_BYTES      (4 * 1024 * 1024)

#define RA_CHALLENGE_MAX         4
#define RA_INDICATOR_TITLE_MAX  192

typedef unsigned int GLuint;

typedef struct {
    uint32_t program;
    uint32_t white_tex;
    int32_t a_pos;
    int32_t vp_w;
    int32_t vp_h;
    int32_t vp_ox;
    int32_t vp_oy_gl;
} UiGfx;

typedef struct {
    void *regular;
    void *bold;
    void *title;
    void *brand;
    void *brand_title;
    void *cjk;
    void *cjk_bold;
    void *cjk_title;
} UiText;

typedef void (*SDL_GetWindowSize_fn)(void *, int *, int *);

typedef void (*ui_gfx_begin_fn)(UiGfx *, int, int);

typedef void (*ui_gfx_rect_fn)(
    UiGfx *, float, float, float, float, unsigned int);

typedef void (*ui_gfx_rect_tex_fn)(
    UiGfx *, float, float, float, float,
    GLuint, unsigned int);

typedef unsigned int (*ui_gfx_upload_rgba_fn)(
    const void *, int, int);

typedef void (*ui_gfx_delete_tex_fn)(GLuint);

typedef float (*ui_text_draw_fn)(
    UiText *, UiGfx *, void *, const char *,
    float, float, unsigned int);

typedef float (*ui_text_width_fn)(
    UiText *,
    void *,
    const char *);

typedef void (*ui_text_ellipsize_fn)(
    UiText *,
    void *,
    const char *,
    char *,
    size_t,
    float);

typedef unsigned char *(*stbi_load_from_memory_fn)(
    const unsigned char *, int,
    int *, int *, int *, int);

typedef void (*stbi_image_free_fn)(void *);

/* minimal dynamic libcurl ABI */
typedef void CURL;
typedef int CURLcode;

typedef CURL *(*curl_easy_init_fn)(void);
typedef CURLcode (*curl_easy_setopt_fn)(CURL *, int, ...);
typedef CURLcode (*curl_easy_perform_fn)(CURL *);
typedef void (*curl_easy_cleanup_fn)(CURL *);

#define CURLOPT_WRITEDATA           10001
#define CURLOPT_URL                 10002
#define CURLOPT_WRITEFUNCTION       20011
#define CURLOPT_FOLLOWLOCATION      52
#define CURLOPT_NOSIGNAL            99
#define CURLOPT_USERAGENT           10018
#define CURLOPT_TIMEOUT_MS          155
#define CURLOPT_CONNECTTIMEOUT_MS   156

extern void ra_popup_swap_entry(void);
extern void ra_audio_hook_entry(void);

static uintptr_t g_launcher_base;
static void (*g_original_swap)(void *);


/* ------------------------------------------------------------------------- */
/* Stage 7D.4b - RetroArch achievement unlock sound                          */
/* ------------------------------------------------------------------------- */

/*
 * Exact RetroArch unlock waveform, decoded at build time from:
 *
 *     assets/retroarch/unlock.ogg
 *
 * Embedded PCM format:
 *     44100 Hz
 *     signed 16-bit little-endian
 *     stereo/interleaved
 */
extern const unsigned char
    _binary_unlock_s16le_pcm_start[];

extern const unsigned char
    _binary_unlock_s16le_pcm_end[];


/*
 * Slight boost because this is being mixed over active DS audio.
 *
 * The source waveform itself is unchanged.
 */
#define RA_SOUND_GAIN_NUM 3
#define RA_SOUND_GAIN_DEN 2

static unsigned g_ra_sound_frames;
static unsigned g_ra_sound_position;
static int g_ra_sound_playing;
static int g_ra_sound_ready;


static void
ra_sound_init(void)
{
    size_t bytes =
        (size_t)(
            _binary_unlock_s16le_pcm_end -
            _binary_unlock_s16le_pcm_start);

    if (bytes < 4 ||
        (bytes & 3U) != 0) {

        fprintf(
            stderr,
            "[RA SOUND] ERROR: invalid embedded PCM "
            "size=%zu\n",
            bytes);

        return;
    }

    /*
     * S16 stereo:
     *
     *   2 bytes left +
     *   2 bytes right =
     *   4 bytes/frame
     */
    g_ra_sound_frames =
        (unsigned)(bytes / 4U);

    __atomic_store_n(
        &g_ra_sound_position,
        0,
        __ATOMIC_RELEASE);

    __atomic_store_n(
        &g_ra_sound_playing,
        0,
        __ATOMIC_RELEASE);

    __atomic_store_n(
        &g_ra_sound_ready,
        1,
        __ATOMIC_RELEASE);

    fprintf(
        stderr,
        "[RA SOUND] RetroArch unlock PCM ready "
        "(%zu bytes, %u frames, gain=1.5x)\n",
        bytes,
        g_ra_sound_frames);
}


static void
ra_sound_trigger(void)
{
    if (!__atomic_load_n(
            &g_ra_sound_ready,
            __ATOMIC_ACQUIRE))
        return;

    __atomic_store_n(
        &g_ra_sound_position,
        0,
        __ATOMIC_RELEASE);

    __atomic_store_n(
        &g_ra_sound_playing,
        1,
        __ATOMIC_RELEASE);

    fprintf(
        stderr,
        "[RA SOUND] achievement chime triggered\n");
}


/*
 * Called from NNDDSS's SDL audio thread after DraStic has filled
 * the normal output buffer.
 *
 * No allocation, filesystem access, decoding, logging, or locks
 * occur here.
 */
void
ra_sound_mix(
    void *stream,
    size_t len)
{
    const int16_t *src =
        (const int16_t *)
        _binary_unlock_s16le_pcm_start;

    int16_t *dst;

    unsigned position;
    unsigned frames;
    unsigned available;
    unsigned i;

    if (!stream ||
        len < 4)
        return;

    if (!__atomic_load_n(
            &g_ra_sound_playing,
            __ATOMIC_ACQUIRE))
        return;

    position =
        __atomic_load_n(
            &g_ra_sound_position,
            __ATOMIC_ACQUIRE);

    if (position >=
        g_ra_sound_frames) {

        __atomic_store_n(
            &g_ra_sound_playing,
            0,
            __ATOMIC_RELEASE);

        return;
    }

    frames =
        (unsigned)(len / 4U);

    available =
        g_ra_sound_frames -
        position;

    if (frames > available)
        frames = available;

    dst =
        (int16_t *)stream;

    for (i = 0;
         i < frames;
         ++i) {

        unsigned src_index =
            (position + i) * 2U;

        unsigned dst_index =
            i * 2U;

        int ra_left =
            ((int)src[src_index] *
             RA_SOUND_GAIN_NUM) /
            RA_SOUND_GAIN_DEN;

        int ra_right =
            ((int)src[src_index + 1] *
             RA_SOUND_GAIN_NUM) /
            RA_SOUND_GAIN_DEN;

        int left =
            (int)dst[dst_index] +
            ra_left;

        int right =
            (int)dst[dst_index + 1] +
            ra_right;

        /*
         * Saturating S16 mix.
         */
        if (left > 32767)
            left = 32767;
        else if (left < -32768)
            left = -32768;

        if (right > 32767)
            right = 32767;
        else if (right < -32768)
            right = -32768;

        dst[dst_index] =
            (int16_t)left;

        dst[dst_index + 1] =
            (int16_t)right;
    }

    position += frames;

    __atomic_store_n(
        &g_ra_sound_position,
        position,
        __ATOMIC_RELEASE);

    if (position >=
        g_ra_sound_frames) {

        __atomic_store_n(
            &g_ra_sound_playing,
            0,
            __ATOMIC_RELEASE);
    }
}


static SDL_GetWindowSize_fn g_get_window_size;
static ui_gfx_begin_fn g_ui_gfx_begin;
static ui_gfx_rect_fn g_ui_gfx_rect;
static ui_gfx_rect_tex_fn g_ui_gfx_rect_tex;
static ui_gfx_upload_rgba_fn g_ui_gfx_upload_rgba;
static ui_gfx_delete_tex_fn g_ui_gfx_delete_tex;
static ui_text_draw_fn g_ui_text_draw;
static ui_text_width_fn g_ui_text_width;
static ui_text_ellipsize_fn g_ui_text_ellipsize;
static stbi_load_from_memory_fn g_stbi_load_from_memory;
static stbi_image_free_fn g_stbi_image_free;

typedef struct {
    uint64_t serial;

    unsigned kind;

    char title[RA_POPUP_TITLE_MAX];
    char subtitle[RA_POPUP_TITLE_MAX];

    unsigned points;

    char badge_url[RA_BADGE_URL_MAX];

    unsigned char *badge_rgba;
    int badge_w;
    int badge_h;

    GLuint badge_tex;
} RaPopupItem;

enum {
    RA_POPUP_KIND_ACHIEVEMENT = 0,
    RA_POPUP_KIND_GAME_STATUS = 1
};

enum {
    RA_BADGE_TARGET_POPUP = 0,
    RA_BADGE_TARGET_CHALLENGE = 1,
    RA_BADGE_TARGET_MENU = 2
};

typedef struct {
    uint64_t serial;
    unsigned target;
    char url[RA_BADGE_URL_MAX];
} RaBadgeJob;

static pthread_mutex_t g_lock =
    PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t g_badge_cv =
    PTHREAD_COND_INITIALIZER;

static RaPopupItem g_queue[RA_POPUP_QUEUE_CAP];
static unsigned g_qhead;
static unsigned g_qtail;
static unsigned g_qcount;

static RaPopupItem g_current;
static int g_current_active;
static struct timespec g_current_started;

static RaBadgeJob g_jobs[RA_BADGE_JOB_CAP];
static unsigned g_job_head;
static unsigned g_job_tail;
static unsigned g_job_count;

static uint64_t g_next_serial = 1;

typedef struct {
    int active;
    uint32_t id;
    uint64_t serial;

    char title[RA_INDICATOR_TITLE_MAX];
    char badge_url[RA_BADGE_URL_MAX];

    unsigned char *badge_rgba;
    int badge_w;
    int badge_h;

    GLuint badge_tex;
} RaChallengeIndicator;

typedef struct {
    int active;
    uint32_t id;
    char title[RA_INDICATOR_TITLE_MAX];
    char progress[64];
    float percent;
} RaProgressIndicator;

static RaChallengeIndicator
    g_challenges[RA_CHALLENGE_MAX];

static RaProgressIndicator
    g_progress_indicator;


typedef struct {
    int active;
    int busy;

    uint64_t serial;

    char url[RA_BADGE_URL_MAX];

    unsigned char *badge_rgba;
    int badge_w;
    int badge_h;

    GLuint badge_tex;
} RaMenuBadge;

static RaMenuBadge
    g_menu_badges[RA_MENU_BADGE_CACHE_CAP];

/*
 * GL textures must only be deleted from the gameplay render thread.
 */
#define RA_CHALLENGE_DELETE_CAP 32

static GLuint
    g_challenge_delete_tex[RA_CHALLENGE_DELETE_CAP];

static unsigned
    g_challenge_delete_count;


typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
    int too_large;
} DownloadBuffer;

static long long ts_ms(const struct timespec *ts)
{
    return (long long)ts->tv_sec * 1000LL +
           ts->tv_nsec / 1000000LL;
}

static int find_main_cb(struct dl_phdr_info *info,
                        size_t size,
                        void *userdata)
{
    (void)size;
    (void)userdata;

    if (!info->dlpi_name ||
        info->dlpi_name[0] == '\0') {
        g_launcher_base =
            (uintptr_t)info->dlpi_addr;
        return 1;
    }

    return 0;
}

static uintptr_t get_launcher_base(void)
{
    if (!g_launcher_base)
        dl_iterate_phdr(find_main_cb, NULL);

    return g_launcher_base;
}

static uintptr_t decode_bl_target(
    uintptr_t site,
    uint32_t insn)
{
    int64_t imm26 =
        (int64_t)(insn & 0x03ffffff);

    if (imm26 & 0x02000000)
        imm26 -= 0x04000000;

    return (uintptr_t)(
        (int64_t)site + (imm26 << 2));
}

static void *alloc_near(uintptr_t site)
{
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t center;
    uintptr_t delta;

    if (ps <= 0)
        ps = 4096;

    center = site & ~((uintptr_t)ps - 1);

    for (delta = (uintptr_t)ps;
         delta < 0x07f00000UL;
         delta += (uintptr_t)ps) {

        uintptr_t candidates[2];
        int i;

        candidates[0] = center + delta;
        candidates[1] =
            center > delta ? center - delta : 0;

        for (i = 0; i < 2; ++i) {
            void *p;

            if (candidates[i] < 0x10000)
                continue;

            p = mmap(
                (void *)candidates[i],
                (size_t)ps,
                PROT_READ | PROT_WRITE,
                MAP_PRIVATE |
                MAP_ANONYMOUS |
                MAP_FIXED_NOREPLACE,
                -1,
                0);

            if (p != MAP_FAILED)
                return p;
        }
    }

    return NULL;
}

static int install_hook(void)
{
    const uint32_t expected = 0x97ffe3cb;

    uintptr_t base = get_launcher_base();
    uintptr_t site;
    uintptr_t original;
    void *island;
    uintptr_t island_addr;
    uintptr_t site_page;
    long ps;
    int64_t delta;
    uint32_t patched;

    if (!base) {
        fprintf(stderr,
                "[RA BADGE] ERROR: launcher base not found\n");
        return 0;
    }

    site = base + 0xcae4;

    if (*(volatile uint32_t *)site != expected) {
        fprintf(stderr,
                "[RA BADGE] REFUSED: +0xCAE4=%08x "
                "expected=%08x\n",
                *(volatile uint32_t *)site,
                expected);
        return 0;
    }

    original = decode_bl_target(site, expected);

    g_original_swap =
        (void (*)(void *))original;

    g_ui_gfx_begin =
        (ui_gfx_begin_fn)(base + 0x142f0);

    g_ui_gfx_rect =
        (ui_gfx_rect_fn)(base + 0x14370);

    g_ui_gfx_rect_tex =
        (ui_gfx_rect_tex_fn)(base + 0x14390);

    g_ui_gfx_upload_rgba =
        (ui_gfx_upload_rgba_fn)(base + 0x143d0);

    g_ui_gfx_delete_tex =
        (ui_gfx_delete_tex_fn)(base + 0x147b0);

    g_ui_text_draw =
        (ui_text_draw_fn)(base + 0x15310);

    g_ui_text_width =
        (ui_text_width_fn)(base + 0x14fb0);

    g_ui_text_ellipsize =
        (ui_text_ellipsize_fn)(base + 0x15190);

    g_stbi_load_from_memory =
        (stbi_load_from_memory_fn)(base + 0x23770);

    g_stbi_image_free =
        (stbi_image_free_fn)(base + 0x1bea0);

    g_get_window_size =
        (SDL_GetWindowSize_fn)
        dlsym(RTLD_NEXT, "SDL_GetWindowSize");

    if (!g_get_window_size) {
        fprintf(stderr,
                "[RA BADGE] ERROR: SDL_GetWindowSize: %s\n",
                dlerror());
        return 0;
    }

    island = alloc_near(site);

    if (!island) {
        fprintf(stderr,
                "[RA BADGE] ERROR: no near island\n");
        return 0;
    }

    island_addr = (uintptr_t)island;

    {
        uint32_t *code = (uint32_t *)island;
        uint64_t target =
            (uint64_t)(uintptr_t)&ra_popup_swap_entry;

        code[0] = 0x58000050; /* ldr x16, #8 */
        code[1] = 0xd61f0200; /* br x16 */

        memcpy((unsigned char *)island + 8,
               &target,
               sizeof(target));
    }

    __builtin___clear_cache(
        (char *)island,
        (char *)island + 16);

    ps = sysconf(_SC_PAGESIZE);

    if (mprotect(
            island,
            (size_t)ps,
            PROT_READ | PROT_EXEC) != 0) {
        fprintf(stderr,
                "[RA BADGE] ERROR: island mprotect: %s\n",
                strerror(errno));
        return 0;
    }

    delta =
        (int64_t)island_addr -
        (int64_t)site;

    if ((delta & 3) != 0 ||
        delta < -134217728LL ||
        delta > 134217724LL) {
        fprintf(stderr,
                "[RA BADGE] ERROR: island outside BL range\n");
        return 0;
    }

    patched =
        0x94000000U |
        ((uint32_t)(delta >> 2) &
         0x03ffffffU);

    site_page =
        site & ~((uintptr_t)ps - 1);

    if (mprotect(
            (void *)site_page,
            (size_t)ps,
            PROT_READ |
            PROT_WRITE |
            PROT_EXEC) != 0) {
        fprintf(stderr,
                "[RA BADGE] ERROR: site mprotect: %s\n",
                strerror(errno));
        return 0;
    }

    *(volatile uint32_t *)site = patched;

    __builtin___clear_cache(
        (char *)site,
        (char *)site + 4);

    mprotect(
        (void *)site_page,
        (size_t)ps,
        PROT_READ | PROT_EXEC);

    fprintf(stderr,
            "[RA BADGE] gameplay hook installed "
            "island=%p\n",
            island);

    return 1;
}

static size_t curl_write_cb(
    void *ptr,
    size_t size,
    size_t nmemb,
    void *userdata)
{
    DownloadBuffer *buf =
        (DownloadBuffer *)userdata;

    size_t bytes = size * nmemb;
    size_t required;
    unsigned char *new_data;

    if (bytes == 0)
        return 0;

    if (buf->size >
        RA_BADGE_MAX_BYTES - bytes) {
        buf->too_large = 1;
        return 0;
    }

    required = buf->size + bytes;

    if (required > buf->capacity) {
        size_t cap =
            buf->capacity ?
            buf->capacity * 2 :
            16384;

        while (cap < required)
            cap *= 2;

        if (cap > RA_BADGE_MAX_BYTES)
            cap = RA_BADGE_MAX_BYTES;

        if (cap < required) {
            buf->too_large = 1;
            return 0;
        }

        new_data =
            (unsigned char *)
            realloc(buf->data, cap);

        if (!new_data)
            return 0;

        buf->data = new_data;
        buf->capacity = cap;
    }

    memcpy(buf->data + buf->size,
           ptr,
           bytes);

    buf->size += bytes;

    return bytes;
}

static int download_badge(
    const char *url,
    DownloadBuffer *buf)
{
    static void *curl_lib;
    static curl_easy_init_fn easy_init;
    static curl_easy_setopt_fn easy_setopt;
    static curl_easy_perform_fn easy_perform;
    static curl_easy_cleanup_fn easy_cleanup;
    static int initialized;

    CURL *curl;
    CURLcode result;

    if (!initialized) {
        initialized = 1;

        curl_lib =
            dlopen("libcurl.so.4",
                   RTLD_LAZY | RTLD_LOCAL);

        if (!curl_lib) {
            fprintf(stderr,
                    "[RA BADGE] ERROR: libcurl: %s\n",
                    dlerror());
            return 0;
        }

        easy_init =
            (curl_easy_init_fn)
            dlsym(curl_lib, "curl_easy_init");

        easy_setopt =
            (curl_easy_setopt_fn)
            dlsym(curl_lib, "curl_easy_setopt");

        easy_perform =
            (curl_easy_perform_fn)
            dlsym(curl_lib, "curl_easy_perform");

        easy_cleanup =
            (curl_easy_cleanup_fn)
            dlsym(curl_lib, "curl_easy_cleanup");

        if (!easy_init ||
            !easy_setopt ||
            !easy_perform ||
            !easy_cleanup) {
            fprintf(stderr,
                    "[RA BADGE] ERROR: incomplete curl ABI\n");
            return 0;
        }
    }

    memset(buf, 0, sizeof(*buf));

    curl = easy_init();

    if (!curl)
        return 0;

    easy_setopt(curl, CURLOPT_URL, url);
    easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    easy_setopt(curl, CURLOPT_USERAGENT,
                "NNDDSS-RetroAchievements/0.5");
    easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                curl_write_cb);
    easy_setopt(curl, CURLOPT_WRITEDATA, buf);

    result = easy_perform(curl);

    easy_cleanup(curl);

    if (result != 0 ||
        buf->too_large ||
        !buf->data ||
        buf->size == 0) {

        free(buf->data);
        memset(buf, 0, sizeof(*buf));
        return 0;
    }

    return 1;
}

static RaPopupItem *find_item_locked(
    uint64_t serial)
{
    unsigned i;

    if (g_current_active &&
        g_current.serial == serial)
        return &g_current;

    for (i = 0;
         i < RA_POPUP_QUEUE_CAP;
         ++i) {

        if (g_queue[i].serial == serial)
            return &g_queue[i];
    }

    return NULL;
}


/*
 * Stage 7B renderer-settings bridge.
 *
 * Challenge and progress state continues to be maintained even when
 * hidden. Only the local rendering snapshot is suppressed. Therefore
 * turning an option back On can immediately redisplay an already
 * active indicator without requiring rcheevos to retrigger it.
 */
typedef int (*ra_renderer_setting_getter_fn)(void);

static ra_renderer_setting_getter_fn
    g_ra_get_challenges_enabled;

static ra_renderer_setting_getter_fn
    g_ra_get_progress_enabled;

static int
    g_ra_renderer_settings_resolved;


static void ra_resolve_renderer_settings(void)
{
    if (g_ra_renderer_settings_resolved)
        return;

    g_ra_renderer_settings_resolved = 1;

    g_ra_get_challenges_enabled =
        (ra_renderer_setting_getter_fn)dlsym(
            RTLD_DEFAULT,
            "ra_settings_challenge_indicators_enabled");

    g_ra_get_progress_enabled =
        (ra_renderer_setting_getter_fn)dlsym(
            RTLD_DEFAULT,
            "ra_settings_progress_indicators_enabled");
}


static int ra_challenges_enabled(void)
{
    ra_resolve_renderer_settings();

    if (!g_ra_get_challenges_enabled)
        return 1;

    return
        g_ra_get_challenges_enabled() ?
        1 :
        0;
}


static int ra_progress_enabled(void)
{
    ra_resolve_renderer_settings();

    if (!g_ra_get_progress_enabled)
        return 1;

    return
        g_ra_get_progress_enabled() ?
        1 :
        0;
}


static RaChallengeIndicator *find_challenge_locked(
    uint64_t serial)
{
    unsigned i;

    for (i = 0;
         i < RA_CHALLENGE_MAX;
         ++i) {

        if (g_challenges[i].active &&
            g_challenges[i].serial == serial)
            return &g_challenges[i];
    }

    return NULL;
}



static RaMenuBadge *find_menu_badge_locked(
    uint64_t serial)
{
    unsigned i;

    for (i = 0;
         i < RA_MENU_BADGE_CACHE_CAP;
         ++i) {

        if (g_menu_badges[i].active &&
            g_menu_badges[i].serial == serial) {

            return &g_menu_badges[i];
        }
    }

    return NULL;
}


static RaMenuBadge *find_menu_badge_url_locked(
    const char *url)
{
    unsigned i;

    if (!url || !*url)
        return NULL;

    for (i = 0;
         i < RA_MENU_BADGE_CACHE_CAP;
         ++i) {

        if (g_menu_badges[i].active &&
            strcmp(
                g_menu_badges[i].url,
                url) == 0) {

            return &g_menu_badges[i];
        }
    }

    return NULL;
}


static RaMenuBadge *alloc_menu_badge_locked(
    const char *url)
{
    unsigned i;

    if (!url || !*url)
        return NULL;

    for (i = 0;
         i < RA_MENU_BADGE_CACHE_CAP;
         ++i) {

        RaMenuBadge *badge =
            &g_menu_badges[i];

        if (badge->active)
            continue;

        memset(
            badge,
            0,
            sizeof(*badge));

        badge->active = 1;

        badge->serial =
            g_next_serial++;

        snprintf(
            badge->url,
            sizeof(badge->url),
            "%s",
            url);

        return badge;
    }

    fprintf(
        stderr,
        "[RA MENU BADGE] cache full\n");

    return NULL;
}


static void queue_challenge_tex_delete_locked(
    GLuint tex)
{
    if (!tex)
        return;

    if (g_challenge_delete_count <
        RA_CHALLENGE_DELETE_CAP) {

        g_challenge_delete_tex[
            g_challenge_delete_count++] = tex;
    }
    else {
        fprintf(stderr,
                "[RA BADGE] WARNING: "
                "challenge texture delete queue full\n");
    }
}


static void *badge_worker(void *unused)
{
    (void)unused;

    fprintf(stderr,
            "[RA BADGE] download worker started\n");

    for (;;) {
        RaBadgeJob job;
        DownloadBuffer body;
        unsigned char *rgba = NULL;
        int iw = 0;
        int ih = 0;
        int channels = 0;

        memset(&job, 0, sizeof(job));

        pthread_mutex_lock(&g_lock);

        while (g_job_count == 0)
            pthread_cond_wait(
                &g_badge_cv,
                &g_lock);

        job = g_jobs[g_job_head];

        memset(&g_jobs[g_job_head],
               0,
               sizeof(g_jobs[g_job_head]));

        g_job_head =
            (g_job_head + 1) %
            RA_BADGE_JOB_CAP;

        --g_job_count;

        pthread_mutex_unlock(&g_lock);

        fprintf(stderr,
                "[RA BADGE] downloading %s\n",
                job.url);

        if (!download_badge(
                job.url,
                &body)) {

            if (job.target ==
                RA_BADGE_TARGET_MENU) {

                pthread_mutex_lock(
                    &g_lock);

                {
                    RaMenuBadge *badge =
                        find_menu_badge_locked(
                            job.serial);

                    if (badge)
                        badge->busy = 0;
                }

                pthread_mutex_unlock(
                    &g_lock);
            }

            fprintf(stderr,
                    "[RA BADGE] download failed\n");
            continue;
        }

        fprintf(stderr,
                "[RA BADGE] downloaded %zu bytes\n",
                body.size);

        if (body.size <= 0x7fffffffU) {
            rgba =
                g_stbi_load_from_memory(
                    body.data,
                    (int)body.size,
                    &iw,
                    &ih,
                    &channels,
                    4);
        }

        free(body.data);

        if (!rgba ||
            iw <= 0 ||
            ih <= 0 ||
            iw > 2048 ||
            ih > 2048) {

            if (rgba)
                g_stbi_image_free(rgba);

            if (job.target ==
                RA_BADGE_TARGET_MENU) {

                pthread_mutex_lock(
                    &g_lock);

                {
                    RaMenuBadge *badge =
                        find_menu_badge_locked(
                            job.serial);

                    if (badge)
                        badge->busy = 0;
                }

                pthread_mutex_unlock(
                    &g_lock);
            }

            fprintf(stderr,
                    "[RA BADGE] PNG decode failed\n");
            continue;
        }

        fprintf(stderr,
                "[RA BADGE] decoded %dx%d\n",
                iw,
                ih);

        pthread_mutex_lock(&g_lock);

        if (job.target ==
            RA_BADGE_TARGET_CHALLENGE) {

            RaChallengeIndicator *challenge =
                find_challenge_locked(job.serial);

            if (challenge) {
                challenge->badge_rgba = rgba;
                challenge->badge_w = iw;
                challenge->badge_h = ih;
                rgba = NULL;
            }
        }
        else if (job.target ==
                 RA_BADGE_TARGET_MENU) {

            RaMenuBadge *badge =
                find_menu_badge_locked(
                    job.serial);

            if (badge) {
                badge->badge_rgba = rgba;
                badge->badge_w = iw;
                badge->badge_h = ih;
                rgba = NULL;
            }
        }
        else {
            RaPopupItem *item =
                find_item_locked(job.serial);

            if (item) {
                item->badge_rgba = rgba;
                item->badge_w = iw;
                item->badge_h = ih;
                rgba = NULL;
            }
        }

        pthread_mutex_unlock(&g_lock);

        if (rgba)
            g_stbi_image_free(rgba);
    }

    return NULL;
}

static void queue_badge_job_locked(
    uint64_t serial,
    unsigned target,
    const char *url)
{
    RaBadgeJob *job;

    if (!url || !*url)
        return;

    if (g_job_count >= RA_BADGE_JOB_CAP) {
        fprintf(stderr,
                "[RA BADGE] job queue full\n");
        return;
    }

    job = &g_jobs[g_job_tail];

    memset(job, 0, sizeof(*job));

    job->serial = serial;
    job->target = target;

    snprintf(job->url,
             sizeof(job->url),
             "%s",
             url);

    g_job_tail =
        (g_job_tail + 1) %
        RA_BADGE_JOB_CAP;

    ++g_job_count;

    pthread_cond_signal(&g_badge_cv);
}


__attribute__((visibility("default")))
int ra_menu_badge_draw(
    UiGfx *gfx,
    const char *url,
    float x,
    float y,
    float size)
{
    RaMenuBadge *badge;

    uint64_t serial = 0;

    GLuint tex = 0;

    unsigned char *rgba = NULL;

    int iw = 0;
    int ih = 0;

    if (!gfx ||
        !url ||
        !*url ||
        size <= 0.0f ||
        !g_ui_gfx_rect_tex ||
        !g_ui_gfx_upload_rgba ||
        !g_ui_gfx_delete_tex ||
        !g_stbi_image_free) {

        return 0;
    }

    pthread_mutex_lock(
        &g_lock);

    badge =
        find_menu_badge_url_locked(
            url);

    if (!badge) {
        badge =
            alloc_menu_badge_locked(
                url);
    }

    if (!badge) {
        pthread_mutex_unlock(
            &g_lock);

        return 0;
    }

    serial =
        badge->serial;

    /*
     * Request the image once. If the global worker queue is currently
     * full, leave busy clear so a later frame can try again.
     */
    if (!badge->badge_tex &&
        !badge->badge_rgba &&
        !badge->busy) {

        if (g_job_count <
            RA_BADGE_JOB_CAP) {

            badge->busy = 1;

            queue_badge_job_locked(
                badge->serial,
                RA_BADGE_TARGET_MENU,
                badge->url);

            fprintf(
                stderr,
                "[RA MENU BADGE] queued %s\n",
                badge->url);
        }
    }

    tex =
        badge->badge_tex;

    /*
     * The worker only decodes PNG -> RGBA.
     * Texture upload remains on the UI/render thread.
     */
    if (!tex &&
        badge->badge_rgba) {

        rgba =
            badge->badge_rgba;

        iw =
            badge->badge_w;

        ih =
            badge->badge_h;

        badge->badge_rgba = NULL;
        badge->badge_w = 0;
        badge->badge_h = 0;
    }

    pthread_mutex_unlock(
        &g_lock);

    if (rgba) {
        GLuint new_tex =
            g_ui_gfx_upload_rgba(
                rgba,
                iw,
                ih);

        g_stbi_image_free(
            rgba);

        if (new_tex) {
            int accepted = 0;

            pthread_mutex_lock(
                &g_lock);

            badge =
                find_menu_badge_locked(
                    serial);

            if (badge &&
                !badge->badge_tex) {

                badge->badge_tex =
                    new_tex;

                badge->busy = 0;

                tex =
                    new_tex;

                accepted = 1;
            }

            pthread_mutex_unlock(
                &g_lock);

            if (!accepted)
                g_ui_gfx_delete_tex(
                    new_tex);

            fprintf(
                stderr,
                "[RA MENU BADGE] texture upload %s\n",
                accepted ?
                    "successful" :
                    "discarded");
        }
        else {
            pthread_mutex_lock(
                &g_lock);

            badge =
                find_menu_badge_locked(
                    serial);

            if (badge)
                badge->busy = 0;

            pthread_mutex_unlock(
                &g_lock);

            fprintf(
                stderr,
                "[RA MENU BADGE] texture upload failed\n");
        }
    }

    if (!tex) {
        pthread_mutex_lock(
            &g_lock);

        badge =
            find_menu_badge_locked(
                serial);

        if (badge)
            tex = badge->badge_tex;

        pthread_mutex_unlock(
            &g_lock);
    }

    if (!tex)
        return 0;

    g_ui_gfx_rect_tex(
        gfx,
        x,
        y,
        size,
        size,
        tex,
        0xffffffffU);

    return 1;
}


__attribute__((visibility("default")))
void ra_popup_enqueue_achievement_badged(
    const char *title,
    unsigned points,
    const char *badge_url)
{
    RaPopupItem *item;
    uint64_t serial;

    if (!title || !*title)
        title = "Achievement";

    pthread_mutex_lock(&g_lock);

    if (g_qcount >= RA_POPUP_QUEUE_CAP) {
        pthread_mutex_unlock(&g_lock);

        fprintf(stderr,
                "[RA POPUP] queue full - dropping %s\n",
                title);
        return;
    }

    serial = g_next_serial++;

    item = &g_queue[g_qtail];

    memset(item, 0, sizeof(*item));

    item->serial = serial;
    item->kind =
        RA_POPUP_KIND_ACHIEVEMENT;

    item->points = points;

    snprintf(item->title,
             sizeof(item->title),
             "%s",
             title);

    if (badge_url) {
        snprintf(item->badge_url,
                 sizeof(item->badge_url),
                 "%s",
                 badge_url);
    }

    g_qtail =
        (g_qtail + 1) %
        RA_POPUP_QUEUE_CAP;

    ++g_qcount;

    queue_badge_job_locked(
        serial,
        RA_BADGE_TARGET_POPUP,
        item->badge_url);

    pthread_mutex_unlock(&g_lock);

    fprintf(stderr,
            "[RA POPUP] queued: \"%s\" "
            "(%u points) badge=%s\n",
            title,
            points,
            badge_url && *badge_url ?
                "yes" : "no");
}

/* backward compatibility with Stage 5 bridge */
__attribute__((visibility("default")))
void ra_popup_enqueue_achievement(
    const char *title,
    unsigned points)
{
    ra_popup_enqueue_achievement_badged(
        title,
        points,
        NULL);
}


/*
 * Stage 7D.1 - timed game/startup status notification.
 *
 * Uses the same proven queue and slide/fade lifetime as achievement
 * popups, but is a distinct item type so it does not pretend to be
 * an achievement or display a points value.
 */
__attribute__((visibility("default")))
void ra_popup_enqueue_game_status(
    const char *game_title,
    unsigned unlocked,
    unsigned total,
    int supported,
    const char *badge_url)
{
    RaPopupItem *item;
    uint64_t serial;

    pthread_mutex_lock(&g_lock);

    if (g_qcount >= RA_POPUP_QUEUE_CAP) {
        pthread_mutex_unlock(&g_lock);

        fprintf(
            stderr,
            "[RA STARTUP] popup queue full\n");

        return;
    }

    serial = g_next_serial++;

    item = &g_queue[g_qtail];

    memset(
        item,
        0,
        sizeof(*item));

    item->serial = serial;
    item->kind =
        RA_POPUP_KIND_GAME_STATUS;

    if (supported) {
        snprintf(
            item->title,
            sizeof(item->title),
            "%s",
            game_title && game_title[0] ?
                game_title :
                "Unknown game");

        snprintf(
            item->subtitle,
            sizeof(item->subtitle),
            "%u / %u achievement%s unlocked",
            unlocked,
            total,
            total == 1 ? "" : "s");
    }
    else {
        snprintf(
            item->title,
            sizeof(item->title),
            "%s",
            "Game not supported");

        snprintf(
            item->subtitle,
            sizeof(item->subtitle),
            "%s",
            "No RetroAchievements set found");
    }

    if (badge_url &&
        badge_url[0]) {

        snprintf(
            item->badge_url,
            sizeof(item->badge_url),
            "%s",
            badge_url);

        queue_badge_job_locked(
            serial,
            RA_BADGE_TARGET_POPUP,
            item->badge_url);

        fprintf(
            stderr,
            "[RA STARTUP] game badge queued: %s\n",
            item->badge_url);
    }

    g_qtail =
        (g_qtail + 1) %
        RA_POPUP_QUEUE_CAP;

    ++g_qcount;

    fprintf(
        stderr,
        "[RA STARTUP] queued: \"%s\" "
        "%u/%u supported=%d\n",
        item->title,
        unlocked,
        total,
        supported ? 1 : 0);

    pthread_mutex_unlock(&g_lock);
}


static unsigned int ra_color_alpha(
    unsigned int color,
    unsigned int alpha)
{
    if (alpha > 255)
        alpha = 255;

    return (color & 0x00ffffffU) |
           (alpha << 24);
}

static float ra_clamp01(float v)
{
    if (v < 0.0f)
        return 0.0f;

    if (v > 1.0f)
        return 1.0f;

    return v;
}



/*
 * Stage 7B.6 - clear all renderer state owned by the previous game.
 *
 * This may be called from the RA lifecycle worker thread.
 * CPU-side state is destroyed under g_lock. GL textures are NOT
 * deleted here; they are queued for deletion by the gameplay
 * render thread using the existing deferred texture-delete path.
 *
 * g_next_serial is deliberately not reset. A badge download that
 * was already in flight for the previous game may complete after
 * this function returns. Keeping serials monotonic prevents that
 * stale result from matching a newly-created popup or challenge.
 */
__attribute__((visibility("default")))
void ra_popup_clear_game_state(void)
{
    unsigned i;

    pthread_mutex_lock(&g_lock);

    /*
     * Current timed achievement popup.
     */
    if (g_current.badge_tex) {
        queue_challenge_tex_delete_locked(
            g_current.badge_tex);
    }

    if (g_current.badge_rgba &&
        g_stbi_image_free) {

        g_stbi_image_free(
            g_current.badge_rgba);
    }

    memset(
        &g_current,
        0,
        sizeof(g_current));

    g_current_active = 0;

    memset(
        &g_current_started,
        0,
        sizeof(g_current_started));


    /*
     * Pending timed achievement popups.
     *
     * Normally queued entries only own decoded CPU pixels, but
     * handle badge_tex too so this remains correct if the popup
     * preparation path changes later.
     */
    for (i = 0;
         i < RA_POPUP_QUEUE_CAP;
         ++i) {

        RaPopupItem *item =
            &g_queue[i];

        if (item->badge_tex) {
            queue_challenge_tex_delete_locked(
                item->badge_tex);
        }

        if (item->badge_rgba &&
            g_stbi_image_free) {

            g_stbi_image_free(
                item->badge_rgba);
        }

        memset(
            item,
            0,
            sizeof(*item));
    }

    g_qhead = 0;
    g_qtail = 0;
    g_qcount = 0;


    /*
     * Badge jobs that have not yet been taken by the download
     * worker belong to the previous game and can be discarded.
     *
     * A job already being downloaded is outside this queue. When
     * it completes its old serial will no longer resolve to any
     * popup/challenge and the worker will discard the decoded data.
     */
    memset(
        g_jobs,
        0,
        sizeof(g_jobs));

    g_job_head = 0;
    g_job_tail = 0;
    g_job_count = 0;


    /*
     * Persistent challenge indicators.
     */
    for (i = 0;
         i < RA_CHALLENGE_MAX;
         ++i) {

        RaChallengeIndicator *challenge =
            &g_challenges[i];

        if (challenge->badge_tex) {
            queue_challenge_tex_delete_locked(
                challenge->badge_tex);
        }

        if (challenge->badge_rgba &&
            g_stbi_image_free) {

            g_stbi_image_free(
                challenge->badge_rgba);
        }

        memset(
            challenge,
            0,
            sizeof(*challenge));
    }


    /*
     * Measured-progress indicator.
     */
    memset(
        &g_progress_indicator,
        0,
        sizeof(g_progress_indicator));

    pthread_mutex_unlock(&g_lock);

    fprintf(
        stderr,
        "[RA RENDERER] previous game visual state cleared\n");
}


__attribute__((visibility("default")))
void ra_indicator_challenge_show(
    unsigned id,
    const char *title,
    const char *badge_url)
{
    unsigned i;
    int slot = -1;
    RaChallengeIndicator *challenge;

    pthread_mutex_lock(&g_lock);

    /*
     * Prefer an existing slot for this achievement.
     * Otherwise use the first free slot.
     */
    for (i = 0;
         i < RA_CHALLENGE_MAX;
         ++i) {

        if (g_challenges[i].active &&
            g_challenges[i].id == id) {

            slot = (int)i;
            break;
        }

        if (slot < 0 &&
            !g_challenges[i].active)
            slot = (int)i;
    }

    /*
     * Extremely unlikely, but if all slots are occupied,
     * replace the last one.
     */
    if (slot < 0)
        slot = RA_CHALLENGE_MAX - 1;

    challenge = &g_challenges[slot];

    /*
     * Old GL texture must be destroyed on render thread.
     */
    if (challenge->badge_tex)
        queue_challenge_tex_delete_locked(
            challenge->badge_tex);

    /*
     * Decoded pixels are not GL state and can be freed here.
     */
    if (challenge->badge_rgba)
        g_stbi_image_free(
            challenge->badge_rgba);

    memset(challenge,
           0,
           sizeof(*challenge));

    challenge->active = 1;
    challenge->id = id;
    challenge->serial = g_next_serial++;

    snprintf(
        challenge->title,
        sizeof(challenge->title),
        "%s",
        title ? title : "Achievement");

    if (badge_url) {
        snprintf(
            challenge->badge_url,
            sizeof(challenge->badge_url),
            "%s",
            badge_url);
    }

    queue_badge_job_locked(
        challenge->serial,
        RA_BADGE_TARGET_CHALLENGE,
        challenge->badge_url);

    pthread_mutex_unlock(&g_lock);

    fprintf(stderr,
            "[RA INDICATOR] challenge show: "
            "%u \"%s\" badge=%s\n",
            id,
            title ? title : "Achievement",
            badge_url && *badge_url ?
                "yes" : "no");
}


__attribute__((visibility("default")))
void ra_indicator_challenge_hide(unsigned id)
{
    unsigned i;

    pthread_mutex_lock(&g_lock);

    for (i = 0;
         i < RA_CHALLENGE_MAX;
         ++i) {

        RaChallengeIndicator *challenge =
            &g_challenges[i];

        if (challenge->active &&
            (id == 0 ||
             challenge->id == id)) {

            if (challenge->badge_tex) {
                queue_challenge_tex_delete_locked(
                    challenge->badge_tex);
            }

            if (challenge->badge_rgba) {
                g_stbi_image_free(
                    challenge->badge_rgba);
            }

            memset(challenge,
                   0,
                   sizeof(*challenge));

            if (id != 0)
                break;
        }
    }

    pthread_mutex_unlock(&g_lock);

    fprintf(stderr,
            "[RA INDICATOR] challenge hide: %u\n",
            id);
}


__attribute__((visibility("default")))
void ra_indicator_progress_update(
    unsigned id,
    const char *title,
    const char *progress,
    float percent)
{
    pthread_mutex_lock(&g_lock);

    memset(&g_progress_indicator,
           0,
           sizeof(g_progress_indicator));

    g_progress_indicator.active = 1;
    g_progress_indicator.id = id;

    snprintf(
        g_progress_indicator.title,
        sizeof(g_progress_indicator.title),
        "%s",
        title ? title : "Achievement");

    snprintf(
        g_progress_indicator.progress,
        sizeof(g_progress_indicator.progress),
        "%s",
        progress ? progress : "");

    if (percent < 0.0f)
        percent = 0.0f;

    if (percent > 100.0f)
        percent = 100.0f;

    g_progress_indicator.percent = percent;

    pthread_mutex_unlock(&g_lock);

    fprintf(stderr,
            "[RA INDICATOR] progress: %u \"%s\" %s %.1f%%\n",
            id,
            title ? title : "Achievement",
            progress ? progress : "",
            percent);
}

__attribute__((visibility("default")))
void ra_indicator_progress_hide(void)
{
    pthread_mutex_lock(&g_lock);

    memset(&g_progress_indicator,
           0,
           sizeof(g_progress_indicator));

    pthread_mutex_unlock(&g_lock);

    fprintf(stderr,
            "[RA INDICATOR] progress hide\n");
}

static void ra_prepare_challenge_textures(void)
{
    GLuint deletes[RA_CHALLENGE_DELETE_CAP];
    unsigned delete_count = 0;
    unsigned i;

    /*
     * Drain deferred texture deletes.
     */
    pthread_mutex_lock(&g_lock);

    delete_count =
        g_challenge_delete_count;

    if (delete_count >
        RA_CHALLENGE_DELETE_CAP)
        delete_count =
            RA_CHALLENGE_DELETE_CAP;

    memcpy(
        deletes,
        g_challenge_delete_tex,
        delete_count * sizeof(GLuint));

    g_challenge_delete_count = 0;

    pthread_mutex_unlock(&g_lock);

    for (i = 0;
         i < delete_count;
         ++i) {

        if (deletes[i])
            g_ui_gfx_delete_tex(
                deletes[i]);
    }

    /*
     * Upload any newly decoded challenge images.
     * Uploads and texture deletion stay on the GL/render thread.
     */
    for (i = 0;
         i < RA_CHALLENGE_MAX;
         ++i) {

        unsigned char *rgba = NULL;
        int iw = 0;
        int ih = 0;
        uint64_t serial = 0;
        GLuint tex = 0;

        pthread_mutex_lock(&g_lock);

        if (g_challenges[i].active &&
            !g_challenges[i].badge_tex &&
            g_challenges[i].badge_rgba) {

            rgba =
                g_challenges[i].badge_rgba;

            iw =
                g_challenges[i].badge_w;

            ih =
                g_challenges[i].badge_h;

            serial =
                g_challenges[i].serial;

            g_challenges[i].badge_rgba = NULL;
            g_challenges[i].badge_w = 0;
            g_challenges[i].badge_h = 0;
        }

        pthread_mutex_unlock(&g_lock);

        if (!rgba)
            continue;

        tex =
            g_ui_gfx_upload_rgba(
                rgba,
                iw,
                ih);

        g_stbi_image_free(rgba);

        if (tex) {
            int accepted = 0;

            pthread_mutex_lock(&g_lock);

            {
                RaChallengeIndicator *challenge =
                    find_challenge_locked(serial);

                if (challenge &&
                    !challenge->badge_tex) {

                    challenge->badge_tex = tex;
                    accepted = 1;
                }
            }

            pthread_mutex_unlock(&g_lock);

            if (!accepted)
                g_ui_gfx_delete_tex(tex);

            fprintf(stderr,
                    "[RA BADGE] challenge texture upload %s\n",
                    accepted ?
                        "successful" :
                        "discarded");
        }
        else {
            fprintf(stderr,
                    "[RA BADGE] challenge texture upload failed\n");
        }
    }
}


static void ra_draw_indicators(
    UiGfx *gfx,
    UiText *text,
    int w,
    int h)
{
    RaChallengeIndicator
        challenges[RA_CHALLENGE_MAX];

    RaProgressIndicator progress;

    unsigned i;
    unsigned active_count = 0;
    unsigned drawn = 0;

    if (!gfx ||
        !text ||
        !text->regular ||
        !text->bold ||
        w <= 0 ||
        h <= 0)
        return;

    /*
     * First service GL work generated by the async downloader.
     */
    ra_prepare_challenge_textures();

    pthread_mutex_lock(&g_lock);

    memcpy(
        challenges,
        g_challenges,
        sizeof(challenges));

    progress =
        g_progress_indicator;

    /*
     * Stage 7B visibility controls.
     *
     * Modify only these local snapshots. The real challenge/progress
     * state above remains untouched.
     */
    if (!ra_challenges_enabled()) {
        unsigned ra_setting_i;

        for (ra_setting_i = 0;
             ra_setting_i < RA_CHALLENGE_MAX;
             ++ra_setting_i) {

            challenges[ra_setting_i].active = 0;
        }
    }

    if (!ra_progress_enabled())
        progress.active = 0;


    pthread_mutex_unlock(&g_lock);

    for (i = 0;
         i < RA_CHALLENGE_MAX;
         ++i) {

        if (challenges[i].active)
            ++active_count;
    }

    if (!active_count &&
        !progress.active)
        return;

    g_ui_gfx_begin(
        gfx,
        w,
        h);

    /*
     * Challenge badges:
     *
     *   72x72 overall
     *   64x64 badge
     *   4px amber frame
     *
     * Anchored to lower-right and stacked leftward.
     */
    if (active_count) {
        const float icon_size = 72.0f;
        const float badge_size = 64.0f;
        const float border = 4.0f;
        const float gap = 8.0f;
        const float margin = 16.0f;

        float y =
            (float)h -
            margin -
            icon_size;

        for (i = 0;
             i < RA_CHALLENGE_MAX;
             ++i) {

            float x;

            if (!challenges[i].active)
                continue;

            x =
                (float)w -
                margin -
                icon_size -
                (float)drawn *
                (icon_size + gap);

            /*
             * Amber challenge frame.
             */
            g_ui_gfx_rect(
                gfx,
                x,
                y,
                icon_size,
                icon_size,
                0xffffb347U);

            /*
             * Dark interior while image is loading.
             */
            g_ui_gfx_rect(
                gfx,
                x + border,
                y + border,
                badge_size,
                badge_size,
                0xff111111U);

            if (challenges[i].badge_tex) {
                g_ui_gfx_rect_tex(
                    gfx,
                    x + border,
                    y + border,
                    badge_size,
                    badge_size,
                    challenges[i].badge_tex,
                    0xffffffffU);
            }

            ++drawn;
        }
    }

    /*
     * Measured progress remains a text panel, but moves above
     * the active challenge-badge row when necessary.
     */
    if (progress.active) {
        float x =
            (float)w - 540.0f;

        float y =
            (float)h - 114.0f;

        float fill;

        char clipped[
            RA_INDICATOR_TITLE_MAX];

        if (active_count)
            y -= 84.0f;

        if (x < 20.0f)
            x = 20.0f;

        if (y < 20.0f)
            y = 20.0f;

        memset(
            clipped,
            0,
            sizeof(clipped));

        g_ui_text_ellipsize(
            text,
            text->regular,
            progress.title,
            clipped,
            sizeof(clipped),
            470.0f);

        if (!clipped[0]) {
            snprintf(
                clipped,
                sizeof(clipped),
                "%s",
                progress.title);
        }

        fill =
            488.0f *
            (progress.percent /
             100.0f);

        g_ui_gfx_rect(
            gfx,
            x,
            y,
            520.0f,
            98.0f,
            0xdd000000U);

        g_ui_gfx_rect(
            gfx,
            x,
            y,
            6.0f,
            98.0f,
            0xff44aaffU);

        g_ui_text_draw(
            text,
            gfx,
            text->bold,
            "Achievement Progress",
            x + 20.0f,
            y + 8.0f,
            0xffffffffU);

        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            clipped,
            x + 20.0f,
            y + 34.0f,
            0xffffffffU);

        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            progress.progress,
            x + 20.0f,
            y + 60.0f,
            0xffffffffU);

        g_ui_gfx_rect(
            gfx,
            x + 16.0f,
            y + 84.0f,
            488.0f,
            6.0f,
            0xff333333U);

        if (fill > 0.0f) {
            g_ui_gfx_rect(
                gfx,
                x + 16.0f,
                y + 84.0f,
                fill,
                6.0f,
                0xff44aaffU);
        }
    }
}




static int
install_audio_hook(void)
{
    const uint32_t expected =
        0xd63f0040U; /* blr x2 */

    uintptr_t base;
    uintptr_t site;
    uintptr_t island_addr;
    uintptr_t site_page;

    void *island;

    long ps;
    int64_t delta;
    uint32_t patched;

    base =
        get_launcher_base();

    if (!base) {
        fprintf(
            stderr,
            "[RA SOUND] ERROR: "
            "launcher base not found\n");

        return 0;
    }

    /*
     * sdl_audio_cb:
     *
     *   +0xA014  mov x1, x19   ; bytes
     *   +0xA018  mov x0, x20   ; PCM stream
     *   +0xA01C  blr x2        ; normal audio fill
     */
    site =
        base + 0xa01c;

    if (*(volatile uint32_t *)site !=
        expected) {

        fprintf(
            stderr,
            "[RA SOUND] REFUSED: "
            "+0xA01C=%08x expected=%08x\n",
            *(volatile uint32_t *)site,
            expected);

        return 0;
    }

    island =
        alloc_near(site);

    if (!island) {
        fprintf(
            stderr,
            "[RA SOUND] ERROR: "
            "no near branch island\n");

        return 0;
    }

    island_addr =
        (uintptr_t)island;

    /*
     * ldr x16, #8
     * br  x16
     * .quad ra_audio_hook_entry
     */
    {
        uint32_t *code =
            (uint32_t *)island;

        uint64_t target =
            (uint64_t)(uintptr_t)
            &ra_audio_hook_entry;

        code[0] =
            0x58000050U;

        code[1] =
            0xd61f0200U;

        memcpy(
            (unsigned char *)island + 8,
            &target,
            sizeof(target));
    }

    __builtin___clear_cache(
        (char *)island,
        (char *)island + 16);

    ps =
        sysconf(_SC_PAGESIZE);

    if (ps <= 0)
        ps = 4096;

    if (mprotect(
            island,
            (size_t)ps,
            PROT_READ |
            PROT_EXEC) != 0) {

        fprintf(
            stderr,
            "[RA SOUND] ERROR: "
            "island mprotect: %s\n",
            strerror(errno));

        return 0;
    }

    delta =
        (int64_t)island_addr -
        (int64_t)site;

    if ((delta & 3) != 0 ||
        delta < -134217728LL ||
        delta > 134217724LL) {

        fprintf(
            stderr,
            "[RA SOUND] ERROR: "
            "island outside BL range\n");

        return 0;
    }

    patched =
        0x94000000U |
        ((uint32_t)(delta >> 2) &
         0x03ffffffU);

    site_page =
        site &
        ~((uintptr_t)ps - 1);

    if (mprotect(
            (void *)site_page,
            (size_t)ps,
            PROT_READ |
            PROT_WRITE |
            PROT_EXEC) != 0) {

        fprintf(
            stderr,
            "[RA SOUND] ERROR: "
            "site mprotect: %s\n",
            strerror(errno));

        return 0;
    }

    *(volatile uint32_t *)site =
        patched;

    __builtin___clear_cache(
        (char *)site,
        (char *)site + 4);

    mprotect(
        (void *)site_page,
        (size_t)ps,
        PROT_READ |
        PROT_EXEC);

    fprintf(
        stderr,
        "[RA SOUND] audio mix hook installed "
        "at +0xA01C island=%p\n",
        island);

    return 1;
}


void ra_popup_swap_dispatch(
    void *window,
    uintptr_t caller_sp)
{
    UiGfx *gfx =
        (UiGfx *)(caller_sp + 0x240);

    UiText *text =
        (UiText *)(caller_sp + 0x2c0);

    struct timespec now;

    char title[RA_POPUP_TITLE_MAX];
    char display_title[RA_POPUP_TITLE_MAX];

    char subtitle[RA_POPUP_TITLE_MAX];

    unsigned kind =
        RA_POPUP_KIND_ACHIEVEMENT;

    unsigned points = 0;

    int have_popup = 0;
    int started_now = 0;
    int badge_expected = 0;

    int w = 0;
    int h = 0;

    GLuint badge_tex = 0;
    GLuint expired_tex = 0;

    unsigned char *upload_rgba = NULL;
    unsigned char *expired_rgba = NULL;

    int upload_w = 0;
    int upload_h = 0;

    uint64_t serial = 0;

    long long age_ms = 0;

    float panel_x = RA_POPUP_PANEL_X;
    float visibility = 1.0f;

    memset(title, 0, sizeof(title));
    memset(display_title, 0, sizeof(display_title));
    memset(subtitle, 0, sizeof(subtitle));

    if (g_get_window_size)
        g_get_window_size(
            window,
            &w,
            &h);

    clock_gettime(
        CLOCK_MONOTONIC,
        &now);

    pthread_mutex_lock(&g_lock);

    if (g_current_active) {
        age_ms =
            ts_ms(&now) -
            ts_ms(&g_current_started);

        if (age_ms >= RA_POPUP_DURATION_MS) {
            expired_tex =
                g_current.badge_tex;

            expired_rgba =
                g_current.badge_rgba;

            memset(&g_current,
                   0,
                   sizeof(g_current));

            g_current_active = 0;
        }
    }

    if (!g_current_active &&
        g_qcount > 0) {

        g_current =
            g_queue[g_qhead];

        memset(&g_queue[g_qhead],
               0,
               sizeof(g_queue[g_qhead]));

        g_qhead =
            (g_qhead + 1) %
            RA_POPUP_QUEUE_CAP;

        --g_qcount;

        g_current_started = now;
        g_current_active = 1;
        started_now = 1;
        age_ms = 0;
    }

    if (g_current_active) {
        serial = g_current.serial;

        snprintf(title,
                 sizeof(title),
                 "%s",
                 g_current.title);

        kind =
            g_current.kind;

        snprintf(
            subtitle,
            sizeof(subtitle),
            "%s",
            g_current.subtitle);

        points = g_current.points;

        badge_tex =
            g_current.badge_tex;

        badge_expected =
            g_current.badge_url[0] != '\0';

        if (!badge_tex &&
            g_current.badge_rgba) {

            upload_rgba =
                g_current.badge_rgba;

            upload_w =
                g_current.badge_w;

            upload_h =
                g_current.badge_h;

            g_current.badge_rgba = NULL;
            g_current.badge_w = 0;
            g_current.badge_h = 0;
        }

        have_popup = 1;
    }

    pthread_mutex_unlock(&g_lock);

    /*
     * GL cleanup and upload stay on the render thread.
     */
    if (expired_tex)
        g_ui_gfx_delete_tex(expired_tex);

    if (expired_rgba)
        g_stbi_image_free(expired_rgba);

    if (upload_rgba) {
        GLuint tex =
            g_ui_gfx_upload_rgba(
                upload_rgba,
                upload_w,
                upload_h);

        g_stbi_image_free(upload_rgba);

        if (tex) {
            pthread_mutex_lock(&g_lock);

            if (g_current_active &&
                g_current.serial == serial &&
                !g_current.badge_tex) {

                g_current.badge_tex = tex;
                badge_tex = tex;
                tex = 0;
            }

            pthread_mutex_unlock(&g_lock);

            if (tex)
                g_ui_gfx_delete_tex(tex);
        }

        fprintf(stderr,
                "[RA BADGE] texture upload %s\n",
                badge_tex ?
                    "successful" :
                    "failed");
    }

    if (started_now) {
        if (kind ==
            RA_POPUP_KIND_GAME_STATUS) {

            fprintf(
                stderr,
                "[RA STARTUP] showing: \"%s\" "
                "\"%s\"\n",
                title,
                subtitle);
        }
        else {
            /*
             * Stage 7D.4:
             * play the unlock sound when this queued achievement
             * actually becomes visible.
             */
            ra_sound_trigger();

            fprintf(
                stderr,
                "[RA POPUP] showing: \"%s\" "
                "(%u points)\n",
                title,
                points);
        }
    }

    if (have_popup &&
        w > 0 &&
        h > 0 &&
        gfx &&
        text &&
        text->regular &&
        text->bold) {

        char points_text[RA_POPUP_TITLE_MAX];

        const char *header_text;

        float text_x;
        float max_title_width;

        float panel_w =
            RA_POPUP_PANEL_W;

        float panel_h =
            RA_POPUP_PANEL_H;

        float panel_off_x =
            RA_POPUP_PANEL_OFF_X;

        unsigned int alpha;
        unsigned int white;
        unsigned int accent;
        unsigned int background;

        /*
         * Select the three lines before calculating geometry.
         */
        if (kind ==
            RA_POPUP_KIND_GAME_STATUS) {

            header_text =
                "RetroAchievements";

            snprintf(
                points_text,
                sizeof(points_text),
                "%s",
                subtitle);
        }
        else {
            header_text =
                "Achievement Unlocked!";

            snprintf(
                points_text,
                sizeof(points_text),
                "+%u point%s",
                points,
                points == 1 ? "" : "s");
        }

        /*
         * Stage 7D.3:
         *
         * Auto-size all timed RetroAchievements notifications
         * using NNDDSS's native text metrics.
         *
         * The existing RA_POPUP_PANEL_W remains the maximum,
         * so unusually long titles still ellipsize safely.
         */
        if (g_ui_text_width) {

            float header_w =
                g_ui_text_width(
                    text,
                    text->bold,
                    header_text);

            float title_w =
                g_ui_text_width(
                    text,
                    text->regular,
                    title);

            float subtitle_w =
                g_ui_text_width(
                    text,
                    text->regular,
                    points_text);

            float widest_text =
                header_w;

            float desired_w;

            if (title_w > widest_text)
                widest_text = title_w;

            if (subtitle_w > widest_text)
                widest_text = subtitle_w;

            desired_w =
                (badge_expected ?
                    130.0f :
                    22.0f) +
                widest_text +
                24.0f;

            /*
             * Preserve the proven old width as the maximum.
             */
            if (desired_w > 0.0f &&
                desired_w < panel_w) {

                panel_w =
                    desired_w;
            }

            /*
             * Match the startup notification geometry.
             */
            panel_h =
                badge_expected ?
                    120.0f :
                    108.0f;

            /*
             * Slide completely offscreen using the actual width.
             */
            panel_off_x =
                -(panel_w + 8.0f);

            if (started_now) {
                if (kind ==
                    RA_POPUP_KIND_GAME_STATUS) {

                    fprintf(
                        stderr,
                        "[RA STARTUP] panel size "
                        "%.0fx%.0f text=%.0f badge=%d\n",
                        panel_w,
                        panel_h,
                        widest_text,
                        badge_expected ? 1 : 0);
                }
                else {
                    fprintf(
                        stderr,
                        "[RA POPUP] panel size "
                        "%.0fx%.0f text=%.0f badge=%d\n",
                        panel_w,
                        panel_h,
                        widest_text,
                        badge_expected ? 1 : 0);
                }
            }
        }

        /*
         * 0-250ms:
         *   cubic ease-out from left + fade in
         *
         * 250-3750ms:
         *   stationary
         *
         * 3750-4000ms:
         *   cubic ease-in to left + fade out
         */
        if (age_ms < RA_POPUP_SLIDE_IN_MS) {
            float t =
                ra_clamp01(
                    (float)age_ms /
                    (float)RA_POPUP_SLIDE_IN_MS);

            float inv = 1.0f - t;

            float eased =
                1.0f -
                (inv * inv * inv);

            panel_x =
                panel_off_x +
                (RA_POPUP_PANEL_X -
                 panel_off_x) *
                eased;

            visibility = t;
        }
        else if (age_ms >
                 RA_POPUP_DURATION_MS -
                 RA_POPUP_SLIDE_OUT_MS) {

            float t =
                ra_clamp01(
                    (float)(
                        age_ms -
                        (RA_POPUP_DURATION_MS -
                         RA_POPUP_SLIDE_OUT_MS)) /
                    (float)RA_POPUP_SLIDE_OUT_MS);

            float eased =
                t * t * t;

            panel_x =
                RA_POPUP_PANEL_X +
                (panel_off_x -
                 RA_POPUP_PANEL_X) *
                eased;

            visibility =
                1.0f - t;
        }
        else {
            panel_x =
                RA_POPUP_PANEL_X;

            visibility = 1.0f;
        }

        alpha =
            (unsigned int)(
                255.0f *
                ra_clamp01(visibility));

        white =
            ra_color_alpha(
                0xffffffffU,
                alpha);

        accent =
            ra_color_alpha(
                0xff44aaffU,
                alpha);

        /*
         * Original panel alpha is 0xdd.
         * Scale it by animation visibility.
         */
        background =
            ra_color_alpha(
                0xdd000000U,
                (unsigned int)(
                    221.0f *
                    ra_clamp01(visibility)));

        /*
         * Reserve badge space from the start if the event
         * has a badge URL. This prevents the text jumping
         * sideways when the asynchronous badge arrives.
         */
        text_x =
            panel_x +
            (badge_expected ?
                130.0f :
                22.0f);

        max_title_width =
            panel_w -
            (text_x - panel_x) -
            24.0f;

        /*
         * NNDDSS handles UTF-8 boundaries itself and adds "...".
         */
        g_ui_text_ellipsize(
            text,
            text->regular,
            title,
            display_title,
            sizeof(display_title),
            max_title_width);

        if (!display_title[0]) {
            snprintf(display_title,
                     sizeof(display_title),
                     "%s",
                     title);
        }

        g_ui_gfx_begin(
            gfx,
            w,
            h);

        g_ui_gfx_rect(
            gfx,
            panel_x,
            RA_POPUP_PANEL_Y,
            panel_w,
            panel_h,
            background);

        g_ui_gfx_rect(
            gfx,
            panel_x,
            RA_POPUP_PANEL_Y,
            6.0f,
            panel_h,
            accent);

        if (badge_tex) {
            g_ui_gfx_rect_tex(
                gfx,
                panel_x + 14.0f,
                RA_POPUP_PANEL_Y + 12.0f,
                96.0f,
                96.0f,
                badge_tex,
                white);
        }

        g_ui_text_draw(
            text,
            gfx,
            text->bold,
            header_text,
            text_x,
            RA_POPUP_PANEL_Y + 12.0f,
            white);

        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            display_title,
            text_x,
            RA_POPUP_PANEL_Y + 44.0f,
            white);

        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            points_text,
            text_x,
            RA_POPUP_PANEL_Y + 76.0f,
            white);
    }

    /*
     * Persistent RA indicators are independent of
     * the timed achievement popup queue.
     */
    ra_draw_indicators(
        gfx,
        text,
        w,
        h);

    if (g_original_swap)
        g_original_swap(window);
}


__attribute__((constructor))
static void ra_popup_badge_init(void)
{
    pthread_t thread;

    fprintf(stderr,
            "[RA BADGE] Stage 6B renderer starting\n");

    if (!install_hook())
        return;

    ra_sound_init();

    if (!install_audio_hook()) {
        fprintf(
            stderr,
            "[RA SOUND] unlock sound disabled - "
            "popup renderer remains active\n");
    }

    if (pthread_create(
            &thread,
            NULL,
            badge_worker,
            NULL) != 0) {

        fprintf(stderr,
                "[RA BADGE] ERROR: "
                "worker thread creation failed\n");
        return;
    }

    pthread_detach(thread);

    /*
     * Synthetic indicator test only.
     */
    {
        const char *test =
            getenv("RA_TEST_INDICATORS");

        if (test &&
            strcmp(test, "1") == 0) {

            ra_indicator_challenge_show(
                900001,
                "Defeat Dracula Without Using Any Healing Items",
                NULL);

            ra_indicator_progress_update(
                900002,
                "Defeat 100 Enemies With The Vampire Killer",
                "42 / 100",
                42.0f);
        }
    }

    /*
     * Optional local visual test. Does not submit or unlock
     * anything on RetroAchievements.
     */
    {
        const char *test =
            getenv("RA_TEST_POPUP");

        if (test &&
            strcmp(test, "1") == 0) {

            ra_popup_enqueue_achievement_badged(
                "This Is An Intentionally Very Long Achievement Title To Verify Smooth Ellipsis Handling On The RG DS Plus",
                10,
                NULL);
        }
    }
}
