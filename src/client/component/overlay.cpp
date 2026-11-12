/* r1-mod overlay: in-game console and server browser (Dear ImGui over the game's Direct3D 9 device).
 *
 * The QA build compiled IW4's console out (the console material and font are still loaded by
 * CL_InitRenderer 0x467E20, but nothing draws them), so this is r1-mod's own:
 *
 *  - Drawing: on the render thread, inside the frame's last scene: render.c hooks the call to
 *    0xAD3DA0 right before the EndScene of 0xA986B0 and calls overlay_render_scene, which draws on
 *    the back buffer of dx.device (0x103208C). The device itself is not modified.
 *  - Input: the game window (0x3749AAC) is subclassed. While a panel is open, keyboard and mouse
 *    messages go to ImGui instead of the game (key and button releases still reach the game, so
 *    nothing stays held), and keyCatchers (0x1A2E7E4) bit 0, IW4's KEYCATCH_CONSOLE, is set: the
 *    game's IN_MouseMove (0x45EE80) then frees the cursor instead of re-centring it.
 *  - Console (` or ~, Shift+` for a taller one): r1-mod output and the game message windows
 *    (chat, obituaries, server prints); commands go through the r1-mod line runner. Tab completes
 *    engine commands and dvars, Up/Down walk the history.
 *  - Server browser (F6, or cx_browser): asks the master in r1-mod.ini ([r1-mod] master=, default
 *    the backend's 127.0.0.1:20810) for the server list, asks each server for getinfo, and joins
 *    through cx_match (the lobby's StartGame path).
 *
 * ImGui runs on the render thread; the window procedure (main thread) feeds it under g_imgui.
 * Console lines arrive from any thread under g_lines, which is never held while taking g_imgui.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <initguid.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <cmath>

#include "component/overlay.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

#define VA_DEVICE      0x103208Cu /* IDirect3DDevice9 * (dx.device), created by the renderer init 0xA9E4B0 */
#define VA_HWND        0x3749AACu /* main game window (hWnd of IN_MouseMove 0x63DE70); 0x3749478 is a startup window, null in game */
#define VA_KEYCATCHERS 0x1A2E7E4u /* bit 0: KEYCATCH_CONSOLE, see IN_MouseMove 0x45EE80 */
#define VA_CONNSTATE   0x1A2E7ECu /* g_connState: 0 lobby, 1..8 connecting and loading, 9 in a match (0x4FA590) */

/* IDirect3DDevice9(Ex) vtable slots */
enum { VT_RESET = 16, VT_PRESENT = 17, VT_ENDSCENE = 42, VT_PRESENTEX = 121, VT_RESETEX = 132,
       VT_COUNT9 = 119, VT_COUNT9EX = 134 };

static const char OOB[] = "\xff\xff\xff\xff";

/* ---- console lines ---- */

/* initialised by the CRT before DllMain, so prints and window messages that come before
 * overlay_start are safe */
struct Lock {
    CRITICAL_SECTION cs;
    Lock() { InitializeCriticalSection(&cs); }
};
static Lock g_lines, g_imgui, g_browser;
static std::deque<std::string> g_line_list;  /* UTF-8, colour codes (^0..^9) kept */
static std::string g_line_partial;
static volatile LONG g_lines_added;
static const size_t MAX_LINES = 4000;

static bool valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0)
            return false;
        for (++p; n > 0; --n, ++p)
            if ((*p & 0xC0) != 0x80)
                return false;
    }
    return true;
}

/* game and server text is UTF-8 or GBK (code page 936); ImGui wants UTF-8 */
static std::string to_utf8(const char *s)
{
    if (valid_utf8(s))
        return s;
    int wn = MultiByteToWideChar(936, 0, s, -1, NULL, 0);
    if (wn <= 0)
        return s;
    std::vector<wchar_t> w(wn);
    MultiByteToWideChar(936, 0, s, -1, w.data(), wn);
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), -1, NULL, 0, NULL, NULL);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w.data(), -1, &out[0], n, NULL, NULL);
    return out;
}

extern "C" void overlay_print(const char *text)
{
    std::string t = to_utf8(text);
    EnterCriticalSection(&g_lines.cs);
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (c == '\n') {
            g_line_list.push_back(g_line_partial);
            g_line_partial.clear();
        } else if (c != '\r') {
            g_line_partial += c;
        }
    }
    while (g_line_list.size() > MAX_LINES)
        g_line_list.pop_front();
    LeaveCriticalSection(&g_lines.cs);
    InterlockedIncrement(&g_lines_added);
}

static std::string strip_colors(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '^' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9')
            ++i;
        else
            o += s[i];
    }
    return o;
}

static ImVec4 code_color(char c)
{
    switch (c) {
    case '0': return ImVec4(0.25f, 0.25f, 0.25f, 1);
    case '1': return ImVec4(1.00f, 0.36f, 0.36f, 1);
    case '2': return ImVec4(0.45f, 0.95f, 0.45f, 1);
    case '3': return ImVec4(1.00f, 0.92f, 0.40f, 1);
    case '4': return ImVec4(0.45f, 0.60f, 1.00f, 1);
    case '5': return ImVec4(0.40f, 0.90f, 1.00f, 1);
    case '6': return ImVec4(1.00f, 0.50f, 0.90f, 1);
    case '8': return ImVec4(1.00f, 0.65f, 0.25f, 1);
    case '9': return ImVec4(0.60f, 0.60f, 0.60f, 1);
    default:  return ImVec4(0.92f, 0.92f, 0.92f, 1);
    }
}

static void draw_colored(const std::string &s)
{
    ImVec4 col = code_color('7');
    size_t start = 0;
    bool first = true;
    for (size_t i = 0; i <= s.size(); ++i) {
        bool code = i + 1 < s.size() && s[i] == '^' && s[i + 1] >= '0' && s[i + 1] <= '9';
        if (i == s.size() || code) {
            if (i > start || (first && i == s.size())) {
                if (!first)
                    ImGui::SameLine(0, 0);
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(s.c_str() + start, s.c_str() + i);
                ImGui::PopStyleColor();
                first = false;
            }
            if (code) {
                col = code_color(s[i + 1]);
                start = i + 2;
                ++i;
            }
        }
    }
}

/* ---- state shared between the window procedure, the game frame and the render thread ---- */

