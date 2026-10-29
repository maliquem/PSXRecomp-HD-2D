// The Lights tab: lights placed by hand per area (the framework's
// area_lights.h; area_lights.txt next to the game's exe, TCP area_lights).
// Included by main.cpp inside its namespace, after the 3D view helpers.
//
// A light is placed by a click on the game's window (a low-level mouse hook
// takes that one click, so the game never sees it): the game answers with the
// world point under it and the surface's normal. The views here (from above
// and from the side, the room's triangles from TCP area_lights "mesh") move
// it and show its height; every change goes to the game at once. A window's
// four corners are dragged on the game's window itself (the hook holds the
// button and follows the mouse); its light's way by the tip of its arrow in
// the views.

enum { PL_POINT = 1, PL_SPOT, PL_WINDOW };
enum { W_ALWAYS = 0, W_DAY, W_NIGHT };
const char* const k_kind_word[] = {"", "point", "spot", "window"};
const char* const k_when_word[] = {"always", "day", "night"};
const char* const k_src_word[] = {"sun", "moon", "fixed"};

struct PLight {
    int kind = PL_POINT;
    V3 p{0, 0, 0}, a{0, 0, 0}, b{0, 0, 0};
    V3 q{0, 0, 0};      // a window's fourth corner, across from p (round it: p, a, q, b)
    float col[3] = {1.0f, 0.85f, 0.6f};
    float power = 1.0f, reach = 600.0f, flicker = 0.0f, cone = 60.0f, dust = 0.0f, az = 0.0f, el = 0.0f;
    bool shadow = false, own_dir = false, has_dir = false;
    V3 dir{0, -1, 0};   // a window's light: the way it travels (world)
    int when = W_ALWAYS, cols = 1, rows = 1, src = 0;
    std::string note;
};
struct PArea {
    unsigned area = 0;
    bool has_amb = false;
    float amb[3] = {0.70f, 0.72f, 0.80f}, amb_night[3] = {0.36f, 0.40f, 0.55f};
    std::string amb_note;
    std::vector<PLight> l;
    bool empty() const { return !has_amb && l.empty(); }
};

// notes go inside a TCP string and a file line: no quote, bar or newline
std::string clean_note(const std::string& s) {
    std::string o;
    for (char c : s) o += c == '"' ? '\'' : (c == '|' || c == '\n' || c == '\r' || c == '#') ? '/' : c;
    return o;
}
std::string light_line(unsigned area, const PLight& l) {
    char b[512];
    int k = snprintf(b, sizeof b, "0x%08x %s %.1f %.1f %.1f", area, k_kind_word[l.kind], l.p.x, l.p.y, l.p.z);
    auto put = [&](const char* fmt, auto... v) {
        if (k < (int)sizeof b) k += snprintf(b + k, sizeof b - (size_t)k, fmt, v...);
    };
    if (l.kind == PL_WINDOW) {
        put("  %.1f %.1f %.1f  %.1f %.1f %.1f panes %d %d light %s", l.a.x, l.a.y, l.a.z, l.b.x, l.b.y, l.b.z, l.cols,
            l.rows, k_src_word[l.src]);
        if (l.own_dir || l.src == 2) put(" %.1f %.1f", l.az, l.el);
        const V3 par = l.a + l.b - l.p;
        if (std::max({std::fabs(l.q.x - par.x), std::fabs(l.q.y - par.y), std::fabs(l.q.z - par.z)}) > 0.05f)
            put(" corner %.1f %.1f %.1f", l.q.x, l.q.y, l.q.z);
        if (l.has_dir) put(" dir %.3f %.3f %.3f", l.dir.x, l.dir.y, l.dir.z);
    }
    if (l.kind == PL_SPOT) put(" at %.1f %.1f %.1f cone %.1f", l.a.x, l.a.y, l.a.z, l.cone);
    put(" color %.2f %.2f %.2f power %.2f", l.col[0], l.col[1], l.col[2], l.power);
    if (l.kind != PL_WINDOW) put(" reach %.0f", l.reach);
    if (l.shadow) put(" shadow");
    if (l.flicker > 0.0f) put(" flicker %.2f", l.flicker);
    if (l.dust > 0.0f) put(" dust %.2f", l.dust);
    if (l.when) put(" when %s", k_when_word[l.when]);
    if (!l.note.empty()) put("  # %s", clean_note(l.note).c_str());
    return b;
}
std::string area_lines(const PArea& a, char sep = '\n') {
    std::string s;
    if (a.has_amb) {
        char b[256];
        int k = snprintf(b, sizeof b, "0x%08x amb %.2f %.2f %.2f night %.2f %.2f %.2f", a.area, a.amb[0], a.amb[1],
                         a.amb[2], a.amb_night[0], a.amb_night[1], a.amb_night[2]);
        if (!a.amb_note.empty()) snprintf(b + k, sizeof b - (size_t)k, "  # %s", clean_note(a.amb_note).c_str());
        s += b;
        s += sep;
    }
    for (const PLight& l : a.l) s += light_line(a.area, l) + sep;
    return s;
}
// one file line into the area (its area word in *area); false: not a light line
bool parse_light_line(char* line, unsigned* area, PArea& into) {
    std::string note;
    if (char* h = strchr(line, '#')) {
        note = h + 1;
        *h = '\0';
        while (!note.empty() && (note.back() == '\n' || note.back() == '\r' || note.back() == ' ')) note.pop_back();
        while (!note.empty() && note.front() == ' ') note.erase(note.begin());
    }
    char* save = nullptr;
    char* t = strtok_s(line, " \t\r\n", &save);
    if (!t || !(t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))) return false;
    *area = (unsigned)strtoul(t, nullptr, 16);
    t = strtok_s(nullptr, " \t\r\n", &save);
    if (!t) return false;
    auto num = [&](float& v) {
        char* w = strtok_s(nullptr, " \t\r\n", &save);
        if (!w) return false;
        v = strtof(w, nullptr);
        return true;
    };
    auto v3 = [&](V3& v) { return num(v.x) && num(v.y) && num(v.z); };
    if (!strcmp(t, "amb")) {
        if (!num(into.amb[0]) || !num(into.amb[1]) || !num(into.amb[2])) return false;
        memcpy(into.amb_night, into.amb, sizeof into.amb);
        char* w = strtok_s(nullptr, " \t\r\n", &save);
        if (w && !strcmp(w, "night")) num(into.amb_night[0]), num(into.amb_night[1]), num(into.amb_night[2]);
        into.has_amb = true;
        into.amb_note = note;
        return true;
    }
    PLight l;
    l.kind = !strcmp(t, "point") ? PL_POINT : !strcmp(t, "spot") ? PL_SPOT : !strcmp(t, "window") ? PL_WINDOW : 0;
    if (!l.kind || !v3(l.p)) return false;
    if (l.kind == PL_WINDOW) {
        if (!v3(l.a) || !v3(l.b)) return false;
        l.reach = 0.0f;
    }
    bool has_q = false;
    while ((t = strtok_s(nullptr, " \t\r\n", &save))) {
        if (!strcmp(t, "color")) num(l.col[0]), num(l.col[1]), num(l.col[2]);
        else if (!strcmp(t, "power")) num(l.power);
        else if (!strcmp(t, "reach")) num(l.reach);
        else if (!strcmp(t, "flicker")) num(l.flicker);
        else if (!strcmp(t, "dust")) num(l.dust);
        else if (!strcmp(t, "cone")) num(l.cone);
        else if (!strcmp(t, "shadow")) l.shadow = true;
        else if (!strcmp(t, "at")) v3(l.a);
        else if (!strcmp(t, "corner")) has_q = v3(l.q);
        else if (!strcmp(t, "dir")) l.has_dir = v3(l.dir);
        else if (!strcmp(t, "panes")) {
            float c = 1, r = 1;
            num(c), num(r);
            l.cols = std::clamp((int)c, 1, 16);
            l.rows = std::clamp((int)r, 1, 16);
        } else if (!strcmp(t, "when")) {
            char* w = strtok_s(nullptr, " \t\r\n", &save);
            l.when = w && !strcmp(w, "day") ? W_DAY : w && !strcmp(w, "night") ? W_NIGHT : W_ALWAYS;
        } else if (!strcmp(t, "light")) {
            char* w = strtok_s(nullptr, " \t\r\n", &save);
            l.src = w && !strcmp(w, "moon") ? 1 : w && !strcmp(w, "fixed") ? 2 : 0;
            // a direction of its own may follow (two numbers): looked at without
            // cutting the next word (strtok_s ends it, and the rest with it)
            const char* x = save ? save : "";
            while (*x == ' ' || *x == '\t') x++;
            char* e = nullptr;
            const float az = strtof(x, &e);
            if (e != x && (*e == '\0' || isspace((unsigned char)*e))) {
                strtok_s(nullptr, " \t\r\n", &save);
                char* y = strtok_s(nullptr, " \t\r\n", &save);
                l.az = az;
                l.el = y ? strtof(y, nullptr) : 30.0f;
                l.own_dir = true;
            }
        }
    }
    if (l.kind == PL_WINDOW && !has_q) l.q = l.a + l.b - l.p;
    l.note = note;
    into.l.push_back(l);
    return true;
}

