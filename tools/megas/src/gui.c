/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native controller: one Nuklear context, one current display, no video queue. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "megas.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <locale.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_IMPLEMENTATION
#include "../vendor/nuklear.h"

#ifdef _WIN32
#define NK_GDI_IMPLEMENTATION
#include "../vendor/nuklear_gdi.h"
#include <windowsx.h>
#include <commdlg.h>
#else
#define NK_XLIB_IMPLEMENTATION
#include "../vendor/nuklear_xlib.h"
#include <X11/keysym.h>
#include <poll.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

enum {
    GUI_MAX_W = 2048,
    GUI_MAX_H = 1280,
    GUI_MAX_FILES = 256,
    GUI_MAX_CHECKPOINTS = 64,
};
typedef struct {
    MgEngine *engine;
    struct nk_context *ctx;
    struct nk_rect viewport, picture;
    int width, height, alive, focused, tab, settings_tab, advanced_settings, res_selected;
    int ram, vm_w, vm_h, passwordless, disk_gb, create_dialog, creating_disk;
    int password_field_visible;
    int android, cpus, cpu_percent;
    float row;
    char qemu[1024], disk[1024], iso[1024], checkpoint[64];
    char rendernode[1024];
    char username[128], password[256];
    struct nk_rect password_bounds;
    char new_disk[1024];
    char feedback[2048], repair[8192], repair_file[1200];
    char pending[8192], response[65536], history[24576];
    char seen_feedback[512];
    struct nk_text_edit editors[9];
    char editor_memory[9][1024], editor_shadow[9][1024];
    unsigned editor_ready;
    char checkpoints[GUI_MAX_CHECKPOINTS][64];
    int checkpoint_count;
    uint32_t held_keys[256];
    unsigned buttons;
    uint64_t painted_generation;
    uint32_t *scaled;
    unsigned scaled_w, scaled_h;
    size_t scaled_capacity;
#ifdef _WIN32
    HWND window;
    HDC dc;
    GdiFont *font;
    HGDIOBJ original_bitmap;
#else
    Display *display;
    Window window;
    int screen;
    Atom close_atom;
    GC display_gc;
    XFont *font;
    XImage *image;
    int browse_target, file_count;
    char browse_dir[1024], files[GUI_MAX_FILES][256];
    unsigned char file_dirs[GUI_MAX_FILES];
#endif
} MgGui;

static int gui_turkish(void)
{
    const char *env_lang = getenv("MEGAS_LANG");
    if (env_lang && !strcmp(env_lang, "en")) return 0;
    return 1;
}

static const char *tr(const char *turkish, const char *english)
{
    return gui_turkish() ? turkish : english;
}

static void gui_note(MgGui *ui, const char *message)
{
    snprintf(ui->feedback, sizeof(ui->feedback), "%s", message);
}

/* Keep short-lived guest passwords out of stale editor/request buffers. */
static void gui_wipe(void *memory, size_t size)
{
    volatile unsigned char *p = memory;
    while (size--) *p++ = 0;
}

static void gui_password_clear(MgGui *ui)
{
    const unsigned slot = 7;
    gui_wipe(ui->password, sizeof(ui->password));
    gui_wipe(ui->editor_memory[slot], sizeof(ui->editor_memory[slot]));
    gui_wipe(ui->editor_shadow[slot], sizeof(ui->editor_shadow[slot]));
    gui_wipe(&ui->editors[slot], sizeof(ui->editors[slot]));
    ui->editor_ready &= ~(1u << slot);
}

/* No shell quoting: every path/text is encoded as a JSON string. */
static int json_string(char *out, size_t cap, const char *in)
{
    size_t n = 0;
    if (cap < 3) return 0;
    out[n++] = '"';
    for (; *in; ++in) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            if (n + 2 >= cap) return 0;
            out[n++] = '\\'; out[n++] = (char)c;
        } else if (c < 32) {
            if (n + 6 >= cap) return 0;
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        } else {
            if (n + 1 >= cap) return 0;
            out[n++] = (char)c;
        }
    }
    out[n++] = '"'; out[n] = 0;
    return 1;
}

static int gui_queue(MgGui *ui, const char *request)
{
    if (ui->pending[0]) return 0;
    if (strlen(request) >= sizeof(ui->pending)) {
        gui_note(ui, tr("Bu işlemdeki metin çok uzun. Daha kısa bir yol veya ad kullanın.",
                        "This action's text is too long. Use a shorter path or name."));
        return 0;
    }
    snprintf(ui->pending, sizeof(ui->pending), "%s", request);
    gui_note(ui, tr("İşlem yürütülüyor… Durum ve kayıtlar burada güncellenir.",
                    "Working… State and activity are updated here."));
    return 1;
}

static void gui_action(MgGui *ui, const char *action)
{
    char request[128];
    if (!strcmp(action, "doom")) {
        const MgStatus *s = mg_engine_status(ui->engine);
        if (!s->running || !s->connected) {
            gui_note(ui, tr("Önce Start ile sanal makineyi başlatın! Doom doğrudan misafir işletim sistemi içinde çalışır.",
                            "Start the VM first! Doom runs inside the guest operating system."));
            return;
        }
    }
    snprintf(request, sizeof(request), "{\"action\":\"%s\"}", action);
    gui_queue(ui, request);
}

static void gui_named(MgGui *ui, const char *action, const char *name)
{
    char value[512], request[768];
    if (!json_string(value, sizeof(value), name)) return;
    snprintf(request, sizeof(request), "{\"action\":\"%s\",\"name\":%s}", action, value);
    gui_queue(ui, request);
}

static void gui_start(MgGui *ui)
{
    char qemu[6144], disk[6144], iso[6144], username[768], password[1536], request[22000], rendernode[768];
    const MgStatus *status = mg_engine_status(ui->engine);
    if (!ui->android && !ui->passwordless && !ui->password[0] && !status->owned_pid) {
        gui_note(ui, tr("Yeni VM için şifresiz oturumu seçin veya yeni bir misafir şifresi girin.",
                        "For a new VM, choose passwordless login or enter a new guest password."));
        return;
    }
    if (!json_string(qemu, sizeof(qemu), ui->qemu) ||
        !json_string(disk, sizeof(disk), ui->disk) ||
        !json_string(iso, sizeof(iso), ui->iso) ||
        !json_string(username, sizeof(username), ui->username)) {
        gui_note(ui, tr("Dosya yolu çok uzun. Daha kısa bir yol seçin.", "File path is too long. Choose a shorter path."));
        gui_wipe(qemu, sizeof(qemu));
        gui_wipe(disk, sizeof(disk));
        gui_wipe(iso, sizeof(iso));
        gui_wipe(username, sizeof(username));
        gui_wipe(password, sizeof(password));
        gui_wipe(request, sizeof(request));
        return;
    }
    if(ui->android){
        if(!json_string(rendernode,sizeof(rendernode),ui->rendernode)){
            gui_note(ui,tr("GPU aygıt yolu çok uzun.","GPU device path is too long."));return;
        }
        snprintf(request,sizeof(request),
                 "{\"action\":\"start\",\"android\":true,\"qemu\":%s,\"disk\":%s,\"iso\":%s,\"rendernode\":%s,\"ram_mb\":%d,\"width\":%d,\"height\":%d,\"cpus\":%d,\"cpu_percent\":%d}",
                 qemu,disk,iso,rendernode,ui->ram,ui->vm_w,ui->vm_h,ui->cpus,ui->cpu_percent);
        gui_queue(ui,request);gui_wipe(request,sizeof(request));return;
    }
    if (!ui->passwordless && ui->password[0] &&
        !json_string(password, sizeof(password), ui->password)) {
        gui_note(ui, tr("Şifre çok uzun.", "Password is too long."));
        gui_wipe(qemu, sizeof(qemu));
        gui_wipe(disk, sizeof(disk));
        gui_wipe(iso, sizeof(iso));
        gui_wipe(username, sizeof(username));
        gui_wipe(password, sizeof(password));
        gui_wipe(request, sizeof(request));
        return;
    }
    if (!ui->passwordless && ui->password[0]) {
        snprintf(request, sizeof(request),
            "{\"action\":\"start\",\"android\":false,\"qemu\":%s,\"disk\":%s,\"iso\":%s,\"username\":%s,"
            "\"password\":%s,\"ram_mb\":%d,\"width\":%d,\"height\":%d,\"passwordless\":false}",
            qemu, disk, iso, username, password, ui->ram, ui->vm_w, ui->vm_h);
    } else {
        snprintf(request, sizeof(request),
            "{\"action\":\"start\",\"android\":false,\"qemu\":%s,\"disk\":%s,\"iso\":%s,\"username\":%s,"
            "\"ram_mb\":%d,\"width\":%d,\"height\":%d,\"passwordless\":%s}",
            qemu, disk, iso, username, ui->ram, ui->vm_w, ui->vm_h,
            ui->passwordless ? "true" : "false");
    }
    if (gui_queue(ui, request)) gui_password_clear(ui);
    gui_wipe(qemu, sizeof(qemu));
    gui_wipe(disk, sizeof(disk));
    gui_wipe(iso, sizeof(iso));
    gui_wipe(username, sizeof(username));
    gui_wipe(password, sizeof(password));
    gui_wipe(request, sizeof(request));
}

static void gui_apply_password_policy(MgGui *ui)
{
    char username[768], password[1536], request[3072];
    if (ui->pending[0]) {
        gui_note(ui, tr("Önce geçerli işlemin tamamlanmasını bekleyin.",
                        "Wait for the current action to finish first."));
        return;
    }
    if (!json_string(username, sizeof(username), ui->username) ||
        (!ui->passwordless && ui->password[0] &&
         !json_string(password, sizeof(password), ui->password))) {
        gui_note(ui, tr("Hesap bilgisi çok uzun.", "The account details are too long."));
        gui_wipe(username, sizeof(username));
        gui_wipe(password, sizeof(password));
        return;
    }
    if (ui->passwordless || !ui->password[0]) {
        snprintf(request, sizeof(request),
                 "{\"action\":\"password\",\"enabled\":%s,\"username\":%s}",
                 ui->passwordless ? "true" : "false", username);
    } else {
        snprintf(request, sizeof(request),
                 "{\"action\":\"password\",\"enabled\":false,\"username\":%s,\"password\":%s}",
                 username, password);
    }
    if (gui_queue(ui, request)) gui_password_clear(ui);
    gui_wipe(username, sizeof(username));
    gui_wipe(password, sizeof(password));
    gui_wipe(request, sizeof(request));
}

