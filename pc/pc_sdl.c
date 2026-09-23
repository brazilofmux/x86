/* pc_sdl.c — a window for the display, and a keyboard with key-up
 *
 * By default text mode keeps its terminal (stdio echo, or the -t painter):
 * the first time the machine enters mode 13h a window opens, 4:3 like the
 * monitor a 320x200 picture was drawn for, and from then on pc_poll hands
 * it a frame at the VGA's 70 Hz; leaving the graphics mode hides it again.
 * With -w (pc_sdl_text) the window is the display from the start: text
 * modes too, drawn as the VGA draws them (pc_vga_text_frame: 720x400 for
 * 80x25, blinking cursor and attributes), letterboxed to the same 4:3.
 *
 * The keyboard is the reason a terminal cannot do this job: it reports
 * keys pressed, never keys released, and a game that polls the keyboard
 * through INT 9 and port 60h — DOOM — needs both to know when to stop
 * running. SDL scancodes become PC scancode set 1, make on key-down and
 * break (| 80h) on key-up, E0-prefixed for the gray keys, and join the same
 * raw queue the terminal feeds; typematic repeats arrive as repeated makes,
 * as they do from a real keyboard. The ASCII each make carries is what the
 * BIOS's own INT 9 puts in the ring buffer for programs that use INT 16h.
 *
 * Built only when SDL2 is found (HAVE_SDL); otherwise these are no-ops and
 * the machine stays headless.
 */
#include "pc.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef HAVE_SDL
#define SDL_MAIN_HANDLED
#include <SDL.h>

static int allowed;                          /* -w, or stdout is a terminal; -W never */
static int text_window;                      /* -w: text modes in the window too */
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_Texture *ttex;                    /* the text screen, tw x th */
static int tw, th;
static unsigned frames;                      /* 70 Hz frames: the blink counters */
static int shown;
static uint64_t next_frame_ns, next_title_ns, title_insns;
static const char *title_prog = "dos-monster";

#define FRAME_NS (1000000000ull / 70)

void pc_sdl_allow(int on, const char *prog) { allowed = on; if (prog) title_prog = prog; }
void pc_sdl_text(int on) { text_window = on; }
int pc_sdl_window_allowed(void) { return allowed; }

static int open_window(void) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "sdl: %s; staying headless\n", SDL_GetError());
        allowed = 0;
        return -1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");  /* nearest: pixels stay pixels */
    win = SDL_CreateWindow(title_prog, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           960, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 320, 200) : NULL;
    if (!tex) {
        fprintf(stderr, "sdl: %s; staying headless\n", SDL_GetError());
        allowed = 0;
        return -1;
    }
    SDL_RenderSetLogicalSize(ren, 640, 480);        /* 4:3, letterboxed if the window is not */
    shown = 1;
    return 0;
}

/* ---- keyboard ------------------------------------------------------------- */

/* SDL scancode → PC set 1. Bit 8 marks the E0-prefixed gray keys. */
static uint16_t set1(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        static const uint8_t az[26] = { 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                        0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
        return az[s - SDL_SCANCODE_A];
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return (uint16_t)(0x02 + (s - SDL_SCANCODE_1));
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return (uint16_t)(0x3B + (s - SDL_SCANCODE_F1));
    switch (s) {
    case SDL_SCANCODE_0: return 0x0B;
    case SDL_SCANCODE_RETURN: return 0x1C;
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_BACKSPACE: return 0x0E;
    case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_MINUS: return 0x0C;
    case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_LSHIFT: return 0x2A;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_RCTRL: return 0x11D;
    case SDL_SCANCODE_RALT: return 0x138;
    case SDL_SCANCODE_UP: return 0x148;
    case SDL_SCANCODE_DOWN: return 0x150;
    case SDL_SCANCODE_LEFT: return 0x14B;
    case SDL_SCANCODE_RIGHT: return 0x14D;
    case SDL_SCANCODE_HOME: return 0x147;
    case SDL_SCANCODE_END: return 0x14F;
    case SDL_SCANCODE_PAGEUP: return 0x149;
    case SDL_SCANCODE_PAGEDOWN: return 0x151;
    case SDL_SCANCODE_INSERT: return 0x152;
    case SDL_SCANCODE_DELETE: return 0x153;
    case SDL_SCANCODE_KP_ENTER: return 0x11C;
    case SDL_SCANCODE_KP_DIVIDE: return 0x135;
    case SDL_SCANCODE_KP_7: return 0x47;
    case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_4: return 0x4B;
    case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_KP_6: return 0x4D;
    case SDL_SCANCODE_KP_1: return 0x4F;
    case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53;
    case SDL_SCANCODE_KP_MINUS: return 0x4A;
    case SDL_SCANCODE_KP_PLUS: return 0x4E;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
    case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    default: return 0;
    }
}

/* The character a US keyboard's make code produces, for the BIOS buffer. */
static uint8_t ascii_for(SDL_Keycode k, uint16_t mod) {
    int shift = (mod & KMOD_SHIFT) != 0, ctrl = (mod & KMOD_CTRL) != 0;
    if (k >= 'a' && k <= 'z') {
        if (ctrl) return (uint8_t)(k - 'a' + 1);
        return (uint8_t)((shift ^ ((mod & KMOD_CAPS) != 0)) ? k - 32 : k);
    }
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return 13;
    case SDLK_ESCAPE: return 27;
    case SDLK_BACKSPACE: return 8;
    case SDLK_TAB: return 9;
    case SDLK_SPACE: return ' ';
    case SDLK_KP_PLUS: return '+';
    case SDLK_KP_MINUS: return '-';
    case SDLK_KP_MULTIPLY: return '*';
    case SDLK_KP_DIVIDE: return '/';
    }
    /* the keypad's digits and point: characters with NumLock on, cursor
     * keys (no character) with it off, as the BIOS decides */
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return (mod & KMOD_NUM) ? (uint8_t)('1' + (k - SDLK_KP_1)) : 0;
    if (k == SDLK_KP_0) return (mod & KMOD_NUM) ? '0' : 0;
    if (k == SDLK_KP_PERIOD) return (mod & KMOD_NUM) ? '.' : 0;
    if (k >= 32 && k < 127) {
        static const char plain[]   = "1234567890-=[]\\;',./`";
        static const char shifted[] = "!@#$%^&*()_+{}|:\"<>?~";
        if (!shift) return (uint8_t)k;
        for (int i = 0; plain[i]; i++) if (plain[i] == k) return (uint8_t)shifted[i];
        return (uint8_t)k;
    }
    return 0;
}

