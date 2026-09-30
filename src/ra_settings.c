#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <link.h>


#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif


/*
 * Only the fields at the beginning of UiSettingsState are
 * required by Stage 7A.
 *
 * Full native structure is 65580 bytes.
 */
typedef struct {
    int tab;            /* +0 */
    int focus;          /* +4 */
    int side_focus;     /* +8 */
} UiSettingsState;


/*
 * Native NNDDSS UiNav.
 */
typedef struct {
    int mx;
    int my;

    bool click;
    bool rclick;
    bool confirm;
    bool cancel;
    bool up;
    bool down;
    bool left;
    bool right;
    bool key_any;
} UiNav;


typedef struct {
    float x;
    float y;
    float w;
    float h;
} UiRect;


typedef enum {
    UI_BTN_NONE = 0,
    UI_BTN_HOVER = 1,
    UI_BTN_PRESSED = 2
} UiBtnState;


/*
 * Known beginning of native UiText.
 */
typedef struct {
    void *regular;       /* +0 */
    void *bold;          /* +8 */
    void *title;         /* +16 */
    void *brand;         /* +24 */
    void *brand_title;   /* +32 */
    void *cjk;           /* +40 */
    void *cjk_bold;      /* +48 */
    void *cjk_title;     /* +56 */
} UiText;


/*
 * UiConfig, UiAssets and UiGfx remain opaque.
 */
typedef void UiConfig;
typedef void UiAssets;
typedef void UiGfx;


/*
 * Native launcher functions.
 */
typedef int (*ui_settings_frame_fn)(
    UiSettingsState *s,
    UiConfig *cfg,
    const char *shaders_dir,
    UiAssets *assets,
    UiGfx *gfx,
    UiText *text,
    const UiNav *nav,
    int w,
    int h,
    bool input,
    void (*on_graphics)(void *),
    void *on_graphics_ud,
    void (*on_ffwd)(void *),
    void *on_ffwd_ud);


typedef bool (*ui_button_fn)(
    UiGfx *gfx,
    UiText *text,
    UiRect rect,
    const char *label,
    UiBtnState state);


typedef void (*ui_text_draw_fn)(
    UiText *text,
    UiGfx *gfx,
    void *font,
    const char *str,
    float x,
    float y,
    unsigned color);

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



typedef void (*ui_gfx_rect_fn)(
    UiGfx *gfx,
    float x,
    float y,
    float w,
    float h,
    unsigned color);


static uintptr_t g_launcher_base;

static ui_settings_frame_fn
    g_original_settings;

static ui_button_fn
    g_ui_button;

static ui_text_draw_fn
    g_ui_text_draw;

static ui_text_width_fn
    g_ui_text_width;

static ui_text_ellipsize_fn
    g_ui_text_ellipsize;


static ui_gfx_rect_fn
    g_ui_gfx_rect;


/*
 * Stage 7A private navigation state.
 *
 * We deliberately do NOT write synthetic focus IDs into
 * NNDDSS's UiSettingsState.
 */
static UiSettingsState *g_owner;

static int g_ra_row_selected;
static int g_ra_page_open;

/*
 * Stage 7E.7C:
 * Set by the direct pause-menu shortcut before entering the
 * native Settings mode. This survives UiSettingsState owner
 * initialization, which normally clears g_ra_page_open.
 */
static int g_ra_pause_open_pending;

static int g_ra_page_focus;


#define RA_ACH_PAGE_MAX_ENTRIES 256
#define RA_ACH_PAGE_VISIBLE      14

/*
 * Internal synthetic achievement created by rcheevos when the
 * emulator is not recognized for hardcore support.
 *
 * Matches RC_CLIENT_ACHIEVEMENT_WARNING_ID in rc_client.c.
 * This is not a real game achievement and must not appear in
 * game achievement totals or the achievement list.
 */
#define RA_ACHIEVEMENT_WARNING_ID 101000001U

/*
 * Stage 7G.2:
 * Confirmed by 7G.1 against live Contra 4 achievements.
 *
 * Matches:
 * RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE = 6
 */
#define RA_ACH_BUCKET_ACTIVE_CHALLENGE 6U

static int g_ra_achievement_page_open;
static int g_ra_achievement_focus;
static int g_ra_achievement_scroll;

static size_t g_ra_achievement_count;

/*
 * Number of Active Challenge entries currently promoted to the
 * beginning of g_ra_achievement_entries.
 */
static size_t g_ra_active_challenge_count;

static uint32_t g_ra_achievement_total;
static uint32_t g_ra_achievement_unlocked;

static uint32_t g_ra_achievement_total_points;
static uint32_t g_ra_achievement_unlocked_points;

/*
 * Selected-achievement description marquee.
 */
static int
    g_ra_description_focus = -1;

static uint64_t
    g_ra_description_epoch_ns;




/* ------------------------------------------------------------------------- */
/* Stage 7C.2 onscreen keyboard                                              */
/* ------------------------------------------------------------------------- */

#define RA_LOGIN_USER_MAX   64
#define RA_LOGIN_PASS_MAX   128
#define RA_LOGIN_STATUS_MAX 256

static int g_ra_login_page_open;
static int g_ra_login_focus;

static int g_ra_keyboard_open;
static int g_ra_keyboard_target;
static int g_ra_keyboard_row;
static int g_ra_keyboard_col;
static int g_ra_keyboard_shift;
static int g_ra_keyboard_symbols;

static char g_ra_login_user[
    RA_LOGIN_USER_MAX];

static char g_ra_login_pass[
    RA_LOGIN_PASS_MAX];

static char g_ra_login_status[
    RA_LOGIN_STATUS_MAX];



/*
 * Persistent RetroAchievements UI options.
 *
 * These default ON when ra.cfg does not yet exist.
 */
#define RA_CONFIG_PATH \
    "/mnt/vendor/deep/nnddss/ra.cfg"

#define RA_CONFIG_TMP_PATH \
    "/mnt/vendor/deep/nnddss/ra.cfg.tmp"

static int g_ra_cfg_loaded;

/*
 * Stage 7H.6:
 * Persistent Hardcore preference.
 *
 * Default ON preserves rcheevos' normal Hardcore default and our
 * proven 7H.3/7H.4/7H.5 behavior when upgrading an existing install.
 */
static int g_ra_hardcore_mode = 1;

static int g_ra_achievement_popups = 1;
static int g_ra_challenge_indicators = 1;
static int g_ra_progress_indicators = 1;


static void ra_cfg_load(void)
{
    FILE *fp;
    char line[256];

    if (g_ra_cfg_loaded)
        return;

    g_ra_cfg_loaded = 1;

    /*
     * Safe defaults for first run or malformed config.
     */
    g_ra_hardcore_mode = 1;
    g_ra_achievement_popups = 1;
    g_ra_challenge_indicators = 1;
    g_ra_progress_indicators = 1;

    fp = fopen(RA_CONFIG_PATH, "r");

    if (!fp) {
        fprintf(
            stderr,
            "[RA SETTINGS] no ra.cfg; using defaults\n");

        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        int value;

        if (sscanf(
                line,
                "hardcore_mode=%d",
                &value) == 1) {

            g_ra_hardcore_mode =
                value ? 1 : 0;

            continue;
        }

        if (sscanf(
                line,
                "achievement_popups=%d",
                &value) == 1) {

            g_ra_achievement_popups =
                value ? 1 : 0;

            continue;
        }

        if (sscanf(
                line,
                "challenge_indicators=%d",
                &value) == 1) {

            g_ra_challenge_indicators =
                value ? 1 : 0;

            continue;
        }

        if (sscanf(
                line,
                "progress_indicators=%d",
                &value) == 1) {

            g_ra_progress_indicators =
                value ? 1 : 0;

            continue;
        }
    }

    fclose(fp);

    fprintf(
        stderr,
        "[RA SETTINGS] loaded config "
        "hardcore=%d popups=%d challenge=%d progress=%d\n",
        g_ra_hardcore_mode,
        g_ra_achievement_popups,
        g_ra_challenge_indicators,
        g_ra_progress_indicators);
}



/*
 * Runtime getters used by the RA event and renderer libraries.
 *
 * These are intentionally exported so the other preload libraries
 * can resolve them with dlsym(RTLD_DEFAULT, ...).
 */
/*
 * Stage 7H.6:
 * Startup preference consumed by the live preload.
 *
 * This is a preference for the NEXT NNDDSS launch. The live module
 * continues to expose ra_hardcore_is_enabled() for the actual
 * rcheevos runtime state.
 */
__attribute__((visibility("default")))
int ra_settings_hardcore_enabled(void)
{
    ra_cfg_load();
    return g_ra_hardcore_mode;
}


__attribute__((visibility("default")))
int ra_settings_achievement_popups_enabled(void)
{
    ra_cfg_load();
    return g_ra_achievement_popups;
}


__attribute__((visibility("default")))
int ra_settings_challenge_indicators_enabled(void)
{
    ra_cfg_load();
    return g_ra_challenge_indicators;
}


__attribute__((visibility("default")))
int ra_settings_progress_indicators_enabled(void)
{
    ra_cfg_load();
    return g_ra_progress_indicators;
}


static int ra_cfg_save(void)
{
    FILE *fp;

    fp = fopen(RA_CONFIG_TMP_PATH, "w");

    if (!fp) {
        fprintf(
            stderr,
            "[RA SETTINGS] ERROR: cannot write %s: %s\n",
            RA_CONFIG_TMP_PATH,
            strerror(errno));

        return 0;
    }

    fprintf(
        fp,
        "hardcore_mode=%d\n"
        "achievement_popups=%d\n"
        "challenge_indicators=%d\n"
        "progress_indicators=%d\n",
        g_ra_hardcore_mode,
        g_ra_achievement_popups,
        g_ra_challenge_indicators,
        g_ra_progress_indicators);

    if (fflush(fp) != 0) {
        fprintf(
            stderr,
            "[RA SETTINGS] ERROR: fflush failed: %s\n",
            strerror(errno));

        fclose(fp);
        unlink(RA_CONFIG_TMP_PATH);
        return 0;
    }

    if (fsync(fileno(fp)) != 0) {
        fprintf(
            stderr,
            "[RA SETTINGS] ERROR: fsync failed: %s\n",
            strerror(errno));

        fclose(fp);
        unlink(RA_CONFIG_TMP_PATH);
        return 0;
    }

    if (fclose(fp) != 0) {
        fprintf(
            stderr,
            "[RA SETTINGS] ERROR: fclose failed: %s\n",
            strerror(errno));

        unlink(RA_CONFIG_TMP_PATH);
        return 0;
    }

    if (rename(
            RA_CONFIG_TMP_PATH,
            RA_CONFIG_PATH) != 0) {

        fprintf(
            stderr,
            "[RA SETTINGS] ERROR: rename failed: %s\n",
            strerror(errno));

        unlink(RA_CONFIG_TMP_PATH);
        return 0;
    }

    fprintf(
        stderr,
        "[RA SETTINGS] saved config "
        "hardcore=%d popups=%d challenge=%d progress=%d\n",
        g_ra_hardcore_mode,
        g_ra_achievement_popups,
        g_ra_challenge_indicators,
        g_ra_progress_indicators);

    return 1;
}


/*
 * Launcher RVAs.
 */
#define RVA_UI_SETTINGS_FRAME  0x31900
#define RVA_UI_BUTTON          0x155e0
#define RVA_UI_TEXT_DRAW       0x15310
#define RVA_UI_TEXT_WIDTH      0x14fb0
#define RVA_UI_TEXT_ELLIPSIZE  0x15190
#define RVA_UI_GFX_RECT        0x14370


typedef struct {
    uintptr_t rva;
    uint32_t expected;
} SettingsCallSite;


/*
 * All four native calls to ui_settings_frame().
 */
static const SettingsCallSite g_call_sites[] = {
    { 0x36fd8, 0x97ffea4a },
    { 0x37924, 0x97ffe7f7 },
    { 0x3ac8c, 0x97ffdb1d },
    { 0x3add4, 0x97ffdacb }
};