static bool g_imgui_ready;
static IDirect3DDevice9 *g_imgui_dev;
static HWND g_imgui_hwnd;
static volatile LONG g_console_open, g_console_tall, g_browser_open, g_focus_console;
static volatile LONG g_stats_open;
static volatile LONG g_game_frames;
static HWND g_hwnd;
static WNDPROC g_orig_wndproc;
static bool g_eat_char, g_catcher_set, g_unicode;
static UINT g_console_key = VK_OEM_3, g_browser_key = VK_F6, g_stats_key = VK_F7;
static char g_master[128] = "127.0.0.1:20810";

/* Panels stay hidden (input goes to the game, no console key catcher) while a match connects and
 * loads: a load with the console open crashed in the class item lookup (0x709603, stale item-table
 * name). Not proven to be the overlay's doing, but the console catcher changes UI paths mid-load. */
static bool loading(void)
{
    unsigned state = *(volatile unsigned *)r1_addr(VA_CONNSTATE);
    return state >= 1 && state <= 8;
}

static bool any_open(void)
{
    return (g_console_open || g_browser_open || g_stats_open) && !loading();
}

/* ---- console panel (render thread, under g_imgui) ---- */

struct Match {
    std::string name, value;
};
static char g_input[1024];
static std::vector<std::string> g_history;
static int g_history_pos = -1;
static std::vector<Match> g_matches;
static std::string g_matches_for = "\x01";
static std::vector<std::string> g_pending_exec;  /* run after g_imgui is released */
static LONG g_lines_seen;
static float g_console_slide;                     /* 0..1 while the console drops down */
static const size_t MAX_HISTORY = 200;

/* command history survives restarts in r1-mod_history.txt next to the DLL */
static std::string history_path(void)
{
    return std::string(r1_dir()) + "r1-mod_history.txt";
}

static void load_history(void)
{
    FILE *f = fopen(history_path().c_str(), "rb");
    char line[1024];
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0])
            g_history.push_back(line);
    }
    fclose(f);
    if (g_history.size() > MAX_HISTORY)
        g_history.erase(g_history.begin(), g_history.end() - MAX_HISTORY);
}

static void add_history(const std::string &line)
{
    if (!g_history.empty() && g_history.back() == line)
        return;
    g_history.push_back(line);
    if (g_history.size() > MAX_HISTORY * 2) {
        /* rewrite trimmed now and then; otherwise just append */
        g_history.erase(g_history.begin(), g_history.end() - MAX_HISTORY);
        FILE *f = fopen(history_path().c_str(), "wb");
        if (f) {
            for (size_t i = 0; i < g_history.size(); ++i)
                fprintf(f, "%s\n", g_history[i].c_str());
            fclose(f);
        }
        return;
    }
    FILE *f = fopen(history_path().c_str(), "ab");
    if (f) {
        fprintf(f, "%s\n", line.c_str());
        fclose(f);
    }
}

static void clear_console(void)
{
    EnterCriticalSection(&g_lines.cs);
    g_line_list.clear();
    LeaveCriticalSection(&g_lines.cs);
}

static void collect_match(const char *name, const char *value, void *ctx)
{
    Match m;
    m.name = name;
    m.value = value ? to_utf8(value) : "";
    ((std::vector<Match> *)ctx)->push_back(m);
}

static std::string first_token(const char *s)
{
    while (*s == ' ' || *s == '/' || *s == '\\')
        ++s;
    const char *e = s;
    while (*e && *e != ' ')
        ++e;
    return std::string(s, e);
}

static void refresh_matches(void)
{
    std::string tok = first_token(g_input);
    const char *at = tok.empty() ? NULL : strstr(g_input, tok.c_str());
    bool has_args = at && at[tok.size()] == ' ';
    std::string key = tok + (has_args ? " " : "");
    if (key == g_matches_for)
        return;
    g_matches_for = key;
    g_matches.clear();
    if (tok.empty())
        return;
    r1_complete(tok.c_str(), 64, collect_match, &g_matches);
    if (has_args) {
        /* once arguments follow, keep only the exact command or dvar (it shows its value) */
        std::vector<Match> exact;
        for (size_t i = 0; i < g_matches.size(); ++i)
            if (!_stricmp(g_matches[i].name.c_str(), tok.c_str()))
                exact.push_back(g_matches[i]);
        g_matches.swap(exact);
    }
    std::sort(g_matches.begin(), g_matches.end(),
              [](const Match &a, const Match &b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
}

static int input_callback(ImGuiInputTextCallbackData *data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory && !g_history.empty()) {
        if (data->EventKey == ImGuiKey_UpArrow)
            g_history_pos = g_history_pos < 0 ? (int)g_history.size() - 1 : std::max(0, g_history_pos - 1);
        else if (g_history_pos >= 0 && ++g_history_pos >= (int)g_history.size())
            g_history_pos = -1;
        data->DeleteChars(0, data->BufTextLen);
        if (g_history_pos >= 0)
            data->InsertChars(0, g_history[g_history_pos].c_str());
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        std::string tok = first_token(data->Buf);
        std::vector<Match> m;
        if (tok.empty())
            return 0;
        r1_complete(tok.c_str(), 256, collect_match, &m);
        if (m.empty())
            return 0;
        /* complete to the longest prefix all matches share; a single match also gets a space */
        std::string common = m[0].name;
        for (size_t i = 1; i < m.size(); ++i) {
            size_t n = 0;
            while (n < common.size() && n < m[i].name.size() &&
                   tolower((unsigned char)common[n]) == tolower((unsigned char)m[i].name[n]))
                ++n;
            common.resize(n);
        }
        if (common.size() < tok.size())
            return 0;
        const char *rest = strstr(data->Buf, tok.c_str());
        std::string tail = rest ? std::string(rest + tok.size()) : std::string();
        std::string line = common + (m.size() == 1 && tail.empty() ? " " : "") + tail;
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, line.c_str());
    }
    return 0;
}

/* ---- stats panel (F7 / cx_statspanel) ----
 * The stats overlay's settings and per-flag overrides (stats.c). Every change is a cx_stats line run on the game
 * thread after the frame, the same command a player can type, so the panel holds no state of its own beyond the
 * edit fields. Values come from stats_get_view / stats_find_flags, which read only memory stats.c owns and the
 * secure buffer (no asset lookups on the render thread). */
static void stats_exec(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    g_pending_exec.push_back(line);
}