struct LightsFile {
    std::string path, error;
    std::vector<PArea> a;
    PArea* find(unsigned area) {
        for (auto& x : a)
            if (x.area == area) return &x;
        return nullptr;
    }
    void load() {
        a.clear();
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return;
        char line[512];
        while (fgets(line, sizeof line, f)) {
            unsigned area = 0;
            PArea tmp;
            char copy[512];
            snprintf(copy, sizeof copy, "%s", line);
            if (!parse_light_line(copy, &area, tmp) || !area) continue;
            PArea* x = find(area);
            if (!x) {
                a.push_back(PArea{});
                x = &a.back();
                x->area = area;
            }
            if (tmp.has_amb) {
                x->has_amb = true;
                memcpy(x->amb, tmp.amb, sizeof tmp.amb);
                memcpy(x->amb_night, tmp.amb_night, sizeof tmp.amb_night);
                x->amb_note = tmp.amb_note;
            }
            for (auto& l : tmp.l) x->l.push_back(l);
        }
        fclose(f);
    }
    bool save() {
        const std::string tmp = path + ".tmp";
        FILE* f = fopen(tmp.c_str(), "w");
        if (!f) {
            error = "nao consegui gravar " + tmp;
            return false;
        }
        fprintf(f, "# area_lights.txt: luzes colocadas a mao por area (psx_sun_tool, aba Luzes). O jogo le ao abrir.\n"
                   "# 0xAREA amb R G B [night R G B]            (o escuro do interior onde nenhuma luz chega)\n"
                   "# 0xAREA point X Y Z color R G B power F reach A [shadow] [flicker F] [when day|night]\n"
                   "# 0xAREA spot X Y Z at X Y Z cone GRAUS ...  (foco)\n"
                   "# 0xAREA window X Y Z  X Y Z  X Y Z panes C L light sun|moon|fixed [AZ EL] [corner X Y Z] ...  (janela)\n");
        for (const auto& x : a)
            if (!x.empty()) fputs(area_lines(x).c_str(), f);
        const bool ok = fclose(f) == 0;
        if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            error = "nao consegui gravar " + path;
            return false;
        }
        error.clear();
        return true;
    }
};