static int find_main_cb(
    struct dl_phdr_info *info,
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



/* ------------------------------------------------------------------------- */
/* Stage 7C account interface                                                */
/* ------------------------------------------------------------------------- */

typedef int (*ra_account_is_logged_in_fn)(void);

typedef int (*ra_account_get_name_fn)(
    char *buffer,
    size_t buffer_size);

typedef int (*ra_account_logout_fn)(void);


/*
 * Stage 7F.3:
 * Flat Rich Presence snapshot exported by the live preload.
 */
typedef int (*ra_rich_presence_snapshot_fn)(
    char *buffer,
    size_t buffer_size,
    int *out_supported);

typedef int (*ra_account_login_fn)(
    const char *username,
    const char *password,
    char *error_buffer,
    size_t error_buffer_size);


/*
 * Stage 7E.1 bridge ABI.
 *
 * This must match RaAchievementSnapshotEntry in the live preload.
 * The UI receives copies only -- never rc_client pointers.
 */
#define RA_ACH_SNAPSHOT_TITLE_MAX       128
#define RA_ACH_SNAPSHOT_DESC_MAX        256
#define RA_ACH_SNAPSHOT_PROGRESS_MAX     24
#define RA_ACH_SNAPSHOT_BADGE_URL_MAX   512

typedef struct {
    uint32_t id;
    uint32_t points;

    float measured_percent;

    uint8_t state;
    uint8_t bucket;
    uint8_t unlocked;
    uint8_t type;

    char title[
        RA_ACH_SNAPSHOT_TITLE_MAX];

    char description[
        RA_ACH_SNAPSHOT_DESC_MAX];

    char measured_progress[
        RA_ACH_SNAPSHOT_PROGRESS_MAX];

    char badge_url[
        RA_ACH_SNAPSHOT_BADGE_URL_MAX];

    char badge_locked_url[
        RA_ACH_SNAPSHOT_BADGE_URL_MAX];
} RaAchievementSnapshotEntry;


typedef int (*ra_achievement_get_snapshot_fn)(
    RaAchievementSnapshotEntry *entries,
    size_t capacity,
    size_t *out_count,
    uint32_t *out_total,
    uint32_t *out_unlocked,
    uint32_t *out_total_points,
    uint32_t *out_unlocked_points);


typedef int (*ra_menu_badge_draw_fn)(
    UiGfx *gfx,
    const char *url,
    float x,
    float y,
    float size);


static RaAchievementSnapshotEntry
    g_ra_achievement_entries[
        RA_ACH_PAGE_MAX_ENTRIES];


static ra_account_is_logged_in_fn
    g_ra_account_is_logged_in;

static ra_account_get_name_fn
    g_ra_account_get_name;

static ra_account_logout_fn
    g_ra_account_logout;

static ra_account_login_fn
    g_ra_account_login;


static ra_rich_presence_snapshot_fn
    g_ra_rich_presence_snapshot;


static ra_achievement_get_snapshot_fn
    g_ra_achievement_get_snapshot;


static ra_menu_badge_draw_fn
    g_ra_menu_badge_draw;

static int
    g_ra_account_bridge_resolved;


static void ra_resolve_account_bridge(void)
{
    if (g_ra_account_bridge_resolved)
        return;

    g_ra_account_bridge_resolved = 1;

    g_ra_account_is_logged_in =
        (ra_account_is_logged_in_fn)
        dlsym(
            RTLD_DEFAULT,
            "ra_account_is_logged_in");

    g_ra_account_get_name =
        (ra_account_get_name_fn)
        dlsym(
            RTLD_DEFAULT,
            "ra_account_get_name");

    g_ra_account_logout =
        (ra_account_logout_fn)
        dlsym(
            RTLD_DEFAULT,
            "ra_account_logout");

    g_ra_account_login =
        (ra_account_login_fn)
        dlsym(
            RTLD_DEFAULT,
            "ra_account_login");


    g_ra_rich_presence_snapshot =
        (ra_rich_presence_snapshot_fn)
        dlsym(
            RTLD_DEFAULT,
            "ra_rich_presence_snapshot");


    g_ra_achievement_get_snapshot =
        (ra_achievement_get_snapshot_fn)
        dlsym(
            RTLD_DEFAULT,
            "ra_achievement_get_snapshot");

    g_ra_menu_badge_draw =
        (ra_menu_badge_draw_fn)
        dlsym(
            RTLD_DEFAULT,
            "ra_menu_badge_draw");

    fprintf(
        stderr,
        "[RA SETTINGS] menu badge bridge draw=%p\n",
        (void *)g_ra_menu_badge_draw);

    fprintf(
        stderr,
        "[RA SETTINGS] achievement bridge snapshot=%p\n",
        (void *)g_ra_achievement_get_snapshot);

    fprintf(
        stderr,
        "[RA SETTINGS] rich presence bridge snapshot=%p\n",
        (void *)g_ra_rich_presence_snapshot);

    fprintf(
        stderr,
        "[RA SETTINGS] account bridge "
        "status=%p name=%p login=%p logout=%p\n",
        (void *)g_ra_account_is_logged_in,
        (void *)g_ra_account_get_name,
        (void *)g_ra_account_login,
        (void *)g_ra_account_logout);
}


static int ra_live_rich_presence(
    char *buffer,
    size_t buffer_size,
    int *out_supported)
{
    if (!buffer ||
        buffer_size == 0)
        return 0;

    buffer[0] = '\0';

    if (out_supported)
        *out_supported = 0;

    ra_resolve_account_bridge();

    if (!g_ra_rich_presence_snapshot)
        return 0;

    return
        g_ra_rich_presence_snapshot(
            buffer,
            buffer_size,
            out_supported);
}


static int ra_live_logged_in(void)
{
    ra_resolve_account_bridge();

    if (g_ra_account_is_logged_in)
        return g_ra_account_is_logged_in() ? 1 : 0;

    /*
     * Compatibility fallback for an older live preload.
     * Stage 7C normally never uses this path.
     */
    {
        const char *token =
            getenv("RA_TOKEN");

        return
            token &&
            token[0] ?
            1 :
            0;
    }
}


static void ra_live_account_name(
    char *buffer,
    size_t buffer_size)
{
    const char *fallback;

    if (!buffer ||
        buffer_size == 0)
        return;

    buffer[0] = '\0';

    ra_resolve_account_bridge();

    if (g_ra_account_get_name &&
        g_ra_account_get_name(
            buffer,
            buffer_size) &&
        buffer[0]) {

        return;
    }

    fallback =
        getenv("RA_USER");

    if (fallback &&
        fallback[0]) {

        snprintf(
            buffer,
            buffer_size,
            "%s",
            fallback);
    }
    else {
        snprintf(
            buffer,
            buffer_size,
            "%s",
            "Not configured");
    }
}



static int ra_live_login(
    const char *username,
    const char *password,
    char *error_buffer,
    size_t error_buffer_size)
{
    ra_resolve_account_bridge();

    if (!g_ra_account_login) {
        if (error_buffer &&
            error_buffer_size) {

            snprintf(
                error_buffer,
                error_buffer_size,
                "%s",
                "Sign In is unavailable");
        }

        fprintf(
            stderr,
            "[RA SETTINGS] login unavailable\n");

        return 0;
    }

    return g_ra_account_login(
        username,
        password,
        error_buffer,
        error_buffer_size);
}


static int ra_live_logout(void)
{
    ra_resolve_account_bridge();

    if (!g_ra_account_logout) {
        fprintf(
            stderr,
            "[RA SETTINGS] logout unavailable\n");

        return 0;
    }

    return
        g_ra_account_logout() ?
        1 :
        0;
}



static int ra_live_achievement_probe(void)
{
    size_t copied = 0;

    uint32_t total = 0;
    uint32_t unlocked = 0;

    uint32_t total_points = 0;
    uint32_t unlocked_points = 0;

    ra_resolve_account_bridge();

    if (!g_ra_achievement_get_snapshot) {
        fprintf(
            stderr,
            "[RA SETTINGS] achievement snapshot unavailable\n");

        return 0;
    }

    if (!g_ra_achievement_get_snapshot(
            NULL,
            0,
            &copied,
            &total,
            &unlocked,
            &total_points,
            &unlocked_points)) {

        fprintf(
            stderr,
            "[RA SETTINGS] achievement snapshot: no loaded game\n");

        return 0;
    }

    fprintf(
        stderr,
        "[RA SETTINGS] achievement snapshot: "
        "%u/%u unlocked, %u/%u points\n",
        unlocked,
        total,
        unlocked_points,
        total_points);

    return 1;
}



static int ra_achievement_refresh(void)
{
    size_t copied = 0;

    memset(
        g_ra_achievement_entries,
        0,
        sizeof(g_ra_achievement_entries));

    g_ra_achievement_count = 0;

    g_ra_achievement_total = 0;
    g_ra_achievement_unlocked = 0;

    g_ra_achievement_total_points = 0;
    g_ra_achievement_unlocked_points = 0;

    ra_resolve_account_bridge();

    if (!g_ra_achievement_get_snapshot) {
        fprintf(
            stderr,
            "[RA SETTINGS] achievement list bridge unavailable\n");

        return 0;
    }

    if (!g_ra_achievement_get_snapshot(
            g_ra_achievement_entries,
            RA_ACH_PAGE_MAX_ENTRIES,
            &copied,
            &g_ra_achievement_total,
            &g_ra_achievement_unlocked,
            &g_ra_achievement_total_points,
            &g_ra_achievement_unlocked_points)) {

        fprintf(
            stderr,
            "[RA SETTINGS] achievement list: no loaded game\n");

        return 0;
    }

    /*
     * rcheevos may inject RC_CLIENT_ACHIEVEMENT_WARNING_ID into
     * the CORE achievement list. It is a zero-point synthetic
     * warning, not a real game achievement.
     */
    {
        size_t read_index;
        size_t write_index = 0;

        for (read_index = 0;
             read_index < copied;
             ++read_index) {

            RaAchievementSnapshotEntry *entry =
                &g_ra_achievement_entries[read_index];

            if (entry->id ==
                RA_ACHIEVEMENT_WARNING_ID) {

                int warning_was_unlocked =
                    entry->unlocked != 0 ||
                    entry->state == 2;

                fprintf(
                    stderr,
                    "[RA SETTINGS] filtered synthetic "
                    "achievement id=%u title=\"%s\"\n",
                    entry->id,
                    entry->title);

                if (g_ra_achievement_total > 0)
                    --g_ra_achievement_total;

                if (warning_was_unlocked &&
                    g_ra_achievement_unlocked > 0) {

                    --g_ra_achievement_unlocked;
                }

                if (g_ra_achievement_total_points >=
                    entry->points) {

                    g_ra_achievement_total_points -=
                        entry->points;
                }

                if (warning_was_unlocked &&
                    g_ra_achievement_unlocked_points >=
                        entry->points) {

                    g_ra_achievement_unlocked_points -=
                        entry->points;
                }

                continue;
            }

            if (write_index != read_index) {
                g_ra_achievement_entries[write_index] =
                    *entry;
            }

            ++write_index;
        }

        copied = write_index;
    }

    /*
     * Stage 7G.2:
     * Stable-partition Active Challenge achievements to the front.
     *
     * Use one temporary entry and memmove() rather than allocating
     * a second RA_ACH_PAGE_MAX_ENTRIES-sized array.
     */
    {
        size_t active_count = 0;
        size_t scan_index;

        for (scan_index = 0;
             scan_index < copied;
             ++scan_index) {

            if (g_ra_achievement_entries[
                    scan_index].bucket !=
                RA_ACH_BUCKET_ACTIVE_CHALLENGE) {
                continue;
            }

            if (scan_index != active_count) {
                RaAchievementSnapshotEntry temp =
                    g_ra_achievement_entries[
                        scan_index];

                memmove(
                    &g_ra_achievement_entries[
                        active_count + 1],
                    &g_ra_achievement_entries[
                        active_count],
                    (scan_index - active_count) *
                        sizeof(
                            g_ra_achievement_entries[0]));

                g_ra_achievement_entries[
                    active_count] = temp;
            }

            ++active_count;
        }

        g_ra_active_challenge_count =
            active_count;
    }

    g_ra_achievement_count = copied;

    fprintf(
        stderr,
        "[RA SETTINGS] achievement list loaded: "
        "%zu copied, %zu active challenge%s, "
        "%u/%u unlocked, %u/%u points\n",
        g_ra_achievement_count,
        g_ra_active_challenge_count,
        g_ra_active_challenge_count == 1 ?
            "" :
            "s",
        g_ra_achievement_unlocked,
        g_ra_achievement_total,
        g_ra_achievement_unlocked_points,
        g_ra_achievement_total_points);

    return 1;
}


static int ra_achievement_entry_unlocked(
    const RaAchievementSnapshotEntry *entry)
{
    if (!entry)
        return 0;

    /*
     * unlocked is the rcheevos softcore/hardcore bitmask.
     * state == 2 is RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED.
     */
    return
        entry->unlocked != 0 ||
        entry->state == 2;
}


static void ra_achievement_clamp_scroll(void)
{
    if (g_ra_achievement_count == 0) {
        g_ra_achievement_focus = 0;
        g_ra_achievement_scroll = 0;
        return;
    }

    if (g_ra_achievement_focus < 0)
        g_ra_achievement_focus = 0;

    if ((size_t)g_ra_achievement_focus >=
        g_ra_achievement_count) {

        g_ra_achievement_focus =
            (int)g_ra_achievement_count - 1;
    }

    if (g_ra_achievement_focus <
        g_ra_achievement_scroll) {

        g_ra_achievement_scroll =
            g_ra_achievement_focus;
    }

    if (g_ra_achievement_focus >=
        g_ra_achievement_scroll +
            RA_ACH_PAGE_VISIBLE) {

        g_ra_achievement_scroll =
            g_ra_achievement_focus -
            RA_ACH_PAGE_VISIBLE +
            1;
    }

    if (g_ra_achievement_scroll < 0)
        g_ra_achievement_scroll = 0;
}


static float ra_panel_width(int w)
{
    float width =
        (float)w - 100.0f;

    /*
     * Avoid spanning both physical displays if the caller
     * supplies the complete dual-panel width.
     */
    if (width > 900.0f)
        width = 900.0f;

    if (width < 300.0f)
        width = 300.0f;

    return width;
}


static void ra_draw_system_entry(
    UiGfx *gfx,
    UiText *text,
    int w,
    int selected)

{
    /*
     * Stage 7E.8C:
     * The old RetroAchievements entry in System Settings
     * is permanently disabled.
     */
    (void)gfx;
    (void)text;
    (void)w;
    (void)selected;
    return;
}




static void ra_login_clear_password(void)
{
    volatile char *p =
        (volatile char *)g_ra_login_pass;

    size_t i;

    for (i = 0;
         i < sizeof(g_ra_login_pass);
         ++i) {

        p[i] = '\0';
    }
}


static void ra_login_prepare(void)
{
    const char *user =
        getenv("RA_USER");

    memset(
        g_ra_login_user,
        0,
        sizeof(g_ra_login_user));

    ra_login_clear_password();

    memset(
        g_ra_login_status,
        0,
        sizeof(g_ra_login_status));

    if (user &&
        user[0]) {

        snprintf(
            g_ra_login_user,
            sizeof(g_ra_login_user),
            "%s",
            user);
    }

    g_ra_login_focus = 0;

    g_ra_keyboard_open = 0;
    g_ra_keyboard_target = 0;
    g_ra_keyboard_row = 0;
    g_ra_keyboard_col = 0;
    g_ra_keyboard_shift = 0;
    g_ra_keyboard_symbols = 0;
}


static char *ra_keyboard_buffer(void)
{
    return
        g_ra_keyboard_target == 0 ?
        g_ra_login_user :
        g_ra_login_pass;
}


static size_t ra_keyboard_buffer_size(void)
{
    return
        g_ra_keyboard_target == 0 ?
        sizeof(g_ra_login_user) :
        sizeof(g_ra_login_pass);
}


static void ra_keyboard_append(
    char value)
{
    char *buffer =
        ra_keyboard_buffer();

    size_t capacity =
        ra_keyboard_buffer_size();

    size_t len =
        strlen(buffer);

    if (!value ||
        len + 1 >= capacity)
        return;

    buffer[len] = value;
    buffer[len + 1] = '\0';
}


static void ra_keyboard_backspace(void)
{
    char *buffer =
        ra_keyboard_buffer();

    size_t len =
        strlen(buffer);

    if (len)
        buffer[len - 1] = '\0';
}


static char ra_keyboard_character(
    int row,
    int col)
{
    static const char *lower[4] = {
        "1234567890",
        "qwertyuiop",
        "asdfghjkl-",
        "zxcvbnm_@."
    };

    static const char *upper[4] = {
        "1234567890",
        "QWERTYUIOP",
        "ASDFGHJKL-",
        "ZXCVBNM_@."
    };

    static const char *symbols[4] = {
        "1234567890",
        "!@#$%^&*()",
        "-_=+[]{};:",
        "'\\\",.<>/?\\\\|"
    };

    if (row < 0 ||
        row >= 4 ||
        col < 0 ||
        col >= 10)
        return '\0';

    if (g_ra_keyboard_symbols)
        return symbols[row][col];

    if (g_ra_keyboard_shift)
        return upper[row][col];

    return lower[row][col];
}


static void ra_keyboard_open_for(
    int target)
{
    g_ra_keyboard_target =
        target ? 1 : 0;

    g_ra_keyboard_open = 1;
    g_ra_keyboard_row = 0;
    g_ra_keyboard_col = 0;
    g_ra_keyboard_shift = 0;
    g_ra_keyboard_symbols = 0;

    memset(
        g_ra_login_status,
        0,
        sizeof(g_ra_login_status));
}


static void ra_keyboard_move_vertical(
    int direction)
{
    if (direction < 0) {
        if (g_ra_keyboard_row == 0) {
            g_ra_keyboard_row = 4;
            g_ra_keyboard_col /= 2;

            if (g_ra_keyboard_col > 4)
                g_ra_keyboard_col = 4;
        }
        else if (g_ra_keyboard_row == 4) {
            g_ra_keyboard_row = 3;
            g_ra_keyboard_col *= 2;

            if (g_ra_keyboard_col > 9)
                g_ra_keyboard_col = 9;
        }
        else {
            --g_ra_keyboard_row;
        }
    }
    else {
        if (g_ra_keyboard_row == 3) {
            g_ra_keyboard_row = 4;
            g_ra_keyboard_col /= 2;

            if (g_ra_keyboard_col > 4)
                g_ra_keyboard_col = 4;
        }
        else if (g_ra_keyboard_row == 4) {
            g_ra_keyboard_row = 0;
            g_ra_keyboard_col *= 2;

            if (g_ra_keyboard_col > 9)
                g_ra_keyboard_col = 9;
        }
        else {
            ++g_ra_keyboard_row;
        }
    }
}


static void ra_keyboard_handle_nav(
    const UiNav *nav)
{
    int columns;

    if (!nav)
        return;

    if (nav->cancel) {
        g_ra_keyboard_open = 0;
        return;
    }

    columns =
        g_ra_keyboard_row == 4 ?
        5 :
        10;

    if (nav->left) {
        --g_ra_keyboard_col;

        if (g_ra_keyboard_col < 0)
            g_ra_keyboard_col =
                columns - 1;
    }

    if (nav->right) {
        ++g_ra_keyboard_col;

        if (g_ra_keyboard_col >= columns)
            g_ra_keyboard_col = 0;
    }

    if (nav->up)
        ra_keyboard_move_vertical(-1);

    if (nav->down)
        ra_keyboard_move_vertical(1);

    if (!nav->confirm)
        return;

    if (g_ra_keyboard_row < 4) {
        ra_keyboard_append(
            ra_keyboard_character(
                g_ra_keyboard_row,
                g_ra_keyboard_col));

        return;
    }

    switch (g_ra_keyboard_col) {
    case 0:
        if (g_ra_keyboard_symbols) {
            g_ra_keyboard_symbols = 0;
            g_ra_keyboard_shift = 0;
        }
        else {
            g_ra_keyboard_shift =
                !g_ra_keyboard_shift;
        }
        break;

    case 1:
        g_ra_keyboard_symbols =
            !g_ra_keyboard_symbols;

        if (g_ra_keyboard_symbols)
            g_ra_keyboard_shift = 0;
        break;

    case 2:
        ra_keyboard_append(' ');
        break;

    case 3:
        ra_keyboard_backspace();
        break;

    case 4:
        g_ra_keyboard_open = 0;
        break;

    default:
        break;
    }
}


static void ra_mask_password(
    char *output,
    size_t output_size)
{
    size_t count;
    size_t i;

    if (!output ||
        output_size == 0)
        return;

    count =
        strlen(g_ra_login_pass);

    if (count >= output_size)
        count =
            output_size - 1;

    for (i = 0;
         i < count;
         ++i) {

        output[i] = '*';
    }

    output[count] = '\0';
}


static void ra_draw_login_page(
    UiGfx *gfx,
    UiText *text,
    int w,
    int h)
{
    UiRect r;

    char username_row[192];
    char password_row[192];
    char masked[RA_LOGIN_PASS_MAX];

    const char *rows[4];

    float width;
    unsigned i;

    if (!gfx ||
        !text ||
        !text->regular ||
        !text->bold)
        return;

    width =
        ra_panel_width(w);

    memset(masked, 0, sizeof(masked));

    ra_mask_password(
        masked,
        sizeof(masked));

    snprintf(
        username_row,
        sizeof(username_row),
        "Username: %s",
        g_ra_login_user[0] ?
            g_ra_login_user :
            "<empty>");

    snprintf(
        password_row,
        sizeof(password_row),
        "Password: %s",
        masked[0] ?
            masked :
            "<empty>");

    rows[0] = username_row;
    rows[1] = password_row;
    rows[2] = "Sign In";
    rows[3] = "Back";

    if (g_ui_gfx_rect) {
        g_ui_gfx_rect(
            gfx,
            20.0f,
            10.0f,
            width + 60.0f,
            (float)h - 20.0f,
            0xee111111U);
    }

    g_ui_text_draw(
        text,
        gfx,
        text->bold,
        "RetroAchievements Sign In",
        50.0f,
        28.0f,
        0xffffffffU);

    for (i = 0;
         i < 4;
         ++i) {

        r.x = 50.0f;
        r.y =
            82.0f +
            ((float)i * 62.0f);

        r.w = width;
        r.h = 50.0f;

        g_ui_button(
            gfx,
            text,
            r,
            rows[i],
            ((int)i ==
             g_ra_login_focus) ?
                UI_BTN_HOVER :
                UI_BTN_NONE);
    }

    if (g_ra_login_status[0]) {
        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            g_ra_login_status,
            50.0f,
            340.0f,
            0xffffffffU);
    }

    g_ui_text_draw(
        text,
        gfx,
        text->regular,
        "A: Select    B: Back",
        50.0f,
        398.0f,
        0xffbbbbbbU);
}