static void gui_key(MgGui *ui, uint32_t keysym, int down)
{
    char request[160], response[2048];
    snprintf(request, sizeof(request), "{\"action\":\"key\",\"keysym\":%u,\"down\":%s,\"checkpoint\":false}",
             keysym, down ? "true" : "false");
    mg_engine_submit(ui->engine, request, response, sizeof(response));
}

static void gui_release(MgGui *ui)
{
    unsigned i;
    for (i = 0; i < 256; ++i) if (ui->held_keys[i]) {
        gui_key(ui, ui->held_keys[i], 0); ui->held_keys[i] = 0;
    }
    if (ui->buttons) {
        char response[2048];
        mg_engine_submit(ui->engine, "{\"action\":\"pointer\",\"x\":0,\"y\":0,\"buttons\":0,\"checkpoint\":false}", response, sizeof(response));
    }
    ui->buttons = 0;
    ui->focused = 0;
#ifdef _WIN32
    ReleaseCapture();
#else
    if (ui->display) {
        XUngrabPointer(ui->display, CurrentTime);
        XUngrabKeyboard(ui->display, CurrentTime);
    }
#endif
}

static int gui_inside(struct nk_rect r, int x, int y)
{
    return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y && x < r.x+r.w && y < r.y+r.h;
}

static void gui_pointer(MgGui *ui, int px, int py, unsigned buttons)
{
    const MgStatus *s = mg_engine_status(ui->engine);
    char request[192], response[2048];
    int x, y;
    if (ui->picture.w <= 0 || ui->picture.h <= 0 || !s->width || !s->height) return;
    x = (int)((px-ui->picture.x) * s->width / ui->picture.w);
    y = (int)((py-ui->picture.y) * s->height / ui->picture.h);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)s->width) x = (int)s->width-1;
    if (y >= (int)s->height) y = (int)s->height-1;
    snprintf(request, sizeof(request), "{\"action\":\"pointer\",\"x\":%d,\"y\":%d,\"buttons\":%u,\"checkpoint\":false}", x, y, buttons);
    mg_engine_submit(ui->engine, request, response, sizeof(response));
}

static void gui_repair_text(MgGui *ui)
{
    const MgStatus *s = mg_engine_status(ui->engine);
    const MgConfig *c = mg_engine_config(ui->engine);
    FILE *file;
    if (s->repair_prompt[0]) snprintf(ui->repair, sizeof(ui->repair), "%s", s->repair_prompt);
    else snprintf(ui->repair, sizeof(ui->repair),
        "Investigate Megas VM controller state. Phase: %s\nFeedback: %s\n"
        "Actions: %llu; failed: %llu; normalized: %llu; events: %llu\n"
        "Journal: %s\nLatest image: %s\n"
        "Find the cause from this journal and image. Preserve VM disk and checkpoints. "
        "Make the smallest fix, verify it, and explain the result. Do not claim a guest change without observing it.\n",
        s->phase, s->feedback, (unsigned long long)s->actions,
        (unsigned long long)s->failed, (unsigned long long)s->normalized,
        (unsigned long long)s->events, s->journal_path, s->image_path);
    snprintf(ui->repair_file, sizeof(ui->repair_file), "%s/repair-prompt.txt", c->runtime_dir);
#ifdef _WIN32
    {
        WCHAR path[1200];
        MultiByteToWideChar(CP_UTF8, 0, ui->repair_file, -1, path, 1200);
        file = _wfopen(path, L"wb");
    }
#else
    file = fopen(ui->repair_file, "wb");
#endif
    if (file) {
        size_t length = strlen(ui->repair);
        int written = fwrite(ui->repair, 1, length, file) == length;
        if (fclose(file) != 0) written = 0;
        if (!written) ui->repair_file[0] = 0;
    } else ui->repair_file[0] = 0;
}

static void gui_copy_repair(MgGui *ui)
{
    int copied = 0;
    gui_repair_text(ui);
#ifdef _WIN32
    if (OpenClipboard(ui->window)) {
        int count = MultiByteToWideChar(CP_UTF8, 0, ui->repair, -1, NULL, 0);
        HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (size_t)count * sizeof(WCHAR));
        if (data) {
            WCHAR *ptr = GlobalLock(data);
            if (ptr) {
                MultiByteToWideChar(CP_UTF8, 0, ui->repair, -1, ptr, count);
                GlobalUnlock(data); EmptyClipboard();
                if (SetClipboardData(CF_UNICODETEXT, data)) copied = 1;
            }
            if (!copied) GlobalFree(data);
        }
        CloseClipboard();
    }
#else
    nk_xlib_copy(nk_handle_ptr(NULL), ui->repair, (int)strlen(ui->repair));
    copied = XGetSelectionOwner(ui->display, xlib.xa_clipboard) == ui->window;
#endif
    if (copied && ui->repair_file[0]) gui_note(ui, tr("Tanılama istemi kopyalandı. Metin dosyası da hazır.",
                                                    "Repair prompt copied. A text file is also available."));
    else if (copied) gui_note(ui, tr("Tanılama istemi kopyalandı.", "Repair prompt copied."));
    else if (ui->repair_file[0]) gui_note(ui, tr("Pano kullanılamadı. İstem aşağıdaki metin dosyasına kaydedildi.",
                                               "Clipboard unavailable. The prompt was saved to the text file below."));
    else gui_note(ui, tr("Pano ve dosya kaydı kullanılamadı. İstem aşağıdaki kutudan seçilebilir.",
                        "Clipboard and file export unavailable. Select the prompt from the box below."));
}

static void gui_harness(MgGui *ui, const char *name)
{
    char request[160];
    gui_repair_text(ui);
    /* The owner already holds the full repair prompt. Do not serialize and
     * truncate it into an unrelated command-length limit. */
    snprintf(request, sizeof(request), "{\"action\":\"harness\",\"name\":\"%s\"}", name);
    gui_queue(ui, request);
}