static void draw_stats(void)
{
    static stats_view edit;
    static bool edit_loaded;
    static char search[64] = "_rank";
    static stats_flag_row rows[100];
    static int flag_edit[2048];  /* by flag index */
    static bool flag_edited[2048];
    stats_view v;
    bool open = true;
    int n, i;
    stats_get_view(&v);
    ImGui::SetNextWindowSize(ImVec2(620 * (ImGui::GetFontSize() / 13.0f), 560 * (ImGui::GetFontSize() / 13.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("r1-mod stats (F7)", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        if (!open)
            g_stats_open = 0;
        return;
    }
    if (!open)
        g_stats_open = 0;
    if (!edit_loaded || ImGui::IsWindowAppearing()) {
        stats_flag_row probe[1];
        edit = v;
        edit_loaded = true;
        r1_log("overlay: stats panel open (overlay %s, level %d, %d replaced values, tables %s, flag search %s)\n",
               v.on ? "on" : "off", v.level, v.replaced, v.tables ? "loaded" : "not loaded",
               stats_find_flags("", probe, 1) ? "ready" : "empty");
    }
    if (!v.tables) {
        ImGui::TextDisabled("Waiting for the rank and unlock tables (they load once Demonware is up).");
        ImGui::End();
        return;
    }
    bool on = v.on != 0;
    if (ImGui::Checkbox("Overlay on", &on))
        stats_exec("cx_stats %d", on ? 1 : 0);
    ImGui::SameLine();
    ImGui::TextDisabled("%d values replaced, %d overrides; off puts the profile's own values back", v.replaced,
                        v.overrides);
    ImGui::Separator();
    ImGui::PushItemWidth(140 * (ImGui::GetFontSize() / 13.0f));
    ImGui::SliderInt("Level", &edit.level, 1, v.max_level);
    ImGui::SameLine();
    ImGui::SliderInt("Prestige", &edit.prestige, 0, v.max_prestige);
    ImGui::InputInt("XP in rank (-1 = keep)", &edit.xp);
    ImGui::InputInt("Gate flag level", &edit.flags);
    ImGui::SameLine();
    ImGui::InputInt("Weapon XP", &edit.weapon_xp);
    ImGui::PopItemWidth();
    if (ImGui::Button("Apply"))
        stats_exec("cx_stats level %d prestige %d xp %d flags %d weaponxp %d", edit.level, edit.prestige, edit.xp,
                   edit.flags, edit.weapon_xp);
    ImGui::SameLine();
    if (ImGui::Button("Revert fields"))
        edit = v;
    ImGui::SameLine();
    if (ImGui::Button("Clear all overrides"))
        stats_exec("cx_stats clearall");
    ImGui::Separator();
    ImGui::SetNextItemWidth(260 * (ImGui::GetFontSize() / 13.0f));
    ImGui::InputText("Find unlock flags", search, sizeof search);
    n = stats_find_flags(search, rows, 100);
    ImGui::SameLine();
    ImGui::TextDisabled("%d%s shown", n, n == 100 ? "+" : "");
    if (ImGui::BeginTable("##flags", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Flag", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Live", ImGuiTableColumnFlags_WidthFixed, 50 * (ImGui::GetFontSize() / 13.0f));
        ImGui::TableSetupColumn("Profile", ImGuiTableColumnFlags_WidthFixed, 55 * (ImGui::GetFontSize() / 13.0f));
        ImGui::TableSetupColumn("Override", ImGuiTableColumnFlags_WidthFixed, 110 * (ImGui::GetFontSize() / 13.0f));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 90 * (ImGui::GetFontSize() / 13.0f));
        ImGui::TableHeadersRow();
        for (i = 0; i < n; i++) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(rows[i].name);
            ImGui::TableNextColumn();
            ImGui::Text("%d", rows[i].live);
            ImGui::TableNextColumn();
            if (rows[i].profile >= 0)
                ImGui::Text("%d", rows[i].profile);
            else
                ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            int fi = rows[i].index & 2047;
            if (!flag_edited[fi])
                flag_edit[fi] = rows[i].override >= 0 ? rows[i].override : rows[i].live;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##v", &flag_edit[fi], 1, 10))
                flag_edited[fi] = true;
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Set")) {
                stats_exec("cx_stats flag %s %d", rows[i].name, flag_edit[fi]);
                flag_edited[fi] = false;
            }
            if (rows[i].override >= 0) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear")) {
                    stats_exec("cx_stats flag %s clear", rows[i].name);
                    flag_edited[fi] = false;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

static void draw_console(void)
{
    ImGuiViewport *vp = ImGui::GetMainViewport();
    /* drops down like IW4's console: eased over ~0.15 s */
    g_console_slide = std::min(1.0f, g_console_slide + ImGui::GetIO().DeltaTime * 7.0f);
    float ease = 1.0f - (1.0f - g_console_slide) * (1.0f - g_console_slide);
    float height = std::floor(vp->Size.y * (g_console_tall ? 0.85f : 0.42f) * ease);
    if (height < ImGui::GetFrameHeightWithSpacing() * 3)
        height = ImGui::GetFrameHeightWithSpacing() * 3;
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, height));
    ImGui::SetNextWindowBgAlpha(0.90f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##r1console", NULL,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();

    float footer = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##output", ImVec2(0, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    {
        bool stick = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;
        LONG added = g_lines_added;
        EnterCriticalSection(&g_lines.cs);
        ImGuiListClipper clipper;
        clipper.Begin((int)g_line_list.size());
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                draw_colored(g_line_list[i]);
        LeaveCriticalSection(&g_lines.cs);
        if (added != g_lines_seen && stick)
            ImGui::SetScrollHereY(1.0f);
        g_lines_seen = added;
        /* the input keeps focus, so page keys scroll the output from here */
        float page = ImGui::GetWindowHeight() * 0.8f;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
            ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - page));
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
            ImGui::SetScrollY(std::min(ImGui::GetScrollMaxY(), ImGui::GetScrollY() + page));
    }
    ImGui::EndChild();
    {
        /* separator with the version on the right, where IW4 printed its build */
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        char tag[80];
        _snprintf(tag, sizeof tag, "%.0f fps | r1-mod " R1_VERSION " | QA 3.3.3.3", ImGui::GetIO().Framerate);
        tag[sizeof tag - 1] = 0;
        ImVec2 ts = ImGui::CalcTextSize(tag);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float y = p.y + ImGui::GetStyle().ItemSpacing.y * 0.5f;
        dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + w - ts.x - 12.0f, y), ImGui::GetColorU32(ImVec4(0.85f, 0.55f, 0.15f, 0.8f)));
        dl->AddText(ImVec2(p.x + w - ts.x, y - ts.y * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), tag);
        ImGui::Dummy(ImVec2(w, ImGui::GetStyle().ItemSpacing.y));
    }
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L))
        clear_console();

    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "]");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (g_focus_console) {
        ImGui::SetKeyboardFocusHere();
        g_focus_console = 0;
    }
    if (ImGui::InputText("##input", g_input, sizeof g_input,
                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
                         ImGuiInputTextFlags_CallbackCompletion, input_callback)) {
        std::string line = g_input;
        size_t a = line.find_first_not_of(" /\\"), b = line.find_last_not_of(' ');
        line = a == std::string::npos ? "" : line.substr(a, b - a + 1);
        if (!line.empty()) {
            add_history(line);
            if (!_stricmp(line.c_str(), "clear") || !_stricmp(line.c_str(), "cls")) {
                clear_console();
            } else {
                overlay_print(("]" + line + "\n").c_str());
                g_pending_exec.push_back(line);
            }
        }
        g_history_pos = -1;
        g_input[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImVec2 below(vp->Pos.x + 24.0f, vp->Pos.y + height + 2.0f);
    ImGui::End();

    refresh_matches();
    if (!g_matches.empty()) {
        ImGui::SetNextWindowPos(below);
        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::Begin("##r1complete", NULL,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::BeginTable("##m", 2, ImGuiTableFlags_SizingFixedFit)) {
            for (size_t i = 0; i < g_matches.size(); ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(g_matches[i].name.c_str());
                ImGui::TableNextColumn();
                if (g_matches[i].value.empty())
                    ImGui::TextDisabled("command");
                else
                    ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", g_matches[i].value.c_str());
            }
            ImGui::EndTable();
        }
        if (g_matches.size() >= 64)
            ImGui::TextDisabled("more...");
        ImGui::End();
    }
}

/* ---- server browser ---- */

struct Server {
    unsigned ip;               /* network order */
    unsigned short port;       /* host order */
    bool answered;
    bool password;
    int ping, clients, humans, maxclients;
    std::string address, host, map, gametype, compat; /* compat: its r1_compat, "" when it declares none */
    ULONGLONG sent;
};

static std::vector<Server> g_servers;      /* under g_browser */
static std::string g_browser_status = "Not refreshed yet";
static volatile LONG g_refreshing;
static ULONGLONG g_last_refresh;
static int g_selected = -1;
static bool g_hide_empty, g_hide_full;
static char g_filter[64];

static bool parse_hostport(const char *s, sockaddr_in *out, unsigned short defport)
{
    char host[128];
    const char *colon = strrchr(s, ':');
    size_t n = colon ? (size_t)(colon - s) : strlen(s);
    addrinfo hints = {0}, *res = NULL;
    if (n >= sizeof host)
        return false;
    memcpy(host, s, n);
    host[n] = 0;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return false;
    *out = *(sockaddr_in *)res->ai_addr;
    out->sin_port = htons(colon ? (unsigned short)atoi(colon + 1) : defport);
    freeaddrinfo(res);
    return true;
}

static std::string info_value(const std::string &info, const char *key)
{
    /* \key\value\key\value... */
    size_t i = 0;
    while (i < info.size()) {
        if (info[i] == '\\')
            ++i;
        size_t k = info.find('\\', i);
        if (k == std::string::npos)
            break;
        size_t v = info.find('\\', k + 1);
        if (v == std::string::npos)
            v = info.size();
        if (!_stricmp(info.substr(i, k - i).c_str(), key))
            return info.substr(k + 1, v - k - 1);
        i = v;
    }
    return "";
}

static DWORD WINAPI refresh_thread(LPVOID unused)
{
    (void)unused;
    std::vector<Server> list;
    std::string status;
    sockaddr_in master;
    SOCKET s = INVALID_SOCKET;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    do {
        if (!parse_hostport(g_master, &master, 20810)) {
            status = std::string("Cannot resolve master ") + g_master;
            break;
        }
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) {
            status = "Cannot open a UDP socket";
            break;
        }
        DWORD timeout = 100;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
        std::string req = std::string(OOB) + "getservers 6 full empty";
        sendto(s, req.data(), (int)req.size(), 0, (sockaddr *)&master, sizeof master);

        bool eot = false, any = false;
        ULONGLONG until = GetTickCount64() + 1500;
        while (!eot && GetTickCount64() < until) {
            char buf[2048];
            sockaddr_in from;
            int fl = sizeof from;
            int n = recvfrom(s, buf, sizeof buf, 0, (sockaddr *)&from, &fl);
            static const char head[] = "\xff\xff\xff\xffgetserversResponse";
            if (n < (int)sizeof head - 1 || from.sin_addr.s_addr != master.sin_addr.s_addr ||
                memcmp(buf, head, sizeof head - 1))
                continue;
            any = true;
            for (int i = sizeof head - 1; i < n; ++i) {
                if (buf[i] != '\\')
                    continue;
                if (i + 4 <= n && !memcmp(buf + i + 1, "EOT", 3)) {
                    eot = true;
                    break;
                }
                if (i + 7 > n)
                    break;
                Server sv = Server();
                memcpy(&sv.ip, buf + i + 1, 4);
                sv.port = (unsigned short)(((unsigned char)buf[i + 5] << 8) | (unsigned char)buf[i + 6]);
                in_addr a;
                a.s_addr = sv.ip;
                char addr[32];
                _snprintf(addr, sizeof addr, "%s:%u", inet_ntoa(a), sv.port);
                sv.address = addr;
                sv.ping = -1;
                list.push_back(sv);
                i += 6;
            }
        }
        if (!any) {
            status = std::string("The master at ") + g_master + " did not answer (is the backend's master running?)";
            break;
        }

        for (size_t i = 0; i < list.size(); ++i) {
            sockaddr_in to = {0};
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = list[i].ip;
            to.sin_port = htons(list[i].port);
            std::string q = std::string(OOB) + "getinfo r1mod";
            list[i].sent = GetTickCount64();
            sendto(s, q.data(), (int)q.size(), 0, (sockaddr *)&to, sizeof to);
        }
        size_t answered = 0;
        until = GetTickCount64() + 2000;
        while (answered < list.size() && GetTickCount64() < until) {
            char buf[4096];
            sockaddr_in from;
            int fl = sizeof from;
            int n = recvfrom(s, buf, sizeof buf - 1, 0, (sockaddr *)&from, &fl);
            static const char head[] = "\xff\xff\xff\xffinfoResponse";
            if (n < (int)sizeof head - 1 || memcmp(buf, head, sizeof head - 1))
                continue;
            buf[n] = 0;
            ULONGLONG now = GetTickCount64();
            for (size_t i = 0; i < list.size(); ++i) {
                Server &sv = list[i];
                if (sv.answered || sv.ip != from.sin_addr.s_addr || sv.port != ntohs(from.sin_port))
                    continue;
                const char *nl = strchr(buf, '\n');
                std::string info = to_utf8(nl ? nl + 1 : "");
                sv.answered = true;
                sv.ping = (int)(now - sv.sent);
                sv.host = strip_colors(info_value(info, "hostname"));
                sv.map = info_value(info, "mapname");
                sv.gametype = info_value(info, "gametype");
                sv.clients = atoi(info_value(info, "clients").c_str());
                std::string humans = info_value(info, "g_humanplayers");
                sv.humans = humans.empty() ? sv.clients : atoi(humans.c_str());
                sv.maxclients = atoi(info_value(info, "sv_maxclients").c_str());
                sv.password = info_value(info, "pswrd") == "1";
                sv.compat = info_value(info, "r1_compat");
                ++answered;
                break;
            }
        }
        char line[128];
        _snprintf(line, sizeof line, "%u server%s listed, %u answered", (unsigned)list.size(),
                  list.size() == 1 ? "" : "s", (unsigned)answered);
        status = line;
    } while (0);
    if (s != INVALID_SOCKET)
        closesocket(s);

    EnterCriticalSection(&g_browser.cs);
    g_servers.swap(list);
    g_browser_status = status;
    g_selected = -1;
    LeaveCriticalSection(&g_browser.cs);
    r1_log("browser: %s\n", status.c_str());
    InterlockedExchange(&g_refreshing, 0);
    return 0;
}