static void ra_draw_keyboard(
    UiGfx *gfx,
    UiText *text,
    int w,
    int h)
{
    float width;
    float key_gap = 4.0f;
    float key_h = 42.0f;
    float char_w;

    int row;
    int col;

    char preview[RA_LOGIN_PASS_MAX];
    char one[2];

    const char *action_labels[5];

    if (!gfx ||
        !text ||
        !text->regular ||
        !text->bold)
        return;

    width =
        ra_panel_width(w);

    char_w =
        (width - (9.0f * key_gap)) /
        10.0f;

    if (g_ui_gfx_rect) {
        g_ui_gfx_rect(
            gfx,
            20.0f,
            10.0f,
            width + 60.0f,
            (float)h - 20.0f,
            0xee111111U);
    }

    g_ui_text_draw(
        text,
        gfx,
        text->bold,
        g_ra_keyboard_target == 0 ?
            "Enter Username" :
            "Enter Password",
        50.0f,
        20.0f,
        0xffffffffU);

    memset(preview, 0, sizeof(preview));

    if (g_ra_keyboard_target == 0) {
        snprintf(
            preview,
            sizeof(preview),
            "%s",
            g_ra_login_user);
    }
    else {
        ra_mask_password(
            preview,
            sizeof(preview));
    }

    if (g_ui_gfx_rect) {
        g_ui_gfx_rect(
            gfx,
            50.0f,
            52.0f,
            width,
            48.0f,
            0xff202020U);
    }

    g_ui_text_draw(
        text,
        gfx,
        text->regular,
        preview[0] ?
            preview :
            "<empty>",
        64.0f,
        64.0f,
        0xffffffffU);

    one[1] = '\0';

    for (row = 0;
         row < 4;
         ++row) {

        for (col = 0;
             col < 10;
             ++col) {

            UiRect r;

            one[0] =
                ra_keyboard_character(
                    row,
                    col);

            r.x =
                50.0f +
                ((float)col *
                 (char_w + key_gap));

            r.y =
                120.0f +
                ((float)row *
                 (key_h + key_gap));

            r.w = char_w;
            r.h = key_h;

            g_ui_button(
                gfx,
                text,
                r,
                one,
                (g_ra_keyboard_row == row &&
                 g_ra_keyboard_col == col) ?
                    UI_BTN_HOVER :
                    UI_BTN_NONE);
        }
    }

    action_labels[0] =
        g_ra_keyboard_symbols ?
            "abc" :
            (g_ra_keyboard_shift ?
                "abc" :
                "ABC");

    action_labels[1] =
        g_ra_keyboard_symbols ?
            "Letters" :
            "#+=";

    action_labels[2] = "Space";
    action_labels[3] = "<-";
    action_labels[4] = "Done";

    {
        float action_w =
            (width - (4.0f * key_gap)) /
            5.0f;

        for (col = 0;
             col < 5;
             ++col) {

            UiRect r;

            r.x =
                50.0f +
                ((float)col *
                 (action_w + key_gap));

            r.y = 304.0f;
            r.w = action_w;
            r.h = 46.0f;

            g_ui_button(
                gfx,
                text,
                r,
                action_labels[col],
                (g_ra_keyboard_row == 4 &&
                 g_ra_keyboard_col == col) ?
                    UI_BTN_HOVER :
                    UI_BTN_NONE);
        }
    }

    g_ui_text_draw(
        text,
        gfx,
        text->regular,
        "D-pad: Move    A: Select    B: Done",
        50.0f,
        390.0f,
        0xffbbbbbbU);
}





/*
 * Smooth per-frame achievement-description marquee.
 *
 * Unlike 7E.5, this does not remove characters from the left.
 * The complete string is drawn at a continuously changing X
 * position and OpenGL scissoring clips it to the footer width.
 */

#define RA_GL_SCISSOR_TEST 0x0c11U
#define RA_GL_SCISSOR_BOX  0x0c10U
#define RA_GL_VIEWPORT     0x0ba2U


typedef void (*ra_gl_enable_fn)(
    unsigned int cap);

typedef void (*ra_gl_disable_fn)(
    unsigned int cap);

typedef void (*ra_gl_scissor_fn)(
    int x,
    int y,
    int width,
    int height);

typedef void (*ra_gl_get_integerv_fn)(
    unsigned int pname,
    int *params);

typedef unsigned char (*ra_gl_is_enabled_fn)(
    unsigned int cap);


typedef struct {
    int was_enabled;
    int previous_box[4];
} RaGlClipState;


static ra_gl_enable_fn
    g_ra_gl_enable;

static ra_gl_disable_fn
    g_ra_gl_disable;

static ra_gl_scissor_fn
    g_ra_gl_scissor;

static ra_gl_get_integerv_fn
    g_ra_gl_get_integerv;

static ra_gl_is_enabled_fn
    g_ra_gl_is_enabled;

static int
    g_ra_gl_clip_resolved;

static int
    g_ra_gl_clip_available;


static uint64_t ra_monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(
            CLOCK_MONOTONIC,
            &ts) != 0) {

        return 0;
    }

    return
        ((uint64_t)ts.tv_sec *
         1000000000ULL) +
        (uint64_t)ts.tv_nsec;
}


static int ra_resolve_gl_clip(void)
{
    if (g_ra_gl_clip_resolved)
        return g_ra_gl_clip_available;

    g_ra_gl_clip_resolved = 1;

    g_ra_gl_enable =
        (ra_gl_enable_fn)
        dlsym(
            RTLD_NEXT,
            "glEnable");

    g_ra_gl_disable =
        (ra_gl_disable_fn)
        dlsym(
            RTLD_NEXT,
            "glDisable");

    g_ra_gl_scissor =
        (ra_gl_scissor_fn)
        dlsym(
            RTLD_NEXT,
            "glScissor");

    g_ra_gl_get_integerv =
        (ra_gl_get_integerv_fn)
        dlsym(
            RTLD_NEXT,
            "glGetIntegerv");

    g_ra_gl_is_enabled =
        (ra_gl_is_enabled_fn)
        dlsym(
            RTLD_NEXT,
            "glIsEnabled");

    if (!g_ra_gl_enable ||
        !g_ra_gl_disable ||
        !g_ra_gl_scissor ||
        !g_ra_gl_get_integerv ||
        !g_ra_gl_is_enabled) {

        fprintf(
            stderr,
            "[RA SETTINGS] smooth marquee: "
            "OpenGL scissor helpers unavailable\n");

        g_ra_gl_clip_available = 0;
        return 0;
    }

    g_ra_gl_clip_available = 1;

    fprintf(
        stderr,
        "[RA SETTINGS] smooth marquee: "
        "OpenGL scissor helpers ready\n");

    return 1;
}