static void gui_palette(struct nk_context *ctx)
{
    struct nk_color colors[NK_COLOR_COUNT];
    int i;
    for (i = 0; i < NK_COLOR_COUNT; ++i) colors[i] = nk_rgb(205, 209, 202);
    colors[NK_COLOR_TEXT] = nk_rgb(28, 33, 28);
    colors[NK_COLOR_WINDOW] = nk_rgb(226, 229, 222);
    colors[NK_COLOR_HEADER] = nk_rgb(213, 217, 209);
    colors[NK_COLOR_BORDER] = nk_rgb(125, 135, 124);
    colors[NK_COLOR_BUTTON] = nk_rgb(198, 205, 196);
    colors[NK_COLOR_BUTTON_HOVER] = nk_rgb(182, 193, 180);
    colors[NK_COLOR_BUTTON_ACTIVE] = nk_rgb(163, 177, 160);
    colors[NK_COLOR_TOGGLE] = nk_rgb(198, 205, 196);
    colors[NK_COLOR_TOGGLE_HOVER] = nk_rgb(182, 193, 180);
    colors[NK_COLOR_TOGGLE_CURSOR] = nk_rgb(78, 95, 75);
    colors[NK_COLOR_SELECT] = nk_rgb(210, 216, 207);
    colors[NK_COLOR_SELECT_ACTIVE] = nk_rgb(171, 189, 168);
    colors[NK_COLOR_SLIDER] = nk_rgb(196, 204, 193);
    colors[NK_COLOR_SLIDER_CURSOR] = nk_rgb(105, 123, 102);
    colors[NK_COLOR_SLIDER_CURSOR_HOVER] = nk_rgb(89, 111, 86);
    colors[NK_COLOR_SLIDER_CURSOR_ACTIVE] = nk_rgb(73, 95, 70);
    colors[NK_COLOR_PROPERTY] = nk_rgb(217, 223, 214);
    colors[NK_COLOR_EDIT] = nk_rgb(240, 242, 236);
    colors[NK_COLOR_EDIT_CURSOR] = nk_rgb(28, 33, 28);
    colors[NK_COLOR_COMBO] = nk_rgb(214, 221, 211);
    colors[NK_COLOR_CHART] = nk_rgb(217, 223, 214);
    colors[NK_COLOR_CHART_COLOR] = nk_rgb(78, 95, 75);
    colors[NK_COLOR_CHART_COLOR_HIGHLIGHT] = nk_rgb(55, 77, 51);
    colors[NK_COLOR_SCROLLBAR] = nk_rgb(218, 224, 216);
    colors[NK_COLOR_SCROLLBAR_CURSOR] = nk_rgb(165, 181, 161);
    colors[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = nk_rgb(145, 163, 141);
    colors[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgb(125, 146, 122);
    colors[NK_COLOR_TAB_HEADER] = nk_rgb(208, 217, 205);
    nk_style_from_table(ctx, colors);
    ctx->style.window.padding = nk_vec2(12, 12);
    ctx->style.button.rounding = 4;
    ctx->style.edit.rounding = 3;
    ctx->style.property.rounding = 3;
}

#ifndef _WIN32
static void gui_read_directory(MgGui *ui)
{
    DIR *dir = opendir(ui->browse_dir);
    struct dirent *entry;
    ui->file_count = 0;
    if (!dir) { gui_note(ui, tr("Bu klasör açılamadı. Başka bir klasör seçin.", "This folder could not be opened. Choose another folder.")); return; }
    while ((entry = readdir(dir)) && ui->file_count < GUI_MAX_FILES) {
        char path[1400]; struct stat st; int index = ui->file_count;
        if (entry->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", ui->browse_dir, entry->d_name);
        if (stat(path, &st) != 0) continue;
        snprintf(ui->files[index], sizeof(ui->files[index]), "%s", entry->d_name);
        ui->file_dirs[index] = S_ISDIR(st.st_mode);
        ++ui->file_count;
    }
    closedir(dir);
}
#endif

static void gui_browse(MgGui *ui, int target)
{
#ifdef _WIN32
    WCHAR path[1024] = {0}; OPENFILENAMEW picker;
    char *field = target == 1 ? ui->disk : target == 2 ? ui->iso : target == 4 ? ui->new_disk : ui->qemu;
    MultiByteToWideChar(CP_UTF8, 0, field, -1, path, 1024);
    memset(&picker, 0, sizeof(picker));
    picker.lStructSize = sizeof(picker); picker.hwndOwner = ui->window;
    picker.lpstrFile = path; picker.nMaxFile = 1024;
    picker.lpstrFilter = target == 4 ? L"VM disk\0*.qcow2\0All files\0*.*\0\0" : L"All files\0*.*\0\0";
    picker.Flags = OFN_NOCHANGEDIR | (target == 4 ? OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST);
    if (target == 4) picker.lpstrDefExt = L"qcow2";
    if (target == 4 ? GetSaveFileNameW(&picker) : GetOpenFileNameW(&picker))
        WideCharToMultiByte(CP_UTF8, 0, path, -1, field, 1024, NULL, NULL);
#else
    const char *home = getenv("HOME");
    ui->browse_target = target;
    snprintf(ui->browse_dir, sizeof(ui->browse_dir), "%s", home ? home : "/");
    gui_read_directory(ui);
#endif
}

/* Persistent editors also give X11's asynchronous clipboard a valid target. */
static void gui_edit(MgGui *ui, int slot, char *field, size_t capacity)
{
    struct nk_text_edit *edit = &ui->editors[slot];
    int n;
    if (!(ui->editor_ready & (1u << slot))) {
        nk_textedit_init_fixed(edit, ui->editor_memory[slot], capacity-1);
        ui->editor_ready |= 1u << slot;
    }
    if (strcmp(field, ui->editor_shadow[slot]) != 0) {
        size_t length = strlen(field);
        nk_str_clear(&edit->string);
        /* nk_str_append_str_utf8 includes its terminating NUL in the byte
         * buffer length. Edit rendering consumes that length and can show
         * the terminator as a stray glyph, so append the actual UTF-8 bytes. */
        if (length <= INT32_MAX)
            nk_str_append_text_char(&edit->string, field, (int)length);
        edit->cursor = nk_utf_len(field, (int)length);
        edit->select_start = edit->select_end = edit->cursor;
    }
    nk_edit_buffer(ui->ctx, slot == 7 ? (NK_EDIT_SIMPLE|NK_EDIT_SELECTABLE) : NK_EDIT_FIELD,
                   edit, nk_filter_default);
    /* Nuklear stores code points in `string.len` and UTF-8 bytes in the
     * string buffer. The external field capacity and memcpy are byte based. */
    n = nk_str_len_char(&edit->string);
    if (n >= (int)capacity) n = (int)capacity-1;
    if (n > 0) memcpy(field, nk_str_get_const(&edit->string), (size_t)n);
    field[n] = 0;
    snprintf(ui->editor_shadow[slot], sizeof(ui->editor_shadow[slot]), "%s", field);
}

static int gui_utf8_chars(const char *text, int length)
{
    int i, count = 0;
    for (i = 0; i < length; ++i)
        if (((unsigned char)text[i] & 0xc0) != 0x80) ++count;
    return count;
}

/* Mask the password in Nuklear's command buffer before either native backend
 * sees it. The edit buffer stays private to this process and has no clipboard. */
static void gui_mask_password(MgGui *ui)
{
    const struct nk_command *command;
    struct nk_rect bounds = ui->password_bounds;
    if (!ui->password_field_visible) return;
    nk_foreach(command, ui->ctx) {
        struct nk_command_text *text;
        int masked;
        if (command->type != NK_COMMAND_TEXT) continue;
        text = (struct nk_command_text *)(void *)command;
        if (!text->length || text->x < bounds.x || text->x >= bounds.x + bounds.w ||
            text->y < bounds.y || text->y >= bounds.y + bounds.h) continue;
        masked = gui_utf8_chars(text->string, text->length);
        memset(text->string, '*', (size_t)text->length);
        text->length = masked;
        text->string[masked] = 0;
    }
}

#ifndef _WIN32
/* Some X servers expose only ISO-8859 core fonts. In a UTF-8 locale Xlib can
 * then draw Turkish letters using unrelated glyphs. For that limited-font
 * fallback, encode visible Latin-1 characters directly and transliterate the
 * four Turkish letters outside Latin-1. Width calculation uses this same
 * conversion, so Nuklear layout and XDrawString agree byte for byte. */
#define GUI_X_TEXT_SCRATCH 65536
static char gui_x_text_scratch[GUI_X_TEXT_SCRATCH];
static int gui_x_latin_fallback;

static size_t gui_x_latin1(const char *source, size_t length, char *dest, size_t capacity)
{
    size_t i = 0, out = 0;
    while (i < length && out + 1 < capacity) {
        const unsigned char *s = (const unsigned char *)source + i;
        uint32_t cp;
        size_t bytes;
        unsigned char first = s[0];
        if (first < 0x80) { cp = first; bytes = 1; }
        else if (first >= 0xc2 && first <= 0xdf && i + 1 < length && (s[1] & 0xc0) == 0x80) {
            cp = ((uint32_t)(first & 0x1f) << 6) | (s[1] & 0x3f); bytes = 2;
        } else if (first >= 0xe0 && first <= 0xef && i + 2 < length &&
                   (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 &&
                   !(first == 0xe0 && s[1] < 0xa0) && !(first == 0xed && s[1] >= 0xa0)) {
            cp = ((uint32_t)(first & 0x0f) << 12) | ((uint32_t)(s[1] & 0x3f) << 6) | (s[2] & 0x3f); bytes = 3;
        } else if (first >= 0xf0 && first <= 0xf4 && i + 3 < length &&
                   (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80 &&
                   !(first == 0xf0 && s[1] < 0x90) && !(first == 0xf4 && s[1] >= 0x90)) {
            cp = ((uint32_t)(first & 0x07) << 18) | ((uint32_t)(s[1] & 0x3f) << 12) |
                 ((uint32_t)(s[2] & 0x3f) << 6) | (s[3] & 0x3f); bytes = 4;
        } else { cp = '?'; bytes = 1; }
        i += bytes;
        if (cp <= 0xff) dest[out++] = (char)cp;
        else {
            switch (cp) {
            case 0x011e: dest[out++] = 'G'; break; /* Ğ */
            case 0x011f: dest[out++] = 'g'; break; /* ğ */
            case 0x0130: dest[out++] = 'I'; break; /* İ */
            case 0x0131: dest[out++] = 'i'; break; /* ı */
            case 0x015e: dest[out++] = 'S'; break; /* Ş */
            case 0x015f: dest[out++] = 's'; break; /* ş */
            case 0x2013: case 0x2014: case 0x2212: dest[out++] = '-'; break;
            case 0x2018: case 0x2019: case 0x201b: dest[out++] = '\''; break;
            case 0x201c: case 0x201d: dest[out++] = '"'; break;
            case 0x2022: dest[out++] = '*'; break;
            case 0x2026:
                if (out + 3 < capacity) { memcpy(dest + out, "...", 3); out += 3; }
                else dest[out++] = '?';
                break;
            case 0x00a0: dest[out++] = ' '; break;
            default: dest[out++] = '?'; break;
            }
        }
    }
    dest[out] = 0;
    return out;
}

static float gui_x_text_width(nk_handle handle, float height, const char *text, int length)
{
    XFont *font = (XFont *)handle.ptr;
    size_t bytes;
    if (!gui_x_latin_fallback)
        return nk_xfont_get_text_width(handle, height, text, length);
    if (!font || !font->xfont || !text || length <= 0) return 0;
    bytes = gui_x_latin1(text, (size_t)length, gui_x_text_scratch, sizeof(gui_x_text_scratch));
    return (float)XTextWidth(font->xfont, gui_x_text_scratch, (int)bytes);
}

static int gui_xfontset_has_charset(XFont *font, const char *charset)
{
    XFontStruct **fonts;
    char **names;
    int count, i;
    if (!font || !font->set) return 0;
    count = XFontsOfFontSet(font->set, &fonts, &names);
    for (i = 0; i < count; ++i)
        if (strstr(names[i], charset)) return 1;
    return 0;
}

static int gui_x_prepare_font(MgGui *ui)
{
    if (!gui_turkish()) return 1;
    /* ISO-8859-9 has the Turkish letters; ISO-10646-1 covers Unicode. */
    if (gui_xfontset_has_charset(ui->font, "iso8859-9") ||
        gui_xfontset_has_charset(ui->font, "iso10646-1")) return 1;
    gui_x_latin_fallback = 1;
    if (ui->font->set) {
        XFreeFontSet(ui->display, ui->font->set);
        ui->font->set = NULL;
    }
    if (!ui->font->xfont) ui->font->xfont = XLoadQueryFont(ui->display, "fixed");
    if (!ui->font->xfont) return 0;
    ui->font->ascent = ui->font->xfont->ascent;
    ui->font->descent = ui->font->xfont->descent;
    ui->font->height = ui->font->ascent + ui->font->descent;
    return 1;
}

static void gui_x_prepare_text(MgGui *ui)
{
    const struct nk_command *command;
    if (!gui_x_latin_fallback) return;
    nk_foreach(command, ui->ctx) {
        struct nk_command_text *text;
        size_t bytes;
        if (command->type != NK_COMMAND_TEXT) continue;
        text = (struct nk_command_text *)(void *)command;
        bytes = gui_x_latin1(text->string, (size_t)text->length,
                             gui_x_text_scratch, sizeof(gui_x_text_scratch));
        memcpy(text->string, gui_x_text_scratch, bytes + 1);
        text->length = (int)bytes;
    }
}
#endif

static void gui_field(MgGui *ui, const char *title, const char *help, char *field, int target)
{
    struct nk_context *ctx = ui->ctx;
    nk_layout_row_dynamic(ctx, ui->row, 1); nk_label(ctx, title, NK_TEXT_LEFT);
    nk_layout_row_begin(ctx, NK_DYNAMIC, ui->row, 2);
    nk_layout_row_push(ctx, 0.80f);
    gui_edit(ui, target-1, field, 1024);
    nk_layout_row_push(ctx, 0.20f);
    if (nk_button_label(ctx, tr("Seç…", "Browse…"))) gui_browse(ui, target);
    nk_layout_row_end(ctx);
    if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, help);
}

static const struct {
    const char *label;
    int w, h;
} res_presets[] = {
    {"Custom", 0, 0},
    {"1920x1080 (1080p FHD)", 1920, 1080},
    {"1920x1200 (Laptop)", 1920, 1200},
    {"1366x768 (Laptop)", 1366, 768},
    {"1280x720 (720p HD)", 1280, 720},
    {"1280x800", 1280, 800},
    {"1024x768", 1024, 768},
    {"1440x900", 1440, 900},
    {"1600x900", 1600, 900},
    {"2560x1440", 2560, 1440},
    {"3840x2160", 3840, 2160}
};
#define RES_PRESETS_COUNT 11
static const char *res_items_tr = 
    "Özel / Custom\0"
    "1920x1080 (1080p FHD)\0"
    "1920x1200 (Laptop)\0"
    "1366x768 (Laptop)\0"
    "1280x720 (720p HD)\0"
    "1280x800\0"
    "1024x768\0"
    "1440x900\0"
    "1600x900\0"
    "2560x1440\0"
    "3840x2160\0\0";

static const char *res_items_en = 
    "Custom\0"
    "1920x1080 (1080p FHD)\0"
    "1920x1200 (Laptop)\0"
    "1366x768 (Laptop)\0"
    "1280x720 (720p HD)\0"
    "1280x800\0"
    "1024x768\0"
    "1440x900\0"
    "1600x900\0"
    "2560x1440\0"
    "3840x2160\0\0";

static void gui_settings(MgGui *ui)
{
    struct nk_context *ctx = ui->ctx;
    char request[192];
    gui_field(ui, tr("Sanal disk", "VM disk"),
        tr("VM'in dosyaları ve işletim sistemi. Geri dönüş için qcow2 biçimi kullanılır.",
           "The VM's files and operating system. Use qcow2 format for checkpoints."), ui->disk, 1);
    nk_layout_row_dynamic(ctx, ui->row, 1);
    if (nk_button_label(ctx, tr("Yeni disk oluştur…", "Create a new disk…"))) {
        const MgConfig *config = mg_engine_config(ui->engine);
        snprintf(ui->new_disk, sizeof(ui->new_disk), "%.950s/vm-%llu.qcow2", config->runtime_dir, (unsigned long long)time(NULL));
        ui->create_dialog = 1;
        gui_release(ui);
    }
    gui_field(ui, tr("Kurulum diski (isteğe bağlı)", "Installer image (optional)"),
        tr("Yeni işletim sistemi kurmak veya bir kurtarma ortamı açmak için ISO dosyası.",
           "An ISO file to install an operating system or boot a rescue environment."), ui->iso, 2);
    nk_layout_row_dynamic(ctx, ui->row, 1);
    if (nk_button_label(ctx, ui->advanced_settings ?
            tr("Gelişmiş seçenekleri gizle", "Hide advanced options") :
            tr("Gelişmiş / geliştirici seçenekleri", "Advanced / developer options")))
        ui->advanced_settings = !ui->advanced_settings;
    if (nk_widget_is_hovered(ctx))
        nk_tooltip(ctx, tr("Standart kullanımda bu seçenekleri değiştirmeniz gerekmez.",
                           "These options do not need to change for normal use."));
    if (ui->advanced_settings) {
        nk_layout_row_dynamic(ctx, ui->row*2, 1);
        nk_label_wrap(ctx, tr("Megas paketi sanal bilgisayar programını otomatik seçer. Bu alanı yalnızca kendi kopyanızı kullanacaksanız değiştirin.",
                             "The Megas package selects the virtual-machine program automatically. Change this only to use your own copy."));
        gui_field(ui, tr("VM programı", "VM executable"),
            tr("Normal kullanımda Megas'ın seçtiği programı kullanın.",
               "Use the program selected by Megas for normal use."), ui->qemu, 3);
    }
    nk_layout_row_dynamic(ctx, ui->row, 1);
    nk_property_int(ctx, tr("VM belleği (MB)", "VM memory (MB)"), 64, &ui->ram, 32768, 64, 16);
    if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, tr("Bu bellek VM'e aittir. Kontrol panelinin belleğinden ayrıdır.", "This memory belongs to the VM, separately from the controller."));
    nk_layout_row_dynamic(ctx, ui->row, 1);
    nk_checkbox_label(ctx,tr("Android: yerel sağlayıcı ve canlı yayın", "Android: local provider and livestream"),&ui->android);
    if(ui->android){
        nk_layout_row_dynamic(ctx,ui->row,1);
        nk_label(ctx,tr("GPU seçimi: boş bırakılırsa otomatik", "GPU override: empty selects automatically"),NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx,ui->row,1);
        gui_edit(ui,8,ui->rendernode,sizeof(ui->rendernode));
        nk_layout_row_dynamic(ctx,ui->row,2);
        nk_property_int(ctx,"vCPU",1,&ui->cpus,4,1,1);
        nk_property_int(ctx,tr("CPU bütçesi %", "CPU budget %"),1,&ui->cpu_percent,400,10,1);
        nk_layout_row_dynamic(ctx,ui->row*3,1);
        nk_label_wrap(ctx,tr("Yerel Android sağlayıcısı, donanım hızlandırması ve ayrılmış kaynak sınırları gerekir. Bu seçenek Android imajı veya Google uygulaması indirmez.",
                             "Requires a local Android provider, hardware acceleration and delegated resource limits. This option does not download Android images or Google apps."));
    }
    nk_layout_row_dynamic(ctx, ui->row, 1);
    nk_label(ctx, tr("Ekran Çözünürlüğü Seçici", "Resolution Selector"), NK_TEXT_LEFT);
    {
        int prev = ui->res_selected;
        const char *items = gui_turkish() ? res_items_tr : res_items_en;
        if (ui->res_selected < 0 || ui->res_selected >= RES_PRESETS_COUNT) ui->res_selected = 0;
        nk_layout_row_dynamic(ctx, ui->row, 1);
        ui->res_selected = nk_combo_string(ctx, items, ui->res_selected, RES_PRESETS_COUNT, (int)ui->row, nk_vec2(340, 260));
        if (ui->res_selected != prev && ui->res_selected > 0 && ui->res_selected < RES_PRESETS_COUNT) {
            ui->vm_w = res_presets[ui->res_selected].w;
            ui->vm_h = res_presets[ui->res_selected].h;
        }
    }
    nk_layout_row_dynamic(ctx, ui->row, 2);
    {
        int prev_w = ui->vm_w, prev_h = ui->vm_h;
        nk_property_int(ctx, tr("Genişlik", "Width"), 320, &ui->vm_w, 4096, 16, 8);
        nk_property_int(ctx, tr("Yükseklik", "Height"), 200, &ui->vm_h, 4096, 16, 8);
        if (ui->vm_w != prev_w || ui->vm_h != prev_h) {
            int match = 0, i;
            for (i = 1; i < RES_PRESETS_COUNT; ++i) {
                if (res_presets[i].w == ui->vm_w && res_presets[i].h == ui->vm_h) {
                    match = i;
                    break;
                }
            }
            ui->res_selected = match;
        }
    }
    nk_layout_row_dynamic(ctx, ui->row, 1);
    if (nk_button_label(ctx, tr("Ekran boyutunu uygula", "Apply screen size"))) {
        snprintf(request, sizeof(request), "{\"action\":\"resize\",\"width\":%d,\"height\":%d}", ui->vm_w, ui->vm_h);
        gui_queue(ui, request);
    }
    nk_layout_row_dynamic(ctx, ui->row*2, 1);
    nk_label_wrap(ctx, tr("Çalışırken ekran boyutu değişebilir; VM içindeki ekran sürücüsü bunu desteklemelidir.",
                         "Screen size can change while running; the VM's display driver must support it."));
    nk_layout_row_dynamic(ctx, ui->row, 1);
    nk_label(ctx, tr("Misafir hesabı", "Guest account"), NK_TEXT_LEFT);
    gui_edit(ui, 6, ui->username, sizeof(ui->username));
    nk_layout_row_dynamic(ctx, ui->row, 1);
    nk_checkbox_label(ctx, tr("Şifresiz oturum (varsayılan)", "Passwordless login (default)"), &ui->passwordless);
    if (!ui->passwordless) {
        nk_layout_row_dynamic(ctx, ui->row, 1);
        nk_label(ctx, tr("Yeni misafir şifresi", "New guest password"), NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, ui->row, 1);
        ui->password_bounds = nk_widget_bounds(ctx);
        ui->password_field_visible = 1;
        gui_edit(ui, 7, ui->password, sizeof(ui->password));
    }
    nk_layout_row_dynamic(ctx, ui->row, 1);
    if (nk_button_label(ctx, tr("Oturum ilkesini uygula", "Apply login policy")))
        gui_apply_password_policy(ui);
    nk_layout_row_dynamic(ctx, ui->row*2.5f, 1);
    nk_label_wrap(ctx, tr("Yeni VM hesabı önce kilitli başlar. Oturum ilkesi QEMU Guest Agent hazır olunca uygulanır; var olan VM'de de agent gerekir. Şifreli oturum için yeni şifre girin. Boş şifre yalnızca kayıtlı oturum/kilit ayarlarını geri getirir; eski şifreyi geri getirmez.",
                         "A new VM account starts locked. The login policy applies when QEMU Guest Agent is ready; existing VMs also need it. Enter a new password for password-required login. With a blank password, the helper restores saved sign-in/lock settings only; it cannot recover an old password."));
}

static void gui_steps(MgGui *ui)
{
    struct nk_context *ctx = ui->ctx;
    const MgStatus *s = mg_engine_status(ui->engine);
    char text[256]; int i;
    ui->checkpoint_count = (int)mg_engine_checkpoints(ui->engine, ui->checkpoints,
                                                       GUI_MAX_CHECKPOINTS);
    nk_layout_row_dynamic(ctx, ui->row*2.4f, 1);
    nk_label_wrap(ctx, tr("Her komut kaydedilir. Dönmek istediğiniz durumu kaydedin; geri veya ileri geçiş için kayıt adını seçin.",
                         "Every command is recorded. Save states you want to revisit, then select a saved state to go back or forward."));
    nk_layout_row_dynamic(ctx, ui->row, 1);
    gui_edit(ui, 3, ui->checkpoint, sizeof(ui->checkpoint));
    nk_layout_row_dynamic(ctx, ui->row, 2);
    if (nk_button_label(ctx, tr("Durumu kaydet", "Save state"))) gui_named(ui, "checkpoint", ui->checkpoint);
    if (nk_button_label(ctx, tr("Bu duruma dön", "Restore state"))) gui_named(ui, "restore", ui->checkpoint);
    for (i=0; i<ui->checkpoint_count; ++i) {
        nk_layout_row_dynamic(ctx, ui->row, 1);
        if (nk_button_label(ctx, ui->checkpoints[i])) {
            snprintf(ui->checkpoint, sizeof(ui->checkpoint), "%s", ui->checkpoints[i]);
            gui_named(ui, "restore", ui->checkpoint);
        }
    }
    nk_layout_row_dynamic(ctx, ui->row, 1);
    snprintf(text, sizeof(text), tr("Son geri dönüş: %.0f ms", "Last restore: %.0f ms"), s->last_restore_ms);
    nk_label(ctx, text, NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, ui->row, 1);
    if (nk_button_label(ctx, tr("Geçmişi yenile", "Refresh history"))) gui_action(ui, "steps");
    nk_layout_row_dynamic(ctx, ui->row*8, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX|NK_EDIT_READ_ONLY, ui->history, sizeof(ui->history), nk_filter_default);
    nk_layout_row_dynamic(ctx, ui->row*2, 1);
    nk_label_wrap(ctx, tr("Kayıtlar diskte tutulur; bellekte VM kopyaları biriktirilmez. Dönüş süresi VM belleğine ve disk hızına bağlıdır.",
                         "States live on disk; VM copies do not accumulate in memory. Restore time depends on VM memory and disk speed."));
}

static void gui_activity(MgGui *ui)
{
    const MgStatus *s = mg_engine_status(ui->engine);
    struct nk_context *ctx = ui->ctx;
    char line[1024];
    nk_layout_row_dynamic(ctx, ui->row*3, 1);
    nk_label_wrap(ctx, ui->feedback[0] ? ui->feedback : s->feedback);
    nk_layout_row_dynamic(ctx, ui->row, 1);
    snprintf(line, sizeof(line), tr("Komut %llu · Başarısız %llu · Düzeltilen %llu", "Actions %llu · Failed %llu · Normalized %llu"),
        (unsigned long long)s->actions, (unsigned long long)s->failed, (unsigned long long)s->normalized);
    nk_label(ctx, line, NK_TEXT_LEFT);
    snprintf(line, sizeof(line), tr("Olay %llu · VM işlem numarası %d", "Events %llu · VM process %d"), (unsigned long long)s->events, s->owned_pid);
    nk_label(ctx, line, NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, ui->row, 1);
    if (nk_button_label(ctx, tr("Ajan için tanılama istemini kopyala", "Copy repair prompt for agent"))) gui_copy_repair(ui);
    nk_layout_row_dynamic(ctx, ui->row*2, 1);
    nk_label_wrap(ctx, tr("Kopyalama ayrıca bir metin dosyası oluşturur. Pano olmadan da istemi kullanabilirsiniz.",
                         "Copying also creates a text file. You can use the prompt without a clipboard."));
    if (ui->repair_file[0]) {
        nk_layout_row_dynamic(ctx, ui->row*2, 1); nk_label_wrap(ctx, ui->repair_file);
    }
    if (ui->repair[0]) {
        nk_layout_row_dynamic(ctx, ui->row*5, 1);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX|NK_EDIT_READ_ONLY,
                                     ui->repair, sizeof(ui->repair), nk_filter_default);
    }
    nk_layout_row_dynamic(ctx, ui->row, 1);
    nk_label(ctx, tr("Kurulu kodlama aracında aç", "Open in installed coding tool"), NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, ui->row, 2);
    if (nk_button_label(ctx, "CC · Claude Code")) gui_harness(ui, "claude");
    if (nk_button_label(ctx, "OC · OpenCode")) gui_harness(ui, "opencode");
    if (nk_button_label(ctx, "GB · Grok Build")) gui_harness(ui, "grok");
    if (nk_button_label(ctx, "CX · Codex")) gui_harness(ui, "codex");
    nk_layout_row_dynamic(ctx, ui->row*2.5f, 1);
    nk_label_wrap(ctx, tr("Düğmeler yalnızca tanınan kurulu aracı açar. Araç bulunamazsa istem hazır kalır; VM kontrolü sürer.",
                         "Buttons launch only a recognized installed tool. If unavailable, the prompt stays ready and VM control continues."));
    nk_layout_row_dynamic(ctx, ui->row*2, 1);
    nk_label_wrap(ctx, s->journal_path);
}

static void gui_controls(MgGui *ui)
{
    struct nk_context *ctx = ui->ctx;
    nk_layout_row_dynamic(ctx, ui->row, 1);
    if (nk_button_label(ctx, "Can it run Doom?")) gui_action(ui, "doom");
    if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx,
        tr("Desteklenen VM'in içinde Doom'u açar. İndirme ve başlatma ilerlemesi Durum bölümünde görünür.",
           "Launches Doom inside a supported VM. Download and launch progress appears in Activity."));
    nk_layout_row_dynamic(ctx, ui->row, 3);
    if (nk_button_label(ctx, tr("Ayarlar", "Settings"))) ui->settings_tab = 0;
    if (nk_button_label(ctx, tr("Adımlar", "Steps"))) ui->settings_tab = 1;
    if (nk_button_label(ctx, tr("Durum", "Activity"))) ui->settings_tab = 2;
    if (ui->settings_tab == 0) gui_settings(ui);
    if (ui->settings_tab == 1) gui_steps(ui);
    if (ui->settings_tab == 2) gui_activity(ui);
}

static void gui_view(MgGui *ui, float height)
{
    struct nk_context *ctx = ui->ctx;
    const MgStatus *s = mg_engine_status(ui->engine);
    unsigned live_w, live_h; uint64_t generation;
    const uint8_t *pixels = mg_rfb_pixels(mg_engine_display(ui->engine), &live_w, &live_h, &generation);
    struct nk_rect bounds;
    nk_layout_row_dynamic(ctx, ui->row, 1);
    nk_label(ctx, ui->focused ? tr("VM kontrolü açık · F12 ile panele dön", "VM input active · F12 returns to controls")
                             : tr("Kontrol etmek için ekrana tıkla", "Click the screen to control the VM"), NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, height - ui->row*2, 1);
    if (nk_widget(&bounds, ctx) != NK_WIDGET_INVALID) {
        ui->viewport = bounds;
        nk_fill_rect(nk_window_get_canvas(ctx), bounds, 4, nk_rgb(64, 70, 63));
        if (!s->connected || !pixels || !live_w || !live_h) {
            const char *message = tr("Canlı ekran bekleniyor. Başlat düğmesiyle VM'i açın.",
                                     "Waiting for live display. Start the VM to connect.");
            nk_draw_text(nk_window_get_canvas(ctx), nk_rect(bounds.x+16, bounds.y+16, bounds.w-32, ui->row*3),
                         message, (int)strlen(message), ctx->style.font, nk_rgb(64,70,63), nk_rgb(235,239,232));
        }
    }
}

#ifndef _WIN32
static void gui_file_picker(MgGui *ui)
{
    struct nk_context *ctx = ui->ctx; int i;
    float w = (float)ui->width - 48, h = (float)ui->height - 48;
    if (!ui->browse_target) return;
    if (nk_begin(ctx, "Choose a file", nk_rect(24,24,w,h), NK_WINDOW_BORDER|NK_WINDOW_TITLE|NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, ui->row, 1);
        gui_edit(ui, 4, ui->browse_dir, sizeof(ui->browse_dir));
        nk_layout_row_dynamic(ctx, ui->row, 3);
        if (nk_button_label(ctx, tr("Klasörü aç", "Open folder"))) gui_read_directory(ui);
        if (nk_button_label(ctx, tr("Üst klasör", "Up"))) {
            char *slash = strrchr(ui->browse_dir, '/');
            if (slash && slash != ui->browse_dir) *slash=0;
            else snprintf(ui->browse_dir, sizeof(ui->browse_dir), "/");
            gui_read_directory(ui);
        }
        if (nk_button_label(ctx, tr("İptal", "Cancel"))) ui->browse_target = 0;
        if (ui->browse_target == 4) {
            nk_layout_row_dynamic(ctx, ui->row, 1);
            if (nk_button_label(ctx, tr("Bu klasörü kullan", "Use this folder"))) {
                char name[1024]; const char *base = strrchr(ui->new_disk, '/');
                snprintf(name, sizeof(name), "%s", base ? base+1 : "new-vm.qcow2");
                if (strlen(ui->browse_dir)+strlen(name)+2 < sizeof(ui->new_disk)) {
                    size_t directory_len=strlen(ui->browse_dir),name_len=strlen(name);
                    memcpy(ui->new_disk,ui->browse_dir,directory_len);
                    ui->new_disk[directory_len]='/';
                    memcpy(ui->new_disk+directory_len+1,name,name_len+1);
                }
                ui->browse_target = 0;
            }
        }
        nk_layout_row_dynamic(ctx, h-ui->row*4, 1);
        if (nk_group_begin(ctx, "files", NK_WINDOW_BORDER)) {
            for (i=0;i<ui->file_count;i++) {
                char label[280];
                snprintf(label,sizeof(label), "%s%s", ui->file_dirs[i] ? "[+] " : "", ui->files[i]);
                nk_layout_row_dynamic(ctx, ui->row, 1);
                if (nk_button_label(ctx,label)) {
                    char path[1300];
                    snprintf(path,sizeof(path), "%s%s%s",ui->browse_dir, strcmp(ui->browse_dir,"/") ? "/" : "",ui->files[i]);
                    if (strlen(path)>=1024) { gui_note(ui,"File path is too long."); continue; }
                    if (ui->file_dirs[i]) { strcpy(ui->browse_dir,path); gui_read_directory(ui); break; }
                    if (ui->browse_target == 4) continue;
                    strcpy(ui->browse_target == 1 ? ui->disk : ui->browse_target == 2 ? ui->iso : ui->qemu,path);
                    ui->browse_target=0; break;
                }
            }
            nk_group_end(ctx);
        }
    }
    nk_end(ctx);
}
#endif

static void gui_create_dialog(MgGui *ui)
{
    struct nk_context *ctx=ui->ctx;
    float width=ui->width>720 ? 660:(float)ui->width-48;
    if(!ui->create_dialog)return;
    if(nk_begin(ctx,"New VM disk",nk_rect(((float)ui->width-width)/2,48,width,ui->row*10+36),NK_WINDOW_BORDER|NK_WINDOW_TITLE)) {
        nk_layout_row_dynamic(ctx,ui->row,1);
        nk_label(ctx,tr("Dosya adı ve konumu", "Filename and location"),NK_TEXT_LEFT);
        gui_edit(ui,5,ui->new_disk,sizeof(ui->new_disk));
        nk_layout_row_dynamic(ctx,ui->row,1);
        if(nk_button_label(ctx,tr("Konum seç…", "Choose location…")))gui_browse(ui,4);
        nk_property_int(ctx,tr("Disk alanı (GB)", "Disk size (GB)"),1,&ui->disk_gb,1024,1,1);
        nk_layout_row_dynamic(ctx,ui->row*2.4f,1);
        nk_label_wrap(ctx,tr("Yeni bir qcow2 disk oluşturur. Bu, VM'in dosyalarını tutan ve geri dönüşü destekleyen disk biçimidir. Yalnızca kullanılan alan diskte yer kaplar. Mevcut dosyalar korunur.",
                             "Creates a qcow2 disk: a VM file that supports restoring saved states. It uses disk space as the VM writes data. Existing files are preserved."));
        nk_layout_row_dynamic(ctx,ui->row,2);
        if(nk_button_label(ctx,tr("Diski oluştur", "Create disk")) && !ui->pending[0]) {
            char path[6144],request[6400];
            if(json_string(path,sizeof(path),ui->new_disk)) {
                snprintf(request,sizeof(request),"{\"action\":\"create\",\"path\":%s,\"size_gb\":%d}",path,ui->disk_gb);
                gui_queue(ui,request);ui->creating_disk=1;ui->create_dialog=0;
            }
        }
        if(nk_button_label(ctx,tr("İptal", "Cancel")))ui->create_dialog=0;
    }
    nk_end(ctx);
}

static void gui_draw(MgGui *ui)
{
    struct nk_context *ctx = ui->ctx;
    const MgStatus *s = mg_engine_status(ui->engine);
    char line[2304];
    float body = (float)ui->height - ui->row*4 - 48;
    ui->password_field_visible=0;
    ui->viewport = nk_rect(0,0,0,0);
    if (nk_begin(ctx, "Megas", nk_rect(0,0,(float)ui->width,(float)ui->height), NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, ui->row*1.35f, 5);
        if (nk_button_label(ctx, "Start")) gui_start(ui);
        if (nk_button_label(ctx, "Pause")) gui_action(ui, "pause");
        if (nk_button_label(ctx, "Resume")) gui_action(ui, "resume");
        if (nk_button_label(ctx, "Stop")) gui_action(ui, "stop");
        if (nk_button_label(ctx, "Save screenshot")) gui_action(ui, "screenshot");
        nk_layout_row_dynamic(ctx, ui->row*1.5f, 1);
        snprintf(line,sizeof(line),"%s · %ux%u · %u threads · %.1f MiB · Android %s / video %s · %s",s->phase,s->width,s->height,
                 s->qemu_threads,(double)s->qemu_memory_bytes/(1024*1024),s->android_ready?"ready":"-",s->video_ready?"live":"-",
                 ui->feedback[0] ? ui->feedback : s->feedback);
        nk_label_wrap(ctx,line);
        if (ui->width >= 960) {
            nk_layout_row_begin(ctx,NK_STATIC,body,2);
            nk_layout_row_push(ctx,(float)ui->width-390);
            if (nk_group_begin(ctx,"display",NK_WINDOW_NO_SCROLLBAR)) { gui_view(ui,body-16); nk_group_end(ctx); }
            nk_layout_row_push(ctx,350);
            if (nk_group_begin(ctx,"controls",0)) { gui_controls(ui); nk_group_end(ctx); }
            nk_layout_row_end(ctx);
        } else {
            nk_layout_row_dynamic(ctx,ui->row,2);
            if (nk_button_label(ctx,tr("Canlı ekran", "Live screen"))) ui->tab=0;
            if (nk_button_label(ctx,tr("Kontroller", "Controls"))) { ui->tab=1; gui_release(ui); }
            nk_layout_row_dynamic(ctx,body-ui->row,1);
            if (nk_group_begin(ctx,"compact",ui->tab ? 0 : NK_WINDOW_NO_SCROLLBAR)) {
                if (ui->tab) gui_controls(ui); else gui_view(ui,body-ui->row-16);
                nk_group_end(ctx);
            }
        }
    }
    nk_end(ctx);
    gui_create_dialog(ui);
    if(ui->create_dialog)ui->viewport=nk_rect(0,0,0,0);
#ifndef _WIN32
    gui_file_picker(ui);
    if (ui->browse_target) ui->viewport=nk_rect(0,0,0,0);
#endif
}

/* A single bounded viewport buffer. Conversion happens only for new live state
 * or a resized viewport, and never allocates a second guest-sized frame. */
static int gui_scale(MgGui *ui)
{
    unsigned source_w,source_h,w,h,x,y;
    uint64_t generation;
    const uint8_t *pixels=mg_rfb_pixels(mg_engine_display(ui->engine),&source_w,&source_h,&generation);
    float scale;
    ui->picture=nk_rect(0,0,0,0);
    if (!pixels || !source_w || !source_h || ui->viewport.w<=0 || ui->viewport.h<=0) return 0;
    scale=ui->viewport.w/(float)source_w;
    if (scale>ui->viewport.h/(float)source_h) scale=ui->viewport.h/(float)source_h;
    w=(unsigned)(source_w*scale); h=(unsigned)(source_h*scale);
    if (!w || !h || w>GUI_MAX_W || h>GUI_MAX_H) return 0;
    ui->picture=nk_rect(ui->viewport.x+(ui->viewport.w-w)/2,ui->viewport.y+(ui->viewport.h-h)/2,(float)w,(float)h);
    if (generation==ui->painted_generation && w==ui->scaled_w && h==ui->scaled_h) return 1;
    if ((size_t)w*h>ui->scaled_capacity) {
        uint32_t *next;
#ifndef _WIN32
        if (ui->image) { ui->image->data=NULL; XDestroyImage(ui->image); ui->image=NULL; }
#endif
        next=realloc(ui->scaled,(size_t)w*h*sizeof(uint32_t));
        if (!next) { gui_note(ui,tr("Boş bellek az. Son durum korunuyor; pencere boyutunu küçültün.", "Low free memory. The last state is preserved; reduce the window size.")); return 0; }
        ui->scaled=next; ui->scaled_capacity=(size_t)w*h;
    }
    for (y=0;y<h;y++) for (x=0;x<w;x++) {
        size_t pos=((size_t)(y*(uint64_t)source_h/h)*source_w+(x*(uint64_t)source_w/w))*4;
        uint32_t color=((uint32_t)pixels[pos]<<16)|((uint32_t)pixels[pos+1]<<8)|pixels[pos+2];
#ifndef _WIN32
        if (DefaultVisual(ui->display,ui->screen)->red_mask!=0xff0000 ||
            DefaultVisual(ui->display,ui->screen)->green_mask!=0xff00 ||
            DefaultVisual(ui->display,ui->screen)->blue_mask!=0xff) {
            Visual *v=DefaultVisual(ui->display,ui->screen);
            unsigned long masks[3]={v->red_mask,v->green_mask,v->blue_mask}; int k;
            color=0;
            for(k=0;k<3;k++) {
                unsigned shift=0; unsigned long mask=masks[k];
                while(mask && !(mask&1)) { shift++;mask>>=1; }
                color|=(uint32_t)(((pixels[pos+k]*(uint64_t)mask+127)/255)<<shift);
            }
        }
#endif
        ui->scaled[(size_t)y*w+x]=color;
    }
#ifndef _WIN32
    if (ui->image && (w!=ui->scaled_w || h!=ui->scaled_h)) { ui->image->data=NULL; XDestroyImage(ui->image);ui->image=NULL; }
    if (!ui->image) ui->image=XCreateImage(ui->display,DefaultVisual(ui->display,ui->screen),
        (unsigned)DefaultDepth(ui->display,ui->screen),ZPixmap,0,(char*)ui->scaled,w,h,32,(int)w*4);
    if (!ui->image || ui->image->bits_per_pixel!=32) {
        gui_note(ui,"This display pixel format is unsupported. Live state remains available through the native controller.");
        return 0;
    }
    /* XImage data uses host-native uint32_t pixels. */
    { uint32_t endian=1; ui->image->byte_order=(*(unsigned char*)&endian) ? LSBFirst : MSBFirst; }
#endif
    ui->scaled_w=w;ui->scaled_h=h;ui->painted_generation=generation;
    return 1;
}

static void gui_render(MgGui *ui)
{
    int show=gui_scale(ui);
    gui_mask_password(ui);
#ifdef _WIN32
    nk_gdi_render(nk_rgb(226,229,222));
    if (show) {
        BITMAPINFO info; memset(&info,0,sizeof(info));
        info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=(LONG)ui->scaled_w;info.bmiHeader.biHeight=-(LONG)ui->scaled_h;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        SetDIBitsToDevice(ui->dc,(int)ui->picture.x,(int)ui->picture.y,ui->scaled_w,ui->scaled_h,
                         0,0,0,ui->scaled_h,ui->scaled,&info,DIB_RGB_COLORS);
    }
#else
    gui_x_prepare_text(ui);
    nk_xlib_render(None, nk_rgb(226,229,222));
    if (show && ui->image) {
        Drawable back = nk_xlib_get_drawable();
        if (back) XPutImage(ui->display, back, ui->display_gc, ui->image,
                            0, 0, (int)ui->picture.x, (int)ui->picture.y, ui->scaled_w, ui->scaled_h);
    }
    nk_xlib_blit(ui->window);
    XFlush(ui->display);
#endif
}

static const char *gui_action_label(const char *action)
{
    if (!strcmp(action,"start")) return tr("Başlat", "Start");
    if (!strcmp(action,"stop")) return tr("Durdur", "Stop");
    if (!strcmp(action,"pause")) return tr("Duraklat", "Pause");
    if (!strcmp(action,"resume")) return tr("Devam et", "Resume");
    if (!strcmp(action,"key")) return tr("Tuş", "Key input");
    if (!strcmp(action,"pointer")) return tr("Fare", "Mouse input");
    if (!strcmp(action,"click")) return tr("Tıkla", "Click");
    if (!strcmp(action,"type")) return tr("Metin yaz", "Type text");
    if (!strcmp(action,"resize")) return tr("Ekran boyutu", "Screen size");
    if (!strcmp(action,"checkpoint")) return tr("Durumu kaydet", "Save state");
    if (!strcmp(action,"restore")) return tr("Duruma dön", "Restore state");
    if (!strcmp(action,"screenshot")) return tr("Ekranı kaydet", "Screenshot");
    return action;
}

static void gui_history(MgGui *ui)
{
    const char *cursor=strstr(ui->response,"\"recent_steps\":[");
    size_t used=0;
    ui->history[0]=0;
    if(!cursor) {snprintf(ui->history,sizeof(ui->history),"%s",mg_engine_status(ui->engine)->feedback);return;}
    cursor+=strlen("\"recent_steps\":[");
    while(*cursor && *cursor!=']') {
        unsigned long long step,duration;char ok[6],action[32];int n;
        if(*cursor==',')cursor++;
        if(sscanf(cursor,"{\"step\":%llu,\"ok\":%5[^,],\"duration_ms\":%llu,\"action\":\"%31[^\"]\"",
                  &step,ok,&duration,action)!=4)break;
        n=snprintf(ui->history+used,sizeof(ui->history)-used,
            tr("Adım %llu · %s · %s · %llu ms\n", "Step %llu · %s · %s · %llu ms\n"),
            step,gui_action_label(action),!strcmp(ok,"true") ? tr("Tamam", "Done") : tr("Tanılama hazır", "Repair ready"),duration);
        if(n<0 || (size_t)n>=sizeof(ui->history)-used)break;
        used+=(size_t)n;
        cursor=strchr(cursor,'}');if(!cursor)break;cursor++;
    }
    if(!used)snprintf(ui->history,sizeof(ui->history),"%s",tr("Henüz kayıtlı bir adım yok.","No recorded steps yet."));
}

static void gui_execute_pending(MgGui *ui)
{
    char request[8192]; const MgStatus *status;
    int refresh_history;
    if (!ui->pending[0]) return;
    snprintf(request,sizeof(request),"%s",ui->pending);
    refresh_history = strstr(request, "\"action\":\"steps\"") != NULL;
    gui_wipe(ui->pending,sizeof(ui->pending));
    int result=mg_engine_submit(ui->engine,request,ui->response,sizeof(ui->response));
    gui_wipe(request,sizeof(request));
    status=mg_engine_status(ui->engine);
    gui_note(ui,status->feedback);
    if (refresh_history) gui_history(ui);
    if (ui->creating_disk) {
        if (!result) snprintf(ui->disk,sizeof(ui->disk),"%s",ui->new_disk);
        ui->creating_disk=0;
    }
}

#ifdef _WIN32
static MgGui *native_gui;
static uint32_t gui_win_keysym(WPARAM key, LPARAM detail)
{
    switch(key) {
    case VK_BACK:return 0xff08;case VK_TAB:return 0xff09;case VK_RETURN:return 0xff0d;
    case VK_ESCAPE:return 0xff1b;case VK_DELETE:return 0xffff;case VK_INSERT:return 0xff63;
    case VK_HOME:return 0xff50;case VK_END:return 0xff57;case VK_PRIOR:return 0xff55;case VK_NEXT:return 0xff56;
    case VK_LEFT:return 0xff51;case VK_UP:return 0xff52;case VK_RIGHT:return 0xff53;case VK_DOWN:return 0xff54;
    case VK_SHIFT:return ((detail>>16)&0xff)==0x36 ? 0xffe2:0xffe1;
    case VK_CONTROL:return (detail&(1<<24)) ? 0xffe4:0xffe3;
    case VK_MENU:return (detail&(1<<24)) ? 0xffea:0xffe9;
    case VK_LWIN:return 0xffeb;case VK_RWIN:return 0xffec;case VK_CAPITAL:return 0xffe5;
    default:break;
    }
    if(key>=VK_F1 && key<=VK_F12) return 0xffbe + (uint32_t)(key-VK_F1);
    { BYTE state[256]; WCHAR text[4]; int n;
      GetKeyboardState(state); state[VK_CONTROL]=state[VK_MENU]=0;
      n=ToUnicode((UINT)key,(UINT)((detail>>16)&255),state,text,4,0);
      if(n>0) return text[0]<256 ? text[0]:0x01000000u|text[0]; }
    return 0;
}

static LRESULT CALLBACK gui_window_proc(HWND window,UINT msg,WPARAM wp,LPARAM lp)
{
    MgGui *ui=native_gui;
    if(!ui) return DefWindowProcW(window,msg,wp,lp);
    if(msg==WM_CLOSE) {ui->alive=0;return 0;}
    if(msg==WM_KILLFOCUS) gui_release(ui);
    if(msg==WM_SIZE) {
        unsigned w=LOWORD(lp),h=HIWORD(lp);
        if(w && h && ui->ctx) {
            HBITMAP bitmap;
            ui->width=(int)w;ui->height=(int)h;
            /* The demo backend deletes a selected bitmap on resize. Select
             * its original bitmap first so the obsolete buffer is released. */
            SelectObject(gdi.memory_dc,ui->original_bitmap);
            DeleteObject(gdi.bitmap);
            bitmap=CreateCompatibleBitmap(gdi.window_dc,(int)w,(int)h);
            gdi.bitmap=bitmap;gdi.width=w;gdi.height=h;
            SelectObject(gdi.memory_dc,bitmap);
        }
        return 0;
    }
    if(msg==WM_GETMINMAXINFO) {
        MINMAXINFO *limits=(MINMAXINFO*)lp;
        limits->ptMinTrackSize.x=640;limits->ptMinTrackSize.y=480;
        limits->ptMaxTrackSize.x=GUI_MAX_W;limits->ptMaxTrackSize.y=GUI_MAX_H;
        limits->ptMaxSize.x=GUI_MAX_W;limits->ptMaxSize.y=GUI_MAX_H;
        return 0;
    }
    if(msg==WM_KEYDOWN || msg==WM_SYSKEYDOWN || msg==WM_KEYUP || msg==WM_SYSKEYUP) {
        int down=msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN;
        if(wp==VK_F12 && down) {gui_release(ui);return 0;}
        if(wp==VK_MENU || wp==VK_LMENU || wp==VK_RMENU || wp==VK_LWIN || wp==VK_RWIN) {
            gui_release(ui);
            return DefWindowProcW(window,msg,wp,lp);
        }
        if(ui->focused && wp<256) {
            uint32_t key=ui->held_keys[wp] ? ui->held_keys[wp]:gui_win_keysym(wp,lp);
            if(key && (!down || !ui->held_keys[wp])) gui_key(ui,key,down);
            ui->held_keys[wp]=down ? key:0;return 0;
        }
    }
    if(msg==WM_CHAR && ui->focused) return 0;
    if(msg==WM_MOUSEMOVE) {
        if(ui->ctx && nk_gdi_handle_event(window,msg,wp,lp)) return 0;
        return DefWindowProcW(window,msg,wp,lp);
    }
    if(msg==WM_LBUTTONDOWN||msg==WM_LBUTTONUP||msg==WM_RBUTTONDOWN||msg==WM_RBUTTONUP||
       msg==WM_MBUTTONDOWN||msg==WM_MBUTTONUP||msg==WM_MOUSEWHEEL) {
        int x=GET_X_LPARAM(lp),y=GET_Y_LPARAM(lp);
        unsigned button=msg==WM_LBUTTONDOWN||msg==WM_LBUTTONUP ? 1:
                        msg==WM_MBUTTONDOWN||msg==WM_MBUTTONUP ? 2:4;
        int down=msg==WM_LBUTTONDOWN||msg==WM_MBUTTONDOWN||msg==WM_RBUTTONDOWN;
        if(msg==WM_MOUSEWHEEL) {POINT point={x,y};ScreenToClient(window,&point);x=point.x;y=point.y;}
        if(down && !gui_inside(ui->picture,x,y)) gui_release(ui);
        if(gui_inside(ui->picture,x,y)||ui->buttons) {
            if(down)ui->focused=1;
            if(down) {ui->buttons|=button;SetCapture(window);}
            if(msg==WM_LBUTTONUP||msg==WM_MBUTTONUP||msg==WM_RBUTTONUP) {ui->buttons&=~button;if(!ui->buttons)ReleaseCapture();}
            if(msg==WM_MOUSEWHEEL) {
                unsigned wheel=GET_WHEEL_DELTA_WPARAM(wp)>0 ? 8:16;
                gui_pointer(ui,x,y,ui->buttons|wheel);gui_pointer(ui,x,y,ui->buttons);
            } else gui_pointer(ui,x,y,ui->buttons);
            return 0;
        }
    }
    if(ui->ctx && nk_gdi_handle_event(window,msg,wp,lp)) return 0;
    return DefWindowProcW(window,msg,wp,lp);
}

static int gui_native_open(MgGui *ui)
{
    WNDCLASSW wc;RECT rect={0,0,1180,760};
    int font_size=16;
    memset(&wc,0,sizeof(wc));wc.lpfnWndProc=gui_window_proc;wc.hInstance=GetModuleHandleW(NULL);
    wc.lpszClassName=L"MegasNative";wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    RegisterClassW(&wc);
    native_gui=ui;
    AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
    ui->window=CreateWindowW(wc.lpszClassName,L"MegasXLR VM Komuta Merkezi",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,NULL,NULL,wc.hInstance,NULL);
    if(!ui->window)return 0;
    ui->dc=GetDC(ui->window);
    font_size=MulDiv(font_size,GetDeviceCaps(ui->dc,LOGPIXELSY),96);
    ui->font=nk_gdifont_create("Segoe UI",font_size);
    if(!ui->font)return 0;
    ui->ctx=nk_gdi_init(ui->font,ui->dc,1180,760);
    ui->original_bitmap=CreateBitmap(1,1,1,1,NULL);
    ui->row=(float)ui->font->height+12;
    ShowWindow(ui->window,SW_SHOW);return ui->ctx!=NULL;
}

static int gui_native_events(MgGui *ui)
{
    MSG message;
    int changed=0;
    while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)) {
        changed=1;
        if(message.message==WM_QUIT) {ui->alive=0;break;}
        TranslateMessage(&message);DispatchMessageW(&message);
    }
    return changed;
}