static void start_refresh(void)
{
    if (InterlockedCompareExchange(&g_refreshing, 1, 0) != 0)
        return;
    g_last_refresh = GetTickCount64();
    EnterCriticalSection(&g_browser.cs);
    g_browser_status = std::string("Refreshing from ") + g_master + "...";
    LeaveCriticalSection(&g_browser.cs);
    HANDLE t = CreateThread(NULL, 0, refresh_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        InterlockedExchange(&g_refreshing, 0);
}

/* ---- message of the day (Plutonium / TeknoMW3 style) ----
 * r1-server reports sv_motd and its hostname in its getinfo reply. When a join starts (cx_match / cx_start, so the
 * browser too) the server is asked once; match.c puts the answer into the game's own loading-screen tip (the
 * setLoadingHint UI command), so nothing is drawn by r1-mod. The text is kept as the server sent it (the game's
 * character set, not UTF-8); match.c reduces it to plain text before the UI sees it. */
static std::string g_motd_host, g_motd_text;  /* under g_browser */
static volatile LONG g_motd_seq;

struct MotdQuery {
    sockaddr_in to;
    LONG seq;
};

static DWORD WINAPI motd_thread(LPVOID param)
{
    MotdQuery *q = (MotdQuery *)param;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        DWORD timeout = 250;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
        static const char head[] = "\xff\xff\xff\xffinfoResponse";
        for (int attempt = 0; attempt < 4; ++attempt) {
            std::string req = std::string(OOB) + "getinfo r1motd";
            sendto(s, req.data(), (int)req.size(), 0, (sockaddr *)&q->to, sizeof q->to);
            char buf[4096];
            sockaddr_in from;
            int fl = sizeof from;
            int n = recvfrom(s, buf, sizeof buf - 1, 0, (sockaddr *)&from, &fl);
            if (n < (int)sizeof head - 1 || memcmp(buf, head, sizeof head - 1) ||
                from.sin_addr.s_addr != q->to.sin_addr.s_addr)
                continue;
            buf[n] = 0;
            const char *nl = strchr(buf, '\n');
            std::string info = nl ? nl + 1 : "";
            std::string host = info_value(info, "hostname"), motd = info_value(info, "sv_motd");
            std::string color = info_value(info, "sv_customTextColor");
            if (q->seq == g_motd_seq) {
                textcolor_set_custom(color.c_str()); /* ^; is this server's colour */
                compat_server_answer(info_value(info, "r1_compat").c_str());
                EnterCriticalSection(&g_browser.cs);
                g_motd_host = host;
                g_motd_text = motd;
                LeaveCriticalSection(&g_browser.cs);
                if (!motd.empty())
                    r1_log("%s^7: %s\n", to_utf8(host.c_str()).c_str(), to_utf8(motd.c_str()).c_str());
            }
            break;
        }
        closesocket(s);
    }
    delete q;
    return 0;
}