static int ra_begin_horizontal_clip(
    float x,
    float width,
    int ui_width,
    RaGlClipState *state)
{
    int viewport[4] = {
        0, 0, 0, 0
    };

    int clip_x;
    int clip_right;
    int clip_y;
    int clip_w;
    int clip_h;

    float scale_x;

    if (!state ||
        ui_width <= 0 ||
        width <= 0.0f ||
        !ra_resolve_gl_clip()) {

        return 0;
    }

    memset(
        state,
        0,
        sizeof(*state));

    g_ra_gl_get_integerv(
        RA_GL_VIEWPORT,
        viewport);

    if (viewport[2] <= 0 ||
        viewport[3] <= 0) {

        return 0;
    }

    state->was_enabled =
        g_ra_gl_is_enabled(
            RA_GL_SCISSOR_TEST) ?
            1 : 0;

    if (state->was_enabled) {
        g_ra_gl_get_integerv(
            RA_GL_SCISSOR_BOX,
            state->previous_box);
    }

    /*
     * UI coordinates are expressed against the launcher width.
     * Convert them to the active GL viewport width.
     */
    scale_x =
        (float)viewport[2] /
        (float)ui_width;

    clip_x =
        viewport[0] +
        (int)(
            x *
            scale_x +
            0.5f);

    clip_right =
        viewport[0] +
        (int)(
            (x + width) *
            scale_x +
            0.5f);

    clip_y =
        viewport[1];

    clip_w =
        clip_right -
        clip_x;

    clip_h =
        viewport[3];

    if (clip_w < 0)
        clip_w = 0;

    /*
     * If some parent renderer already has scissoring active,
     * intersect our horizontal marquee clip with its box.
     */
    if (state->was_enabled) {
        int old_left =
            state->previous_box[0];

        int old_bottom =
            state->previous_box[1];

        int old_right =
            old_left +
            state->previous_box[2];

        int old_top =
            old_bottom +
            state->previous_box[3];

        int new_right =
            clip_x +
            clip_w;

        if (clip_x < old_left)
            clip_x = old_left;

        if (new_right > old_right)
            new_right = old_right;

        if (new_right < clip_x)
            new_right = clip_x;

        clip_w =
            new_right -
            clip_x;

        clip_y =
            old_bottom;

        clip_h =
            old_top -
            old_bottom;
    }
    else {
        g_ra_gl_enable(
            RA_GL_SCISSOR_TEST);
    }

    g_ra_gl_scissor(
        clip_x,
        clip_y,
        clip_w,
        clip_h);

    return 1;
}


static void ra_end_horizontal_clip(
    const RaGlClipState *state)
{
    if (!state ||
        !g_ra_gl_clip_available) {

        return;
    }

    if (state->was_enabled) {
        g_ra_gl_scissor(
            state->previous_box[0],
            state->previous_box[1],
            state->previous_box[2],
            state->previous_box[3]);
    }
    else {
        g_ra_gl_disable(
            RA_GL_SCISSOR_TEST);
    }
}


static void ra_draw_scrolling_description(
    UiGfx *gfx,
    UiText *text,
    const char *description,
    float x,
    float y,
    float max_width,
    int screen_width)
{
    const double start_hold_seconds =
        1.0;

    const double pixels_per_second =
        60.0;

    const double end_hold_seconds =
        0.8;

    float text_width;
    float max_offset;
    float offset = 0.0f;

    uint64_t now;

    double elapsed;
    double scroll_seconds;

    RaGlClipState clip_state;

    int clip_active = 0;

    if (!description ||
        !description[0] ||
        !gfx ||
        !text ||
        !text->regular) {

        return;
    }

    if (!g_ui_text_width) {
        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            description,
            x,
            y,
            0xffddddddU);

        return;
    }

    text_width =
        g_ui_text_width(
            text,
            text->regular,
            description);

    /*
     * Short descriptions do not scroll.
     */
    if (text_width <= max_width) {
        g_ra_description_focus =
            g_ra_achievement_focus;

        g_ra_description_epoch_ns = 0;

        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            description,
            x,
            y,
            0xffddddddU);

        return;
    }

    max_offset =
        text_width -
        max_width;

    now =
        ra_monotonic_ns();

    if (g_ra_description_focus !=
            g_ra_achievement_focus ||
        g_ra_description_epoch_ns == 0) {

        g_ra_description_focus =
            g_ra_achievement_focus;

        g_ra_description_epoch_ns =
            now;
    }

    elapsed =
        (double)(
            now -
            g_ra_description_epoch_ns) /
        1000000000.0;

    scroll_seconds =
        (double)max_offset /
        pixels_per_second;

    if (elapsed <
        start_hold_seconds) {

        offset = 0.0f;
    }
    else if (elapsed <
             start_hold_seconds +
             scroll_seconds) {

        double moving_time =
            elapsed -
            start_hold_seconds;

        offset =
            (float)(
                moving_time *
                pixels_per_second);

        if (offset > max_offset)
            offset = max_offset;
    }
    else if (elapsed <
             start_hold_seconds +
             scroll_seconds +
             end_hold_seconds) {

        offset =
            max_offset;
    }
    else {
        /*
         * Start the cycle over.
         */
        g_ra_description_epoch_ns =
            now;

        offset = 0.0f;
    }

    /*
     * Restrict the footer horizontally, then draw the complete
     * string shifted by a continuously varying pixel offset.
     */
    clip_active =
        ra_begin_horizontal_clip(
            x,
            max_width,
            screen_width,
            &clip_state);

    if (clip_active) {
        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            description,
            x - offset,
            y,
            0xffddddddU);

        ra_end_horizontal_clip(
            &clip_state);
    }
    else if (g_ui_text_ellipsize) {
        /*
         * Safe fallback if GL clipping cannot be established.
         */
        char clipped[512];

        memset(
            clipped,
            0,
            sizeof(clipped));

        g_ui_text_ellipsize(
            text,
            text->regular,
            description,
            clipped,
            sizeof(clipped),
            max_width);

        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            clipped,
            x,
            y,
            0xffddddddU);
    }
}


static void ra_draw_achievement_page(
    UiGfx *gfx,
    UiText *text,
    int w,
    int h)
{
    char summary[256];
    char range_text[192];
    char footer[512];

    float width;

    unsigned row;

    if (!gfx ||
        !text ||
        !text->regular ||
        !text->bold)
        return;

    width =
        ra_panel_width(w);

    if (g_ui_gfx_rect) {
        g_ui_gfx_rect(
            gfx,
            20.0f,
            10.0f,
            width + 60.0f,
            (float)h - 20.0f,
            0xee111111U);
    }

    g_ui_text_draw(
        text,
        gfx,
        text->bold,
        "Achievements",
        50.0f,
        22.0f,
        0xffffffffU);

    snprintf(
        summary,
        sizeof(summary),
        "%u / %u unlocked    %u / %u points",
        g_ra_achievement_unlocked,
        g_ra_achievement_total,
        g_ra_achievement_unlocked_points,
        g_ra_achievement_total_points);

    g_ui_text_draw(
        text,
        gfx,
        text->regular,
        summary,
        50.0f,
        52.0f,
        0xffbbbbbbU);

    if (g_ra_achievement_count == 0) {
        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            "No achievements are available for the current game.",
            50.0f,
            105.0f,
            0xffffffffU);

        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            "B: Back",
            50.0f,
            (float)h - 36.0f,
            0xffbbbbbbU);

        return;
    }

    ra_achievement_clamp_scroll();

    for (row = 0;
         row < RA_ACH_PAGE_VISIBLE;
         ++row) {

        size_t index =
            (size_t)g_ra_achievement_scroll +
            row;

        const RaAchievementSnapshotEntry *entry;

        UiRect r;

        char label[512];

        const char *badge_url = NULL;

        int unlocked;

        float text_x;

        if (index >=
            g_ra_achievement_count)
            break;

        entry =
            &g_ra_achievement_entries[index];

        unlocked =
            ra_achievement_entry_unlocked(
                entry);

        snprintf(
            label,
            sizeof(label),
            "%s  -  %u pt%s",
            entry->title,
            entry->points,
            entry->points == 1 ?
                "" :
                "s");

        /*
         * Prefer the locked artwork until this achievement is unlocked.
         * Fall back to whichever URL is available.
         */
        if (unlocked) {
            badge_url =
                entry->badge_url[0] ?
                    entry->badge_url :
                    entry->badge_locked_url;
        }
        else {
            badge_url =
                entry->badge_locked_url[0] ?
                    entry->badge_locked_url :
                    entry->badge_url;
        }

        r.x = 50.0f;

        r.y =
            82.0f +
            ((float)row * 40.0f);

        r.w = width;
        r.h = 36.0f;

        /*
         * Preserve NNDDSS's native row/highlight appearance, but don't
         * allow ui_button() to center our achievement label.
         */
        g_ui_button(
            gfx,
            text,
            r,
            "",
            ((int)index ==
             g_ra_achievement_focus) ?
                UI_BTN_HOVER :
                UI_BTN_NONE);

        /*
         * Badge area:
         *
         *   row x = 50
         *   badge x = 54
         *   badge = 32x32
         *   title x = 94
         */
        if (badge_url &&
            badge_url[0]) {

            if (g_ui_gfx_rect) {
                g_ui_gfx_rect(
                    gfx,
                    r.x + 4.0f,
                    r.y + 2.0f,
                    32.0f,
                    32.0f,
                    0xff222222U);
            }

            if (g_ra_menu_badge_draw) {
                g_ra_menu_badge_draw(
                    gfx,
                    badge_url,
                    r.x + 4.0f,
                    r.y + 2.0f,
                    32.0f);
            }

            text_x =
                r.x + 44.0f;
        }
        else {
            text_x =
                r.x + 8.0f;
        }

        /*
         * Explicit text draw gives us true left alignment.
         */
        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            label,
            text_x,
            r.y + 7.0f,
            0xffffffffU);
    }

    /*
     * Stage 7G.2:
     * Visually separate active challenges from the regular list.
     *
     * Active Challenges occupy indexes:
     *
     *     0 .. g_ra_active_challenge_count - 1
     *
     * Draw the divider only when the boundary between Active
     * Challenges and normal achievements is visible.
     */
    if (g_ui_gfx_rect &&
        g_ra_active_challenge_count > 0 &&
        g_ra_active_challenge_count <
            g_ra_achievement_count) {

        size_t first_visible =
            (size_t)g_ra_achievement_scroll;

        size_t last_visible_exclusive =
            first_visible +
            RA_ACH_PAGE_VISIBLE;

        size_t boundary =
            g_ra_active_challenge_count;

        if (last_visible_exclusive >
            g_ra_achievement_count) {

            last_visible_exclusive =
                g_ra_achievement_count;
        }

        if (boundary > first_visible &&
            boundary < last_visible_exclusive) {

            float divider_y =
                82.0f +
                ((float)(
                    boundary -
                    first_visible) *
                 40.0f) -
                2.0f;

            g_ui_gfx_rect(
                gfx,
                50.0f,
                divider_y,
                width,
                1.0f,
                0xff555555U);
        }
    }

    /*
     * Make it obvious that the achievement list continues beyond
     * the currently visible window.
     */
    {
        size_t first =
            (size_t)g_ra_achievement_scroll + 1;

        size_t last =
            (size_t)g_ra_achievement_scroll +
            RA_ACH_PAGE_VISIBLE;

        if (last > g_ra_achievement_count)
            last = g_ra_achievement_count;

        if (g_ra_achievement_scroll > 0 &&
            last < g_ra_achievement_count) {

            snprintf(
                range_text,
                sizeof(range_text),
                "More above    %zu-%zu of %zu    More below",
                first,
                last,
                g_ra_achievement_count);
        }
        else if (g_ra_achievement_scroll > 0) {
            snprintf(
                range_text,
                sizeof(range_text),
                "More above    %zu-%zu of %zu",
                first,
                last,
                g_ra_achievement_count);
        }
        else if (last < g_ra_achievement_count) {
            snprintf(
                range_text,
                sizeof(range_text),
                "%zu-%zu of %zu    More below",
                first,
                last,
                g_ra_achievement_count);
        }
        else {
            snprintf(
                range_text,
                sizeof(range_text),
                "%zu-%zu of %zu",
                first,
                last,
                g_ra_achievement_count);
        }

        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            range_text,
            50.0f,
            (float)h - 84.0f,
            0xff999999U);
    }

    {
        const RaAchievementSnapshotEntry *entry =
            &g_ra_achievement_entries[
                g_ra_achievement_focus];

        if (entry->measured_progress[0] &&
            !ra_achievement_entry_unlocked(
                entry)) {

            snprintf(
                footer,
                sizeof(footer),
                "%s    Progress: %s (%.1f%%)",
                entry->description,
                entry->measured_progress,
                entry->measured_percent);
        }
        else {
            snprintf(
                footer,
                sizeof(footer),
                "%s",
                entry->description);
        }
    }

    ra_draw_scrolling_description(
        gfx,
        text,
        footer,
        50.0f,
        (float)h - 58.0f,
        width - 8.0f,
        w);

    g_ui_text_draw(
        text,
        gfx,
        text->regular,
        "D-pad: Scroll    B: Back",
        50.0f,
        (float)h - 32.0f,
        0xffbbbbbbU);
}