static void gui_native_close(MgGui *ui)
{
    if(ui->ctx) {
        SelectObject(gdi.memory_dc,ui->original_bitmap);
        DeleteObject(gdi.bitmap); DeleteDC(gdi.memory_dc);
        DeleteObject(ui->original_bitmap); nk_free(&gdi.ctx);
    }
    nk_gdifont_del(ui->font);
    if(ui->dc)ReleaseDC(ui->window,ui->dc);
    if(ui->window)DestroyWindow(ui->window);
    native_gui=NULL;
}
#else
static int gui_native_open(MgGui *ui)
{
    XSetWindowAttributes attributes;XSizeHints hints;
    double dpi;int font_px;
    char font_name[256];
    ui->display=XOpenDisplay(NULL);if(!ui->display)return 0;
    ui->screen=DefaultScreen(ui->display);
    memset(&attributes,0,sizeof(attributes));
    attributes.event_mask=ExposureMask|KeyPressMask|KeyReleaseMask|ButtonPressMask|
        ButtonReleaseMask|PointerMotionMask|StructureNotifyMask|FocusChangeMask;
    ui->window=XCreateWindow(ui->display,RootWindow(ui->display,ui->screen),0,0,1180,760,0,
        DefaultDepth(ui->display,ui->screen),InputOutput,DefaultVisual(ui->display,ui->screen),CWEventMask,&attributes);
    {
        Atom net_wm_name = XInternAtom(ui->display, "_NET_WM_NAME", False);
        Atom net_wm_icon_name = XInternAtom(ui->display, "_NET_WM_ICON_NAME", False);
        Atom utf8_string = XInternAtom(ui->display, "UTF8_STRING", False);
        const char *title = "MegasXLR VM Komuta Merkezi";
        XStoreName(ui->display, ui->window, title);
        XSetIconName(ui->display, ui->window, title);
        XChangeProperty(ui->display, ui->window, net_wm_name, utf8_string, 8, PropModeReplace,
                        (const unsigned char *)title, (int)strlen(title));
        XChangeProperty(ui->display, ui->window, net_wm_icon_name, utf8_string, 8, PropModeReplace,
                        (const unsigned char *)title, (int)strlen(title));
    }
    ui->close_atom=XInternAtom(ui->display,"WM_DELETE_WINDOW",False);
    XSetWMProtocols(ui->display,ui->window,&ui->close_atom,1);
    memset(&hints,0,sizeof(hints));hints.flags=PMinSize|PMaxSize;
    hints.min_width=640;hints.min_height=480;hints.max_width=GUI_MAX_W;hints.max_height=GUI_MAX_H;
    XSetWMNormalHints(ui->display,ui->window,&hints);
    {XClassHint class_hint={(char*)"megas",(char*)"Megas"};XSetClassHint(ui->display,ui->window,&class_hint);}
    dpi=DisplayWidthMM(ui->display,ui->screen)>0 ? DisplayWidth(ui->display,ui->screen)*25.4/DisplayWidthMM(ui->display,ui->screen):96;
    if(dpi<72)dpi=96;
    if(dpi>192)dpi=192;
    font_px=(int)(15*dpi/96);
    snprintf(font_name,sizeof(font_name),"-*-helvetica-medium-r-normal--%d-*-*-*-*-*-*-*,fixed",font_px);
    /* XFontSet construction consults LC_CTYPE. Initialize it before the
     * backend creates its font set; nk_xlib_init configures the locale later. */
    if (!setlocale(LC_CTYPE, "") || !XSupportsLocale()) return 0;
    ui->font = nk_xfont_create(ui->display, font_name);
    if (!ui->font) return 0;
    gui_x_latin_fallback = 0;
    if (!gui_x_prepare_font(ui)) return 0;
    ui->ctx = nk_xlib_init(ui->font, ui->display, ui->screen, ui->window, 1180, 760);
    if (ui->ctx) ui->font->handle.width = gui_x_text_width;
    ui->display_gc = XCreateGC(ui->display, ui->window, 0, NULL);
    XSetGraphicsExposures(ui->display, ui->display_gc, False);
    ui->row = (float)ui->font->height + 12;
    XMapWindow(ui->display, ui->window);
    return ui->ctx != NULL;
}