extern "C" void overlay_motd_query(const char *ip, unsigned short port)
{
    MotdQuery *q = new MotdQuery();
    char addr[80];
    _snprintf(addr, sizeof addr - 1, "%s:%u", ip, port);
    addr[sizeof addr - 1] = 0;
    q->seq = InterlockedIncrement(&g_motd_seq);
    EnterCriticalSection(&g_browser.cs);
    g_motd_host.clear();
    g_motd_text.clear();
    LeaveCriticalSection(&g_browser.cs);
    if (!parse_hostport(addr, &q->to, port)) {
        delete q;
        return;
    }
    HANDLE t = CreateThread(NULL, 0, motd_thread, q, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        delete q;
}

extern "C" int overlay_motd_get(char *host, int host_size, char *motd, int motd_size)
{
    int have;
    EnterCriticalSection(&g_browser.cs);
    have = !g_motd_text.empty();
    _snprintf(host, host_size - 1, "%s", g_motd_host.c_str());
    host[host_size - 1] = 0;
    _snprintf(motd, motd_size - 1, "%s", g_motd_text.c_str());
    motd[motd_size - 1] = 0;
    LeaveCriticalSection(&g_browser.cs);
    return have;
}

static std::string pretty_map(const std::string &map)
{
    static const char *const names[][2] = {
        {"mp_terminal_cls", "Terminal"}, {"mp_seatown", "Seatown"}, {"mp_mideast", "Mideast"},
        {"mp_crash_tropical", "Crash Tropical"}, {"mp_killhouse_sh", "Killhouse"},
        {"mp_shipment_long", "Shipment Long"}, {"mp_rust_long", "Rust Long"}, {"mp_cargoship", "Cargoship"},
        {"mp_bridge", "Bridge"}, {"mp_boomtown", "Boomtown"}, {"mp_subbase_sh", "Sub Base"},
        {"mp_suburbs", "Suburbs"}, {"mp_mountain", "Mountain"}, {"mp_firingrange", "Firing Range"},
        {"mp_seatown", "Seatown"}, {"mp_dr_box", "Box"}, {"mp_dr_cistern", "Cistern"},
        {"mp_ambush_sh", "Ambush"}, {"mp_abandon_sh_mk", "Abandon"}, {"mp_cha_quad", "Quad"},
        {"mp_con_spring", "Construction"}, {"mp_hunted_spring", "Hunted"}, {"mp_nuked", "Nuketown"},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i)
        if (map == names[i][0])
            return names[i][1];
    return map;
}

static const char *pretty_gametype(const std::string &g)
{
    static const char *const names[][2] = {
        {"war", "Team Deathmatch"}, {"dm", "Free-for-all"}, {"dom", "Domination"}, {"sd", "Search and Destroy"},
        {"sab", "Sabotage"}, {"koth", "Headquarters"}, {"ctf", "Capture the Flag"}, {"conf", "Kill Confirmed"},
        {"dd", "Demolition"}, {"gun", "Gun Game"}, {"infect", "Infected"},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i)
        if (g == names[i][0])
            return names[i][1];
    return g.c_str();
}

static void join_server(const Server &sv)
{
    char line[96];
    in_addr a;
    a.s_addr = sv.ip;
    _snprintf(line, sizeof line, "cx_match %s %u", inet_ntoa(a), sv.port);
    overlay_print(("^3Joining " + (sv.host.empty() ? sv.address : sv.host) + " (" + sv.address + ")\n").c_str());
    g_pending_exec.push_back(line);
    g_browser_open = 0;
    g_console_open = 0;
}

static void draw_browser(void)
{
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(980.0f, vp->Size.x - 40), std::min(560.0f, vp->Size.y - 80)),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    EnterCriticalSection(&g_browser.cs);
    std::vector<Server> list = g_servers;
    std::string status = g_browser_status;
    LeaveCriticalSection(&g_browser.cs);

    char title[64];
    _snprintf(title, sizeof title, "Server Browser (%u)###r1browser", (unsigned)list.size());
    bool open = true;
    if (!ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    if (!open)
        g_browser_open = 0;
    bool typing = ImGui::IsAnyItemActive();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !typing && ImGui::IsKeyPressed(ImGuiKey_F5))
        start_refresh();

    ImGui::BeginDisabled(g_refreshing != 0);
    if (ImGui::Button(g_refreshing ? "Refreshing..." : "Refresh"))
        start_refresh();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Hide empty", &g_hide_empty);
    ImGui::SameLine();
    ImGui::Checkbox("Hide full", &g_hide_full);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##filter", "Search name or map", g_filter, sizeof g_filter);

    std::vector<int> rows;
    std::string needle = g_filter;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
    for (int i = 0; i < (int)list.size(); ++i) {
        const Server &sv = list[i];
        if (g_hide_empty && sv.answered && sv.humans == 0)
            continue;
        if (g_hide_full && sv.answered && sv.maxclients > 0 && sv.clients >= sv.maxclients)
            continue;
        if (!needle.empty()) {
            std::string hay = sv.host + " " + sv.map + " " + pretty_map(sv.map) + " " + sv.address;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (hay.find(needle) == std::string::npos)
                continue;
        }
        rows.push_back(i);
    }

    float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGuiTableFlags tf = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                         ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter;
    if (ImGui::BeginTable("##servers", 5, tf, ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Server", ImGuiTableColumnFlags_WidthStretch, 0, 0);
        float em = ImGui::GetFontSize();
        ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthFixed, em * 9, 1);
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, em * 10, 2);
        ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, em * 8, 3);
        ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, em * 3.5f, 4);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs *spec = ImGui::TableGetSortSpecs()) {
            if (spec->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs &c = spec->Specs[0];
                std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) {
                    const Server &x = list[a], &y = list[b];
                    int d = 0;
                    switch (c.ColumnUserID) {
                    case 0: d = _stricmp(x.host.c_str(), y.host.c_str()); break;
                    case 1: d = _stricmp(pretty_map(x.map).c_str(), pretty_map(y.map).c_str()); break;
                    case 2: d = _stricmp(x.gametype.c_str(), y.gametype.c_str()); break;
                    case 3: d = x.humans != y.humans ? x.humans - y.humans : x.clients - y.clients; break;
                    default: d = (x.ping < 0 ? 1 << 30 : x.ping) - (y.ping < 0 ? 1 << 30 : y.ping); break;
                    }
                    return c.SortDirection == ImGuiSortDirection_Ascending ? d < 0 : d > 0;
                });
            }
        }

        for (size_t r = 0; r < rows.size(); ++r) {
            const Server &sv = list[rows[r]];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(rows[r]);
            std::string name = sv.answered ? (sv.host.empty() ? sv.address : sv.host) : sv.address + "  (no answer)";
            if (sv.password)
                name = "[password] " + name;
            /* a server on another compatibility number (one that declares none is not marked) */
            if (sv.answered && !sv.compat.empty() && atoi(sv.compat.c_str()) != R1_COMPAT)
                name = "[r1_compat " + sv.compat + "] " + name;
            if (ImGui::Selectable(name.c_str(), g_selected == rows[r],
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                g_selected = rows[r];
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && sv.answered)
                    join_server(sv);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s\nr1_compat: %s (this client: %d)", sv.address.c_str(), sv.map.c_str(),
                                  sv.compat.empty() ? "not declared" : sv.compat.c_str(), R1_COMPAT);
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(pretty_map(sv.map).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(pretty_gametype(sv.gametype));
            ImGui::TableNextColumn();
            if (sv.answered) {
                int bots = sv.clients - sv.humans;
                bool full = sv.maxclients > 0 && sv.clients >= sv.maxclients;
                if (full)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                if (bots > 0)
                    ImGui::Text("%d/%d +%d bots", sv.humans, sv.maxclients, bots);
                else
                    ImGui::Text("%d/%d", sv.humans, sv.maxclients);
                if (full)
                    ImGui::PopStyleColor();
            }
            ImGui::TableNextColumn();
            if (sv.answered) {
                ImVec4 c = sv.ping < 60 ? ImVec4(0.45f, 0.95f, 0.45f, 1) : sv.ping < 120 ? ImVec4(1.0f, 0.85f, 0.35f, 1)
                                                                              : ImVec4(1.0f, 0.4f, 0.4f, 1);
                ImGui::TextColored(c, "%d", sv.ping);
            } else {
                ImGui::TextDisabled("-");
            }
        }
        if (rows.empty()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", g_refreshing ? "Asking the master for servers..."
                                      : list.empty() ? status.c_str() : "No server matches the filters");
        }
        ImGui::EndTable();
    }

    bool can_join = g_selected >= 0 && g_selected < (int)list.size() && list[g_selected].answered;
    if (can_join && !typing && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
        join_server(list[g_selected]);
    ImGui::BeginDisabled(!can_join);
    if (ImGui::Button("Join") && can_join)
        join_server(list[g_selected]);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (g_last_refresh && !g_refreshing)
        ImGui::TextDisabled("%s, %us ago  |  F5 refresh, Enter join  |  master %s", status.c_str(),
                            (unsigned)((GetTickCount64() - g_last_refresh) / 1000), g_master);
    else
        ImGui::TextDisabled("%s  |  master %s", status.c_str(), g_master);
    ImGui::End();
}