static void ra_draw_page(
    UiGfx *gfx,
    UiText *text,
    int w,
    int h)
{
    char account[256];
    char account_clipped[256];
    char username[128];
    char hardcore[128];
    char popups[128];
    char challenges[128];
    char progress[128];
    char footer[192];

    /*
     * Stage 7F.4 polished Rich Presence header.
     */
    char rich_presence[512];
    char rich_presence_clipped[512];

    const char *rows[7];

    int logged_in;
    int rich_presence_available;
    int rich_presence_supported;

    float width;
    unsigned i;

    ra_cfg_load();

    if (!gfx ||
        !text ||
        !text->regular ||
        !text->bold)
        return;

    logged_in =
        ra_live_logged_in();

    ra_live_account_name(
        username,
        sizeof(username));

    rich_presence[0] = '\0';
    rich_presence_clipped[0] = '\0';
    rich_presence_supported = 0;

    rich_presence_available =
        ra_live_rich_presence(
            rich_presence,
            sizeof(rich_presence),
            &rich_presence_supported);

    if (logged_in) {
        snprintf(
            account,
            sizeof(account),
            "Signed in as %s",
            username[0] ?
                username :
                "Unknown");
    }
    else {
        snprintf(
            account,
            sizeof(account),
            "%s",
            "Not signed in");
    }

    account_clipped[0] = '\0';

    snprintf(
        hardcore,
        sizeof(hardcore),
        "Hardcore Mode: %s",
        g_ra_hardcore_mode ?
            "On" :
            "Off");

    snprintf(
        popups,
        sizeof(popups),
        "Achievement Popups: %s",
        g_ra_achievement_popups ?
            "On" :
            "Off");

    snprintf(
        challenges,
        sizeof(challenges),
        "Challenge Indicators: %s",
        g_ra_challenge_indicators ?
            "On" :
            "Off");

    snprintf(
        progress,
        sizeof(progress),
        "Progress Indicators: %s",
        g_ra_progress_indicators ?
            "On" :
            "Off");

    rows[0] = "Achievements  >";
    rows[1] = hardcore;
    rows[2] =
        logged_in ?
            "Sign Out" :
            "Sign In";
    rows[3] = popups;
    rows[4] = challenges;
    rows[5] = progress;
    rows[6] = "Back";

    width =
        ra_panel_width(w);

    if (g_ui_gfx_rect) {
        g_ui_gfx_rect(
            gfx,
            20.0f,
            10.0f,
            width + 60.0f,
            (float)h - 20.0f,
            0xee111111U);
    }

    g_ui_text_draw(
        text,
        gfx,
        text->bold,
        "RetroAchievements",
        50.0f,
        24.0f,
        0xffffffffU);

    /*
     * Stage 7F.4:
     * Treat Rich Presence and account identity as a proper
     * informational header instead of selectable menu rows.
     */

    if (g_ui_text_ellipsize) {
        g_ui_text_ellipsize(
            text,
            text->regular,
            account,
            account_clipped,
            sizeof(account_clipped),
            width);

        if (rich_presence_available &&
            rich_presence_supported &&
            rich_presence[0]) {

            g_ui_text_ellipsize(
                text,
                text->regular,
                rich_presence,
                rich_presence_clipped,
                sizeof(rich_presence_clipped),
                width);
        }
    }
    else {
        snprintf(
            account_clipped,
            sizeof(account_clipped),
            "%s",
            account);

        if (rich_presence_available &&
            rich_presence_supported &&
            rich_presence[0]) {

            snprintf(
                rich_presence_clipped,
                sizeof(rich_presence_clipped),
                "%s",
                rich_presence);
        }
    }

    if (rich_presence_available &&
        rich_presence_supported &&
        rich_presence_clipped[0]) {

        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            rich_presence_clipped,
            50.0f,
            49.0f,
            0xffddddddU);
    }
    else {
        g_ui_text_draw(
            text,
            gfx,
            text->regular,
            "No Rich Presence for this game",
            50.0f,
            49.0f,
            0xff999999U);
    }

    g_ui_text_draw(
        text,
        gfx,
        text->regular,
        account_clipped,
        50.0f,
        72.0f,
        0xffaaaaaaU);

    if (g_ui_gfx_rect) {
        g_ui_gfx_rect(
            gfx,
            50.0f,
            96.0f,
            width,
            1.0f,
            0xff444444U);
    }

    for (i = 0; i < 7; ++i) {
        UiRect r;

        r.x = 50.0f;
        r.y =
            112.0f +
            ((float)i * 40.0f);

        r.w = width;
        r.h = 36.0f;

        g_ui_button(
            gfx,
            text,
            r,
            rows[i],
            ((int)i ==
             g_ra_page_focus) ?
                UI_BTN_HOVER :
                UI_BTN_NONE);
    }

    switch (g_ra_page_focus) {
    case 0:
        snprintf(
            footer,
            sizeof(footer),
            "%s",
            "A opens the Achievements list");
        break;

    case 1:
        snprintf(
            footer,
            sizeof(footer),
            "%s",
            "A / Left / Right toggles Hardcore; applies after relaunch");
        break;

    case 2:
        snprintf(
            footer,
            sizeof(footer),
            "%s",
            logged_in ?
                "A disconnects this RetroAchievements account" :
                "A opens the RetroAchievements login screen");
        break;

    case 3:
        snprintf(
            footer,
            sizeof(footer),
            "%s",
            "A / Left / Right toggles Achievement Popups");
        break;

    case 4:
        snprintf(
            footer,
            sizeof(footer),
            "%s",
            "A / Left / Right toggles Challenge Indicators");
        break;

    case 5:
        snprintf(
            footer,
            sizeof(footer),
            "%s",
            "A / Left / Right toggles Progress Indicators");
        break;

    default:
        snprintf(
            footer,
            sizeof(footer),
            "%s",
            "A or B returns to Pause");
        break;
    }

    g_ui_text_draw(
        text,
        gfx,
        text->regular,
        footer,
        50.0f,
        398.0f,
        0xffbbbbbbU);
}


/*
 * Call the original NNDDSS settings renderer.
 *
 * While our synthetic RetroAchievements row is selected, temporarily
 * hide NNDDSS's native focus so Firmware Language / Clear Recent does
 * not remain highlighted underneath our own highlighted row.
 *
 * We modify only s->focus on the real state object and restore it
 * immediately after the native frame returns. This avoids making a
 * partial copy of the very large UiSettingsState structure.
 */
static int ra_call_original_settings(
    UiSettingsState *s,
    UiConfig *cfg,
    const char *shaders_dir,
    UiAssets *assets,
    UiGfx *gfx,
    UiText *text,
    const UiNav *nav,
    int w,
    int h,
    bool input,
    void (*on_graphics)(void *),
    void *on_graphics_ud,
    void (*on_ffwd)(void *),
    void *on_ffwd_ud)
{
    int result;
    int saved_focus = 0;
    bool mask_native_focus = false;

    if (!g_original_settings)
        return 0;

    if (s &&
        s->tab == 3 &&
        g_ra_row_selected &&
        !g_ra_page_open) {

        saved_focus = s->focus;
        s->focus = -1;
        mask_native_focus = true;
    }

    result =
        g_original_settings(
            s,
            cfg,
            shaders_dir,
            assets,
            gfx,
            text,
            nav,
            w,
            h,
            input,
            on_graphics,
            on_graphics_ud,
            on_ffwd,
            on_ffwd_ud);

    if (mask_native_focus)
        s->focus = saved_focus;

    return result;
}


/*
 * Main Stage 7A wrapper.
 */
