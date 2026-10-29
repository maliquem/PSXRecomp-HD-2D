/* bug_report_ui.cpp - the bug report's form in Dear ImGui (bug_report.h).
 *
 * With recomp-ui's Dear ImGui in the build (RECOMP_LAUNCHER, SDL3) and an
 * OpenGL window, the form is drawn here over the presented frame instead of
 * on the title page's canvas: UTF-8 text in the launcher's TrueType font
 * (assets/fonts, Lato), the type / severity / frequency as drop-down lists,
 * the moment's picture, mouse, keyboard and controller, in the launcher's
 * PlayStation colours. bug_report.c keeps the moment, the report's folder and
 * what is written; this file only holds the form while it is open.
 *
 * Threads: SDL events and the controller are taken on the main thread (the
 * title page's pause loop) and queued; the form is built and drawn at the
 * present on the thread that owns the GL context, which replays the queue
 * into ImGui. Save / discard go back to the main thread as an action. One
 * mutex guards it all; nothing waits on another thread while holding it. */

#include "bug_report.h"

#include "mod_plugins.h"
#include "psx_sdl.h"

#if defined(PSX_SDL3) && defined(RECOMP_LAUNCHER) && defined(__has_include)
#if __has_include("imgui.h") && __has_include("imgui_impl_opengl3.h") && __has_include("launcher_theme.h")
#define BR_UI 1
#endif
#endif

#if defined(BR_UI)

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"   /* ClearActiveID: the controller leaves the description */
#include "launcher_theme.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#define BR_GL_PIXEL_UNPACK_BUFFER         0x88EC
#define BR_GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF

namespace {

enum { EV_KEY, EV_TEXT, EV_MOUSE_POS, EV_MOUSE_BUTTON, EV_WHEEL };
struct Ev {
    int kind = 0, key = 0, mods = 0, down = 0;
    float x = 0, y = 0;
    std::string text;
};

std::mutex g_mu;

/* Main thread. */
SDL_Window *g_window = nullptr;     /* the game's OpenGL window; NULL: no form here */
bool g_cursor_shown = false;        /* the form showed a hidden cursor */
std::string g_font_dir;

/* The form while it is open (g_mu). */
bool g_open = false;
std::vector<std::string> g_choice[3];   /* shown names: types, severities, frequencies */
int g_value[3];
char g_text[BUG_REPORT_TEXT_MAX + 1];   /* the description as typed (saved) */
/* The description as shown: g_text with a soft '\n' before each g_soft raw
 * offset, so lines wrap at the box (ImGui 1.91 does not wrap an input). */
char g_disp[BUG_REPORT_TEXT_MAX * 2 + 2];
std::string g_disp_prev;
std::vector<int> g_soft;
float g_wrap_w = 0;
int g_caret_prev = 0;
std::vector<uint32_t> g_thumb;
int g_thumb_w = 0, g_thumb_h = 0;
bool g_thumb_new = false;
int g_photo = 0, g_state = 0, g_clip = 0;
int g_action = 0;                   /* BUG_REPORT_UI_*, taken by the main thread */
bool g_confirm = false;             /* the next discard throws the report away */
bool g_focus_text = false;          /* put the caret in the description */
std::vector<Ev> g_queue;
std::string g_pending;              /* typed before the description took the caret */
uint32_t g_pad = 0, g_pad_seen = 0; /* PSX_MOD_PAGE_* held / as ImGui last saw it */
float g_pixel_scale = 1.0f;         /* window coordinates -> drawable pixels */
bool g_popup_was_open = false, g_text_was_active = false;   /* the last frame */
bool g_leave_text = false;          /* a controller direction while typing: leave the box */

/* GL thread (g_mu too). */
ImGuiContext *g_ctx = nullptr;
bool g_gl_ready = false;
GLuint g_tex = 0;
int g_font_px = 0;
ImFont *g_title_font = nullptr;
uint64_t g_last_tick = 0;

ImVec4 col(const LngColor &c, float a = -1.0f) { return ImVec4(c.r, c.g, c.b, a < 0 ? c.a : a); }

bool file_exists(const std::string &p) {
    FILE *f = std::fopen(p.c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}

void apply_style(float u) {
    const LauncherTheme th = launcher_theme_psx();
    ImGuiStyle &s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsDark(&s);
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = col(th.panel, 0.97f);
    c[ImGuiCol_PopupBg] = col(th.panel, 0.99f);
    c[ImGuiCol_Border] = col(th.border);
    c[ImGuiCol_FrameBg] = col(th.control);
    c[ImGuiCol_FrameBgHovered] = col(th.control_hovered);
    c[ImGuiCol_FrameBgActive] = col(th.control_hovered);
    c[ImGuiCol_Button] = col(th.control);
    c[ImGuiCol_ButtonHovered] = col(th.control_hovered);
    c[ImGuiCol_ButtonActive] = col(th.accent_dim);
    c[ImGuiCol_Header] = col(th.accent_dim, 0.85f);
    c[ImGuiCol_HeaderHovered] = col(th.accent, 0.70f);
    c[ImGuiCol_HeaderActive] = col(th.accent);
    c[ImGuiCol_Text] = col(th.text);
    c[ImGuiCol_TextDisabled] = col(th.text_muted);
    c[ImGuiCol_TextSelectedBg] = col(th.accent, 0.40f);
    c[ImGuiCol_NavCursor] = col(th.focus_ring);
    c[ImGuiCol_ScrollbarBg] = col(th.panel, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = col(th.border);
    c[ImGuiCol_ScrollbarGrabHovered] = col(th.control_hovered);
    c[ImGuiCol_ScrollbarGrabActive] = col(th.accent_dim);
    s.WindowRounding = th.radius_lg;
    s.ChildRounding = s.FrameRounding = s.PopupRounding = s.GrabRounding = th.radius_sm;
    s.WindowPadding = ImVec2(th.spacing_lg, th.spacing_lg);
    s.FramePadding = ImVec2(th.spacing_sm * 1.25f, th.spacing_sm * 0.8f);
    s.ItemSpacing = ImVec2(th.spacing_sm * 1.5f, th.spacing_sm * 1.25f);
    s.WindowBorderSize = s.PopupBorderSize = s.FrameBorderSize = 1.0f;
    s.ScrollbarSize = 14.0f;
    s.ScaleAllSizes(u);
}

/* The launcher's Lato (regular, bold for the title) at px, Latin with accents;
 * ImGui's own font when the files are missing. */
void load_fonts(int px) {
    static const ImWchar k_ranges[] = {
        0x0020, 0x00FF,   /* Basic Latin + Latin-1 (accents) */
        0x0100, 0x017F,   /* Latin Extended-A */
        0x2010, 0x2027,   /* dashes, curly quotes, ellipsis */
        0x20AC, 0x20AC,   /* euro */
        0,
    };
    ImGuiIO &io = ImGui::GetIO();
    if (g_gl_ready) ImGui_ImplOpenGL3_DestroyFontsTexture();   /* the next NewFrame builds it again */
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    const std::string reg = g_font_dir + "LatoLatin-Regular.ttf", bold = g_font_dir + "LatoLatin-Bold.ttf";
    ImFont *body = file_exists(reg) ? io.Fonts->AddFontFromFileTTF(reg.c_str(), (float)px, &cfg, k_ranges) : nullptr;
    if (!body) {
        ImFontConfig d;
        d.SizePixels = (float)px;
        body = io.Fonts->AddFontDefault(&d);
    }
    g_title_font = file_exists(bold) ? io.Fonts->AddFontFromFileTTF(bold.c_str(), px * 1.3f, &cfg, k_ranges) : nullptr;
    if (!g_title_font) g_title_font = body;
    io.FontDefault = body;
    g_font_px = px;
    apply_style((float)px / 18.0f);
}

void upload_thumb(void) {
    GLint prev_tex = 0, prev_pbo = 0, prev_align = 4, prev_row = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(BR_GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_pbo);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row);
    typedef void (APIENTRY *BindBufferFn)(GLenum, GLuint);
    static BindBufferFn bind_buffer = (BindBufferFn)SDL_GL_GetProcAddress("glBindBuffer");
    if (prev_pbo && bind_buffer) bind_buffer(BR_GL_PIXEL_UNPACK_BUFFER, 0);   /* pixels from memory */
    if (!g_tex) glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_thumb_w, g_thumb_h, 0, GL_BGRA, GL_UNSIGNED_BYTE, g_thumb.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    if (prev_pbo && bind_buffer) bind_buffer(BR_GL_PIXEL_UNPACK_BUFFER, (GLuint)prev_pbo);
    g_thumb_new = false;
}

ImGuiKey imgui_key(int k) {
    switch (k) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_INSERT: return ImGuiKey_Insert;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_A: return ImGuiKey_A;
    case SDLK_C: return ImGuiKey_C;
    case SDLK_V: return ImGuiKey_V;
    case SDLK_X: return ImGuiKey_X;
    case SDLK_Y: return ImGuiKey_Y;
    case SDLK_Z: return ImGuiKey_Z;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

/* Discard: a second press when something was written. */
void discard_press(void) {
    if (g_text[0] && !g_confirm) g_confirm = true;
    else g_action = BUG_REPORT_UI_DISCARD;
}

/* The queued input into ImGui. Escape (no list open) and Circle discard,
 * Ctrl+Enter and Start save: ImGui never sees those, so its Escape cannot
 * revert the description. Text goes to the description, which takes the
 * caret first when it does not have it. */
void replay(ImGuiIO &io) {
    for (const Ev &v : g_queue) {
        switch (v.kind) {
        case EV_KEY: {
            const bool ctrl = (v.mods & SDL_KMOD_CTRL) != 0;
            io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
            io.AddKeyEvent(ImGuiMod_Shift, (v.mods & SDL_KMOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (v.mods & SDL_KMOD_ALT) != 0);
            if (v.down && v.key == SDLK_ESCAPE && !g_popup_was_open) { discard_press(); break; }
            if (v.down && ctrl && (v.key == SDLK_RETURN || v.key == SDLK_KP_ENTER)) {
                g_action = BUG_REPORT_UI_SAVE;
                break;
            }
            const ImGuiKey k = imgui_key(v.key);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, v.down != 0);
            break;
        }
        case EV_TEXT: g_pending += v.text; break;
        case EV_MOUSE_POS: io.AddMousePosEvent(v.x, v.y); break;
        case EV_MOUSE_BUTTON:
            io.AddMousePosEvent(v.x, v.y);
            if (v.key >= 1 && v.key <= 3) io.AddMouseButtonEvent(v.key == 1 ? 0 : v.key == 3 ? 1 : 2, v.down != 0);
            break;
        case EV_WHEEL: io.AddMouseWheelEvent(v.x, v.y); break;
        default: break;
        }
    }
    g_queue.clear();
    if (!g_pending.empty()) {
        if (g_text_was_active) {
            io.AddInputCharactersUTF8(g_pending.c_str());
            g_pending.clear();
        } else {
            g_focus_text = true;
        }
    }
    /* the controller: D-pad / Cross / L1 / R1 move and press, Circle closes a
     * list or discards, Start saves */
    const uint32_t now = g_pad, was = g_pad_seen, down = now & ~was;
    g_pad_seen = now;
    static const struct { uint32_t bit; ImGuiKey key; } k_pad[] = {
        { PSX_MOD_PAGE_UP, ImGuiKey_GamepadDpadUp }, { PSX_MOD_PAGE_DOWN, ImGuiKey_GamepadDpadDown },
        { PSX_MOD_PAGE_LEFT, ImGuiKey_GamepadDpadLeft }, { PSX_MOD_PAGE_RIGHT, ImGuiKey_GamepadDpadRight },
        { PSX_MOD_PAGE_SOUTH, ImGuiKey_GamepadFaceDown }, { PSX_MOD_PAGE_L1, ImGuiKey_GamepadL1 },
        { PSX_MOD_PAGE_R1, ImGuiKey_GamepadR1 },
    };
    for (const auto &p : k_pad)
        if ((now ^ was) & p.bit) io.AddKeyEvent(p.key, (now & p.bit) != 0);
    /* the description keeps every direction for its caret: a press leaves it */
    if (g_text_was_active && (down & (PSX_MOD_PAGE_UP | PSX_MOD_PAGE_DOWN | PSX_MOD_PAGE_LEFT | PSX_MOD_PAGE_RIGHT)))
        g_leave_text = true;
    if (down & PSX_MOD_PAGE_EAST) {
        if (g_popup_was_open) io.AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
        else discard_press();
    } else if ((was & ~now) & PSX_MOD_PAGE_EAST) {
        io.AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
    }
    if (down & PSX_MOD_PAGE_START) g_action = BUG_REPORT_UI_SAVE;
}

int utf8_len(unsigned char c) { return c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }

/* raw -> lines no wider than width: disp, with a soft '\n' before each raw
 * offset in soft. Spaces stay at the end of their line. */
void wrap(const std::string &raw, float width, std::string &disp, std::vector<int> &soft) {
    ImFont *font = ImGui::GetFont();
    const float scale = ImGui::GetFontSize() / font->FontSize;
    const char *const b = raw.data(), *const e = b + raw.size();
    disp.clear();
    soft.clear();
    for (const char *line = b;;) {
        const char *nl = static_cast<const char *>(std::memchr(line, '\n', (size_t)(e - line)));
        const char *le = nl ? nl : e, *s = line;
        while (s < le) {
            const char *brk = font->CalcWordWrapPositionA(scale, s, le, width);
            if (brk <= s + 1) brk = s + utf8_len((unsigned char)*s);   /* at least one whole character */
            while (brk < le && *brk == ' ') brk++;
            if (brk >= le) break;
            disp.append(s, brk);
            disp.push_back('\n');
            soft.push_back((int)(brk - b));
            s = brk;
        }
        disp.append(s, le);
        if (!nl) break;
        disp.push_back('\n');
        line = nl + 1;
    }
}

/* The description box's callback: after an edit (or a new width), the shown
 * text without its soft breaks is the typed text again; wrap it anew and keep
 * the caret on the same character. */
int on_description(ImGuiInputTextCallbackData *d) {
    const float width = *static_cast<const float *>(d->UserData);
    if (d->EventFlag == ImGuiInputTextFlags_CallbackAlways && width == g_wrap_w) {
        g_caret_prev = d->CursorPos;
        return 0;
    }
    const std::string &old = g_disp_prev;
    const int on = (int)old.size(), nn = d->BufTextLen;
    /* the soft breaks still standing, as offsets in the new buffer: those
     * outside the edited span (between the common prefix and suffix) */
    std::vector<int> keep;
    if (d->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        int p = 0, s = 0;
        while (p < on && p < nn && old[(size_t)p] == d->Buf[p]) p++;
        while (s < on - p && s < nn - p && old[(size_t)(on - 1 - s)] == d->Buf[nn - 1 - s]) s++;
        for (size_t i = 0; i < g_soft.size(); i++) {
            const int q = g_soft[i] + (int)i;
            if (q < p) keep.push_back(q);
            else if (q >= on - s) keep.push_back(q + nn - on);
        }
    } else {
        for (size_t i = 0; i < g_soft.size(); i++) keep.push_back(g_soft[i] + (int)i);
    }
    std::string raw;
    raw.reserve((size_t)nn);
    int caret = d->CursorPos;
    size_t k = 0;
    for (int i = 0; i < nn; i++) {
        if (k < keep.size() && keep[k] == i && d->Buf[i] == '\n') {
            k++;
            if (i < d->CursorPos) caret--;
            continue;
        }
        raw.push_back(d->Buf[i]);
    }
    /* Backspace / Delete took only a soft break: they meant the character
     * before / after it */
    if (d->EventFlag == ImGuiInputTextFlags_CallbackEdit && nn < on && raw == g_text) {
        if (g_caret_prev > d->CursorPos && caret > 0) {
            int at = caret - 1;
            while (at > 0 && ((unsigned char)raw[(size_t)at] & 0xC0) == 0x80) at--;
            raw.erase((size_t)at, (size_t)(caret - at));
            caret = at;
        } else if (caret < (int)raw.size()) {
            raw.erase((size_t)caret, (size_t)utf8_len((unsigned char)raw[(size_t)caret]));
        }
    }
    if (raw.size() > BUG_REPORT_TEXT_MAX) {
        size_t n = BUG_REPORT_TEXT_MAX;
        while (n > 0 && ((unsigned char)raw[n] & 0xC0) == 0x80) n--;
        raw.resize(n);
    }
    if (caret > (int)raw.size()) caret = (int)raw.size();
    std::string disp;
    wrap(raw, width, disp, g_soft);
    d->DeleteChars(0, d->BufTextLen);
    d->InsertChars(0, disp.c_str(), disp.c_str() + disp.size());
    int pos = caret;
    for (int r : g_soft) if (r <= caret) pos++;
    d->CursorPos = d->SelectionStart = d->SelectionEnd = pos;
    g_caret_prev = pos;
    g_disp_prev = disp;
    std::memcpy(g_text, raw.c_str(), raw.size() + 1);
    g_wrap_w = width;
    return 0;
}

/* A drop-down list of the shown names; true when the player picked one. */
bool choice_list(const char *id, int which) {
    bool picked = false;
    const std::vector<std::string> &names = g_choice[which];
    int &v = g_value[which];
    ImGui::SetNextItemWidth(-FLT_MIN);
    /* the list's own margin: the frame's, not the panel's */
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().FramePadding);
    const bool open = ImGui::BeginCombo(id, names.empty() ? "" : names[(size_t)v].c_str(),
                                        ImGuiComboFlags_HeightLargest);
    ImGui::PopStyleVar();
    if (open) {
        for (int i = 0; i < (int)names.size(); i++) {
            const bool sel = i == v;
            if (ImGui::Selectable(names[(size_t)i].c_str(), sel)) { v = i; picked = true; }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return picked;
}

void build(int w, int h) {
    const LauncherTheme th = launcher_theme_psx();
    const float px = (float)g_font_px;
    const float panel_w = std::min(w * 0.92f, px * 42.0f);
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(panel_w, 0), ImVec2(panel_w, h * 0.96f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin("##bug_report", nullptr, flags)) { ImGui::End(); return; }
    /* the form keeps the keyboard: a click on the game behind gives it back */
    if (!g_popup_was_open && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        ImGui::SetWindowFocus();
    bool changed = false;
    if (g_leave_text) {
        g_leave_text = false;
        if (g_text_was_active) {
            ImGui::ClearActiveID();
            ImGui::SetNavCursorVisible(true);
        }
    }

    ImGui::PushFont(g_title_font);
    ImGui::TextColored(col(th.accent), "%s", bug_report_tr("Report a bug"));
    ImGui::PopFont();
    ImGui::Spacing();

    /* the choices on the left, the moment and what was taken on the right */
    const float thumb_w = panel_w * 0.30f;
    if (ImGui::BeginTable("##top", 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("form", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("moment", ImGuiTableColumnFlags_WidthFixed, thumb_w);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        static const char *const k_label[3] = { "Type", "Severity", "Frequency" };
        static const char *const k_id[3] = { "##type", "##severity", "##frequency" };
        float label_w = 0;
        for (const char *l : k_label) label_w = std::max(label_w, ImGui::CalcTextSize(bug_report_tr(l)).x);
        for (int r = 0; r < 3; r++) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(bug_report_tr(k_label[r]));
            ImGui::SameLine(label_w + ImGui::GetStyle().ItemSpacing.x * 2.5f);
            if (choice_list(k_id[r], r)) changed = true;
        }
        ImGui::TableSetColumnIndex(1);
        if (g_tex && g_thumb_w > 0 && g_thumb_h > 0)
            ImGui::Image((ImTextureID)(intptr_t)g_tex, ImVec2(thumb_w, thumb_w * g_thumb_h / g_thumb_w));
        const ImVec4 good = col(th.good), missing = col(th.text_muted);
        ImGui::TextColored(g_photo ? good : missing, "%s", bug_report_tr("Photo"));
        ImGui::SameLine();
        ImGui::TextColored(g_state > 0 ? good : missing, "%s%s", bug_report_tr("State"),
                           g_state < 0 ? "\xE2\x80\xA6" : "");
        ImGui::SameLine();
        if (g_clip > 0)
            ImGui::TextColored(good, "%s (%d)", bug_report_tr("Clip"), g_clip);
        else
            ImGui::TextColored(missing, "%s", bug_report_tr("Clip"));
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(bug_report_tr("Description"));
    if (g_focus_text) {
        ImGui::SetKeyboardFocusHere();
        g_focus_text = false;
    }
    {
        const ImGuiStyle &st = ImGui::GetStyle();
        float wrap_w = ImGui::GetContentRegionAvail().x - st.FramePadding.x * 2 - st.ScrollbarSize -
                       ImGui::CalcTextSize(" ").x;
        if (wrap_w < px) wrap_w = px;
        if (!g_text_was_active && wrap_w != g_wrap_w) {   /* a new width while the box is idle */
            std::string disp;
            wrap(g_text, wrap_w, disp, g_soft);
            std::memcpy(g_disp, disp.c_str(), disp.size() + 1);
            g_disp_prev = disp;
            g_wrap_w = wrap_w;
        }
        const ImGuiInputTextFlags tf = ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackAlways |
                                       ImGuiInputTextFlags_NoUndoRedo;
        if (ImGui::InputTextMultiline("##description", g_disp, sizeof g_disp, ImVec2(-FLT_MIN, px * 1.35f * 7.0f),
                                      tf, on_description, &wrap_w))
            changed = true;
    }
    g_text_was_active = ImGui::IsItemActive();
    if (!g_text[0] && !g_text_was_active) {
        const ImVec2 at = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + ImGui::GetStyle().FramePadding.x,
                                                   at.y + ImGui::GetStyle().FramePadding.y),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            bug_report_tr("Type here what happened."));
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, col(th.accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(th.accent, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(th.accent_dim));
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent_text));
    if (ImGui::Button(bug_report_tr("Save report"))) g_action = BUG_REPORT_UI_SAVE;
    ImGui::PopStyleColor(4);
    ImGui::SameLine();
    if (g_confirm) ImGui::PushStyleColor(ImGuiCol_Text, col(th.warn));
    if (ImGui::Button(bug_report_tr(g_confirm ? "Discard? Press again" : "Discard"))) discard_press();
    if (g_confirm) ImGui::PopStyleColor();
    ImGui::SameLine();
    {
        const char *hint = bug_report_tr("Ctrl+Enter saves \xC2\xB7 Esc discards");
        const float hw = ImGui::CalcTextSize(hint).x;
        const float right = ImGui::GetContentRegionMax().x;
        if (ImGui::GetCursorPosX() + hw < right) ImGui::SetCursorPosX(right - hw);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", hint);
    }
    if (changed) g_confirm = false;
    g_popup_was_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    ImGui::End();
}

}  // namespace

extern "C" {

void bug_report_ui_set_window(void *sdl_window) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_window = static_cast<SDL_Window *>(sdl_window);
    if (g_window && g_font_dir.empty()) {
        const char *base = SDL_GetBasePath();
        g_font_dir = std::string(base ? base : "") + "assets/fonts/";
    }
}

int bug_report_ui_open(const BugReportUiForm *f) {
    if (!f) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_window) return 0;
    const char *const *names[3] = { f->types, f->severities, f->frequencies };
    const int counts[3] = { f->type_count, f->severity_count, f->frequency_count };
    const int values[3] = { f->type, f->severity, f->frequency };
    for (int i = 0; i < 3; i++) {
        g_choice[i].clear();
        for (int k = 0; k < counts[i]; k++) g_choice[i].push_back(names[i][k] ? names[i][k] : "");
        g_value[i] = values[i] >= 0 && values[i] < counts[i] ? values[i] : 0;
    }
    g_text[0] = g_disp[0] = 0;
    g_disp_prev.clear();
    g_soft.clear();
    g_wrap_w = 0;
    g_caret_prev = 0;
    g_thumb.clear();
    g_thumb_w = g_thumb_h = 0;
    if (f->thumb && f->thumb_w > 0 && f->thumb_h > 0) {
        g_thumb.assign(f->thumb, f->thumb + (size_t)f->thumb_w * f->thumb_h);
        g_thumb_w = f->thumb_w;
        g_thumb_h = f->thumb_h;
    }
    g_thumb_new = !g_thumb.empty();
    g_photo = f->photo;
    g_state = f->state;
    g_clip = f->clip_frames;
    g_action = 0;
    g_confirm = false;
    g_focus_text = true;
    g_queue.clear();
    g_pending.clear();
    g_pad = g_pad_seen = 0;
    g_popup_was_open = g_text_was_active = g_leave_text = false;
    const float density = SDL_GetWindowPixelDensity(g_window);
    g_pixel_scale = density > 0 ? density : 1.0f;
    /* the mouse picks from the lists: the system's pointer while the form is up */
    g_cursor_shown = !SDL_CursorVisible();
    if (g_cursor_shown) SDL_ShowCursor();
    g_open = true;
    return 1;
}

void bug_report_ui_close(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_open) return;
    g_open = false;
    g_queue.clear();
    g_pending.clear();
    g_thumb.clear();
    g_thumb_new = false;
    if (g_cursor_shown) SDL_HideCursor();
    g_cursor_shown = false;
}

int bug_report_ui_event(const void *sdl_event) {
    if (!sdl_event) return 0;
    const SDL_Event &e = *static_cast<const SDL_Event *>(sdl_event);
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_open) return 0;
    Ev v;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        v.kind = EV_KEY;
        v.key = (int)e.key.key;
        v.mods = (int)e.key.mod;
        v.down = e.type == SDL_EVENT_KEY_DOWN;
        break;
    case SDL_EVENT_TEXT_INPUT:
        v.kind = EV_TEXT;
        v.text = e.text.text ? e.text.text : "";
        break;
    case SDL_EVENT_MOUSE_MOTION:
        v.kind = EV_MOUSE_POS;
        v.x = e.motion.x * g_pixel_scale;
        v.y = e.motion.y * g_pixel_scale;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        v.kind = EV_MOUSE_BUTTON;
        v.key = e.button.button;
        v.down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        v.x = e.button.x * g_pixel_scale;
        v.y = e.button.y * g_pixel_scale;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        v.kind = EV_WHEEL;
        v.x = e.wheel.x;
        v.y = e.wheel.y;
        break;
    default:
        return 0;
    }
    g_queue.push_back(std::move(v));
    return 1;
}