/* ---- ImGui frame ---- */

static void load_fonts(ImGuiIO &io)
{
    char windir[MAX_PATH], path[MAX_PATH];
    GetWindowsDirectoryA(windir, sizeof windir);
    _snprintf(path, sizeof path, "%s\\Fonts\\consola.ttf", windir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES || !io.Fonts->AddFontFromFileTTF(path, 16.0f))
        io.Fonts->AddFontDefault();
    /* Chinese text in chat, server names and dvar values */
    static const char *const cjk[] = {"msyh.ttc", "msyh.ttf", "simsun.ttc"};
    for (size_t i = 0; i < sizeof cjk / sizeof cjk[0]; ++i) {
        _snprintf(path, sizeof path, "%s\\Fonts\\%s", windir, cjk[i]);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            ImFontConfig merge;
            merge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(path, 16.0f, &merge);
            break;
        }
    }
}

static bool init_imgui(IDirect3DDevice9 *dev)
{
    if (!g_hwnd)
        return false;
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 4.0f;
    st.FrameRounding = 3.0f;
    st.ScrollbarRounding = 3.0f;
    st.WindowBorderSize = 1.0f;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.08f, 0.94f);
    st.Colors[ImGuiCol_Header] = ImVec4(0.85f, 0.55f, 0.15f, 0.35f);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.85f, 0.55f, 0.15f, 0.55f);
    st.Colors[ImGuiCol_HeaderActive] = ImVec4(0.85f, 0.55f, 0.15f, 0.75f);
    st.Colors[ImGuiCol_Button] = ImVec4(0.85f, 0.55f, 0.15f, 0.45f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.85f, 0.55f, 0.15f, 0.70f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.95f, 0.60f, 0.15f, 0.90f);
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.30f, 0.20f, 0.08f, 1.00f);
    {
        /* sized for 1080p; larger screens scale text and spacing up */
        RECT rc;
        float scale = 1.0f;
        if (GetClientRect(g_hwnd, &rc) && rc.bottom > 1080)
            scale = std::min(2.5f, rc.bottom / 1080.0f);
        st.ScaleAllSizes(scale);
        st.FontScaleMain = scale;
    }
    load_fonts(io);
    if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX9_Init(dev)) {
        r1_log("overlay: ImGui backend init failed\n");
        ImGui::DestroyContext();
        return false;
    }
    g_imgui_dev = dev;
    g_imgui_hwnd = g_hwnd;
    g_imgui_ready = true;
    r1_log("overlay: ImGui %s ready\n", IMGUI_VERSION);
    return true;
}