__attribute__((visibility("default")))
int ra_settings_frame_dispatch(
    UiSettingsState *s,
    UiConfig *cfg,
    const char *shaders_dir,
    UiAssets *assets,
    UiGfx *gfx,
    UiText *text,
    const UiNav *nav,
    int w,
    int h,
    bool input,
    void (*on_graphics)(void *),
    void *on_graphics_ud,
    void (*on_ffwd)(void *),
    void *on_ffwd_ud)
{
    int result;
    int just_selected = 0;
    UiNav local_nav;
    const UiNav *pass_nav = nav;

    if (!g_original_settings)
        return 0;

    /*
     * Different frontend/pause settings state instance.
     */
    if (s != g_owner) {
        g_owner = s;
        g_ra_row_selected = 0;
        g_ra_page_open = 0;
        g_ra_page_focus = 0;

        g_ra_achievement_page_open = 0;
        g_ra_achievement_focus = 0;
        g_ra_achievement_scroll = 0;
        g_ra_achievement_count = 0;

        g_ra_login_page_open = 0;
        g_ra_login_focus = 0;
        g_ra_keyboard_open = 0;

        ra_login_clear_password();
    }

    /*
     * Stage 7E.7C:
     *
     * Opening RetroAchievements directly from Pause changes
     * the native pause state to PAUSE_MODE_SETTINGS. On the
     * first frame, NNDDSS may present a different
     * UiSettingsState instance. The normal owner reset above
     * deliberately clears all private RA page state.
     *
     * Re-apply the direct-open request AFTER that reset.
     */
    if (g_ra_pause_open_pending) {

        g_ra_pause_open_pending = 0;

        g_ra_row_selected = 0;

        g_ra_page_open = 1;
        g_ra_page_focus = 0;

        g_ra_achievement_page_open = 0;
        g_ra_achievement_focus = 0;
        g_ra_achievement_scroll = 0;

        g_ra_login_page_open = 0;
        g_ra_login_focus = 0;
        g_ra_keyboard_open = 0;

        ra_login_clear_password();

        fprintf(
            stderr,
            "[RA PAUSE] pending direct-open applied\n");
    }

    /*
     * Stage 7E.8C:
     * Normal Settings is entirely native NNDDSS.
     *
     * Only continue into the RA dispatcher when the Pause
     * shortcut explicitly opened the RA page.
     */
    if (!g_ra_page_open) {

        g_ra_row_selected = 0;

        return g_original_settings(
            s,
            cfg,
            shaders_dir,
            assets,
            gfx,
            text,
            nav,
            w,
            h,
            input,
            on_graphics,
            on_graphics_ud,
            on_ffwd,
            on_ffwd_ud);
    }

    /*
     * Stage 7E.2 achievement list subpage.
     *
     * Consume navigation here so held Confirm from the parent page cannot
     * leak into another action.
     */
    if (g_ra_page_open &&
        g_ra_achievement_page_open) {

        if (nav &&
            input) {

            if (nav->cancel) {
                g_ra_achievement_page_open = 0;
                g_ra_achievement_focus = 0;
                g_ra_achievement_scroll = 0;

                /*
                 * Return focus to the Achievements row.
                 */
                g_ra_page_focus = 0;

                fprintf(
                    stderr,
                    "[RA SETTINGS] Achievements page closed\n");

                ra_draw_page(
                    gfx,
                    text,
                    w,
                    h);

                return 1;
            }

            if (g_ra_achievement_count > 0) {
                if (nav->up) {
                    --g_ra_achievement_focus;

                    if (g_ra_achievement_focus < 0) {
                        g_ra_achievement_focus =
                            (int)g_ra_achievement_count - 1;
                    }

                    ra_achievement_clamp_scroll();
                }

                if (nav->down) {
                    ++g_ra_achievement_focus;

                    if ((size_t)g_ra_achievement_focus >=
                        g_ra_achievement_count) {

                        g_ra_achievement_focus = 0;
                        g_ra_achievement_scroll = 0;
                    }

                    ra_achievement_clamp_scroll();
                }

                /*
                 * Left/right move one visible page at a time.
                 */
                if (nav->left) {
                    g_ra_achievement_focus -=
                        RA_ACH_PAGE_VISIBLE;

                    if (g_ra_achievement_focus < 0)
                        g_ra_achievement_focus = 0;

                    ra_achievement_clamp_scroll();
                }

                if (nav->right) {
                    g_ra_achievement_focus +=
                        RA_ACH_PAGE_VISIBLE;

                    if ((size_t)g_ra_achievement_focus >=
                        g_ra_achievement_count) {

                        g_ra_achievement_focus =
                            (int)g_ra_achievement_count - 1;
                    }

                    ra_achievement_clamp_scroll();
                }
            }
        }

        ra_draw_achievement_page(
            gfx,
            text,
            w,
            h);

        return 1;
    }


    /*
     * Stage 7C.2 Sign In subpage and onscreen keyboard.
     *
     * Consume all navigation here so none of these button edges
     * leak through to the parent RetroAchievements page.
     */
    if (g_ra_page_open &&
        g_ra_login_page_open) {

        if (nav &&
            input) {

            if (g_ra_keyboard_open) {
                ra_keyboard_handle_nav(
                    nav);
            }
            else {
                if (nav->cancel) {
                    g_ra_login_page_open = 0;
                    g_ra_login_focus = 0;

                    ra_login_clear_password();

                    fprintf(
                        stderr,
                        "[RA SETTINGS] Sign In page closed\n");

                    ra_draw_page(
                        gfx,
                        text,
                        w,
                        h);

                    return 1;
                }

                if (nav->up) {
                    --g_ra_login_focus;

                    if (g_ra_login_focus < 0)
                        g_ra_login_focus = 3;
                }

                if (nav->down) {
                    ++g_ra_login_focus;

                    if (g_ra_login_focus > 3)
                        g_ra_login_focus = 0;
                }

                if (nav->confirm) {
                    switch (g_ra_login_focus) {
                    case 0:
                        ra_keyboard_open_for(0);
                        break;

                    case 1:
                        ra_keyboard_open_for(1);
                        break;

                    case 2:
                        memset(
                            g_ra_login_status,
                            0,
                            sizeof(g_ra_login_status));

                        if (!g_ra_login_user[0] ||
                            !g_ra_login_pass[0]) {

                            snprintf(
                                g_ra_login_status,
                                sizeof(g_ra_login_status),
                                "%s",
                                "Username and password are required");
                        }
                        else {
                            fprintf(
                                stderr,
                                "[RA SETTINGS] Sign In submitted for %s\n",
                                g_ra_login_user);

                            snprintf(
                                g_ra_login_status,
                                sizeof(g_ra_login_status),
                                "%s",
                                "Signing in...");

                            {
                                char error[
                                    RA_LOGIN_STATUS_MAX];

                                int ok;

                                memset(
                                    error,
                                    0,
                                    sizeof(error));

                                ok =
                                    ra_live_login(
                                        g_ra_login_user,
                                        g_ra_login_pass,
                                        error,
                                        sizeof(error));

                                /*
                                 * Password is no longer needed after
                                 * the authentication attempt.
                                 */
                                ra_login_clear_password();

                                if (ok &&
                                    ra_live_logged_in()) {

                                    fprintf(
                                        stderr,
                                        "[RA SETTINGS] Sign In successful\n");

                                    g_ra_login_page_open = 0;
                                    g_ra_login_focus = 0;

                                    ra_draw_page(
                                        gfx,
                                        text,
                                        w,
                                        h);

                                    return 1;
                                }

                                snprintf(
                                    g_ra_login_status,
                                    sizeof(g_ra_login_status),
                                    "%s",
                                    error[0] ?
                                        error :
                                        "Sign In failed");

                                fprintf(
                                    stderr,
                                    "[RA SETTINGS] Sign In failed\n");
                            }
                        }
                        break;

                    case 3:
                        g_ra_login_page_open = 0;
                        g_ra_login_focus = 0;

                        ra_login_clear_password();

                        fprintf(
                            stderr,
                            "[RA SETTINGS] Sign In Back selected\n");

                        ra_draw_page(
                            gfx,
                            text,
                            w,
                            h);

                        return 1;

                    default:
                        break;
                    }
                }
            }
        }

        if (g_ra_keyboard_open) {
            ra_draw_keyboard(
                gfx,
                text,
                w,
                h);
        }
        else {
            ra_draw_login_page(
                gfx,
                text,
                w,
                h);
        }

        return 1;
    }


    /*
     * Our dedicated RA page.
     */
    if (g_ra_page_open) {
        if (nav && input) {

            if (nav->cancel) {

                /*
                 * Stage 7E.8D:
                 * Keep the RA page on-screen for this final
                 * settings frame. The Pause wrapper will switch
                 * directly back to the Pause menu immediately
                 * after this dispatcher returns.
                 */
                ra_draw_page(
                    gfx,
                    text,
                    w,
                    h);

                g_ra_page_open = 0;
                g_ra_row_selected = 0;
                g_ra_page_focus = 0;

                fprintf(
                    stderr,
                    "[RA SETTINGS] closing directly to Pause\n");

                return 1;
            }

            if (nav->up) {
                --g_ra_page_focus;

                if (g_ra_page_focus < 0)
                    g_ra_page_focus = 6;
            }

            if (nav->down) {
                ++g_ra_page_focus;

                if (g_ra_page_focus > 6)
                    g_ra_page_focus = 0;
            }

            /*
             * Stage 7B options.
             *
             * A, Left or Right all toggle the selected option.
             */
            /*
             * Stage 7H.6 Hardcore preference.
             *
             * This does not mutate a running rcheevos session.
             * It is consumed before game load on the next process launch.
             */
            if ((nav->confirm ||
                 nav->left ||
                 nav->right) &&
                g_ra_page_focus == 1) {

                g_ra_hardcore_mode =
                    !g_ra_hardcore_mode;

                fprintf(
                    stderr,
                    "[RA SETTINGS] Hardcore preference -> %d "
                    "(applies after relaunch)\n",
                    g_ra_hardcore_mode);

                ra_cfg_save();
            }

            /*
             * Existing visual options shifted down by one row.
             */
            if ((nav->confirm ||
                 nav->left ||
                 nav->right) &&
                g_ra_page_focus >= 3 &&
                g_ra_page_focus <= 5) {

                switch (g_ra_page_focus) {
                case 3:
                    g_ra_achievement_popups =
                        !g_ra_achievement_popups;
                    break;

                case 4:
                    g_ra_challenge_indicators =
                        !g_ra_challenge_indicators;
                    break;

                case 5:
                    g_ra_progress_indicators =
                        !g_ra_progress_indicators;
                    break;

                default:
                    break;
                }

                ra_cfg_save();
            }

            /*
             * Stage 7E.2 achievement list.
             */
            if (nav->confirm &&
                g_ra_page_focus == 0) {

                fprintf(
                    stderr,
                    "[RA SETTINGS] Achievements selected\n");

                g_ra_achievement_page_open = 1;
                g_ra_achievement_focus = 0;
                g_ra_achievement_scroll = 0;

                ra_achievement_refresh();

                ra_draw_achievement_page(
                    gfx,
                    text,
                    w,
                    h);

                return 1;
            }


            /*
             * Stage 7C.1 account action.
             */
            if (nav->confirm &&
                g_ra_page_focus == 2) {

                if (ra_live_logged_in()) {
                    fprintf(
                        stderr,
                        "[RA SETTINGS] Sign Out selected\n");

                    if (!ra_live_logout()) {
                        fprintf(
                            stderr,
                            "[RA SETTINGS] Sign Out reported an error\n");
                    }
                }
                else {
                    fprintf(
                        stderr,
                        "[RA SETTINGS] Sign In selected\n");

                    ra_login_prepare();

                    g_ra_login_page_open = 1;
                    g_ra_login_focus = 0;
                }
            }

            if (nav->confirm &&
                g_ra_page_focus == 6) {

                /*
                 * Stage 7E.8D:
                 * Preserve the current RA frame instead of
                 * falling through into the native System page.
                 */
                ra_draw_page(
                    gfx,
                    text,
                    w,
                    h);

                g_ra_page_open = 0;
                g_ra_row_selected = 0;
                g_ra_page_focus = 0;

                fprintf(
                    stderr,
                    "[RA SETTINGS] Back -> Pause\n");

                return 1;
            }
        }

        if (g_ra_page_open) {
            ra_draw_page(
                gfx,
                text,
                w,
                h);

            /*
             * Nonzero means remain in settings.
             */
            return 1;
        }

        /*
         * Back button was pressed. Render System again without
         * passing the same Confirm edge through to NNDDSS.
         */
        if (nav) {
            local_nav = *nav;
            local_nav.confirm = false;
            local_nav.cancel = false;
            local_nav.up = false;
            local_nav.down = false;
            local_nav.left = false;
            local_nav.right = false;
            local_nav.click = false;
            pass_nav = &local_nav;
        }

        result =
            ra_call_original_settings(
                s,
                cfg,
                shaders_dir,
                assets,
                gfx,
                text,
                pass_nav,
                w,
                h,
                input,
                on_graphics,
                on_graphics_ud,
                on_ffwd,
                on_ffwd_ud);

        if (result) {
            ra_draw_system_entry(
                gfx,
                text,
                w,
                1);
        }

        return result;
    }


    /*
     * Synthetic row is valid only while System tab is active.
     */
    if (!s || s->tab != 3) {
        g_ra_row_selected = 0;
    }


    /*
     * Enter our synthetic row from either end of the native
     * System list:
     *
     *   Firmware Language (7) + Down -> RetroAchievements
     *   Clear Recent      (5) + Up   -> RetroAchievements
     */
    if (s &&
        nav &&
        input &&
        s->tab == 3 &&
        !g_ra_row_selected &&
        ((s->focus == 7 && nav->down) ||
         (s->focus == 5 && nav->up))) {

        g_ra_row_selected = 0;
        just_selected = 1;

        local_nav = *nav;
        local_nav.up = false;
        local_nav.down = false;
        local_nav.confirm = false;

        pass_nav = &local_nav;

        fprintf(
            stderr,
            "[RA SETTINGS] RetroAchievements row selected\n");
    }


    /*
     * Navigation while our synthetic row is selected.
     */
    if (s &&
        nav &&
        input &&
        s->tab == 3 &&
        g_ra_row_selected &&
        !just_selected) {

        if (nav->confirm) {
            g_ra_page_open = 1;
            g_ra_page_focus = 0;

            fprintf(
                stderr,
                "[RA SETTINGS] RetroAchievements page opened\n");

            ra_draw_page(
                gfx,
                text,
                w,
                h);

            return 1;
        }

        if (nav->up) {
            g_ra_row_selected = 0;

            /*
             * Up from RetroAchievements returns to the final
             * native System row: Firmware Language.
             */
            s->focus = 7;

            local_nav = *nav;
            local_nav.up = false;
            local_nav.down = false;
            pass_nav = &local_nav;
        }
        else if (nav->down) {
            /*
             * Natural wrap from our final synthetic row back
             * to the first native System item.
             */
            g_ra_row_selected = 0;
            s->focus = 5;

            local_nav = *nav;
            local_nav.up = false;
            local_nav.down = false;
            pass_nav = &local_nav;
        }
        else if (!pass_nav || pass_nav == nav) {
            /*
             * Prevent left/right/confirm from accidentally
             * modifying Firmware Language underneath our
             * synthetic row.
             *
             * Cancel is intentionally preserved so B still
             * exits Settings normally.
             */
            local_nav = *nav;
            local_nav.confirm = false;
            local_nav.left = false;
            local_nav.right = false;
            local_nav.click = false;
            pass_nav = &local_nav;
        }
    }


    result =
        ra_call_original_settings(
            s,
            cfg,
            shaders_dir,
            assets,
            gfx,
            text,
            pass_nav,
            w,
            h,
            input,
            on_graphics,
            on_graphics_ud,
            on_ffwd,
            on_ffwd_ud);


    if (!result) {
        g_ra_row_selected = 0;
        g_ra_page_open = 0;
        g_ra_page_focus = 0;
        return result;
    }


    /*
     * Draw our extra native-style entry only on System tab.
     */
    if (s && s->tab == 3) {
        ra_draw_system_entry(
            gfx,
            text,
            w,
            g_ra_row_selected);
    }

    return result;
}


/*
 * Allocate executable memory within AArch64 BL range
 * of the launcher.
 */
static void *alloc_branch_island(
    uintptr_t base)
{
    long page_size =
        sysconf(_SC_PAGESIZE);

    uintptr_t off;

    if (page_size <= 0)
        page_size = 4096;

    for (off = 0x00100000;
         off < 0x07000000;
         off += 0x00010000) {

        uintptr_t addr =
            (base + off) &
            ~((uintptr_t)page_size - 1);

        void *p =
            mmap(
                (void *)addr,
                (size_t)page_size,
                PROT_READ |
                PROT_WRITE |
                PROT_EXEC,
                MAP_PRIVATE |
                MAP_ANONYMOUS |
                MAP_FIXED_NOREPLACE,
                -1,
                0);

        if (p != MAP_FAILED)
            return p;
    }

    return NULL;
}


static int encode_bl(
    uintptr_t from,
    uintptr_t to,
    uint32_t *out)
{
    intptr_t delta =
        (intptr_t)to -
        (intptr_t)from;

    intptr_t imm26;

    if ((delta & 3) != 0)
        return 0;

    if (delta < -(1LL << 27) ||
        delta >= (1LL << 27))
        return 0;

    imm26 =
        delta >> 2;

    *out =
        0x94000000U |
        ((uint32_t)imm26 &
         0x03ffffffU);

    return 1;
}


static int patch_word(
    uintptr_t address,
    uint32_t value)
{
    long page_size =
        sysconf(_SC_PAGESIZE);

    uintptr_t page;

    if (page_size <= 0)
        page_size = 4096;

    page =
        address &
        ~((uintptr_t)page_size - 1);

    if (mprotect(
            (void *)page,
            (size_t)page_size,
            PROT_READ |
            PROT_WRITE |
            PROT_EXEC) != 0) {

        fprintf(
            stderr,
            "[RA SETTINGS] mprotect RWX failed: %s\n",
            strerror(errno));

        return 0;
    }

    *(volatile uint32_t *)address =
        value;

    __builtin___clear_cache(
        (char *)address,
        (char *)(address + 4));

    if (mprotect(
            (void *)page,
            (size_t)page_size,
            PROT_READ |
            PROT_EXEC) != 0) {

        fprintf(
            stderr,
            "[RA SETTINGS] warning: "
            "mprotect RX failed: %s\n",
            strerror(errno));
    }

    return 1;
}