// ---- the click on the game's window ----------------------------------------------------
// While armed, a low-level mouse hook takes the next left click outside this
// tool's own window, over a window of the game (its process, or any SDL
// window when the game's process is not known): its place over that window's
// client area (0..1) and the client's height over its width.
// In drag mode (a window's corners) it stays armed: the press (taken), the
// moves while held (the game sees them, no click) and the release (taken)
// each give a place, and up tells the release.
struct GameClick {
    std::atomic<bool> armed{false}, got{false}, drag{false}, down{false}, up{false};
    float fx = 0, fy = 0, aspect = 0.5625f;
    HWND own = nullptr, held = nullptr;
    DWORD game_pid = 0;
    HHOOK hook = nullptr;
    bool swallow_up = false;
};
GameClick g_click;
LRESULT CALLBACK click_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_click.armed && g_click.drag) {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lp;
        auto place = [&](HWND w) {
            POINT p = m->pt;
            RECT rc;
            ScreenToClient(w, &p);
            GetClientRect(w, &rc);
            if (rc.right <= 0 || rc.bottom <= 0) return false;
            g_click.fx = std::clamp((float)p.x / (float)rc.right, 0.0f, 1.0f);
            g_click.fy = std::clamp((float)p.y / (float)rc.bottom, 0.0f, 1.0f);
            g_click.aspect = (float)rc.bottom / (float)rc.right;
            g_click.got = true;
            return true;
        };
        if (g_click.down && g_click.held) {
            if (wp == WM_MOUSEMOVE) place(g_click.held);
            else if (wp == WM_LBUTTONUP) {
                place(g_click.held);
                g_click.down = false;
                g_click.up = true;
                return 1;
            }
        } else if (wp == WM_LBUTTONDOWN) {
            HWND w = GetAncestor(WindowFromPoint(m->pt), GA_ROOT);
            DWORD pid = 0;
            if (w) GetWindowThreadProcessId(w, &pid);
            char cls[64] = "";
            if (w) GetClassNameA(w, cls, sizeof cls);
            if (w && w != g_click.own && (g_click.game_pid ? pid == g_click.game_pid : strcmp(cls, "SDL_app") == 0) && place(w)) {
                g_click.held = w;
                g_click.down = true;
                return 1;
            }
        }
    } else if (code == HC_ACTION && g_click.armed) {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lp;
        if (wp == WM_LBUTTONUP && g_click.swallow_up) {
            g_click.swallow_up = false;
            return 1;
        }
        if (wp == WM_LBUTTONDOWN) {
            HWND w = GetAncestor(WindowFromPoint(m->pt), GA_ROOT);
            DWORD pid = 0;
            if (w) GetWindowThreadProcessId(w, &pid);
            char cls[64] = "";
            if (w) GetClassNameA(w, cls, sizeof cls);
            const bool game = w && w != g_click.own && (g_click.game_pid ? pid == g_click.game_pid : strcmp(cls, "SDL_app") == 0);
            if (game) {
                POINT p = m->pt;
                RECT rc;
                ScreenToClient(w, &p);
                GetClientRect(w, &rc);
                if (rc.right > 0 && rc.bottom > 0) {
                    g_click.fx = (float)p.x / (float)rc.right;
                    g_click.fy = (float)p.y / (float)rc.bottom;
                    g_click.aspect = (float)rc.bottom / (float)rc.right;
                    g_click.got = true;
                    g_click.armed = false;
                    g_click.swallow_up = true;
                    return 1;   // the game never sees this click
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
void click_arm(bool on, bool drag = false) {
    if (on && !g_click.hook) g_click.hook = SetWindowsHookExA(WH_MOUSE_LL, click_hook, GetModuleHandleA(nullptr), 0);
    g_click.got = false;
    g_click.drag = on && drag;
    g_click.down = false;
    g_click.up = false;
    g_click.armed = on && g_click.hook;
    if (!on && g_click.hook && !g_click.swallow_up) {
        UnhookWindowsHookEx(g_click.hook);
        g_click.hook = nullptr;
    }
}

// ---- the tab ------------------------------------------------------------------------------
enum { K_SET = 10, K_MARK, K_FORCE, K_PICK, K_MESH, K_POLL };
enum { ARM_NONE = 0, ARM_NEW_POINT, ARM_NEW_SPOT, ARM_MOVE, ARM_AIM, ARM_WIN1, ARM_WIN2, ARM_CORNERS };

// a window's frame: across it (w), into the room (n, toward the camera), up
struct WinFrame {
    V3 w, n, up{0, 1, 0};
};
V3 win_center(const PLight& l) { return (l.p + l.a + l.q + l.b) * 0.25f; }
WinFrame win_frame(const PLight& l, V3 cam) {
    WinFrame f;
    f.w = norm(l.a - l.p);
    f.n = norm(cross(l.q - l.p, l.b - l.a));   // across its diagonals: any four corners
    if (dot(f.n, cam - l.p) < 0) f.n = f.n * -1.0f;
    return f;
}
// its light's way from the angle across (degrees, 0 = straight in) and the drop (degrees down)
V3 win_dir(const WinFrame& f, float across, float drop) {
    const float a = across * k_deg, e = drop * k_deg;
    return norm(f.n * (std::cos(e) * std::cos(a)) + f.w * (std::cos(e) * std::sin(a)) - f.up * std::sin(e));
}
// the light placed by a view: its window's center, or the light itself
V3 anchor(const PLight& l) { return l.kind == PL_WINDOW ? win_center(l) : l.p; }
const float k_tip = 300.0f;   // a window's arrow: this long in the world

struct LightsTab {
    GameLink* link = nullptr;
    LightsFile file;
    unsigned area = 0;
    PArea cur;             // what the game is shown for the area
    int sel = -1;
    int arm = ARM_NONE;
    unsigned pick_seq = 0, mesh_seq = 0, poll_seq = 0;
    float lift = 40.0f;    // a new light: this far off the surface clicked
    std::string msg;
    bool running = false, cam_ok = false;
    V3 cam_r{1, 0, 0}, cam_f{0, 0, 1}, cam_p{0, 0, 0};
    std::vector<float> mesh;   // the room's triangles (x, y, z per corner)
    unsigned mesh_area = 0;
    int shown_mark = -3, shown_force = -1;
    char note[96] = "";
    std::string copy_from;
    V3 win_p1{0, 0, 0}, win_n1{0, 0, 1};   // the first corner clicked, its wall's normal
    bool win_redo = false;                 // the clicks remake the picked window's corners
    // dragging a window's corners on the game: the corner held (-1 none yet),
    // pressed, released (the last places still coming)
    int corner = -1, shown_corner = -2;
    bool corner_down = false, corner_up = false;
    bool drag_tip = false;   // the views drag the arrow's tip (not the light)
    ImVec2 tip_seen[2];      // the picked light's tip on each view (tests)

    PArea saved(unsigned a) {
        PArea* x = file.find(a);
        if (x) return *x;
        PArea e;
        e.area = a;
        return e;
    }
    bool dirty() { return area_lines(cur) != area_lines(saved(area)); }
    void send() {
        if (!area) return;
        char head[64];
        snprintf(head, sizeof head, "{\"cmd\":\"area_lights\",\"area\":\"0x%08x\",\"set\":\"", area);
        std::string lines = area_lines(cur, '|');
        link->send_keyed(K_SET, head + lines + "\"}", area);
    }
    void send_saved(unsigned a) {
        char head[64];
        snprintf(head, sizeof head, "{\"cmd\":\"area_lights\",\"area\":\"0x%08x\",\"set\":\"", a);
        link->send_keyed(K_SET, head + area_lines(saved(a), '|') + "\"}", a);
    }
    void switch_area(unsigned a) {
        if (area && dirty()) send_saved(area);   // what was not applied goes back
        area = a;
        cur = saved(a);
        sel = cur.l.empty() ? -1 : 0;
        arm = ARM_NONE;
        click_arm(false);
        corner_end();
        msg.clear();
        mesh.clear();
        mesh_area = 0;
    }
    // the tab shown (and the tool visible): markers and the lights forced on in the game
    void visible(bool on) {
        const int mark = on ? sel : -2, force = on ? 1 : 0, cn = on ? corner : -1;
        if (mark != shown_mark || cn != shown_corner) {
            link->send_keyed(K_MARK, "{\"cmd\":\"area_lights\",\"mark\":" + std::to_string(mark) +
                                         ",\"corner\":" + std::to_string(cn) + "}", 0);
            shown_mark = mark;
            shown_corner = cn;
        }
        if (force != shown_force) {
            link->send_keyed(K_FORCE, "{\"cmd\":\"area_lights\",\"force\":" + std::to_string(force) + "}", 0);
            shown_force = force;
        }
        link->poll_lights(on);
        if (!on && arm) arm = ARM_NONE, click_arm(false), corner_end();
    }
    void corner_end() {
        corner = -1;
        corner_down = corner_up = false;
    }
    void corners_mode(bool on) {
        corner_end();
        arm = on ? ARM_CORNERS : ARM_NONE;
        click_arm(on, true);
        msg = !on ? "" : g_click.hook ? "Segure o botão num canto da janela no jogo e arraste. Esc termina."
                                      : "Não consegui escutar o mouse (SetWindowsHookEx).";
    }
    void changed() { send(); }
    // the game's answers: the poll (camera, running), a pick, the mesh
    void update(unsigned game_area) {
        if (game_area && game_area != area) switch_area(game_area);
        const bool pick_idle = link->idle(K_PICK);   // before the answers: all of them are in then
        if (arm == ARM_CORNERS) {
            if (g_click.down) corner_down = true;
            if (g_click.up.exchange(false)) corner_up = true;
        }
        std::string r;
        unsigned s = link->answer(K_POLL, r);
        if (s != poll_seq) {
            poll_seq = s;
            float v = 0;
            running = read_nums(find_key(r.c_str(), "running"), &v, 1) == 1 && v > 0.5f;
            float c[10];
            cam_ok = read_nums(find_key(r.c_str(), "camera"), c, 10) == 10 && c[0] > 0.5f;
            if (cam_ok) cam_r = {c[1], c[2], c[3]}, cam_f = {c[4], c[5], c[6]}, cam_p = {c[7], c[8], c[9]};
        }
        s = link->answer(K_PICK, r);
        if (s != pick_seq) {
            pick_seq = s;
            float w[3], n[3];
            if (strstr(r.c_str(), "\"ok\":true") && read_nums(find_key(r.c_str(), "world"), w, 3) == 3 &&
                read_nums(find_key(r.c_str(), "normal"), n, 3) == 3)
                picked(V3{w[0], w[1], w[2]}, V3{n[0], n[1], n[2]});
            else if (arm != ARM_CORNERS) {
                const char* e = find_key(r.c_str(), "error");
                msg = std::string("Clique sem chão 3D: ") + (e ? std::string(e).substr(0, 80) : "sem resposta");
            }
        }
        if (corner_up && pick_idle && !g_click.got) corner_end();   // its last place is in
        s = link->answer(K_MESH, r);
        if (s != mesh_seq) {
            mesh_seq = s;
            mesh.clear();
            const char* p = find_key(r.c_str(), "mesh");
            if (p) {
                while (*p == ' ' || *p == '[') p++;
                while (*p && *p != ']') {
                    char* e;
                    const float v = strtof(p, &e);
                    if (e == p) break;
                    mesh.push_back(v);
                    p = e;
                    while (*p == ',' || *p == ' ') p++;
                }
            }
            mesh.resize(mesh.size() / 9 * 9);
            mesh_area = area;
        }
        if (g_click.got.exchange(false)) {
            char req[200];
            snprintf(req, sizeof req, "{\"cmd\":\"area_lights\",\"pick\":1,\"wx\":%.5f,\"wy\":%.5f,\"wa\":%.5f}", g_click.fx,
                     g_click.fy, g_click.aspect);
            link->send_keyed(K_PICK, req, 0);
        }
        if (area && mesh_area != area && cam_ok) request_mesh();
    }
    void request_mesh() {
        mesh_area = area;   // asked (the answer fills it)
        link->send_keyed(K_MESH, "{\"cmd\":\"area_lights\",\"mesh\":1}", 0);
    }
    // a window's corner dragged on the game: the one nearest the press, then
    // where the mouse is, on the window's wall (the surface there when it is
    // that wall; else where the camera's ray through it meets the window's plane)
    void corner_to(V3 w, V3 n) {
        if (!corner_down || sel < 0 || sel >= (int)cur.l.size() || cur.l[sel].kind != PL_WINDOW) return;
        PLight& l = cur.l[sel];
        V3* c[4] = {&l.p, &l.a, &l.q, &l.b};
        const V3 mid = win_center(l), nw = norm(cross(l.q - l.p, l.b - l.a));
        V3 x = w;
        if (std::fabs(dot(n, nw)) < 0.9f || std::fabs(dot(w - mid, nw)) > 30.0f) {
            const V3 ray = w - cam_p;
            const float dn = dot(nw, ray);
            if (std::fabs(dn) > 1e-4f) x = cam_p + ray * (dot(nw, mid - cam_p) / dn);
        }
        if (corner < 0) {
            float bd = 1e30f;
            for (int k = 0; k < 4; k++) {
                const V3 d = *c[k] - x;
                if (dot(d, d) < bd) bd = dot(d, d), corner = k;
            }
            msg = "Canto " + std::to_string(corner + 1) + " seguindo o mouse.";
        }
        *c[corner] = x;
        changed();
    }
    void picked(V3 w, V3 n) {
        if (arm == ARM_CORNERS) {
            corner_to(w, n);
            return;
        }
        const V3 at = w + n * lift;
        if (arm == ARM_NEW_POINT || arm == ARM_NEW_SPOT) {
            PLight l;
            l.kind = arm == ARM_NEW_SPOT ? PL_SPOT : PL_POINT;
            l.p = at;
            l.a = w;   // a spot faces the surface clicked
            l.note = l.kind == PL_SPOT ? "foco" : "luz";
            if (l.kind == PL_SPOT) {   // a spot hangs over its target, looking down at it
                l.p = w + V3{0, 1, 0} * 220.0f;
                l.a = w;
            }
            cur.l.push_back(l);
            sel = (int)cur.l.size() - 1;
            msg = "Luz nova no ponto clicado.";
        } else if (arm == ARM_MOVE && sel >= 0) {
            const V3 off = cur.l[sel].kind == PL_SPOT ? cur.l[sel].a - cur.l[sel].p : V3{0, 0, 0};
            cur.l[sel].p = at;
            if (cur.l[sel].kind == PL_SPOT) cur.l[sel].a = at + off;
            msg = "Luz movida.";
        } else if (arm == ARM_AIM && sel >= 0) {
            cur.l[sel].a = w;
            msg = "Foco apontado.";
        } else if (arm == ARM_WIN1) {
            win_p1 = w;
            win_n1 = n;
            arm = ARM_WIN2;
            msg = "Agora o canto oposto da janela.";
            click_arm(true);
            return;
        } else if (arm == ARM_WIN2) {
            // an upright rectangle on the wall between the two corners
            const V3 up{0, 1, 0}, ww = norm(cross(up, win_n1)), d = w - win_p1;
            const float dw = dot(d, ww), dh = dot(d, up);
            const V3 o = win_p1 + ww * std::min(0.0f, dw) + up * std::min(0.0f, dh);
            PLight l = win_redo && sel >= 0 ? cur.l[sel] : PLight{};
            l.kind = PL_WINDOW;
            l.p = o;
            l.a = o + ww * std::fabs(dw);
            l.b = o + up * std::fabs(dh);
            l.q = l.a + l.b - l.p;
            l.reach = 0.0f;
            if (!win_redo) {
                l.cols = 2, l.rows = 3, l.dust = 0.5f, l.note = "janela";
                l.col[0] = 1.0f, l.col[1] = 0.86f, l.col[2] = 0.62f;
                // straight in, a little toward the camera, 32 degrees down
                const WinFrame f = win_frame(l, cam_p);
                const float side = dot(f.w, cam_f) > 0 ? -25.0f : 25.0f;
                l.dir = win_dir(f, side, 32.0f);
                l.has_dir = true;
            }
            if (win_redo && sel >= 0) cur.l[sel] = l;
            else {
                cur.l.push_back(l);
                sel = (int)cur.l.size() - 1;
            }
            win_redo = false;
            msg = std::fabs(dw) < 4.0f || std::fabs(dh) < 4.0f ? "Janela muito fina: clique em cantos opostos." : "Janela marcada.";
        }
        arm = ARM_NONE;
        click_arm(false);
        changed();
    }
    void arm_click(int what) {
        arm = what;
        click_arm(true);
        msg = g_click.hook ? "Clique no jogo (o jogo não recebe esse clique). Esc cancela."
                           : "Não consegui escutar o mouse (SetWindowsHookEx).";
    }
    void apply() {
        PArea* x = file.find(area);
        if (!x) {
            file.a.push_back(PArea{});
            x = &file.a.back();
        }
        *x = cur;
        x->area = area;
        if (x->empty()) file.a.erase(file.a.begin() + (x - file.a.data()));
        msg = file.save() ? "Gravado em " + file.path : file.error;
    }
    void revert() {
        cur = saved(area);
        if (sel >= (int)cur.l.size()) sel = (int)cur.l.size() - 1;
        changed();
    }
    // floor axes (y up): the camera's right and forward laid flat
    V3 flat(V3 v) {
        v.y = 0;
        return norm(v);
    }
    // the whole light (a window: its four corners; a spot: what it faces too)
    void shift(PLight& l, V3 d) {
        l.p = l.p + d;
        if (l.kind == PL_SPOT) l.a = l.a + d;
        if (l.kind == PL_WINDOW) l.a = l.a + d, l.b = l.b + d, l.q = l.q + d;
    }
    void nudge(V3 d) {
        if (sel < 0) return;
        shift(cur.l[sel], d);
        changed();
    }
    void keys() {
        if (sel < 0 || ImGui::GetIO().WantTextInput) return;
        const float st = ImGui::GetIO().KeyShift ? 40.0f : 10.0f;
        const V3 r = flat(cam_r), f = flat(cam_f);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) nudge(r * -st);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nudge(r * st);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) nudge(f * st);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) nudge(f * -st);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) nudge(V3{0, st, 0});
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) nudge(V3{0, -st, 0});
    }

    // ---- the views: from above (forward up) and from the side (up up) ----
    void views(ImVec2 p0, ImVec2 size) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float hh = size.y * 0.5f - 4.0f;
        view_one(dl, p0, ImVec2(size.x, hh), true);
        view_one(dl, ImVec2(p0.x, p0.y + hh + 8.0f), ImVec2(size.x, hh), false);
    }
    void view_one(ImDrawList* dl, ImVec2 p0, ImVec2 size, bool top) {
        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton(top ? "##top" : "##side", size);
        const bool active = ImGui::IsItemActive();
        const ImVec2 p1(p0.x + size.x, p0.y + size.y);
        dl->PushClipRect(p0, p1, true);
        dl->AddRectFilled(p0, p1, IM_COL32(28, 30, 38, 255));
        const V3 r = flat(cam_r), f = flat(cam_f), up{0, 1, 0};
        auto ax = [&](V3 v) { return ImVec2(dot(v, r), top ? -dot(v, f) : -v.y); };
        // the bounds: the room's triangles and the lights
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
        auto grow = [&](V3 v) {
            const ImVec2 a = ax(v);
            x0 = std::min(x0, a.x), x1 = std::max(x1, a.x), y0 = std::min(y0, a.y), y1 = std::max(y1, a.y);
        };
        for (size_t i = 0; i + 2 < mesh.size(); i += 3) grow(V3{mesh[i], mesh[i + 1], mesh[i + 2]});
        for (const auto& l : cur.l) grow(anchor(l));
        if (x0 > x1) x0 = -500, x1 = 500, y0 = -500, y1 = 500;
        const float pad = 40.0f, sc = std::min((size.x - 2 * pad) / std::max(x1 - x0, 1.0f), (size.y - 2 * pad) / std::max(y1 - y0, 1.0f));
        const ImVec2 c((x0 + x1) * 0.5f, (y0 + y1) * 0.5f), mid(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
        auto S = [&](V3 v) {
            const ImVec2 a = ax(v);
            return ImVec2(mid.x + (a.x - c.x) * sc, mid.y + (a.y - c.y) * sc);
        };
        auto W = [&](ImVec2 s, float keep) {   // a place on the view back to the world (keep: the axis it hides)
            const float ux = (s.x - mid.x) / sc + c.x, uy = (s.y - mid.y) / sc + c.y;
            return top ? r * ux + f * (-uy) + up * keep : r * ux + f * keep + up * (-uy);
        };
        // the room: triangles far to near, lit by their facing
        struct T {
            ImVec2 p[3];
            float d;
            ImU32 c;
        };
        std::vector<T> tris;
        for (size_t i = 0; i + 8 < mesh.size(); i += 9) {
            const V3 a{mesh[i], mesh[i + 1], mesh[i + 2]}, b{mesh[i + 3], mesh[i + 4], mesh[i + 5]},
                cc{mesh[i + 6], mesh[i + 7], mesh[i + 8]};
            const V3 n = norm(cross(b - a, cc - a));
            const float k = top ? std::fabs(n.y) : std::fabs(dot(n, f));
            const int g = (int)(70 + 110 * k);
            T t{{S(a), S(b), S(cc)}, top ? (a.y + b.y + cc.y) : -(dot(a, f) + dot(b, f) + dot(cc, f)),
                IM_COL32(g, g - 6, g - 14, 255)};
            tris.push_back(t);
        }
        std::sort(tris.begin(), tris.end(), [](const T& a, const T& b) { return a.d < b.d; });
        for (const T& t : tris) dl->AddTriangleFilled(t.p[0], t.p[1], t.p[2], t.c);
        // an arrow's tip (a window's light, what a spot faces): false = none
        auto tip_of = [&](const PLight& l, V3& t) {
            if (l.kind == PL_WINDOW && l.has_dir) t = win_center(l) + l.dir * k_tip;
            else if (l.kind == PL_SPOT) t = l.a;
            else return false;
            return true;
        };
        // the lights: a disc (its reach, from above), a dot, its number
        for (int i = 0; i < (int)cur.l.size(); i++) {
            const PLight& l = cur.l[i];
            const ImVec2 s = S(anchor(l));
            const ImU32 col = IM_COL32((int)(std::min(l.col[0], 1.0f) * 255), (int)(std::min(l.col[1], 1.0f) * 255),
                                       (int)(std::min(l.col[2], 1.0f) * 255), 255);
            if (top && l.kind != PL_WINDOW) dl->AddCircle(s, l.reach * sc, (col & 0x00FFFFFF) | 0x50000000, 48, 1.5f);
            if (!top) {   // its height over the floor below (y 0 or the lowest point under it)
                const V3 a = anchor(l);
                dl->AddLine(s, S(V3{a.x, 0.0f, a.z}), IM_COL32(255, 255, 255, 60), 1.0f);
            }
            if (l.kind == PL_SPOT) dl->AddLine(s, S(l.a), (col & 0x00FFFFFF) | 0xA0000000, 2.0f);
            if (l.kind == PL_WINDOW) {   // its four corners and its light's way
                dl->AddQuad(S(l.p), S(l.a), S(l.q), S(l.b), col, 2.0f);
                if (l.has_dir) dl->AddLine(s, S(win_center(l) + l.dir * k_tip), (col & 0x00FFFFFF) | 0x90000000, 3.0f);
            }
            V3 t;
            if (tip_of(l, t)) {   // the tip to drag: a handle
                const ImVec2 ts = S(t);
                if (i == sel) tip_seen[top ? 0 : 1] = ts;
                const bool held = i == sel && drag_tip && ImGui::IsMouseDown(ImGuiMouseButton_Left);
                if (held && l.kind == PL_WINDOW) dl->AddCircle(s, k_tip * sc, IM_COL32(255, 255, 255, 40), 64, 1.0f);
                dl->AddCircleFilled(ts, held ? 7.0f : 5.0f, (col & 0x00FFFFFF) | 0xE0000000);
                dl->AddCircle(ts, held ? 9.0f : 6.5f, i == sel ? IM_COL32(255, 190, 50, 255) : IM_COL32(0, 0, 0, 200), 20, 1.5f);
            }
            dl->AddCircleFilled(s, i == sel ? 8.0f : 6.0f, col);
            dl->AddCircle(s, i == sel ? 10.0f : 7.0f, i == sel ? IM_COL32(255, 190, 50, 255) : IM_COL32(0, 0, 0, 200), 24, 2.0f);
            char n[8];
            snprintf(n, sizeof n, "%d", i + 1);
            dl->AddText(ImVec2(s.x + 10, s.y - 8), IM_COL32(255, 255, 255, 220), n);
        }
        // drag: an arrow's tip (the picked light's first), else the light
        // nearest the press, along the view's plane
        ImGuiIO& io = ImGui::GetIO();
        auto d2 = [&](ImVec2 s) { return (s.x - io.MousePos.x) * (s.x - io.MousePos.x) + (s.y - io.MousePos.y) * (s.y - io.MousePos.y); };
        if (ImGui::IsItemClicked() && arm == ARM_NONE && !cur.l.empty()) {
            int best = -1;
            float bd = 14.0f * 14.0f;
            drag_tip = false;
            for (int j = 0; j < (int)cur.l.size(); j++) {
                const int i = (j + std::max(sel, 0)) % (int)cur.l.size();   // the picked one wins a tie
                V3 t;
                if (tip_of(cur.l[i], t) && d2(S(t)) < bd) bd = d2(S(t)), best = i, drag_tip = true;
            }
            if (best < 0) {
                bd = 18.0f * 18.0f;
                for (int i = 0; i < (int)cur.l.size(); i++)
                    if (d2(S(anchor(cur.l[i]))) < bd) bd = d2(S(anchor(cur.l[i]))), best = i;
            }
            if (best >= 0) sel = best;
        }
        if (active && arm == ARM_NONE && sel >= 0 && sel < (int)cur.l.size() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
            PLight& l = cur.l[sel];
            V3 t;
            if (drag_tip && l.kind == PL_SPOT) {
                l.a = W(io.MousePos, top ? l.a.y : dot(l.a, f));
            } else if (drag_tip && tip_of(l, t)) {
                // the tip stays on a sphere around the window's center: the view
                // gives two of its axes, the hidden one keeps its side
                const V3 c = win_center(l), hid = top ? up : f;
                const V3 m = W(io.MousePos, dot(c, hid)) - c;   // in the view's plane through c
                const float ml = std::sqrt(dot(m, m));
                const float most = k_tip * 0.996f;   // a few degrees off flat, at least
                const V3 v = ml > most ? m * (most / ml) : m;
                const float th = dot(t - c, hid);
                const float side = th > 0.0f ? 1.0f : th < 0.0f || top ? -1.0f : 1.0f;
                V3 d = norm(v + hid * (side * std::sqrt(std::max(k_tip * k_tip - dot(v, v), 0.0f))));
                const WinFrame fr = win_frame(l, cam_p);   // never from the room's side
                const float k = dot(d, fr.n);
                if (k < 0.08f) d = norm(d + fr.n * (0.08f - k));
                l.dir = d;
            } else {
                const V3 a = anchor(l);
                shift(l, W(io.MousePos, top ? a.y : dot(a, f)) - a);
            }
            changed();
        }
        dl->AddText(ImVec2(p0.x + 8, p0.y + 6), IM_COL32(255, 255, 255, 170),
                    top ? "De cima (para cima = para onde a câmera olha)" : "De lado (altura)");
        dl->AddText(ImVec2(p0.x + 8, p1.y - 6 - ImGui::GetTextLineHeight()), IM_COL32(255, 255, 255, 110),
                    "Arraste a luz, ou a ponta da seta (direção)");
        if (mesh.empty())
            dl->AddText(ImVec2(p0.x + 8, p0.y + 6 + ImGui::GetTextLineHeightWithSpacing()), IM_COL32(255, 220, 120, 200),
                        "(sem a malha da sala: Atualizar vista)");
        dl->PopClipRect();
    }

    // a window: its corners again, panes, the light's way, sun / moon, dust
    bool window_editor(PLight& l) {
        bool ch = false;
        ImGui::BeginDisabled(arm != ARM_NONE);
        if (ImGui::SmallButton("Refazer os cantos (2 cliques)")) {
            win_redo = true;
            arm_click(ARM_WIN1);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        bool cm = arm == ARM_CORNERS;
        ImGui::BeginDisabled(arm != ARM_NONE && !cm);
        if (ImGui::Checkbox("Arrastar os cantos no jogo", &cm)) corners_mode(cm);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ligado: segure o botão num canto da janela, no jogo, e arraste.\n"
                              "O canto mais perto do clique segue o mouse. O jogo não recebe esses cliques.");
        int cr[2] = {l.cols, l.rows};
        ImGui::SetNextItemWidth(160);
        if (ImGui::SliderInt2("Vidros (colunas, linhas)", cr, 1, 8)) l.cols = cr[0], l.rows = cr[1], ch = true;
        const WinFrame f = win_frame(l, cam_p);
        if (!l.has_dir) l.dir = win_dir(f, 0.0f, 35.0f), l.has_dir = true, ch = true;
        ImGui::TextDisabled("Direção: arraste a ponta da seta nas vistas.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reto para dentro")) l.dir = win_dir(f, 0.0f, 35.0f), ch = true;
        int src = l.src;
        ImGui::TextUnformatted("Luz:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Sol (de noite, luar)", src == 0)) l.src = 0, ch = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("Só luar", src == 1)) l.src = 1, ch = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("Fixa", src == 2)) l.src = 2, ch = true;
        ImGui::SetNextItemWidth(160);
        ch |= ImGui::SliderFloat("Poeira", &l.dust, 0.0f, 1.0f, "%.2f");
        return ch;
    }
    // ---- the panel ----
    void color_presets(PLight& l) {
        struct P {
            const char* name;
            float c[3];
            float flick;
        };
        static const P k[] = {{"Vela", {1.0f, 0.62f, 0.30f}, 0.3f}, {"Lampião", {1.0f, 0.72f, 0.42f}, 0.15f},
                              {"Lareira", {1.0f, 0.50f, 0.22f}, 0.35f}, {"Dia", {1.0f, 0.90f, 0.75f}, 0.0f},
                              {"Luar", {0.62f, 0.74f, 1.0f}, 0.0f}};
        for (int i = 0; i < 5; i++) {
            if (i) ImGui::SameLine();
            if (ImGui::SmallButton(k[i].name)) {
                memcpy(l.col, k[i].c, sizeof l.col);
                l.flicker = k[i].flick;
                changed();
            }
        }
    }
    void panel(bool connected, float night) {
        if (!connected) {
            ImGui::TextDisabled("Sem o jogo: as luzes aparecem quando ele conectar.");
            return;
        }
        if (!area) {
            ImGui::TextDisabled("Área: nenhuma ainda (entre num lugar com chão 3D)");
            return;
        }
        ImGui::Text("Área: 0x%08x", area);
        if (!running)
            ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "A luz do jogo não está rodando: ligue Config > Lights > Placed (ou Fire).");
        ImGui::Separator();
        // the ambient
        bool amb = cur.has_amb;
        if (ImGui::Checkbox("Escurecer onde não há luz (ambiente do interior)", &amb)) cur.has_amb = amb, changed();
        ImGui::BeginDisabled(!cur.has_amb);
        const ImGuiColorEditFlags cf = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs;
        if (ImGui::ColorEdit3("De dia", cur.amb, cf)) changed();
        ImGui::SameLine();
        if (ImGui::ColorEdit3("De noite", cur.amb_night, cf)) changed();
        ImGui::SameLine();
        ImGui::TextDisabled(night > 0.5f ? "(agora: noite)" : "(agora: dia)");
        float k = (cur.amb[0] + cur.amb[1] + cur.amb[2]) / 3.0f;
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Escuro de dia  ").x);
        if (ImGui::SliderFloat("Escuro de dia", &k, 0.2f, 1.0f, "%.2f")) {
            const float m = (cur.amb[0] + cur.amb[1] + cur.amb[2]) / 3.0f;
            for (float& v : cur.amb) v = m > 1e-3f ? v * k / m : k;
            changed();
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        // the list
        ImGui::BeginDisabled(arm != ARM_NONE);
        if (ImGui::Button("+ Luz (clique no jogo)")) arm_click(ARM_NEW_POINT);
        ImGui::SameLine();
        if (ImGui::Button("+ Foco")) arm_click(ARM_NEW_SPOT);
        ImGui::SameLine();
        if (ImGui::Button("+ Janela")) {
            win_redo = false;
            arm_click(ARM_WIN1);
            msg = "Clique num canto da janela pintada (depois no canto oposto).";
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A luz do sol (ou da lua) entra por ela: feixe no ar e mancha no chão.");
        ImGui::SameLine();
        ImGui::BeginDisabled(sel < 0);
        if (ImGui::Button("Duplicar")) {
            PLight l = cur.l[sel];
            l.p = l.p + flat(cam_r) * 60.0f;
            cur.l.push_back(l);
            sel = (int)cur.l.size() - 1;
            changed();
        }
        ImGui::SameLine();
        if (ImGui::Button("Apagar")) {
            cur.l.erase(cur.l.begin() + sel);
            sel = std::min(sel, (int)cur.l.size() - 1);
            changed();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (arm == ARM_CORNERS) {
            ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "Arrastando os cantos no jogo...");
            ImGui::SameLine();
            if (ImGui::SmallButton("Terminar") || ImGui::IsKeyPressed(ImGuiKey_Escape)) corners_mode(false);
        } else if (arm != ARM_NONE) {
            ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "Esperando o clique no jogo...");
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancelar") || ImGui::IsKeyPressed(ImGuiKey_Escape)) arm = ARM_NONE, click_arm(false), msg.clear();
        }
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("Afastar da superfície", &lift, 0.0f, 150.0f, "%.0f");
        if (ImGui::BeginListBox("##lights", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 5.5f))) {
            for (int i = 0; i < (int)cur.l.size(); i++) {
                const PLight& l = cur.l[i];
                char t[160];
                snprintf(t, sizeof t, "%d. %s%s  %s", i + 1, l.kind == PL_SPOT ? "Foco" : l.kind == PL_WINDOW ? "Janela" : "Luz",
                         l.shadow ? " (sombra)" : "", l.note.c_str());
                if (ImGui::Selectable(t, i == sel)) sel = i;
            }
            ImGui::EndListBox();
        }
        int shadows = 0;
        for (const auto& l : cur.l) shadows += l.shadow && l.kind != PL_WINDOW;
        if (shadows > 2) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Só as 2 primeiras com sombra fazem sombra.");
        if (sel >= 0 && sel < (int)cur.l.size()) light_editor(cur.l[sel]);
        ImGui::Separator();
        if (ImGui::Button("Aplicar (gravar)")) apply();
        ImGui::SameLine();
        ImGui::BeginDisabled(!dirty());
        if (ImGui::Button("Descartar")) revert();
        ImGui::EndDisabled();
        if (dirty()) ImGui::SameLine(), ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "(não aplicado)");
        ImGui::SameLine();
        if (ImGui::Button("Atualizar vista")) request_mesh();
        // copy another area's lights here (houses of a town share their furniture)
        std::vector<std::string> others;
        for (const auto& x : file.a)
            if (x.area != area) {
                char b[96];
                snprintf(b, sizeof b, "0x%08x (%d luzes)", x.area, (int)x.l.size());
                others.push_back(b);
            }
        if (!others.empty()) {
            ImGui::SetNextItemWidth(220);
            if (ImGui::BeginCombo("##from", "Copiar de outra área...")) {
                for (const auto& o : others)
                    if (ImGui::Selectable(o.c_str())) {
                        const unsigned a = (unsigned)strtoul(o.c_str(), nullptr, 16);
                        cur = saved(a);
                        cur.area = area;
                        sel = cur.l.empty() ? -1 : 0;
                        changed();
                        msg = "Copiado de " + o.substr(0, 10) + " (aplique para gravar).";
                    }
                ImGui::EndCombo();
            }
        }
        if (!msg.empty()) ImGui::TextWrapped("%s", msg.c_str());
        ImGui::TextDisabled("Setas: move no chão (Shift = mais). PgUp/PgDn: altura. Arraste nas vistas.");
        ImGui::TextDisabled("Seta: a direção da luz (janela) ou para onde o foco aponta: arraste a ponta.");
    }
    void light_editor(PLight& l) {
        ImGui::PushID(&l);
        bool ch = false;
        const float lw = ImGui::CalcTextSize("Alcance (unid.)  ").x;
        char nb[96];
        snprintf(nb, sizeof nb, "%s", l.note.c_str());
        ImGui::SetNextItemWidth(-lw);
        if (ImGui::InputText("Nota", nb, sizeof nb)) l.note = nb, ch = true;
        int kind = l.kind == PL_SPOT;
        if (l.kind == PL_WINDOW) ch |= window_editor(l);
        if (l.kind != PL_WINDOW) {
            if (ImGui::RadioButton("Luz", kind == 0)) {
                l.kind = PL_POINT;
                ch = true;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Foco", kind == 1)) {
                l.kind = PL_SPOT;
                if (dot(l.a - l.p, l.a - l.p) < 1.0f) l.a = l.p - V3{0, 200, 0};
                ch = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(arm != ARM_NONE);
            if (ImGui::SmallButton("Mover com clique")) arm_click(ARM_MOVE);
            if (l.kind == PL_SPOT) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Mirar com clique")) arm_click(ARM_AIM);
            }
            ImGui::EndDisabled();
        }
        if (l.kind != PL_WINDOW) {
            ImGui::SetNextItemWidth(-lw);
            ch |= ImGui::DragFloat3("Posição", &l.p.x, 1.0f, -100000.0f, 100000.0f, "%.0f");
        }
        if (l.kind == PL_SPOT) {
            ImGui::SetNextItemWidth(-lw);
            ch |= ImGui::DragFloat3("Aponta para", &l.a.x, 1.0f, -100000.0f, 100000.0f, "%.0f");
            ImGui::SetNextItemWidth(-lw);
            ch |= ImGui::SliderFloat("Abertura", &l.cone, 5.0f, 170.0f, "%.0f°");
        }
        ImGui::SetNextItemWidth(-lw);
        ch |= ImGui::ColorEdit3("Cor", l.col, ImGuiColorEditFlags_Float);
        color_presets(l);
        ImGui::SetNextItemWidth(-lw);
        ch |= ImGui::SliderFloat("Força", &l.power, 0.0f, 5.0f, "%.2f");
        if (l.kind != PL_WINDOW) {
            ImGui::SetNextItemWidth(-lw);
            ch |= ImGui::SliderFloat("Alcance (unid.)", &l.reach, 50.0f, 2500.0f, "%.0f");
            ch |= ImGui::Checkbox("Sombra", &l.shadow);
            ImGui::SameLine();
        }
        if (l.kind != PL_WINDOW) {
            ImGui::SetNextItemWidth(120);
            ch |= ImGui::SliderFloat("Chama", &l.flicker, 0.0f, 1.0f, "%.2f");
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(110);
        const char* wn[] = {"sempre", "só de dia", "só de noite"};
        ch |= ImGui::Combo("Acende", &l.when, wn, 3);
        ImGui::PopID();
        if (ch) changed();
    }
};