static int gui_native_events(MgGui *ui)
{
    int changed=0;
    while(XPending(ui->display)) {
        changed=1;
        XEvent event;XNextEvent(ui->display,&event);
        if(event.type==ClientMessage && (Atom)event.xclient.data.l[0]==ui->close_atom) {ui->alive=0;continue;}
        if(event.type==FocusOut)gui_release(ui);
        if(event.type==ConfigureNotify) {
            ui->width=event.xconfigure.width;ui->height=event.xconfigure.height;
            if(ui->width>GUI_MAX_W || ui->height>GUI_MAX_H) {
                ui->width=ui->width>GUI_MAX_W ? GUI_MAX_W:ui->width;
                ui->height=ui->height>GUI_MAX_H ? GUI_MAX_H:ui->height;
                XResizeWindow(ui->display,ui->window,(unsigned)ui->width,(unsigned)ui->height);
            }
            if(ui->width>0 && ui->height>0)
                nk_xlib_resize((unsigned)ui->width, (unsigned)ui->height);
            continue;
        }
        if(event.type==KeyPress||event.type==KeyRelease) {
            uint32_t key=(uint32_t)XLookupKeysym(&event.xkey,(event.xkey.state&ShiftMask)?1:0);
            unsigned code=event.xkey.keycode;int down=event.type==KeyPress;
            if(key==XK_F12 && down) {gui_release(ui);continue;}
            if(key==XK_Alt_L || key==XK_Alt_R || key==XK_Super_L || key==XK_Super_R ||
               key==XK_Meta_L || key==XK_Meta_R) {
                gui_release(ui);
                continue;
            }
            if(ui->focused && code<256) {
                if(ui->held_keys[code])key=ui->held_keys[code];
                /* Ignore X11's artificial release/press repeat pair. */
                if(!down && XPending(ui->display)) {
                    XEvent next;XPeekEvent(ui->display,&next);
                    if(next.type==KeyPress && next.xkey.keycode==code && next.xkey.time==event.xkey.time) {
                        XNextEvent(ui->display,&next);continue;
                    }
                }
                if(key && (!down||!ui->held_keys[code]))gui_key(ui,key,down);
                ui->held_keys[code]=down ? key:0;continue;
            }
        }
        if(event.type==MotionNotify) {
            XEvent next;
            while(XPending(ui->display)) {
                XPeekEvent(ui->display, &next);
                if(next.type == MotionNotify && next.xmotion.window == event.xmotion.window) {
                    XNextEvent(ui->display, &event);
                } else break;
            }
            nk_xlib_handle_event(ui->display, ui->screen, ui->window, &event);
            continue;
        }
        if(event.type==ButtonPress||event.type==ButtonRelease) {
            int x=event.xbutton.x;
            int y=event.xbutton.y;
            unsigned button=event.xbutton.button;
            int down=event.type==ButtonPress;
            if(down && !gui_inside(ui->picture,x,y))gui_release(ui);
            if(gui_inside(ui->picture,x,y)||ui->buttons) {
                if(down)ui->focused=1;
                if(button<=3 && button) {
                    if(down)ui->buttons|=1u<<(button-1);else ui->buttons&=~(1u<<(button-1));
                }
                if(button==4||button==5) {
                    if(down){gui_pointer(ui,x,y,ui->buttons|(button==4 ? 8:16));gui_pointer(ui,x,y,ui->buttons);}
                }else gui_pointer(ui,x,y,ui->buttons);
                continue;
            }
        }
        nk_xlib_handle_event(ui->display,ui->screen,ui->window,&event);
    }
    return changed;
}