static int install_settings_hook(void)
{
    unsigned i;

    uint8_t *island;
    uint32_t branch;

    dl_iterate_phdr(
        find_main_cb,
        NULL);

    if (!g_launcher_base) {
        fprintf(
            stderr,
            "[RA SETTINGS] REFUSED: "
            "launcher base unavailable\n");

        return 0;
    }

    /*
     * Validate every call site before modifying anything.
     */
    for (i = 0;
         i < sizeof(g_call_sites) /
             sizeof(g_call_sites[0]);
         ++i) {

        uintptr_t address =
            g_launcher_base +
            g_call_sites[i].rva;

        uint32_t found =
            *(volatile uint32_t *)address;

        if (found !=
            g_call_sites[i].expected) {

            fprintf(
                stderr,
                "[RA SETTINGS] REFUSED: "
                "+0x%lx=%08x expected=%08x\n",
                (unsigned long)
                    g_call_sites[i].rva,
                found,
                g_call_sites[i].expected);

            return 0;
        }
    }

    g_original_settings =
        (ui_settings_frame_fn)
        (g_launcher_base +
         RVA_UI_SETTINGS_FRAME);

    g_ui_button =
        (ui_button_fn)
        (g_launcher_base +
         RVA_UI_BUTTON);

    g_ui_text_draw =
        (ui_text_draw_fn)
        (g_launcher_base +
         RVA_UI_TEXT_DRAW);

    g_ui_text_width =
        (ui_text_width_fn)
        (g_launcher_base +
         RVA_UI_TEXT_WIDTH);

    g_ui_text_ellipsize =
        (ui_text_ellipsize_fn)
        (g_launcher_base +
         RVA_UI_TEXT_ELLIPSIZE);

    g_ui_gfx_rect =
        (ui_gfx_rect_fn)
        (g_launcher_base +
         RVA_UI_GFX_RECT);


    island =
        alloc_branch_island(
            g_launcher_base);

    if (!island) {
        fprintf(
            stderr,
            "[RA SETTINGS] ERROR: "
            "could not allocate branch island\n");

        return 0;
    }

    /*
     * AArch64:
     *
     *   ldr x16, [pc, #8]
     *   br  x16
     *   .quad ra_settings_frame_dispatch
     */
    *(uint32_t *)(island + 0) =
        0x58000050U;

    *(uint32_t *)(island + 4) =
        0xd61f0200U;

    *(uint64_t *)(island + 8) =
        (uint64_t)(uintptr_t)
        &ra_settings_frame_dispatch;

    __builtin___clear_cache(
        (char *)island,
        (char *)(island + 16));


    for (i = 0;
         i < sizeof(g_call_sites) /
             sizeof(g_call_sites[0]);
         ++i) {

        uintptr_t address =
            g_launcher_base +
            g_call_sites[i].rva;

        if (!encode_bl(
                address,
                (uintptr_t)island,
                &branch)) {

            fprintf(
                stderr,
                "[RA SETTINGS] ERROR: "
                "branch out of range for +0x%lx\n",
                (unsigned long)
                    g_call_sites[i].rva);

            return 0;
        }

        if (!patch_word(
                address,
                branch)) {

            fprintf(
                stderr,
                "[RA SETTINGS] ERROR: "
                "patch failed at +0x%lx\n",
                (unsigned long)
                    g_call_sites[i].rva);

            return 0;
        }
    }

    fprintf(
        stderr,
        "[RA SETTINGS] hook installed "
        "island=%p\n",
        island);

    return 1;
}


__attribute__((constructor))
static void ra_settings_init(void)
{
    fprintf(
        stderr,
        "[RA SETTINGS] Stage 7B starting\n");

    ra_cfg_load();

    install_settings_hook();
}


/* ========================================================================= */
/* Stage 7E.7A - direct pause-menu RetroAchievements shortcut                */
/* ========================================================================= */

/*
 * Native pause layout:
 *
 *   0..4  Save slots
 *   5     Continue
 *   6     Settings
 *   7     Cheats
 *   8     Exit
 *
 * We deliberately DO NOT modify PAUSE_F_COUNT.
 *
 * The RA shortcut is maintained as synthetic wrapper state between
 * native Cheats (7) and native Exit (8).
 */

#define RA_RVA_UI_PAUSE_FRAME 0x388c0

#define RA_PAUSE_MODE_MENU      0
#define RA_PAUSE_MODE_SETTINGS  1

#define RA_PAUSE_F_CHEATS       7
#define RA_PAUSE_F_EXIT         8


/*
 * Only the prefix that we access.
 *
 * DWARF:
 *   mode          +0
 *   selected_slot +4
 *   focus         +8
 *   slot_action   +12
 *   slot_filled   +16
 */
typedef struct {
    int mode;
    int selected_slot;
    int focus;
    int slot_action;

    uint8_t slot_filled;
    uint8_t _pad17;
    uint8_t _pad18;
    uint8_t _pad19;
} RaPauseStatePrefix;


/*
 * Exact UiNav layout from DWARF.
 *
 *   mx       +0
 *   my       +4
 *   click    +8
 *   rclick   +9
 *   confirm  +10
 *   cancel   +11
 *   up       +12
 *   down     +13
 *   left     +14
 *   right    +15
 *   key_any  +16
 *
 * Struct size = 20 due 4-byte alignment.
 */
typedef struct {
    int mx;
    int my;

    uint8_t click;
    uint8_t rclick;
    uint8_t confirm;
    uint8_t cancel;
    uint8_t up;
    uint8_t down;
    uint8_t left;
    uint8_t right;
    uint8_t key_any;

    uint8_t _pad17;
    uint8_t _pad18;
    uint8_t _pad19;
} RaPauseNav;


_Static_assert(
    sizeof(RaPauseNav) == 20,
    "RaPauseNav ABI mismatch");


typedef int (*ra_ui_pause_frame_fn)(
    RaPauseStatePrefix *s,
    UiGfx *gfx,
    UiText *text,
    void *assets,
    const char *game_title,
    int win_w,
    int win_h,
    const RaPauseNav *nav,
    const void *ctx);


typedef struct {
    uintptr_t rva;
    uint32_t expected;
} RaPauseCallSite;


/*
 * Calls observed in sdl_frontend_run().
 */
static const RaPauseCallSite
g_ra_pause_call_sites[] = {
    {
        0x0000bec4,
        0x9400b27f
    },
    {
        0x0000da24,
        0x9400aba7
    }
};


static ra_ui_pause_frame_fn
    g_ra_original_pause;


/*
 * Synthetic shortcut state.
 */
static RaPauseStatePrefix *
    g_ra_pause_owner;

static int
    g_ra_pause_shortcut_selected;

static int
    g_ra_pause_shortcut_session;


/*
 * Simple first-pass row.
 *
 * This is intentionally independent of NNDDSS's internal
 * PAUSE_F_COUNT and choice array. Once controller behavior
 * is verified we can match its native row geometry exactly.
 */
static __attribute__((unused)) void ra_pause_draw_shortcut(
    UiGfx *gfx,
    UiText *text,
    int win_w,
    int win_h,
    int selected)
{
    float logical_w;
    float x;
    float y;
    float w;
    float h;

    unsigned int bg;
    unsigned int fg;

    const char *label;

    if (!gfx ||
        !text ||
        !text->regular ||
        !g_ui_gfx_rect ||
        !g_ui_text_draw ||
        win_w <= 0 ||
        win_h <= 0) {

        return;
    }

    /*
     * NNDDSS may expose the combined dual-screen width.
     * Treat anything wider than ~1200 as two side-by-side
     * panels and place the shortcut on the left panel.
     */
    logical_w =
        (win_w > 1200) ?
        ((float)win_w * 0.5f) :
        (float)win_w;

    x =
        logical_w * 0.04f;

    w =
        logical_w * 0.42f;

    if (w > 430.0f)
        w = 430.0f;

    if (w < 260.0f)
        w = 260.0f;

    h = 42.0f;

    /*
     * Temporary test placement.
     *
     * The hook/navigation behavior is the important part of
     * 7E.7A. We'll use the actual handheld result to match the
     * native Cheats/Exit spacing exactly if necessary.
     */
    y =
        (float)win_h * 0.70f;

    bg =
        selected ?
        0xff424242U :
        0xcc202020U;

    fg =
        selected ?
        0xffffffffU :
        0xffddddddU;

    g_ui_gfx_rect(
        gfx,
        x,
        y,
        w,
        h,
        bg);

    label =
        selected ?
        "> RetroAchievements" :
        "  RetroAchievements";

    g_ui_text_draw(
        text,
        gfx,
        text->regular,
        label,
        x + 14.0f,
        y + 10.0f,
        fg);
}


static void ra_pause_open_ra(
    RaPauseStatePrefix *s)
{
    if (!s)
        return;

    /*
     * Enter native Settings mode, but prime our existing
     * settings-frame hook so it immediately renders the
     * RetroAchievements root page instead.
     */
    g_ra_pause_open_pending = 1;

    g_ra_page_open = 1;
    g_ra_page_focus = 0;

    g_ra_achievement_page_open = 0;
    g_ra_login_page_open = 0;

    g_ra_description_epoch_ns = 0;

    g_ra_pause_shortcut_selected = 0;
    g_ra_pause_shortcut_session = 1;

    s->mode =
        RA_PAUSE_MODE_SETTINGS;

    fprintf(
        stderr,
        "[RA PAUSE] opening RetroAchievements page\n");
}


static int ra_pause_frame_dispatch(
    RaPauseStatePrefix *s,
    UiGfx *gfx,
    UiText *text,
    void *assets,
    const char *game_title,
    int win_w,
    int win_h,
    const RaPauseNav *nav_in,
    const void *ctx)
{
    RaPauseNav nav_local;

    const RaPauseNav *nav =
        nav_in;

    int result;


    if (!g_ra_original_pause)
        return 0;


    /*
     * A new UiPauseState means a new pause session.
     */
    if (s != g_ra_pause_owner) {
        g_ra_pause_owner =
            s;

        g_ra_pause_shortcut_selected =
            0;

        g_ra_pause_shortcut_session =
            0;
    }


    /*
     * If RA was opened through our shortcut, backing out of
     * the RA root page should return directly to the pause
     * menu rather than exposing the nested System settings.
     */
    if (g_ra_pause_shortcut_session &&
        s &&
        s->mode == RA_PAUSE_MODE_SETTINGS &&
        !g_ra_page_open) {

        s->mode =
            RA_PAUSE_MODE_MENU;

        s->focus =
            RA_PAUSE_F_CHEATS;

        g_ra_pause_shortcut_session =
            0;

        g_ra_pause_shortcut_selected =
            1;

        fprintf(
            stderr,
            "[RA PAUSE] returned from RetroAchievements page\n");
    }


    /*
     * We only synthesize navigation while the real pause
     * frame is on its top-level menu.
     */
    if (s &&
        nav_in &&
        s->mode == RA_PAUSE_MODE_MENU) {

        nav_local =
            *nav_in;

        nav =
            &nav_local;


        if (g_ra_pause_shortcut_selected) {

            /*
             * Do not let mouse hover or native confirm/down/up
             * activate the underlying Cheats/Exit rows while
             * our synthetic row owns focus.
             */
            nav_local.mx =
                -100000;

            nav_local.my =
                -100000;

            nav_local.click = 0;
            nav_local.rclick = 0;

            nav_local.confirm = 0;
            nav_local.up = 0;
            nav_local.down = 0;


            if (nav_in->up) {

                g_ra_pause_shortcut_selected =
                    0;

                s->focus =
                    RA_PAUSE_F_CHEATS;

                fprintf(
                    stderr,
                    "[RA PAUSE] shortcut -> Cheats\n");
            }
            else if (nav_in->down) {

                g_ra_pause_shortcut_selected =
                    0;

                s->focus =
                    RA_PAUSE_F_EXIT;

                fprintf(
                    stderr,
                    "[RA PAUSE] shortcut -> Exit\n");
            }
            else if (nav_in->confirm ||
                     nav_in->click) {

                ra_pause_open_ra(
                    s);
            }
            else if (nav_in->cancel) {

                /*
                 * Let the native pause frame process Cancel.
                 */
                g_ra_pause_shortcut_selected =
                    0;

                nav_local.cancel =
                    nav_in->cancel;
            }
        }
        else {

            /*
             * Cheats + Down enters the synthetic RA row.
             */
            if (s->focus ==
                    RA_PAUSE_F_CHEATS &&
                nav_in->down) {

                g_ra_pause_shortcut_selected =
                    1;

                nav_local.down =
                    0;

                nav_local.mx =
                    -100000;

                nav_local.my =
                    -100000;

                fprintf(
                    stderr,
                    "[RA PAUSE] Cheats -> shortcut\n");
            }

            /*
             * Exit + Up enters the same synthetic row.
             */
            else if (s->focus ==
                         RA_PAUSE_F_EXIT &&
                     nav_in->up) {

                g_ra_pause_shortcut_selected =
                    1;

                nav_local.up =
                    0;

                nav_local.mx =
                    -100000;

                nav_local.my =
                    -100000;

                fprintf(
                    stderr,
                    "[RA PAUSE] Exit -> shortcut\n");
            }
        }
    }
    else if (s &&
             s->mode !=
                 RA_PAUSE_MODE_SETTINGS) {

        g_ra_pause_shortcut_selected =
            0;
    }


    result =
        g_ra_original_pause(
            s,
            gfx,
            text,
            assets,
            game_title,
            win_w,
            win_h,
            nav,
            ctx);


    /*
     * The RA dispatcher may have closed the root page during
     * the native settings call we just made. Convert that back
     * into top-level pause mode immediately.
     */
    if (g_ra_pause_shortcut_session &&
        s &&
        s->mode == RA_PAUSE_MODE_SETTINGS &&
        !g_ra_page_open) {

        s->mode =
            RA_PAUSE_MODE_MENU;

        s->focus =
            RA_PAUSE_F_CHEATS;

        g_ra_pause_shortcut_session =
            0;

        g_ra_pause_shortcut_selected =
            1;

        fprintf(
            stderr,
            "[RA PAUSE] RA Back -> pause shortcut\n");
    }


    /*
     * 7E.7B:
     * The RA row is now rendered inside the native pause
     * ui_choice() sequence, while the correct screen and
     * viewport are still active.
     */
    return result;
}