static bool os_cursor_visible(void)
{
    CURSORINFO ci = {sizeof ci};
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
}

/* Builds this frame's panels; false when nothing is open. Takes g_imgui and leaves it held only
 * when it returns true. */
static bool build_frame(IDirect3DDevice9 *dev)
{
    if (!any_open())
        return false;
    EnterCriticalSection(&g_imgui.cs);
    if (!g_imgui_ready && !init_imgui(dev)) {
        LeaveCriticalSection(&g_imgui.cs);
        return false;
    }
    if (dev != g_imgui_dev) {
        /* the renderer created a new device (vid_restart) */
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplDX9_Init(dev);
        g_imgui_dev = dev;
    }
    if (g_hwnd != g_imgui_hwnd) {
        /* the game made a new window (overlay_game_frame moved the input hook to it); the Win32
         * backend reads the display size from its window every frame, and the old one is gone */
        ImGui_ImplWin32_Shutdown();
        ImGui_ImplWin32_Init(g_hwnd);
        g_imgui_hwnd = g_hwnd;
        r1_log("overlay: ImGui moved to window 0x%p\n", (void *)g_hwnd);
    }
    ImGui::GetIO().MouseDrawCursor = !os_cursor_visible();
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (g_browser_open && !g_refreshing && GetTickCount64() - g_last_refresh > 30000)
        start_refresh();
    if (g_console_open)
        draw_console();
    if (g_browser_open)
        draw_browser();
    if (g_stats_open)
        draw_stats();
    ImGui::Render();
    return true;
}

static void run_pending(void)
{
    std::vector<std::string> run;
    EnterCriticalSection(&g_imgui.cs);
    run.swap(g_pending_exec);
    LeaveCriticalSection(&g_imgui.cs);
    for (size_t i = 0; i < run.size(); ++i)
        r1_exec(run[i].c_str());
}

/* ---- drawing ----
 * Runs on the render thread from the call to 0xAD3DA0 that ends the frame (RB_EndFrame-like
 * 0xA986B0, right before its EndScene), so the scene is still open. Nothing in the device is
 * modified: swapping dx.device's vtable crashed the renderer (0xA945F8). */

static void release_device_objects(void)
{
    if (g_imgui_ready)
        ImGui_ImplDX9_InvalidateDeviceObjects();
}

/* frame times for cx_frametime: the interval between two calls of overlay_render_scene (once per drawn frame), the
 * last FT_RING of them, in milliseconds */
#define FT_RING 4096
static float g_ft_ms[FT_RING];
static volatile LONG g_ft_n;
static LONGLONG g_ft_last, g_ft_freq;