static void gui_native_close(MgGui *ui)
{
    if(ui->image){ui->image->data=NULL;XDestroyImage(ui->image);}
    if(ui->ctx){free(xlib.clipboard_data);xlib.clipboard_data=NULL;nk_xlib_shutdown();}
    if(ui->font)nk_xfont_del(ui->display,ui->font);
    if(ui->display_gc)XFreeGC(ui->display,ui->display_gc);
    if(ui->window)XDestroyWindow(ui->display,ui->window);
    if(ui->display)XCloseDisplay(ui->display);
}
#endif

int mg_gui_run(MgEngine *engine)
{
    MgGui *ui=calloc(1,sizeof(*ui));const MgConfig *config;
    MgStatus painted_status={0};
    uint64_t last_paint=0;
    if(!ui)return 1;
    ui->engine=engine;ui->width=1180;ui->height=760;ui->alive=1;
    config=mg_engine_config(engine);
    snprintf(ui->qemu,sizeof(ui->qemu),"%s",config->qemu);
    snprintf(ui->disk,sizeof(ui->disk),"%s",config->disk);
    snprintf(ui->iso,sizeof(ui->iso),"%s",config->iso);
    snprintf(ui->rendernode,sizeof(ui->rendernode),"%s",config->rendernode);
    snprintf(ui->username,sizeof(ui->username),"megas");
    snprintf(ui->checkpoint,sizeof(ui->checkpoint),"step-1");
    ui->ram=(int)config->ram_mb;ui->vm_w=(int)config->width;ui->vm_h=(int)config->height;
    ui->android=config->android;ui->cpus=(int)config->cpus;ui->cpu_percent=(int)config->cpu_percent;
    ui->passwordless=config->passwordless;ui->disk_gb=16;
    for (int i = 1; i < RES_PRESETS_COUNT; ++i) {
        if (res_presets[i].w == ui->vm_w && res_presets[i].h == ui->vm_h) {
            ui->res_selected = i;
            break;
        }
    }
    if(!gui_native_open(ui)) {
        fprintf(stderr,"Megas cannot open a native desktop window. Run `megas serve` for headless control; "
                       "copy this prompt to your coding agent: Enable the native display session for Megas, "
                       "preserve VM data, and verify the GUI can open.\n");
        gui_native_close(ui);free(ui);return 1;
    }
    gui_palette(ui->ctx);
    while(ui->alive && !mg_interrupted()) {
        const MgStatus *status;
        uint64_t now;
        int changed;
        mg_engine_tick(engine);
        if (strcmp(ui->seen_feedback,mg_engine_status(engine)->feedback) != 0) {
            snprintf(ui->seen_feedback,sizeof(ui->seen_feedback),"%s",mg_engine_status(engine)->feedback);
            if (!ui->pending[0]) gui_note(ui,ui->seen_feedback);
        }
        nk_input_begin(ui->ctx);changed=gui_native_events(ui);nk_input_end(ui->ctx);
        status=mg_engine_status(engine);now=mgp_now();
        if(changed || ui->pending[0] || !last_paint || now-last_paint>=250 ||
           status->frame_generation!=painted_status.frame_generation ||
           status->connected!=painted_status.connected ||
           status->running!=painted_status.running ||
           strcmp(status->phase,painted_status.phase) ||
           strcmp(status->feedback,painted_status.feedback) ||
           nk_input_is_mouse_down(&ui->ctx->input,NK_BUTTON_LEFT) ||
           nk_input_is_mouse_down(&ui->ctx->input,NK_BUTTON_RIGHT)) {
            gui_draw(ui);gui_render(ui);
            painted_status=*mg_engine_status(engine);last_paint=now;
        }
        /* The working feedback has been painted before bounded operations run. */
        gui_execute_pending(ui);
#ifdef _WIN32
        MsgWaitForMultipleObjectsEx(0,NULL,16,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
#else
        if (!XPending(ui->display)) {
            struct pollfd fd={ConnectionNumber(ui->display),POLLIN,0};
            poll(&fd,1,16);
        }
#endif
    }
    gui_release(ui);gui_password_clear(ui);gui_wipe(ui->pending,sizeof(ui->pending));
    gui_native_close(ui);free(ui->scaled);free(ui);
    return 0;
}