static int ra_install_pause_shortcut_hook(void)
{
    unsigned int i;

    uint8_t *island;
    uint32_t branch;


    if (!g_launcher_base) {
        dl_iterate_phdr(
            find_main_cb,
            NULL);
    }

    if (!g_launcher_base) {
        fprintf(
            stderr,
            "[RA PAUSE] REFUSED: "
            "launcher base unavailable\n");

        return 0;
    }


    /*
     * Validate BOTH native pause calls before touching either.
     */
    for (i = 0;
         i < sizeof(g_ra_pause_call_sites) /
             sizeof(g_ra_pause_call_sites[0]);
         ++i) {

        uintptr_t address =
            g_launcher_base +
            g_ra_pause_call_sites[i].rva;

        uint32_t found =
            *(volatile uint32_t *)address;

        if (found !=
            g_ra_pause_call_sites[i].expected) {

            fprintf(
                stderr,
                "[RA PAUSE] REFUSED: "
                "+0x%lx=%08x expected=%08x\n",
                (unsigned long)
                    g_ra_pause_call_sites[i].rva,
                found,
                g_ra_pause_call_sites[i].expected);

            return 0;
        }
    }


    g_ra_original_pause =
        (ra_ui_pause_frame_fn)
        (g_launcher_base +
         RA_RVA_UI_PAUSE_FRAME);


    /*
     * These are also initialized by the existing settings
     * constructor, but resolve them here too so constructor
     * ordering cannot matter.
     */
    if (!g_ui_gfx_rect) {
        g_ui_gfx_rect =
            (ui_gfx_rect_fn)
            (g_launcher_base +
             RVA_UI_GFX_RECT);
    }

    if (!g_ui_text_draw) {
        g_ui_text_draw =
            (ui_text_draw_fn)
            (g_launcher_base +
             RVA_UI_TEXT_DRAW);
    }


    island =
        alloc_branch_island(
            g_launcher_base);

    if (!island) {
        fprintf(
            stderr,
            "[RA PAUSE] ERROR: "
            "could not allocate branch island\n");

        return 0;
    }


    /*
     * AArch64 branch island:
     *
     *   ldr x16, [pc, #8]
     *   br  x16
     *   .quad ra_pause_frame_dispatch
     */
    *(uint32_t *)(island + 0) =
        0x58000050U;

    *(uint32_t *)(island + 4) =
        0xd61f0200U;

    *(uint64_t *)(island + 8) =
        (uint64_t)(uintptr_t)
        &ra_pause_frame_dispatch;

    __builtin___clear_cache(
        (char *)island,
        (char *)(island + 16));


    /*
     * Both branches were validated before this loop.
     */
    for (i = 0;
         i < sizeof(g_ra_pause_call_sites) /
             sizeof(g_ra_pause_call_sites[0]);
         ++i) {

        uintptr_t address =
            g_launcher_base +
            g_ra_pause_call_sites[i].rva;

        if (!encode_bl(
                address,
                (uintptr_t)island,
                &branch)) {

            fprintf(
                stderr,
                "[RA PAUSE] ERROR: "
                "branch out of range +0x%lx\n",
                (unsigned long)
                    g_ra_pause_call_sites[i].rva);

            return 0;
        }

        if (!patch_word(
                address,
                branch)) {

            fprintf(
                stderr,
                "[RA PAUSE] ERROR: "
                "patch failed +0x%lx\n",
                (unsigned long)
                    g_ra_pause_call_sites[i].rva);

            return 0;
        }
    }


    fprintf(
        stderr,
        "[RA PAUSE] Stage 7E.7A hook installed "
        "island=%p\n",
        island);

    return 1;
}


/*
 * Separate constructor on purpose.
 *
 * This keeps the proven 7E.6 settings installer unchanged.
 */
__attribute__((constructor))
static void ra_pause_shortcut_init(void)
{
    fprintf(
        stderr,
        "[RA PAUSE] Stage 7E.7A starting\n");

    if (!ra_install_pause_shortcut_hook()) {
        fprintf(
            stderr,
            "[RA PAUSE] shortcut hook NOT installed\n");
    }
}



/* ========================================================================= */
/* Stage 7E.7B - native pause choice rendering                               */
/* ========================================================================= */

/*
 * Native top-level ui_choice calls:
 *
 *   0x38fb0  Continue
 *   0x3906c  Game Settings
 *   0x3911c  Cheats
 *   0x391e0  Exit to menu
 *
 * Instead of drawing after ui_pause_frame(), intercept these
 * calls while NNDDSS still owns the correct pause render target.
 *
 * The native four rows occupy four button-heights. We reduce
 * each row to 80% of the original height, fitting:
 *
 *   Continue
 *   Game Settings
 *   Cheats
 *   RetroAchievements
 *   Exit to menu
 *
 * into exactly the same total vertical extent.
 */

#define RA_RVA_UI_CHOICE 0x15710


static ui_button_fn
    g_ra_original_choice;


static const RaPauseCallSite
g_ra_pause_choice_call_sites[] = {
    {
        0x00038fb0,
        0x97ff71d8
    },
    {
        0x0003906c,
        0x97ff71a9
    },
    {
        0x0003911c,
        0x97ff717d
    },
    {
        0x000391e0,
        0x97ff714c
    },

    /*
     * Stage 7E.7D:
     * alternate Cheats renderer used when native focus
     * is already on Cheats.
     */
    {
        0x00039d28,
        0x97ff6e7a
    }
};


static bool ra_pause_choice_common(
    int native_row,
    UiGfx *gfx,
    UiText *text,
    UiRect rect,
    const char *label,
    UiBtnState state)
{
    bool result;


    if (!g_ra_original_choice)
        return false;


    /*
     * Our synthetic focus leaves NNDDSS's real pause focus on
     * Cheats or Exit. Hide that underlying native highlight so
     * RetroAchievements is the only turquoise row.
     */
    if (g_ra_pause_shortcut_selected) {
        state =
            (UiBtnState)0;
    }


    /*
     * Render the real native choice exactly where NNDDSS put it.
     */
    result =
        g_ra_original_choice(
            gfx,
            text,
            rect,
            label,
            state);


    /*
     * native_row 2 = Cheats.
     *
     * The RA row uses the exact native Cheats UiRect and moves
     * it down by one native row height. This keeps Continue,
     * Settings, Cheats and Exit completely untouched.
     */
    if (native_row == 2) {

        UiRect ra_rect;

        UiBtnState ra_state;


        ra_rect =
            rect;

        ra_rect.y +=
            rect.h;


        ra_state =
            g_ra_pause_shortcut_selected ?
            (UiBtnState)1 :
            (UiBtnState)0;


        (void)g_ra_original_choice(
            gfx,
            text,
            ra_rect,
            "RetroAchievements",
            ra_state);
    }


    return result;
}


static bool ra_pause_choice_continue(
    UiGfx *gfx,
    UiText *text,
    UiRect rect,
    const char *label,
    UiBtnState state)
{
    return ra_pause_choice_common(
        0,
        gfx,
        text,
        rect,
        label,
        state);
}


static bool ra_pause_choice_settings(
    UiGfx *gfx,
    UiText *text,
    UiRect rect,
    const char *label,
    UiBtnState state)
{
    return ra_pause_choice_common(
        1,
        gfx,
        text,
        rect,
        label,
        state);
}


static bool ra_pause_choice_cheats(
    UiGfx *gfx,
    UiText *text,
    UiRect rect,
    const char *label,
    UiBtnState state)
{
    return ra_pause_choice_common(
        2,
        gfx,
        text,
        rect,
        label,
        state);
}


static bool ra_pause_choice_exit(
    UiGfx *gfx,
    UiText *text,
    UiRect rect,
    const char *label,
    UiBtnState state)
{
    return ra_pause_choice_common(
        3,
        gfx,
        text,
        rect,
        label,
        state);
}


static int ra_install_native_pause_choice_hook(void)
{
    static ui_button_fn const wrappers[] = {
        ra_pause_choice_continue,
        ra_pause_choice_settings,
        ra_pause_choice_cheats,
        ra_pause_choice_exit,

        /*
         * Alternate native Cheats renderer.
         */
        ra_pause_choice_cheats
    };

    uint8_t *islands[5] = {
        NULL, NULL, NULL, NULL, NULL
    };

    uint32_t branches[5] = {
        0, 0, 0, 0, 0
    };

    unsigned int i;


    if (!g_launcher_base) {
        dl_iterate_phdr(
            find_main_cb,
            NULL);
    }

    if (!g_launcher_base) {
        fprintf(
            stderr,
            "[RA PAUSE UI] REFUSED: "
            "launcher base unavailable\n");

        return 0;
    }


    /*
     * Validate all four native ui_choice calls before
     * modifying any of them.
     */
    for (i = 0;
         i < sizeof(g_ra_pause_choice_call_sites) /
             sizeof(g_ra_pause_choice_call_sites[0]);
         ++i) {

        uintptr_t address =
            g_launcher_base +
            g_ra_pause_choice_call_sites[i].rva;

        uint32_t found =
            *(volatile uint32_t *)address;

        if (found !=
            g_ra_pause_choice_call_sites[i].expected) {

            fprintf(
                stderr,
                "[RA PAUSE UI] REFUSED: "
                "+0x%lx=%08x expected=%08x\n",
                (unsigned long)
                    g_ra_pause_choice_call_sites[i].rva,
                found,
                g_ra_pause_choice_call_sites[i].expected);

            return 0;
        }
    }


    g_ra_original_choice =
        (ui_button_fn)
        (g_launcher_base +
         RA_RVA_UI_CHOICE);


    /*
     * Prepare every island and branch before touching code.
     */
    for (i = 0;
         i < sizeof(g_ra_pause_choice_call_sites) /
             sizeof(g_ra_pause_choice_call_sites[0]);
         ++i) {

        uintptr_t address =
            g_launcher_base +
            g_ra_pause_choice_call_sites[i].rva;


        islands[i] =
            alloc_branch_island(
                g_launcher_base);

        if (!islands[i]) {
            fprintf(
                stderr,
                "[RA PAUSE UI] ERROR: "
                "could not allocate island %u\n",
                i);

            return 0;
        }


        *(uint32_t *)(islands[i] + 0) =
            0x58000050U;

        *(uint32_t *)(islands[i] + 4) =
            0xd61f0200U;

        *(uint64_t *)(islands[i] + 8) =
            (uint64_t)(uintptr_t)
            wrappers[i];


        __builtin___clear_cache(
            (char *)islands[i],
            (char *)(islands[i] + 16));


        if (!encode_bl(
                address,
                (uintptr_t)islands[i],
                &branches[i])) {

            fprintf(
                stderr,
                "[RA PAUSE UI] ERROR: "
                "branch out of range +0x%lx\n",
                (unsigned long)
                    g_ra_pause_choice_call_sites[i].rva);

            return 0;
        }
    }


    /*
     * Everything validated successfully. Apply the four BLs.
     */
    for (i = 0;
         i < sizeof(g_ra_pause_choice_call_sites) /
             sizeof(g_ra_pause_choice_call_sites[0]);
         ++i) {

        uintptr_t address =
            g_launcher_base +
            g_ra_pause_choice_call_sites[i].rva;

        if (!patch_word(
                address,
                branches[i])) {

            fprintf(
                stderr,
                "[RA PAUSE UI] ERROR: "
                "patch failed +0x%lx\n",
                (unsigned long)
                    g_ra_pause_choice_call_sites[i].rva);

            return 0;
        }
    }


    fprintf(
        stderr,
        "[RA PAUSE UI] Stage 7E.7B "
        "native choices installed\n");

    return 1;
}


__attribute__((constructor))
static void ra_native_pause_choice_init(void)
{
    fprintf(
        stderr,
        "[RA PAUSE UI] Stage 7E.7B starting\n");

    if (!ra_install_native_pause_choice_hook()) {
        fprintf(
            stderr,
            "[RA PAUSE UI] native choice hook NOT installed\n");
    }
}



/* ========================================================================= */
/* Stage 7E.7C - pause shortcut fixes                                        */
/*
 *  - direct RA open survives UiSettingsState owner initialization
 *  - native Continue/Settings/Cheats/Exit geometry restored
 *  - RA rendered directly beneath Cheats with native ui_choice styling
 */
/* ========================================================================= */



/* ========================================================================= */
/* Stage 7E.7D - alternate Cheats renderer                                   */
/*
 * NNDDSS has separate normal and selected rendering paths for Cheats.
 * Both now route through ra_pause_choice_cheats(), so the synthetic
 * RetroAchievements row remains visible regardless of native focus.
 */
/* ========================================================================= */



/* ========================================================================= */
/* Stage 7E.8C - System menu removed                                         */
/*
 * RetroAchievements is exposed only from the in-game Pause menu.
 * Normal System Settings is passed directly to NNDDSS.
 */
/* ========================================================================= */



/* ========================================================================= */
/* Stage 7E.8D - clean Pause return                                          */
/*
 * Closing the RetroAchievements root page no longer renders the native
 * System settings page for an intermediate frame. The final RA frame is
 * retained until the Pause wrapper returns to the top-level Pause menu.
 */
/* ========================================================================= */



/* ========================================================================= */
/* Stage 7F.3 - Rich Presence UI                                             */
/*
 * The existing RA root page displays the current Rich Presence message as
 * a non-focusable, ellipsized status line beneath the title.
 *
 * Existing seven-row navigation and focus numbering are unchanged.
 */
/* ========================================================================= */



/* ========================================================================= */
/* Stage 7F.4 - polished RA header                                           */
/*
 * RetroAchievements root page layout:
 *
 *   RetroAchievements
 *   <Rich Presence>
 *   Signed in as <user>
 *   -----------------------
 *   Achievements
 *   Sign In/Out
 *   Achievement Popups
 *   Challenge Indicators
 *   Progress Indicators
 *   Back
 *
 * Account identity and Rich Presence are informational header content.
 * Only six rows participate in focus/navigation.
 */
/* ========================================================================= */



/* ========================================================================= */
/* Stage 7G.2 - Active Challenges First                                      */
/*
 * Achievement-list presentation only:
 *
 *   - bucket 6 Active Challenges are stably promoted to the top
 *   - a thin divider separates them from ordinary achievements
 *   - no synthetic rows are inserted
 *   - focus/scroll indexes still map directly to real achievements
 *   - live rcheevos state is untouched
 */
/* ========================================================================= */



/* ========================================================================= */
/* Stage 7H.6 - Persistent Hardcore toggle                                   */
/*
 * Root page:
 *
 *   0 Achievements >
 *   1 Hardcore Mode: On/Off
 *   2 Sign In/Out
 *   3 Achievement Popups
 *   4 Challenge Indicators
 *   5 Progress Indicators
 *   6 Back
 *
 * hardcore_mode is persisted in ra.cfg and exported through
 * ra_settings_hardcore_enabled().
 */
/* ========================================================================= */