void bug_report_ui_text(const char *utf8) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_open || !utf8 || !*utf8) return;
    Ev v;
    v.kind = EV_TEXT;
    v.text = utf8;
    g_queue.push_back(std::move(v));
}

void bug_report_ui_key(int sdlk, int mods) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_open) return;
    Ev v;
    v.kind = EV_KEY;
    v.key = sdlk;
    v.mods = mods;
    v.down = 1;
    g_queue.push_back(v);
    v.down = 0;
    g_queue.push_back(v);
}

void bug_report_ui_pad(uint32_t held) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_pad = held;
}

int bug_report_ui_action(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    const int a = g_action;
    g_action = 0;
    return a;
}

void bug_report_ui_values(int *type, int *severity, int *frequency, char *text, int cap) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (type) *type = g_value[0];
    if (severity) *severity = g_value[1];
    if (frequency) *frequency = g_value[2];
    if (text && cap > 0) std::snprintf(text, (size_t)cap, "%s", g_text);
}

void bug_report_ui_set(int which, int value) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (which >= 0 && which < 3 && value >= 0 && value < (int)g_choice[which].size()) g_value[which] = value;
}

int bug_report_ui_draw(int w, int h) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_open || w <= 0 || h <= 0) return 0;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    if (!g_ctx) {
        g_ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(g_ctx);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    }
    ImGui::SetCurrentContext(g_ctx);
    if (!g_gl_ready) g_gl_ready = ImGui_ImplOpenGL3_Init(nullptr);
    if (!g_gl_ready) {
        ImGui::SetCurrentContext(prev);
        return 0;
    }
    /* about 34 lines of text to the window's height */
    const int px = std::max(14, std::min(72, (int)std::lround(h / 34.0)));
    if (std::abs(px - g_font_px) >= 2) load_fonts(px);
    if (g_thumb_new) upload_thumb();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    const uint64_t now = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency();
    io.DeltaTime = g_last_tick && now > g_last_tick && freq ? (float)((double)(now - g_last_tick) / (double)freq)
                                                            : 1.0f / 60.0f;
    if (io.DeltaTime <= 0 || io.DeltaTime > 0.5f) io.DeltaTime = 1.0f / 60.0f;
    g_last_tick = now;
    replay(io);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    build(w, h);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    ImGui::SetCurrentContext(prev);
    return 1;
}