static void frame_tick(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_ft_freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_ft_freq = f.QuadPart;
    }
    if (g_ft_last) {
        g_ft_ms[g_ft_n & (FT_RING - 1)] = (float)((now.QuadPart - g_ft_last) * 1000.0 / (double)g_ft_freq);
        InterlockedIncrement(&g_ft_n);
    }
    g_ft_last = now.QuadPart;
}

/* the most recent frame intervals (ms), oldest first; returns how many were copied */
extern "C" int overlay_frame_times(float *out, int max)
{
    LONG n = g_ft_n;
    int count = n < FT_RING ? (int)n : FT_RING, i;
    if (count > max)
        count = max;
    for (i = 0; i < count; i++)
        out[i] = g_ft_ms[(n - count + i) & (FT_RING - 1)];
    return count;
}

/* forget the samples so far (a new measurement starts) */
extern "C" void overlay_frame_times_reset(void)
{
    g_ft_n = 0;
}

extern "C" void overlay_render_scene(void)
{
    static bool was_open;
    IDirect3DDevice9 *dev = *(IDirect3DDevice9 **)r1_addr(VA_DEVICE);
    bool open;
    frame_tick();
    open = any_open();
    if (!open || !dev) {
        if (was_open) {
            /* default-pool buffers would block a later Reset of a non-Ex device */
            EnterCriticalSection(&g_imgui.cs);
            release_device_objects();
            LeaveCriticalSection(&g_imgui.cs);
            was_open = false;
        }
        run_pending();
        return;
    }
    was_open = true;
    if (dev->TestCooperativeLevel() != D3D_OK) {
        EnterCriticalSection(&g_imgui.cs);
        release_device_objects();
        LeaveCriticalSection(&g_imgui.cs);
        return;
    }
    IDirect3DSurface9 *bb = NULL, *rt = NULL;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        return;
    dev->GetRenderTarget(0, &rt);
    if (build_frame(dev)) {
        if (rt != bb)
            dev->SetRenderTarget(0, bb);
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        if (rt && rt != bb)
            dev->SetRenderTarget(0, rt);
        LeaveCriticalSection(&g_imgui.cs);
    }
    if (rt)
        rt->Release();
    bb->Release();
    run_pending();
}

/* ---- window and input ---- */

static void close_all(void)
{
    g_console_open = 0;
    g_browser_open = 0;
    g_stats_open = 0;
}

static bool is_input_message(UINT m)
{
    return (m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST);
}

static bool is_release(UINT m)
{
    return m == WM_KEYUP || m == WM_SYSKEYUP || m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP ||
           m == WM_XBUTTONUP;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    bool was_open = any_open();
    if (m == WM_KEYDOWN && !(l & (1 << 30))) {
        if (w == g_console_key) {
            if (!g_console_open)
                g_console_slide = 0.0f;
            if (GetKeyState(VK_SHIFT) & 0x8000) {
                g_console_tall = !g_console_tall || !g_console_open;
                g_console_open = 1;
            } else {
                g_console_open = !g_console_open;
                g_console_tall = 0;
            }
            if (g_console_open)
                g_focus_console = 1;
            g_eat_char = true;
            goto toggled;
        }
        if (w == g_browser_key) {
            g_browser_open = !g_browser_open;
            goto toggled;
        }
        if (w == g_stats_key) {
            g_stats_open = !g_stats_open;
            goto toggled;
        }
        if (w == VK_ESCAPE && was_open) {
            close_all();
            goto toggled;
        }
    }
    if (m == WM_CHAR && g_eat_char) {
        g_eat_char = false;
        return 0;
    }
    if (was_open && is_input_message(m) && m != WM_SYSKEYDOWN && m != WM_SYSKEYUP && m != WM_SYSCHAR) {
        EnterCriticalSection(&g_imgui.cs);
        if (g_imgui_ready)
            ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        LeaveCriticalSection(&g_imgui.cs);
        /* releases still reach the game, so nothing pressed before opening stays held */
        if (!is_release(m))
            return 0;
    }
    return g_unicode ? CallWindowProcW(g_orig_wndproc, h, m, w, l) : CallWindowProcA(g_orig_wndproc, h, m, w, l);

toggled:
    if (was_open && !any_open()) {
        EnterCriticalSection(&g_imgui.cs);
        if (g_imgui_ready) {
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearInputMouse();
        }
        LeaveCriticalSection(&g_imgui.cs);
    }
    return 0;
}

extern "C" void overlay_game_frame(void)
{
    InterlockedIncrement(&g_game_frames);
    HWND h = *(HWND *)r1_addr(VA_HWND);
    if (h && h != g_hwnd && IsWindow(h)) {
        /* main thread, which owns the window */
        /* subclass with the window's own character set, so WM_CHAR reaches ImGui as the window sends it */
        bool unicode = IsWindowUnicode(h) != FALSE;
        WNDPROC prev = unicode ? (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)wndproc)
                               : (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)wndproc);
        if (prev && prev != wndproc) {
            g_unicode = unicode;
            g_orig_wndproc = prev;
            g_hwnd = h;
            r1_log("overlay: input hooked on window 0x%p\n", (void *)h);
        }
    }
    /* keep the game's console catcher set while a panel is open: the mouse is freed, not re-centred */
    volatile LONG *catchers = (volatile LONG *)r1_addr(VA_KEYCATCHERS);
    if (any_open()) {
        if (!(*catchers & 1))
            InterlockedOr(catchers, 1);
        g_catcher_set = true;
    } else if (g_catcher_set) {
        InterlockedAnd(catchers, ~1);
        g_catcher_set = false;
    }
}

extern "C" void overlay_toggle_browser(void)
{
    g_browser_open = !g_browser_open;
}

extern "C" void overlay_toggle_stats(void)
{
    g_stats_open = !g_stats_open;
}

extern "C" void overlay_start(void)
{
    char ini[MAX_PATH];
    _snprintf(ini, sizeof ini, "%sr1-mod.ini", r1_dir());
    g_console_key = GetPrivateProfileIntA("r1-mod", "console_key", VK_OEM_3, ini);
    g_browser_key = GetPrivateProfileIntA("r1-mod", "browser_key", VK_F6, ini);
    g_stats_key = GetPrivateProfileIntA("r1-mod", "stats_key", VK_F7, ini);
    GetPrivateProfileStringA("r1-mod", "master", "127.0.0.1:20810", g_master, sizeof g_master, ini);
    load_history();
    r1_log("overlay: console key 0x%02X, browser key 0x%02X, master %s\n", g_console_key, g_browser_key, g_master);
}