static void key(const SDL_KeyboardEvent *e, int down) {
    uint16_t code = set1(e->keysym.scancode);
    if (!code) return;
    uint8_t c8 = (uint8_t)(code & 0x7F);
    if (code & 0x100) pc_kbd_raw_key(0xE0, 0);
    /* E0-prefixed keys carry a character only where the key makes one:
     * keypad Enter and slash; the gray cursor keys make none */
    pc_kbd_raw_key(down ? c8 : (uint8_t)(c8 | 0x80), down ? ascii_for(e->keysym.sym, e->keysym.mod) : 0);
}

/* ---- frames ---------------------------------------------------------------- */

/* The text screen into ttex, (re)made at the screen's size: smooth
 * scaling, since 720x400 into a 4:3 frame is no whole multiple and nearest
 * would draw the strokes of a letter unevenly wide. */
static int draw_text(x86_cpu *c) {
    enum { MW = 1188, MH = 480 };
    static uint8_t trgb[MW * MH * 3];
    int w, h;
    if (pc_vga_text_frame(c, trgb, MW, MH, &w, &h, frames) < 0) return -1;
    if (!ttex || w != tw || h != th) {
        if (ttex) SDL_DestroyTexture(ttex);
        ttex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!ttex) return -1;
        SDL_SetTextureScaleMode(ttex, SDL_ScaleModeLinear);
        tw = w; th = h;
    }
    SDL_UpdateTexture(ttex, NULL, trgb, w * 3);
    return 0;
}

void pc_sdl_poll(x86_cpu *c, uint64_t now) {
    if (!allowed) return;
    static uint8_t rgb[640 * 480 * 3];
    static int gw = 320, gh = 200;
    if (now >= next_frame_ns) {
        next_frame_ns = now + FRAME_NS;
        frames++;
        int fw = 0, fh = 0;
        int graphics = pc_vga_frame(c, rgb, &fw, &fh) == 0;
        int text = !graphics && text_window;
        if ((graphics || text) && !win && open_window() < 0) return;
        if (win) {
            if (text && draw_text(c) < 0) text = 0;
            if ((graphics || text) && !shown) { SDL_ShowWindow(win); shown = 1; }
            if (!graphics && !text && shown) { SDL_HideWindow(win); shown = 0; }
            if (graphics && (fw != gw || fh != gh)) {    /* the mode changed size: a texture to match */
                SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, fw, fh);
                if (t) { SDL_DestroyTexture(tex); tex = t; gw = fw; gh = fh; }
            }
            if (graphics && fw == gw) SDL_UpdateTexture(tex, NULL, rgb, gw * 3);
            if (graphics || text) {
                SDL_RenderClear(ren);
                SDL_RenderCopy(ren, graphics ? tex : ttex, NULL, NULL);
                SDL_RenderPresent(ren);
                /* the host's arrow goes while the program shows its own cursor */
                int r, cl; uint16_t sm, cm;
                int guest_cursor = text && pc_mouse_text_cursor(&r, &cl, &sm, &cm);
                static int host_hidden = -1;
                if (guest_cursor != host_hidden) { SDL_ShowCursor(guest_cursor ? SDL_DISABLE : SDL_ENABLE); host_hidden = guest_cursor; }
            }
        }
    }
    if (!win) return;
    if (now >= next_title_ns) {                  /* the HUD, for now: speed in the title */
        char t[128];
        double mips = next_title_ns ? (double)(c->insn_count - title_insns) / 1e6 : 0;
        snprintf(t, sizeof t, "%s — %.1f MIPS", title_prog, mips);
        SDL_SetWindowTitle(win, t);
        title_insns = c->insn_count;
        next_title_ns = now + 1000000000ull;
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            fprintf(stderr, "dos-monster: window closed\n");
            pc_kbd_shutdown();
            SDL_Quit();
            exit(0);
        case SDL_KEYDOWN: key(&e.key, 1); break;
        case SDL_KEYUP:   key(&e.key, 0); break;
        /* The renderer's logical size makes these 640x480-frame coordinates. */
        case SDL_MOUSEMOTION: pc_mouse_motion(e.motion.x, e.motion.y); break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1
                  : e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
            pc_mouse_button(b, e.type == SDL_MOUSEBUTTONDOWN);
            break;
        }
        }
    }
}

void pc_sdl_shutdown(void) {
    if (win) SDL_Quit();
    win = NULL;
}

#else  /* no SDL: headless */
void pc_sdl_allow(int on, const char *prog) { (void)on; (void)prog; }
void pc_sdl_text(int on) { (void)on; }
int pc_sdl_window_allowed(void) { return 0; }
void pc_sdl_poll(x86_cpu *c, uint64_t now) { (void)c; (void)now; }
void pc_sdl_shutdown(void) {}
#endif