void bug_report_ui_release_gl(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_ctx) return;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_ctx);
    if (g_gl_ready) ImGui_ImplOpenGL3_Shutdown();
    if (g_tex) glDeleteTextures(1, &g_tex);
    ImGui::DestroyContext(g_ctx);
    ImGui::SetCurrentContext(prev == g_ctx ? nullptr : prev);
    g_ctx = nullptr;
    g_gl_ready = false;
    g_tex = 0;
    g_font_px = 0;
    g_title_font = nullptr;
    g_last_tick = 0;
}

}  // extern "C"

#else  /* no Dear ImGui here: bug_report.c draws its form on the page canvas */

extern "C" {
void bug_report_ui_set_window(void *sdl_window) { (void)sdl_window; }
int  bug_report_ui_open(const BugReportUiForm *f) { (void)f; return 0; }
void bug_report_ui_close(void) {}
int  bug_report_ui_event(const void *sdl_event) { (void)sdl_event; return 0; }
void bug_report_ui_text(const char *utf8) { (void)utf8; }
void bug_report_ui_key(int sdlk, int mods) { (void)sdlk; (void)mods; }
void bug_report_ui_pad(uint32_t held) { (void)held; }
int  bug_report_ui_action(void) { return 0; }
void bug_report_ui_values(int *type, int *severity, int *frequency, char *text, int cap) {
    (void)type; (void)severity; (void)frequency; (void)text; (void)cap;
}
void bug_report_ui_set(int which, int value) { (void)which; (void)value; }
int  bug_report_ui_draw(int w, int h) { (void)w; (void)h; return 0; }
void bug_report_ui_release_gl(void) {}
}

#endif
