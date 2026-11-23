#include "ui.h"

#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <cstring>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "agent.h"
#include "config.h"
#include "json.h"

namespace clam {
namespace {

// ---------------------------------------------------------------- theme


// ---------------------------------------------------------------- themes
enum class Theme { Black, White, PinkBlueDark, PinkBlueLight, Sunset, Ocean, Forest, Aurora };

struct ThemeOption {
    Theme theme;
    const char* label;
    const char* key;
};

constexpr ThemeOption kThemeOptions[] = {
    {Theme::Black, "Black", "black"},
    {Theme::White, "White", "white"},
    {Theme::PinkBlueDark, "Pink/blue", "pinkblue"},
    {Theme::PinkBlueLight, "Pink/blue light", "pinkblue_light"},
    {Theme::Sunset, "Sunset", "sunset"},
    {Theme::Ocean, "Ocean", "ocean"},
    {Theme::Forest, "Forest", "forest"},
    {Theme::Aurora, "Aurora", "aurora"},
};
constexpr int kThemeCount = static_cast<int>(sizeof(kThemeOptions) / sizeof(kThemeOptions[0]));
constexpr int kThemesPerRow = 4;
constexpr int kThemeThumbW = 100;
constexpr int kThemeThumbH = 44;
constexpr int kThemeRowPitch = 66;
constexpr int kThemeRows = (kThemeCount + kThemesPerRow - 1) / kThemesPerRow;
constexpr int kThemeCardH = 30 + kThemeRows * kThemeRowPitch;

struct Palette {
    COLORREF bg, panel, card, card_user, border, text, muted, accent, input_bg;
    bool light = false;      // light themes need dark-on-white controls
    bool gradient = false;   // gradient themes paint a vertical backdrop
    COLORREF grad_top, grad_bottom;
};

Palette palette_for(Theme theme) {
    switch (theme) {
        case Theme::White:
            // All-neutral light theme: white fills, grey text.
            return Palette{RGB(250, 250, 250), RGB(255, 255, 255), RGB(255, 255, 255),
                           RGB(246, 246, 246), RGB(214, 214, 214), RGB(24, 24, 24),
                           RGB(118, 118, 118), RGB(24, 24, 24), RGB(255, 255, 255),
                           true, false, RGB(0, 0, 0), RGB(0, 0, 0)};
        case Theme::PinkBlueDark:
            // Soft pink-to-blue backdrop; cards stay dark and neutral.
            return Palette{RGB(18, 16, 30),  RGB(24, 21, 38),    RGB(30, 26, 46),
                           RGB(38, 32, 56),  RGB(64, 54, 92),    RGB(238, 235, 250),
                           RGB(168, 160, 196), RGB(255, 139, 199), RGB(26, 22, 42),
                           false, true, RGB(76, 32, 66), RGB(24, 36, 82)};
        case Theme::PinkBlueLight:
            return Palette{RGB(252, 250, 255), RGB(255, 255, 255), RGB(255, 255, 255),
                           RGB(246, 242, 255), RGB(220, 212, 240), RGB(36, 28, 56),
                           RGB(126, 116, 152), RGB(190, 66, 146), RGB(255, 255, 255),
                           true, true, RGB(255, 226, 241), RGB(228, 239, 255)};
        case Theme::Sunset:
            return Palette{RGB(24, 16, 22), RGB(32, 22, 29), RGB(40, 27, 35), RGB(51, 34, 44),
                           RGB(84, 56, 70), RGB(255, 242, 234), RGB(196, 158, 148),
                           RGB(255, 148, 92), RGB(34, 23, 30),
                           false, true, RGB(104, 40, 54), RGB(36, 31, 76)};
        case Theme::Ocean:
            return Palette{RGB(10, 20, 28), RGB(15, 29, 38), RGB(20, 38, 49), RGB(27, 50, 64),
                           RGB(50, 83, 100), RGB(230, 246, 250), RGB(144, 181, 196),
                           RGB(92, 214, 235), RGB(16, 34, 43),
                           false, true, RGB(25, 86, 110), RGB(10, 28, 50)};
        case Theme::Forest:
            return Palette{RGB(10, 18, 14), RGB(15, 27, 21), RGB(21, 36, 28), RGB(28, 48, 37),
                           RGB(53, 82, 64), RGB(233, 247, 238), RGB(148, 180, 160),
                           RGB(126, 218, 154), RGB(17, 31, 24),
                           false, true, RGB(38, 92, 65), RGB(10, 27, 30)};
        case Theme::Aurora:
            return Palette{RGB(13, 14, 27), RGB(19, 21, 36), RGB(26, 29, 48), RGB(35, 39, 62),
                           RGB(63, 69, 106), RGB(240, 240, 255), RGB(167, 170, 208),
                           RGB(170, 150, 255), RGB(21, 23, 41),
                           false, true, RGB(54, 37, 104), RGB(16, 64, 86)};
        case Theme::Black:
        default:
            return Palette{RGB(10, 10, 10), RGB(16, 16, 16), RGB(22, 22, 22), RGB(30, 30, 30),
                           RGB(48, 48, 48), RGB(235, 235, 235), RGB(140, 140, 140),
                           RGB(235, 235, 235), RGB(22, 22, 22),
                           false, false, RGB(0, 0, 0), RGB(0, 0, 0)};
    }
}

// Vertical black->grey gradient used as the app backdrop for pink/blue themes.
void paint_gradient(HDC dc, RECT area, COLORREF top, COLORREF bottom) {
    TRIVERTEX verts[2];
    verts[0].x = area.left;
    verts[0].y = area.top;
    verts[0].Red = static_cast<COLOR16>(GetRValue(top) << 8);
    verts[0].Green = static_cast<COLOR16>(GetGValue(top) << 8);
    verts[0].Blue = static_cast<COLOR16>(GetBValue(top) << 8);
    verts[0].Alpha = 0xFF00;
    verts[1].x = area.right;
    verts[1].y = area.bottom;
    verts[1].Red = static_cast<COLOR16>(GetRValue(bottom) << 8);
    verts[1].Green = static_cast<COLOR16>(GetGValue(bottom) << 8);
    verts[1].Blue = static_cast<COLOR16>(GetBValue(bottom) << 8);
    verts[1].Alpha = 0xFF00;
    GRADIENT_RECT rect{0, 1};
    GradientFill(dc, verts, 2, &rect, 1, GRADIENT_FILL_RECT_V);
}

Palette g_palette = palette_for(Theme::Black);

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

// Keep the native frame in step with the painted theme. Failure is harmless on
// older Windows builds that do not know these DWM attributes.
void update_window_frame(HWND hwnd) {
    if (!hwnd) return;
    BOOL dark = g_palette.light ? FALSE : TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                                     sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }
    DWORD corners = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    COLORREF border = g_palette.border;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

void apply_theme(Theme theme) {
    g_palette = palette_for(theme);
}

// Helper so old constant names keep working.
#define kBg        (g_palette.bg)
#define kPanel     (g_palette.panel)
#define kCard      (g_palette.card)
#define kCardUser  (g_palette.card_user)
#define kBorder    (g_palette.border)
#define kText      (g_palette.text)
#define kMuted     (g_palette.muted)
#define kAccent    (g_palette.accent)
#define kInputBg   (g_palette.input_bg)
#define kWarn      (g_palette.muted)
#define kDanger    (g_palette.light ? RGB(182, 38, 57) : RGB(255, 122, 134))

// Readable text on top of the accent colour, whatever the theme's accent is.
COLORREF accent_text_color() {
    const int luminance = (GetRValue(kAccent) * 299 + GetGValue(kAccent) * 587 +
                           GetBValue(kAccent) * 114) / 1000;
    return luminance > 150 ? kBg : RGB(255, 255, 255);
}

COLORREF blend_color(COLORREF a, COLORREF b, int percent) {
    percent = std::clamp(percent, 0, 100);
    int r = (GetRValue(a) * (100 - percent) + GetRValue(b) * percent + 50) / 100;
    int g = (GetGValue(a) * (100 - percent) + GetGValue(b) * percent + 50) / 100;
    int bl = (GetBValue(a) * (100 - percent) + GetBValue(b) * percent + 50) / 100;
    return RGB(r, g, bl);
}

COLORREF hover_fill() { return blend_color(kCard, kText, g_palette.light ? 7 : 9); }
COLORREF selection_fill() {
    return g_palette.light ? blend_color(kInputBg, RGB(60, 120, 220), 20)
                           : blend_color(kInputBg, RGB(140, 180, 255), 22);
}
COLORREF code_fill() {
    return g_palette.light ? blend_color(kCard, kText, 4) : blend_color(kBg, kText, 4);
}
COLORREF danger_fill() {
    return g_palette.light ? RGB(255, 241, 243) : RGB(45, 23, 27);
}

// Colour of the full-window gradient at a given y, so panels such as the
// composer footer can continue it without a visible seam.
COLORREF gradient_color_at(int y, int top, int bottom) {
    if (bottom <= top) return g_palette.grad_top;
    const int percent = static_cast<int>((static_cast<long long>(y - top) * 100) /
                                         (bottom - top));
    return blend_color(g_palette.grad_top, g_palette.grad_bottom, percent);
}

constexpr int kRadiusCard = 14;
constexpr int kRadiusControl = 10;
constexpr int kRadiusLarge = 18;
constexpr int kHeaderH = 48;
constexpr int kSidebarW = 56;
constexpr int kSidebarRevealW = 12;
constexpr int kFooterH = 140;
constexpr int kUserPreviewWords = 15;
constexpr int kUserToggleLimit = 300;  // CidUserToggle .. CidNewChatTab - 1
constexpr UINT WM_APP_TURN = WM_APP + 1;

enum class Tab { Chat, Activity, Settings };

struct UiMessage {
    Role role;
    std::string text;
    std::vector<std::string> images;
    bool expanded = false;
};

struct ActivityItem {
    std::string label;
    std::string summary;
    std::string detail;
    bool expanded = false;
};

struct Attachment {
    std::string path;
    HBITMAP bitmap = nullptr;
    int width = 0;
    int height = 0;
};


// One conversation, bound to its own project directory.
struct ChatSession {
    std::string title;
    std::string project;
    std::vector<UiMessage> transcript;
    std::vector<ActivityItem> activity;
    std::string id;
    long long updated_at = 0;
};

enum class AsyncKind { Tool, Answer, Error, Done, Models };

struct AsyncEvent {
    AsyncKind kind;
    std::string text;
    std::string name;
    std::vector<std::string> images;
};

struct HitRegion {
    RECT rect;
    int id;
    bool content = false;
};

// A painted text area that supports click-drag selection.
struct TextHit {
    int region;
    RECT box;
    std::string text;
};

enum ControlId {
    CidNone = 0,
    CidChat, CidActivity, CidSettings,
    CidModel, CidEffort, CidPerm, CidNewChat,
    CidSend, CidAttach, CidPaste, CidCancel,
    CidKeySave, CidProjectOpen, CidProjectBrowse, CidRefresh, CidModelPick,
    CidSidebarReveal = 390,
    CidTheme = 400,
    CidUserToggle = 500,
    CidNewChatTab = 800,
    CidAttachmentChip = 900,
    CidAttachmentChipEnd = 1000,
    // Chat history can grow well past the old 40-chat limit, so give saved
    // conversations their own wide id ranges.
    CidChatTab = 10000,
    CidChatClose = 20000,
    CidActivityRow = 30000,
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND composer_edit = nullptr;   // multiline chat input
    HWND key_field_edit = nullptr;  // password field for the API key
    HWND project_edit = nullptr;    // project folder path
    std::string input;

    Settings settings;
    std::string api_key;
    std::string root;
    Tab tab = Tab::Chat;

    std::vector<ChatSession> sessions;
    int active_session = 0;
    int sidebar_hover = -1;
    bool sidebar_open = true;
    bool sidebar_hide_armed = false;
    int sidebar_scroll = 0;
    bool sidebar_scroll_to_active = true;
    RECT sidebar_tooltip{};
    bool sidebar_tooltip_visible = false;
    Theme theme = Theme::Black;
    std::string thinking;
    std::string project_buffer_shadow;

    std::vector<UiMessage>& transcript() { return sessions[active_index()].transcript; }
    std::vector<ActivityItem>& activity() { return sessions[active_index()].activity; }
    size_t active_index();
    std::vector<Attachment> attachments;

    bool busy = false;
    bool models_loading = false;
    bool dropdown_open = false;
    bool key_editing = false;
    std::string activity_status;
    std::string error;
    std::vector<std::string> models;
    int dropdown_hover = -1;
    int model_scroll = 0;

    int scroll = 0;
    int content_height = 0;
    int hover_id = CidNone;
    bool caret_on = true;
    ULONGLONG caret_tick = 0;

    // Text selection: which region, and the character offsets inside it.
    // 0 = composer, >0 = transcript message index (1-based).
    int sel_region = 0;
    int sel_start = 0;
    int sel_end = 0;
    bool sel_dragging = false;
    int caret_pos = 0;
    int pressed_id = CidNone;

    std::string key_buffer;
    std::string project_buffer;

    std::vector<TextHit> text_regions;
    RECT composer_box{};
    std::vector<HitRegion> hits;
    std::vector<RECT> model_rows;
    std::vector<int> activity_rows;
    std::vector<RECT> attachment_chips;

    std::atomic<bool> cancel{false};

    // Repositions a child control without touching focus or forcing a repaint loop.
    void layout_edit(HWND control, int x, int y, int width, int height, int round_radius = 0) {
        if (!control || width <= 0 || height <= 0) return;
        RECT current{};
        GetWindowRect(control, &current);
        POINT origin{current.left, current.top};
        ScreenToClient(window, &origin);
        const bool moved = origin.x != x || origin.y != y ||
                           current.right - current.left != width ||
                           current.bottom - current.top != height;
        if (moved) {
            SetWindowPos(control, nullptr, x, y, width, height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        if (round_radius > 0) {
            HRGN existing = CreateRectRgn(0, 0, 0, 0);
            const int type = GetWindowRgn(control, existing);
            DeleteObject(existing);
            if (moved || type == ERROR) {
                HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                                 round_radius * 2, round_radius * 2);
                SetWindowRgn(control, region, TRUE);
            }
        }
    }

    HFONT font_ui = nullptr;
    HFONT font_title = nullptr;
    HFONT font_brand = nullptr;
    HFONT font_heading = nullptr;
    HFONT font_small = nullptr;
    HFONT font_tiny = nullptr;
    HFONT font_mono = nullptr;
};

AppState* g_app = nullptr;

// Width reserved by the rail. When it is hidden the content expands into the
// space instead of leaving an empty gutter behind.
int sidebar_width() { return g_app && g_app->sidebar_open ? kSidebarW : 0; }


Theme theme_from_string(const std::string& name) {
    for (const auto& option : kThemeOptions) {
        if (name == option.key) return option.theme;
    }
    return Theme::Black;
}

std::string theme_to_string(Theme theme) {
    for (const auto& option : kThemeOptions) {
        if (theme == option.theme) return option.key;
    }
    return "black";
}

// ---------------------------------------------------------------- chat files

std::string chats_dir() { return join_path(config_dir(), "chats"); }

std::string make_session_id() {
    FILETIME file_time{};
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    static unsigned long long sequence = 0;
    return "chat-" + std::to_string(value.QuadPart) + "-" +
           std::to_string(++sequence);
}

long long now_seconds() {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string relative_time(long long timestamp) {
    if (timestamp <= 0) return std::string();
    const long long delta = std::max(0LL, now_seconds() - timestamp);
    if (delta < 60) return "just now";
    if (delta < 3600) return std::to_string(delta / 60) + "m ago";
    if (delta < 86400) return std::to_string(delta / 3600) + "h ago";
    if (delta < 604800) return std::to_string(delta / 86400) + "d ago";
    return std::to_string(delta / 604800) + "w ago";
}

const char* role_name(Role role) {
    switch (role) {
        case Role::User: return "user";
        case Role::Tool: return "tool";
        case Role::Assistant:
        default: return "assistant";
    }
}

Role role_from_name(const std::string& name) {
    if (name == "user") return Role::User;
    if (name == "tool") return Role::Tool;
    return Role::Assistant;
}

Json string_array(const std::vector<std::string>& values) {
    Json array = Json::make_array();
    for (const auto& value : values) array.push_back(Json(value));
    return array;
}

Json session_to_json(const ChatSession& session) {
    Json root = Json::make_object();
    root.set("version", Json(1));
    root.set("id", Json(session.id));
    root.set("title", Json(session.title));
    root.set("project", Json(session.project));
    root.set("updated_at", Json(session.updated_at));

    Json transcript = Json::make_array();
    for (const auto& message : session.transcript) {
        Json item = Json::make_object();
        item.set("role", Json(role_name(message.role)));
        item.set("text", Json(message.text));
        item.set("images", string_array(message.images));
        item.set("expanded", Json(message.expanded));
        transcript.push_back(item);
    }
    root.set("transcript", transcript);

    Json activity = Json::make_array();
    for (const auto& entry : session.activity) {
        Json item = Json::make_object();
        item.set("label", Json(entry.label));
        item.set("summary", Json(entry.summary));
        item.set("detail", Json(entry.detail));
        item.set("expanded", Json(entry.expanded));
        activity.push_back(item);
    }
    root.set("activity", activity);
    return root;
}

long long file_time_seconds(const FILETIME& file_time) {
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    if (value.QuadPart < 116444736000000000ULL) return 0;
    return static_cast<long long>(value.QuadPart / 10000000ULL - 11644473600ULL);
}

bool session_from_json(const Json& root, const std::string& id, ChatSession& session) {
    session = ChatSession{};
    session.id = id;
    session.title = root["title"].string_or("New chat");
    if (session.title.empty()) session.title = "New chat";
    session.project = root["project"].string_or("");
    session.updated_at = static_cast<long long>(root["updated_at"].as_number(0));

    for (const auto& item : root["transcript"].array()) {
        UiMessage message;
        message.role = role_from_name(item["role"].string_or("assistant"));
        message.text = item["text"].string_or("");
        message.expanded = item["expanded"].as_bool(false);
        for (const auto& image : item["images"].array()) {
            std::string path = image.string_or("");
            if (!path.empty()) message.images.push_back(path);
        }
        session.transcript.push_back(std::move(message));
    }

    for (const auto& item : root["activity"].array()) {
        ActivityItem entry;
        entry.label = item["label"].string_or("");
        entry.summary = item["summary"].string_or("");
        entry.detail = item["detail"].string_or("");
        entry.expanded = item["expanded"].as_bool(false);
        session.activity.push_back(std::move(entry));
    }
    return !session.transcript.empty() || !session.activity.empty();
}

std::vector<ChatSession> load_chat_sessions() {
    std::vector<ChatSession> sessions;
    const std::string dir = chats_dir();
    if (!is_directory(dir)) return sessions;

    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW(utf8_to_wide(join_path(dir, "*.json")).c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) return sessions;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = wide_to_utf8(data.cFileName);
        const size_t dot = name.find_last_of('.');
        const std::string id = dot == std::string::npos ? name : name.substr(0, dot);
        std::string raw;
        if (!read_utf8_file(join_path(dir, name), raw) || raw.empty()) continue;
        auto parsed = Json::parse(raw);
        if (!parsed) continue;
        ChatSession session;
        if (!session_from_json(*parsed, id, session)) continue;
        if (session.updated_at <= 0) {
            session.updated_at = file_time_seconds(data.ftLastWriteTime);
        }
        sessions.push_back(std::move(session));
    } while (FindNextFileW(handle, &data));
    FindClose(handle);

    // Oldest first, so the newest conversation sits at the bottom of the rail
    // and the active conversation is the last item.
    std::sort(sessions.begin(), sessions.end(),
              [](const ChatSession& a, const ChatSession& b) {
                  if (a.updated_at != b.updated_at) return a.updated_at < b.updated_at;
                  return a.id < b.id;
              });
    return sessions;
}

void save_chat_session(ChatSession& session) {
    if (session.transcript.empty() && session.activity.empty()) return;
    if (session.id.empty()) session.id = make_session_id();
    session.updated_at = now_seconds();
    const std::string dir = chats_dir();
    if (!make_directories(dir)) return;
    const std::string path = join_path(dir, session.id + ".json");
    const std::string temporary = path + ".tmp";
    const std::string content = session_to_json(session).dump(2);
    if (!write_utf8_file(temporary, content)) {
        write_utf8_file(path, content);
        return;
    }
    if (!MoveFileExW(utf8_to_wide(temporary).c_str(), utf8_to_wide(path).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(utf8_to_wide(temporary).c_str());
        write_utf8_file(path, content);
    }
}

void save_active_chat() {
    if (!g_app || g_app->sessions.empty()) return;
    save_chat_session(g_app->sessions[g_app->active_index()]);
}

void delete_chat_file(const ChatSession& session) {
    if (session.id.empty()) return;
    const std::string path = join_path(chats_dir(), session.id + ".json");
    if (path_exists(path)) DeleteFileW(utf8_to_wide(path).c_str());
}

size_t AppState::active_index() {
    if (sessions.empty()) {
        ChatSession session;
        session.title = "New chat";
        session.project = root;
        session.id = make_session_id();
        session.updated_at = now_seconds();
        sessions.push_back(std::move(session));
    }
    if (active_session < 0 || active_session >= static_cast<int>(sessions.size())) {
        active_session = 0;
    }
    return static_cast<size_t>(active_session);
}

// Creates a chat tab for a project folder.
void add_session(const std::string& project, const std::string& title) {
    AppState* app = g_app;
    std::string trimmed = title.empty() ? std::string("New chat") : title;
    ChatSession session;
    session.title = trimmed;
    session.project = project;
    session.id = make_session_id();
    session.updated_at = now_seconds();
    app->sessions.push_back(std::move(session));
    app->active_session = static_cast<int>(app->sessions.size()) - 1;
    if (!project.empty()) app->root = project;
    app->scroll = 0;
    app->sidebar_scroll_to_active = true;
}

std::string project_label(const std::string& path) {
    if (path.empty()) return "no project";
    std::string trimmed = path;
    while (!trimmed.empty() && (trimmed.back() == '\\' || trimmed.back() == '/')) trimmed.pop_back();
    size_t slash = trimmed.find_last_of("\\/");
    return slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
}


void post_async(AsyncKind kind, const std::string& text, const std::string& name = std::string()) {
    auto* event = new AsyncEvent{kind, text, name, {}};
    PostMessageW(g_app->window, WM_APP_TURN, 0, reinterpret_cast<LPARAM>(event));
}

std::wstring wide(const std::string& text) { return utf8_to_wide(text); }

bool font_installed(const wchar_t* face) {
    HKEY key = nullptr;
    std::wstring subkey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_READ, &key);
    }
    if (!key) return false;
    DWORD index = 0;
    wchar_t value_name[256];
    wchar_t data[512];
    bool found = false;
    DWORD name_len = static_cast<DWORD>(sizeof(value_name) / sizeof(value_name[0]));
    DWORD data_len = static_cast<DWORD>(sizeof(data));
    while (RegEnumValueW(key, index++, value_name, &name_len, nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(data), &data_len) == ERROR_SUCCESS) {
        if (wcsstr(data, face)) {
            found = true;
            break;
        }
        name_len = static_cast<DWORD>(sizeof(value_name) / sizeof(value_name[0]));
        data_len = static_cast<DWORD>(sizeof(data));
    }
    RegCloseKey(key);
    return found;
}

const wchar_t* ui_face() {
    // Segoe UI Variable is the Windows 11 system UI face; Segoe UI and Tahoma
    // keep the same clean look on older Windows versions.
    for (const wchar_t* face : {L"Segoe UI Variable Text", L"Segoe UI", L"Tahoma"}) {
        if (font_installed(face)) return face;
    }
    return L"Segoe UI";
}

const wchar_t* mono_face() {
    for (const wchar_t* face : {L"Cascadia Mono", L"Cascadia Code", L"Consolas",
                                L"Courier New"}) {
        if (font_installed(face)) return face;
    }
    return L"Consolas";
}

HFONT make_ui_font(int size, int weight) {
    return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, ui_face());
}

HFONT make_mono_font(int size, int weight) {
    return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       FIXED_PITCH | FF_MODERN, mono_face());
}

void fill_rect_solid(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

// Fills a rounded rectangle and returns the border in a single pass.
void rounded_rect(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, brush);
    HGDIOBJ op = SelectObject(dc, pen);
    if (radius <= 0) {
        Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    } else {
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(brush);
    DeleteObject(pen);
}

void rounded_frame(HDC dc, RECT rect, COLORREF border, int radius) {
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
}

// Clips drawing to a rounded rectangle. The caller must restore `saved`.
int clip_round_rect(HDC dc, RECT rect, int radius) {
    int saved = SaveDC(dc);
    HRGN region = CreateRoundRectRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1,
                                     radius * 2, radius * 2);
    SelectClipRgn(dc, region);
    DeleteObject(region);
    return saved;
}

int text_width(HDC dc, const std::string& text, HFONT font) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    std::wstring w = wide(text);
    GetTextExtentPoint32W(dc, w.c_str(), static_cast<int>(w.size()), &size);
    SelectObject(dc, old);
    return size.cx;
}

int draw(HDC dc, const std::string& text, RECT rect, COLORREF color, HFONT font,
         UINT format = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    std::wstring w = wide(text);
    int height = DrawTextW(dc, w.c_str(), static_cast<int>(w.size()), &rect, format);
    SelectObject(dc, old);
    return height;
}

int measure(HDC dc, const std::string& text, int width, HFONT font) {
    HGDIOBJ old = SelectObject(dc, font);
    RECT rect{0, 0, width, 0};
    std::wstring w = wide(text);
    DrawTextW(dc, w.c_str(), static_cast<int>(w.size()), &rect,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_LEFT);
    SelectObject(dc, old);
    return rect.bottom - rect.top;
}

bool hit(RECT rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
           point.y < rect.bottom;
}

bool g_hit_content = false;

void add_hit(RECT rect, int id) {
    g_app->hits.push_back(HitRegion{rect, id, g_hit_content});
}

int hit_test(POINT point) {
    for (auto it = g_app->hits.rbegin(); it != g_app->hits.rend(); ++it) {
        if (!hit(it->rect, point)) continue;
        // Scrolling content is clipped below the header; it must never steal a
        // click from the header controls it slides underneath.
        if (point.y < kHeaderH && it->content) continue;
        return it->id;
    }
    return CidNone;
}

void reset_hits() {
    g_hit_content = false;
    g_app->hits.clear();
    g_app->model_rows.clear();
    g_app->activity_rows.clear();
    g_app->attachment_chips.clear();
    g_app->text_regions.clear();
}

std::string get_window_text(HWND control) {
    if (!control) return std::string();
    int length = GetWindowTextLengthW(control);
    if (length <= 0) return std::string();
    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, buffer.data(), length + 1);
    buffer.resize(static_cast<size_t>(length));
    return wide_to_utf8(buffer);
}

// ---------------------------------------------------------------- data
void save() { save_settings(g_app->settings); }

std::string strip_markdown(const std::string& input);
bool start_turn(const std::string& text);

void handle_async(AsyncEvent* event) {
    AppState* app = g_app;
    switch (event->kind) {
        case AsyncKind::Tool:
            if (event->name == "__status") {
                app->activity_status = event->text;
            } else if (event->name == "__thinking") {
                app->thinking = event->text;
            } else {
                std::string summary = event->text;
                size_t newline = summary.find('\n');
                if (newline != std::string::npos) summary = summary.substr(0, newline);
                summary = trim(summary);
                if (summary.size() > 100) summary = summary.substr(0, 100) + "\u2026";
                std::string label = event->name;
                if (label == "write_file") label = "file change";
                else if (label == "read_file") label = "file read";
                else if (label == "list_files") label = "files";
                else if (label == "search_text") label = "search";
                else if (label == "run_command") label = "command";
                else if (label == "web_search") label = "web search";
                else if (label == "fetch_url") label = "web fetch";
                else if (label == "visual_test" || label == "view_image") label = "image view";
                else if (label == "system_info") label = "hardware";
                else if (label.rfind("arduino", 0) == 0) label = "arduino";
                else if (label == "serial_read") label = "serial";
                app->activity().push_back(ActivityItem{label, summary, event->text, false});
                save_active_chat();
            }
            break;
        case AsyncKind::Answer:
            app->transcript().push_back(UiMessage{Role::Assistant, event->text, {}});
            app->activity_status.clear();
            save_active_chat();
            break;
        case AsyncKind::Error:
            app->error = event->text;
            break;
        case AsyncKind::Done:
            app->busy = false;
            app->activity_status.clear();
            break;
        case AsyncKind::Models:
            app->models_loading = false;
            if (event->text.rfind("__error__", 0) == 0) {
                app->error = event->name;
            } else {
                app->models.clear();
                size_t start = 0;
                while (start <= event->text.size()) {
                    size_t end = event->text.find('\n', start);
                    if (end == std::string::npos) end = event->text.size();
                    std::string line = event->text.substr(start, end - start);
                    if (!line.empty() &&
                        std::find(app->models.begin(), app->models.end(), line) == app->models.end()) {
                        app->models.push_back(line);
                    }
                    if (end == event->text.size()) break;
                    start = end + 1;
                }
                if (!app->models.empty() &&
                    std::find(app->models.begin(), app->models.end(), app->settings.model) ==
                        app->models.end()) {
                    app->settings.model = app->models.front();
                    save();
                }
            }
            break;
    }
    delete event;
    InvalidateRect(app->window, nullptr, FALSE);
}

void refresh_models() {
    if (g_app->api_key.empty() || g_app->models_loading) return;
    g_app->models_loading = true;
    std::string key = g_app->api_key;
    std::thread([key] {
        std::string error;
        std::vector<std::string> models = fetch_models(key, error);
        if (models.empty()) {
            post_async(AsyncKind::Models, "__error__" + error,
                       error.empty() ? "No code-capable models available." : error);
        } else {
            std::string joined;
            for (const auto& model : models) joined += model + "\n";
            post_async(AsyncKind::Models, joined);
        }
    }).detach();
}

bool start_turn(const std::string& text) {
    AppState* app = g_app;
    if (app->busy) return false;
    if (app->api_key.empty()) {
        app->error = "Add your NVIDIA API key in Settings.";
        app->tab = Tab::Settings;
        InvalidateRect(app->window, nullptr, FALSE);
        return false;
    }
    if (text.empty() && app->attachments.empty()) return false;

    std::vector<std::string> images;
    for (const auto& attachment : app->attachments) images.push_back(attachment.path);

    std::string caption = text;
    for (size_t i = 0; i < images.size(); ++i) {
        caption += " [image " + std::to_string(i + 1) + "]";
    }
    if (caption.empty()) caption = "Analyze the attached image and help with the task.";

    std::vector<Message> history;
    for (const auto& message : app->transcript()) {
        Message converted;
        converted.role = message.role;
        converted.text = message.text;
        converted.images = message.images;
        history.push_back(converted);
    }
    Message user;
    user.role = Role::User;
    user.text = caption;
    user.images = images;
    history.push_back(user);

    app->transcript().push_back(UiMessage{Role::User, caption, images});
    {
        ChatSession& session = app->sessions[app->active_index()];
        if (session.project.empty()) session.project = app->root;
        if (session.title.empty() || session.title == "New chat") {
            std::string title = strip_markdown(text);
            std::replace(title.begin(), title.end(), '\n', ' ');
            if (title.size() > 28) title = title.substr(0, 28);
            session.title = title.empty() ? "New chat" : title;
            session.project = app->root;
        }
    }
    for (auto& attachment : app->attachments) {
        if (attachment.bitmap) DeleteObject(attachment.bitmap);
    }
    app->attachments.clear();
    save_active_chat();
    app->busy = true;
    app->error.clear();
    app->activity_status = "thinking\u2026";
    app->scroll = 0;

    struct TurnArgs {
        std::string key, model, effort, root;
        std::vector<Message> history;
    };
    auto* args = new TurnArgs{app->api_key, app->settings.model, app->settings.effort, app->root,
                              history};

    AgentCallbacks callbacks;
    callbacks.on_status = [](const std::string& status) { post_async(AsyncKind::Tool, status, "__status"); };
    callbacks.on_thinking = [](const std::string& thinking) {
        post_async(AsyncKind::Tool, thinking, "__thinking");
    };
    callbacks.on_tool = [](const std::string& name, const ToolResult& result) {
        post_async(AsyncKind::Tool, result.output, name);
    };
    callbacks.on_answer = [](const std::string& answer) { post_async(AsyncKind::Answer, answer); };
    callbacks.on_error = [](const std::string& error) { post_async(AsyncKind::Error, error); };
    callbacks.on_done = [] { post_async(AsyncKind::Done, std::string()); };

    app->cancel = false;
    std::thread([args, callbacks, app] {
        std::vector<Message> updated;
        run_turn(args->key, args->model, args->effort, args->root, args->history, app->cancel,
                 callbacks, &updated);
        delete args;
    }).detach();
    return true;
}

// ---------------------------------------------------------------- attachments
HBITMAP load_bitmap(const std::string& path, int& width, int& height) {
    HBITMAP bitmap = static_cast<HBITMAP>(LoadImageW(
        nullptr, wide(path).c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
    if (!bitmap) return nullptr;
    BITMAP info{};
    GetObjectW(bitmap, sizeof(info), &info);
    width = info.bmWidth;
    height = info.bmHeight;
    return bitmap;
}

void attach_paths(const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        if (g_app->attachments.size() >= static_cast<size_t>(kMaxImagesPerMessage)) {
            g_app->error = "Up to 10 images per message.";
            break;
        }
        bool exists = false;
        for (const auto& attachment : g_app->attachments) {
            if (attachment.path == path) exists = true;
        }
        if (exists) continue;
        Attachment attachment;
        attachment.path = path;
        attachment.bitmap = load_bitmap(path, attachment.width, attachment.height);
        g_app->attachments.push_back(attachment);
    }
    InvalidateRect(g_app->window, nullptr, FALSE);
}

void attach_dialog(HWND owner) {
    wchar_t buffer[8192] = {0};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp\0All files\0*.*\0";
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = 8192;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog)) return;
    std::vector<std::string> paths;
    const wchar_t* first = buffer;
    const wchar_t* next = first + wcslen(first) + 1;
    if (*next) {
        std::string dir = wide_to_utf8(first);
        for (const wchar_t* name = next; *name; name += wcslen(name) + 1) {
            paths.push_back(join_path(dir, wide_to_utf8(name)));
        }
    } else {
        paths.push_back(wide_to_utf8(first));
    }
    attach_paths(paths);
}

// Saves an HBITMAP as a real PNG via GDI+ so the model API accepts it.
bool save_bitmap_as_png(HBITMAP bitmap, const std::string& path) {
    if (!bitmap) return false;
    bool ok = false;
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return false;

    Gdiplus::Bitmap image(bitmap, nullptr);
    if (image.GetLastStatus() == Gdiplus::Ok) {
        CLSID encoder{};
        UINT count = 0;
        UINT size = 0;
        if (Gdiplus::GetImageEncodersSize(&count, &size) == Gdiplus::Ok && size > 0) {
            std::vector<unsigned char> buffer(size);
            auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
            if (Gdiplus::GetImageEncoders(count, size, codecs) == Gdiplus::Ok) {
                for (UINT i = 0; i < count; ++i) {
                    if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
                        encoder = codecs[i].Clsid;
                        ok = true;
                        break;
                    }
                }
            }
        }
        if (ok) {
            std::wstring wide_path = utf8_to_wide(path);
            ok = image.Save(wide_path.c_str(), &encoder, nullptr) == Gdiplus::Ok;
        }
    }
    Gdiplus::GdiplusShutdown(token);
    return ok;
}

void attach_clipboard(HWND owner) {
    if (!OpenClipboard(owner)) return;
    HBITMAP handle = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    if (!handle) {
        CloseClipboard();
        g_app->error = "No image in the clipboard. Copy a screenshot first.";
        InvalidateRect(g_app->window, nullptr, FALSE);
        return;
    }
    BITMAP info{};
    GetObjectW(handle, sizeof(info), &info);

    // Copy to a private 32-bit DIB so we can hand it to the PNG encoder.
    HDC screen = GetDC(nullptr);
    HDC source_dc = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(source_dc, handle);
    BITMAPINFO header{};
    header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    header.bmiHeader.biWidth = info.bmWidth;
    header.bmiHeader.biHeight = -info.bmHeight;
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP copy = CreateDIBSection(screen, &header, DIB_RGB_COLORS, &bits, nullptr, 0);
    // Blit the clipboard bitmap into the DIB section.
    if (copy && bits) {
        HDC target_dc = CreateCompatibleDC(screen);
        HGDIOBJ target_old = SelectObject(target_dc, copy);
        BitBlt(target_dc, 0, 0, info.bmWidth, info.bmHeight, source_dc, 0, 0, SRCCOPY);
        SelectObject(target_dc, target_old);
        DeleteDC(target_dc);
    }
    SelectObject(source_dc, old);
    DeleteDC(source_dc);
    ReleaseDC(nullptr, screen);
    CloseClipboard();

    if (!copy) {
        g_app->error = "Could not read the clipboard image.";
        InvalidateRect(g_app->window, nullptr, FALSE);
        return;
    }

    std::string dir = join_path(config_dir(), "clipboard");
    make_directories(dir);
    std::string path = join_path(dir, "paste-" + std::to_string(GetTickCount64()) + ".png");
    if (save_bitmap_as_png(copy, path)) {
        attach_paths({path});
    } else {
        g_app->error = "Could not save the clipboard image as PNG.";
        InvalidateRect(g_app->window, nullptr, FALSE);
    }
    DeleteObject(copy);
}

void remove_attachment(int index) {
    if (index < 0 || index >= static_cast<int>(g_app->attachments.size())) return;
    if (g_app->attachments[static_cast<size_t>(index)].bitmap) {
        DeleteObject(g_app->attachments[static_cast<size_t>(index)].bitmap);
    }
    g_app->attachments.erase(g_app->attachments.begin() + index);
    InvalidateRect(g_app->window, nullptr, FALSE);
}

// ---------------------------------------------------------------- drawing
void draw_button(HDC dc, RECT rect, const std::string& label, int id, bool primary = false,
                 bool active = false) {
    bool hovered = g_app->hover_id == id;
    const bool light = g_palette.light;
    COLORREF fill;
    COLORREF border;
    COLORREF text;
    if (primary) {
        // Primary: filled with the text colour (white in dark themes, black in light).
        fill = kText;
        border = kText;
        text = kBg;
    } else if (active) {
        fill = kText;
        border = kText;
        text = kBg;
    } else if (light) {
        // Light themes: every button is a white fill with a hairline border.
        fill = hovered ? hover_fill() : kCard;
        border = hovered ? kMuted : kBorder;
        text = kText;
    } else {
        fill = hovered ? hover_fill() : kCard;
        border = hovered ? kMuted : kBorder;
        text = kText;
    }
    rounded_rect(dc, rect, fill, border, kRadiusControl);
    draw(dc, label, rect, text, g_app->font_small, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    add_hit(rect, id);
}

// Models emit markdown; render it as clean text (no '#', '**', '`', bullets).
std::string strip_markdown(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    size_t i = 0;
    bool line_start = true;
    while (i < input.size()) {
        char c = input[i];
        if (line_start) {
            // Skip leading '#', '>', and markdown bullet markers.
            size_t j = i;
            while (j < input.size() && input[j] == ' ') ++j;
            bool skipped = false;
            while (j < input.size() && (input[j] == '#' || input[j] == '>')) {
                ++j;
                skipped = true;
            }
            if (j < input.size() && input[j] == ' ') ++j;
            if (j < input.size() && (input[j] == '-' || input[j] == '*' || input[j] == '+') &&
                j + 1 < input.size() && input[j + 1] == ' ') {
                j += 2;
                skipped = true;
                out.append("\u2022 ");
            }
            i = j;
            line_start = false;
            (void)skipped;
            continue;
        }
        // Bold / italic / code markers are dropped.
        if (c == '*' || c == '`') {
            ++i;
            continue;
        }
        if (c == '_' ) {
            // Keep snake_case identifiers, drop _emphasis_.
            bool word_left = i > 0 && (std::isalnum(static_cast<unsigned char>(input[i - 1])));
            bool word_right = i + 1 < input.size() &&
                              std::isalnum(static_cast<unsigned char>(input[i + 1]));
            if (!(word_left && word_right)) {
                ++i;
                continue;
            }
        }
        if (c == '#') {
            ++i;
            continue;
        }
        out.push_back(c);
        if (c == '\n') line_start = true;
        ++i;
    }
    // Collapse 3+ blank lines.
    std::string cleaned;
    int newlines = 0;
    for (char c : out) {
        if (c == '\n') {
            if (++newlines > 2) continue;
        } else {
            newlines = 0;
        }
        cleaned.push_back(c);
    }
    return trim(cleaned);
}

int count_words(const std::string& text) {
    int words = 0;
    bool in_word = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            ++words;
        }
    }
    return words;
}

std::string first_words(const std::string& text, int max_words) {
    if (max_words <= 0) return std::string();
    int words = 0;
    bool in_word = false;
    size_t cut = text.size();
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isspace(c)) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            ++words;
            if (words == max_words + 1) {
                cut = i;
                break;
            }
        }
    }
    std::string out = text.substr(0, cut);
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
    if (cut < text.size()) out += "\u2026";
    return out;
}

// Region ids for text selection. 0 = composer draft; N = transcript[N-1].
constexpr int kSelNone = -1;

struct TextLayout {
    RECT box;          // where the text is drawn
    std::string text;  // displayed text
    int line_height;
};

// Splits text into wrapped lines, returning per-character rects on demand.
std::vector<std::string> wrap_text(HDC dc, const std::string& text, int width, HFONT font) {
    std::vector<std::string> lines;
    std::wstring w = utf8_to_wide(text);
    HGDIOBJ old = SelectObject(dc, font);
    std::string current;
    std::wstring current_w;
    size_t pos = 0;
    while (pos < w.size()) {
        size_t nl = w.find(L'\n', pos);
        std::wstring paragraph = w.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? w.size() : nl + 1;
        if (paragraph.empty()) {
            lines.push_back(std::string());
            continue;
        }
        std::wstring line;
        size_t i = 0;
        while (i < paragraph.size()) {
            // Grow the line until it exceeds the width.
            size_t take = 1;
            while (i + take <= paragraph.size()) {
                std::wstring candidate = line + paragraph.substr(i, take);
                SIZE size{};
                GetTextExtentPoint32W(dc, candidate.c_str(), static_cast<int>(candidate.size()), &size);
                if (size.cx > width) break;
                ++take;
            }
            if (take > 1) take -= 1;
            if (take == 0) take = 1;
            line += paragraph.substr(i, take);
            i += take;
            // Break on the last space when we can.
            if (i < paragraph.size()) {
                size_t space = line.find_last_of(L' ');
                if (space != std::wstring::npos && space > 0 && space + 1 < line.size()) {
                    std::wstring carry = line.substr(space + 1);
                    line = line.substr(0, space);
                    i -= carry.size();
                }
            }
            lines.push_back(wide_to_utf8(line));
            line.clear();
        }
        if (!line.empty()) lines.push_back(wide_to_utf8(line));
    }
    SelectObject(dc, old);
    if (lines.empty()) lines.push_back(std::string());
    return lines;
}

// Maps a point to a character offset in the joined line text.
int offset_at_point(HDC dc, const std::vector<std::string>& lines, const RECT& box,
                    int line_height, HFONT font, POINT point) {
    int line_index = (point.y - box.top) / std::max(1, line_height);
    if (line_index < 0) return 0;
    if (line_index >= static_cast<int>(lines.size())) {
        int total = 0;
        for (const auto& line : lines) total += static_cast<int>(utf8_to_wide(line).size()) + 1;
        return std::max(0, total - 1);
    }
    int offset = 0;
    for (int i = 0; i < line_index; ++i) offset += static_cast<int>(utf8_to_wide(lines[i]).size()) + 1;
    std::wstring w = utf8_to_wide(lines[line_index]);
    HGDIOBJ old = SelectObject(dc, font);
    int best = 0;
    int best_distance = INT_MAX;
    for (size_t i = 0; i <= w.size(); ++i) {
        SIZE size{};
        GetTextExtentPoint32W(dc, w.c_str(), static_cast<int>(i), &size);
        int distance = std::abs(box.left + size.cx - point.x);
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<int>(i);
        }
    }
    SelectObject(dc, old);
    return offset + best;
}

// Paints a selection highlight behind the given character range.
void paint_selection(HDC dc, const std::vector<std::string>& lines, const RECT& box,
                     int line_height, HFONT font, int start, int end) {
    if (end <= start) return;
    HGDIOBJ old = SelectObject(dc, font);
    int offset = 0;
    for (size_t li = 0; li < lines.size(); ++li) {
        std::wstring w = utf8_to_wide(lines[li]);
        int line_start = offset;
        int line_end = offset + static_cast<int>(w.size());
        int from = std::max(start, line_start);
        int to = std::min(end, line_end);
        if (to > from) {
            SIZE left_size{}, right_size{};
            GetTextExtentPoint32W(dc, w.c_str(), from - line_start, &left_size);
            GetTextExtentPoint32W(dc, w.c_str(), to - line_start, &right_size);
            RECT highlight{box.left + left_size.cx, box.top + line_height * static_cast<int>(li),
                           box.left + right_size.cx, box.top + line_height * (static_cast<int>(li) + 1)};
            HBRUSH brush = CreateSolidBrush(selection_fill());
            FillRect(dc, &highlight, brush);
            DeleteObject(brush);
        }
        offset = line_end + 1;
    }
    SelectObject(dc, old);
}

// Maps a UTF-16 character offset to a wrapped line and column.
void line_for_offset(const std::vector<std::string>& lines, int position, int& line, int& column) {
    if (position < 0) position = 0;
    int offset = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const int length = static_cast<int>(utf8_to_wide(lines[i]).size());
        if (position <= offset + length) {
            line = static_cast<int>(i);
            column = position - offset;
            return;
        }
        offset += length + 1;
    }
    line = lines.empty() ? 0 : static_cast<int>(lines.size()) - 1;
    column = lines.empty() ? 0 : static_cast<int>(utf8_to_wide(lines.back()).size());
}

// Slices UTF-8 text using UTF-16 character offsets (the offsets used by the
// painted selection rectangles).
std::string slice_utf16(const std::string& text, int start, int end) {
    std::wstring value = utf8_to_wide(text);
    if (start < 0) start = 0;
    if (end > static_cast<int>(value.size())) end = static_cast<int>(value.size());
    if (end <= start) return std::string();
    return wide_to_utf8(value.substr(static_cast<size_t>(start),
                                     static_cast<size_t>(end - start)));
}

// Copies the selected slice of a message (used by Ctrl+C).
std::string selected_text(const std::string& raw, int start, int end) {
    std::string stripped = strip_markdown(raw);
    if (start < 0) start = 0;
    if (end > static_cast<int>(stripped.size())) end = static_cast<int>(stripped.size());
    if (end <= start) return std::string();
    return stripped.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
}


int prefix_width(HDC dc, const std::string& text, int chars, HFONT font) {
    HGDIOBJ old = SelectObject(dc, font);
    std::wstring w = utf8_to_wide(text);
    if (chars < 0) chars = 0;
    if (chars > static_cast<int>(w.size())) chars = static_cast<int>(w.size());
    SIZE size{};
    GetTextExtentPoint32W(dc, w.c_str(), chars, &size);
    SelectObject(dc, old);
    return size.cx;
}

int composer_offset_from_point(HDC dc, RECT box, POINT point) {
    RECT body{box.left + 16, box.top + 12, box.right - 16, box.bottom - 10};
    if (body.right <= body.left) return 0;
    const std::vector<std::string> lines =
        wrap_text(dc, g_app->input, body.right - body.left, g_app->font_ui);
    return offset_at_point(dc, lines, body, 20, g_app->font_ui, point);
}

int paint_transcript(HDC dc, RECT client) {
    AppState* app = g_app;
    const int left = sidebar_width() + 18;
    const int right = client.right - 26;
    const int width = right - left;
    int y = kHeaderH + 12 - app->scroll;

    for (const auto& message : app->transcript()) {
        if (message.role == Role::User) {
            const std::string full_user = strip_markdown(message.text);
            const bool collapsible = count_words(full_user) > kUserPreviewWords;
            const std::string shown_user =
                (collapsible && !message.expanded) ? first_words(full_user, kUserPreviewWords)
                                                   : full_user;
            const int message_index = static_cast<int>(&message - app->transcript().data());
            const int region_user = message_index + 1;
            const int chip_h = static_cast<int>(message.images.size()) * 20;
            const int toggle_h = collapsible ? 20 : 0;
            const int card_w =
                std::min(width, std::max(220, text_width(dc, shown_user, app->font_ui) + 32));
            const int body_w = std::max(40, card_w - 32);
            const int text_h = measure(dc, shown_user, body_w, app->font_ui);
            const int card_h = text_h + 26 + chip_h + toggle_h;
            RECT card{right - card_w, y, right, y + card_h};
            rounded_rect(dc, card, kCardUser, kBorder, kRadiusCard);
            RECT body{card.left + 16, card.top + 12, card.right - 16,
                      card.bottom - 12 - toggle_h};
            std::vector<std::string> user_lines =
                wrap_text(dc, shown_user, body.right - body.left, app->font_ui);
            if (app->sel_region == region_user && app->sel_end > app->sel_start) {
                paint_selection(dc, user_lines, body, 20, app->font_ui, app->sel_start,
                                app->sel_end);
            }
            draw(dc, shown_user, body, kText, app->font_ui);
            g_app->text_regions.push_back(TextHit{region_user, body, shown_user});

            int chip_y = card.bottom - 12 - toggle_h - chip_h;
            for (const auto& image : message.images) {
                std::string name = image.substr(image.find_last_of("\\/") + 1);
                RECT chip{card.left + 16, chip_y, card.left + 180, chip_y + 17};
                draw(dc, "▣ " + name, chip, kMuted, app->font_tiny,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                chip_y += 20;
            }

            if (collapsible) {
                const int toggle_id = CidUserToggle + message_index;
                RECT toggle{card.right - 38, card.bottom - 28, card.right - 12, card.bottom - 8};
                const bool hovered = app->hover_id == toggle_id;
                if (hovered) {
                    rounded_rect(dc, toggle, hover_fill(), hover_fill(), 6);
                }
                draw(dc, message.expanded ? "▲" : "▼", toggle,
                     hovered ? kText : kMuted, app->font_tiny,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (message_index < kUserToggleLimit) add_hit(toggle, toggle_id);
            }
            y = card.bottom + 14;
        } else {
            const std::string shown = strip_markdown(message.text);
            std::vector<std::string> lines =
                wrap_text(dc, shown, width - 36, app->font_ui);
            const int line_h = 20;
            int text_h = static_cast<int>(lines.size()) * line_h;
            int card_h = text_h + 46;
            RECT card{left, y, right, y + card_h};
            rounded_rect(dc, card, kCard, kBorder, kRadiusCard);
            RECT label{card.left + 18, card.top + 12, card.right, card.top + 28};
            draw(dc, "CLAM BUILD", label, kAccent, app->font_tiny, DT_LEFT | DT_SINGLELINE);
            RECT body{card.left + 18, card.top + 32, card.right - 18, card.bottom - 12};
            int region = static_cast<int>(&message - app->transcript().data()) + 1;
            if (app->sel_region == region && app->sel_end > app->sel_start) {
                paint_selection(dc, lines, body, line_h, app->font_ui, app->sel_start, app->sel_end);
            }
            draw(dc, shown, body, kText, app->font_ui);
            g_app->text_regions.push_back(TextHit{region, body, shown});
            y = card.bottom + 14;
        }
    }

    if (app->busy) {
        std::string label = app->activity_status.empty() ? "working\u2026" : app->activity_status;
        RECT pill{left, y, left + 16 + text_width(dc, label, app->font_small) + 34, y + 30};
        rounded_rect(dc, pill, kPanel, kBorder, 15);
        // animated dots
        int phase = static_cast<int>((GetTickCount64() / 300) % 3);
        std::string dots(static_cast<size_t>(phase + 1), '.');
        RECT dot_rect{pill.left + 12, pill.top + 7, pill.left + 22, pill.bottom};
        draw(dc, dots, dot_rect, kAccent, app->font_small, DT_LEFT | DT_SINGLELINE);
        RECT text{pill.left + 30, pill.top + 7, pill.right - 10, pill.bottom};
        draw(dc, label, text, kMuted, app->font_small, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += 40;
    }

    if (!app->error.empty()) {
        int h = measure(dc, app->error, width - 36, app->font_ui) + 24;
        RECT card{left, y, right, y + h};
        rounded_rect(dc, card, danger_fill(), kDanger, kRadiusCard);
        RECT body{card.left + 18, card.top + 12, card.right - 18, card.bottom - 10};
        draw(dc, app->error, body, kDanger, app->font_ui);
        y = card.bottom + 10;
    }

    if (app->transcript().empty() && !app->busy) {
        int center = (client.bottom + kHeaderH) / 2 - 60;
        RECT title{left, center - 46, right, center - 8};
        draw(dc, "What should we build?", title, kText, app->font_title,
             DT_CENTER | DT_SINGLELINE);
        RECT sub{left, center - 2, right, center + 22};
        draw(dc, "Working in " + app->root, sub, kMuted, app->font_small,
             DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    app->content_height = y + app->scroll;
    return y;
}

void paint_activity(HDC dc, RECT client) {
    AppState* app = g_app;
    const int left = sidebar_width() + 18;
    const int right = client.right - 26;
    int y = kHeaderH + 12 - app->scroll;

    RECT heading{left, y, left + 140, y + 28};
    draw(dc, "Activity", heading, kText, app->font_heading, DT_LEFT | DT_SINGLELINE);
    RECT hint{left + 104, y + 6, right, y + 28};
    draw(dc, "collapsed \u2014 click to expand", hint, kMuted, app->font_tiny,
         DT_LEFT | DT_SINGLELINE);
    y += 36;

    if (!app->thinking.empty()) {
        std::vector<std::string> lines =
            wrap_text(dc, strip_markdown(app->thinking), right - left - 28, app->font_small);
        int h = static_cast<int>(lines.size()) * 18 + 30;
        RECT card{left, y, right, y + h};
        rounded_rect(dc, card, kCard, kBorder, kRadiusCard);
        draw(dc, "THINKING", RECT{card.left + 14, card.top + 8, card.right, card.top + 22}, kMuted,
             app->font_tiny, DT_LEFT | DT_SINGLELINE);
        draw(dc, strip_markdown(app->thinking),
             RECT{card.left + 14, card.top + 24, card.right - 14, card.bottom - 8}, kMuted,
             app->font_small);
        y = card.bottom + 10;
    }

    if (app->activity().empty()) {
        RECT empty{left, y + 8, right, y + 34};
        draw(dc, "Nothing has run yet.", empty, kMuted, app->font_small,
             DT_LEFT | DT_SINGLELINE);
        app->content_height = kHeaderH + 120;
        return;
    }

    for (size_t i = 0; i < app->activity().size(); ++i) {
        auto& item = app->activity()[i];
        RECT header{left, y, right, y + 32};
        bool hovered = g_app->hover_id == static_cast<int>(CidActivityRow) + static_cast<int>(i);
        rounded_rect(dc, header, hovered ? hover_fill() : kCard,
                     item.expanded ? kAccent : kBorder, kRadiusControl);
        RECT label{header.left + 14, header.top + 7, header.right - 14, header.bottom};
        std::string text = std::string(item.expanded ? "\u25BE  " : "\u25B8  ") + item.label + "  \u00B7  " +
                           item.summary;
        draw(dc, text, label, item.expanded ? kText : kMuted, app->font_small,
             DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        add_hit(header, static_cast<int>(CidActivityRow) + static_cast<int>(i));
        y += 36;
        if (item.expanded) {
            int h = measure(dc, item.detail, right - left - 40, app->font_mono) + 22;
            RECT body{left + 12, y, right - 12, y + h};
            rounded_rect(dc, body, code_fill(), kBorder, kRadiusControl);
            RECT inner{body.left + 14, body.top + 11, body.right - 14, body.bottom - 10};
            draw(dc, item.detail, inner, kText, app->font_mono);
            y += h + 6;
        }
        y += 6;
    }
    app->content_height = y + app->scroll;
}

// Shared Settings layout so the painted cards and the real EDIT controls agree,
// including while the page is scrolled.
struct SettingsLayout {
    int left = 0;
    int width = 0;
    RECT title{};
    RECT error_bar{};
    RECT key_card{}, key_field{}, key_button{}, key_hint{};
    RECT project_card{}, project_field{}, project_browse{}, project_open{};
    RECT model_card{}, model_field{}, model_refresh{}, model_hint{};
    RECT effort_card{};
    RECT theme_card{};
    RECT perm_card{}, perm_chip{};
    RECT plugin_card{};
    int content_height = 0;
};

SettingsLayout settings_layout(RECT client, int scroll) {
    SettingsLayout g;
    const int side = g_app && g_app->sidebar_open ? kSidebarW : 0;
    g.left = side + 18;
    const int available = static_cast<int>(client.right) - side - 44;
    g.width = std::max(320, std::min(720, available));
    const int right = g.left + g.width;
    int y = kHeaderH + 14 - scroll;

    g.title = RECT{g.left, y, right, y + 28};
    y += 34;
    if (g_app && !g_app->error.empty()) {
        g.error_bar = RECT{g.left, y, right, y + 34};
        y += 42;
    }

    g.key_card = RECT{g.left, y, right, y + 96};
    g.key_field = RECT{g.key_card.left + 16, g.key_card.top + 34, g.key_card.right - 120,
                       g.key_card.top + 68};
    g.key_button = RECT{g.key_card.right - 108, g.key_card.top + 34, g.key_card.right - 16,
                        g.key_card.top + 68};
    g.key_hint = RECT{g.key_card.left + 16, g.key_card.bottom - 24, g.key_card.right - 16,
                      g.key_card.bottom - 6};
    y = g.key_card.bottom + 12;

    g.project_card = RECT{g.left, y, right, y + 92};
    g.project_field = RECT{g.project_card.left + 16, g.project_card.top + 34,
                           g.project_card.right - 220, g.project_card.top + 68};
    g.project_browse = RECT{g.project_card.right - 208, g.project_card.top + 34,
                            g.project_card.right - 116, g.project_card.top + 68};
    g.project_open = RECT{g.project_card.right - 108, g.project_card.top + 34,
                          g.project_card.right - 16, g.project_card.top + 68};
    y = g.project_card.bottom + 12;

    g.model_card = RECT{g.left, y, right, y + 96};
    g.model_field = RECT{g.model_card.left + 16, g.model_card.top + 32,
                         g.model_card.right - 116, g.model_card.top + 62};
    g.model_refresh = RECT{g.model_card.right - 104, g.model_card.top + 32,
                           g.model_card.right - 16, g.model_card.top + 62};
    g.model_hint = RECT{g.model_card.left + 16, g.model_card.top + 68, g.model_card.right - 16,
                        g.model_card.top + 86};
    y = g.model_card.bottom + 12;

    g.effort_card = RECT{g.left, y, right, y + 76};
    y = g.effort_card.bottom + 12;

    g.theme_card = RECT{g.left, y, right, y + kThemeCardH};
    y = g.theme_card.bottom + 12;

    g.perm_card = RECT{g.left, y, right, y + 76};
    g.perm_chip = RECT{g.perm_card.left + 16, g.perm_card.top + 32, g.perm_card.left + 140,
                       g.perm_card.top + 62};
    y = g.perm_card.bottom + 12;

    g.plugin_card = RECT{g.left, y, right, y + 60};
    g.content_height = g.plugin_card.bottom + scroll + 16;
    return g;
}

void paint_settings(HDC dc, RECT client) {
    AppState* app = g_app;
    const SettingsLayout layout = settings_layout(client, app->scroll);

    draw(dc, "Settings", layout.title, kText, app->font_heading, DT_LEFT | DT_SINGLELINE);
    if (!app->error.empty() && layout.error_bar.bottom > layout.error_bar.top) {
        rounded_rect(dc, layout.error_bar, danger_fill(), kDanger, kRadiusControl);
        draw(dc, app->error,
             RECT{layout.error_bar.left + 14, layout.error_bar.top + 8,
                  layout.error_bar.right - 14, layout.error_bar.bottom - 6},
             kDanger, app->font_small, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // API key
    const bool key_focus = GetFocus() == app->key_field_edit;
    const bool key_has_text = app->key_field_edit && GetWindowTextLengthW(app->key_field_edit) > 0;
    const bool key_editing = app->key_editing || key_has_text;
    rounded_rect(dc, layout.key_card, kCard, kBorder, kRadiusCard);
    draw(dc, "NVIDIA API KEY",
         RECT{layout.key_card.left + 16, layout.key_card.top + 10, layout.key_card.right,
              layout.key_card.top + 26},
         kMuted, app->font_tiny, DT_LEFT | DT_SINGLELINE);
    // The real EDIT control paints itself inside this rounded frame.
    rounded_rect(dc, layout.key_field, kInputBg, key_focus ? kAccent : kBorder, kRadiusControl);
    draw_button(dc, layout.key_button, key_editing ? "Save" : "Change", CidKeySave, key_editing);
    draw(dc, "Stored with Windows DPAPI in %APPDATA%\\clam", layout.key_hint, kMuted,
         app->font_tiny, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Project
    const bool project_focus = GetFocus() == app->project_edit;
    rounded_rect(dc, layout.project_card, kCard, kBorder, kRadiusCard);
    draw(dc, "PROJECT FOLDER",
         RECT{layout.project_card.left + 16, layout.project_card.top + 10,
              layout.project_card.right, layout.project_card.top + 26},
         kMuted, app->font_tiny, DT_LEFT | DT_SINGLELINE);
    rounded_rect(dc, layout.project_field, kInputBg, project_focus ? kAccent : kBorder,
                 kRadiusControl);
    draw_button(dc, layout.project_browse, "Browse", CidProjectBrowse);
    draw_button(dc, layout.project_open, "Open", CidProjectOpen, true);

    // Model
    rounded_rect(dc, layout.model_card, kCard, kBorder, kRadiusCard);
    draw(dc, "MODEL",
         RECT{layout.model_card.left + 16, layout.model_card.top + 10, layout.model_card.right,
              layout.model_card.top + 26},
         kMuted, app->font_tiny, DT_LEFT | DT_SINGLELINE);
    rounded_rect(dc, layout.model_field, kInputBg,
                 app->hover_id == CidModelPick ? kMuted : kBorder, kRadiusControl);
    const std::string model_label =
        app->settings.model + (model_supports_vision(app->settings.model) ? "   \u25C9 vision" : "");
    draw(dc, model_label,
         RECT{layout.model_field.left + 10, layout.model_field.top + 6,
              layout.model_field.right - 24, layout.model_field.bottom},
         kText, app->font_small, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw(dc, app->dropdown_open ? "\u25B2" : "\u25BC",
         RECT{layout.model_field.right - 22, layout.model_field.top + 6,
              layout.model_field.right - 6, layout.model_field.bottom},
         kMuted, app->font_small, DT_LEFT | DT_SINGLELINE);
    add_hit(layout.model_field, CidModelPick);
    draw_button(dc, layout.model_refresh, app->models_loading ? "Loading" : "Refresh", CidRefresh);
    draw(dc, std::to_string(app->models.size()) + " code-capable models", layout.model_hint, kMuted,
         app->font_tiny, DT_LEFT | DT_SINGLELINE);

    // Effort
    rounded_rect(dc, layout.effort_card, kCard, kBorder, kRadiusCard);
    draw(dc, "REASONING EFFORT",
         RECT{layout.effort_card.left + 16, layout.effort_card.top + 10, layout.effort_card.right,
              layout.effort_card.top + 26},
         kMuted, app->font_tiny, DT_LEFT | DT_SINGLELINE);
    int chip_x = layout.effort_card.left + 16;
    for (const auto& level : kEffortLevels) {
        const int w = 12 + text_width(dc, level, app->font_small) + 12;
        RECT chip{chip_x, layout.effort_card.top + 32, chip_x + std::max(70, w),
                  layout.effort_card.top + 62};
        const bool selected = app->settings.effort == level;
        rounded_rect(dc, chip, selected ? kText : kInputBg, selected ? kText : kBorder,
                     kRadiusControl);
        draw(dc, level, chip, selected ? kBg : kMuted, app->font_small,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        add_hit(chip, CidEffort);
        chip_x = chip.right + 8;
    }
    const std::string budget = std::to_string(effort_tokens(app->settings.effort)) + " tokens";
    draw(dc, budget,
         RECT{layout.effort_card.right - 120, layout.effort_card.top + 38,
              layout.effort_card.right - 16, layout.effort_card.top + 58},
         kMuted, app->font_tiny, DT_RIGHT | DT_SINGLELINE);

    // Theme picker with live thumbnails.
    rounded_rect(dc, layout.theme_card, kCard, kBorder, kRadiusCard);
    draw(dc, "THEME",
         RECT{layout.theme_card.left + 16, layout.theme_card.top + 10, layout.theme_card.right,
              layout.theme_card.top + 26},
         kMuted, app->font_tiny, DT_LEFT | DT_SINGLELINE);
    for (int i = 0; i < kThemeCount; ++i) {
        const int row = i / kThemesPerRow;
        const int column = i % kThemesPerRow;
        const int tx = layout.theme_card.left + 16 + column * (kThemeThumbW + 10);
        const int ty = layout.theme_card.top + 30 + row * kThemeRowPitch;
        RECT thumb{tx, ty, tx + kThemeThumbW, ty + kThemeThumbH};
        const Palette pv = palette_for(kThemeOptions[i].theme);
        const bool selected = app->theme == kThemeOptions[i].theme;
        if (pv.gradient) {
            rounded_rect(dc, thumb, pv.bg, pv.border, 9);
            const int saved = clip_round_rect(dc, thumb, 9);
            paint_gradient(dc, thumb, pv.grad_top, pv.grad_bottom);
            RestoreDC(dc, saved);
            rounded_frame(dc, thumb, selected ? kAccent : pv.border, 9);
        } else {
            rounded_rect(dc, thumb, pv.bg, selected ? kAccent : pv.border, 9);
        }

        // Mini chat bubble preview.
        RECT bubble{thumb.left + 8, thumb.top + 8, thumb.right - 8, thumb.top + 28};
        rounded_rect(dc, bubble, pv.card_user, pv.border, 5);
        rounded_rect(dc, RECT{bubble.left + 6, bubble.top + 6, bubble.right - 18,
                              bubble.top + 9},
                     pv.text, pv.text, 1);
        rounded_rect(dc, RECT{thumb.left + 8, thumb.top + 34, thumb.right - 8,
                              thumb.top + 38},
                     pv.accent, pv.accent, 2);

        draw(dc, kThemeOptions[i].label,
             RECT{thumb.left - 4, thumb.bottom + 2, thumb.right + 4, thumb.bottom + 16},
             selected ? kText : kMuted, app->font_tiny,
             DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        add_hit(thumb, CidTheme + i);
    }

    // Permissions
    rounded_rect(dc, layout.perm_card, kCard, kBorder, kRadiusCard);
    draw(dc, "PERMISSIONS",
         RECT{layout.perm_card.left + 16, layout.perm_card.top + 10, layout.perm_card.right,
              layout.perm_card.top + 26},
         kMuted, app->font_tiny, DT_LEFT | DT_SINGLELINE);
    rounded_rect(dc, layout.perm_chip, app->settings.allow_all ? kText : kInputBg,
                 app->settings.allow_all ? kText : kBorder, kRadiusControl);
    draw(dc, app->settings.allow_all ? "\u25CF Allow all" : "\u25CB Manual", layout.perm_chip,
         app->settings.allow_all ? kBg : kMuted, app->font_small,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    add_hit(layout.perm_chip, CidPerm);
    draw(dc, app->settings.allow_all ? "Commands and overwrites run without asking"
                                     : "Clam asks before commands and overwrites",
         RECT{layout.perm_card.left + 152, layout.perm_card.top + 38,
              layout.perm_card.right - 16, layout.perm_card.top + 58},
         kMuted, app->font_tiny, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Plugins
    const auto plugins = load_plugins();
    RECT plugin_card = layout.plugin_card;
    plugin_card.bottom = plugin_card.top + 60 + static_cast<int>(plugins.size()) * 20;
    rounded_rect(dc, plugin_card, kCard, kBorder, kRadiusCard);
    draw(dc, "PLUGINS",
         RECT{plugin_card.left + 16, plugin_card.top + 10, plugin_card.right, plugin_card.top + 26},
         kMuted, app->font_tiny, DT_LEFT | DT_SINGLELINE);
    if (plugins.empty()) {
        draw(dc, "No plugins yet. Place .luau files in " + plugins_dir(),
             RECT{plugin_card.left + 16, plugin_card.top + 32, plugin_card.right - 16,
                  plugin_card.top + 52},
             kMuted, app->font_tiny, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        int py = plugin_card.top + 32;
        for (const auto& plugin : plugins) {
            draw(dc, "\u2022 " + plugin.name + " \u2014 " + plugin.description,
                 RECT{plugin_card.left + 16, py, plugin_card.right - 16, py + 18}, kText,
                 app->font_tiny, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            py += 20;
        }
    }
    app->content_height = plugin_card.bottom + app->scroll + 16;
}


// Left rail: one tab per conversation, each remembering its own project.
// Compact icon rail: one square per chat, with project initials beneath.
int paint_sidebar(HDC dc, RECT client) {
    AppState* app = g_app;
    app->sidebar_tooltip_visible = false;
    if (!app->sidebar_open) {
        // A slim grip remains at the edge so the rail is discoverable. Moving
        // the pointer into this strip brings the rail back.
        const int cy = (kHeaderH + client.bottom) / 2;
        RECT grip{3, cy - 21, 7, cy + 21};
        rounded_rect(dc, grip, blend_color(kBg, kText, 22), kBorder, 2);
        add_hit(RECT{0, kHeaderH, kSidebarRevealW, client.bottom}, CidSidebarReveal);
        return 0;
    }

    RECT rail{0, kHeaderH, kSidebarW, client.bottom};
    fill_rect_solid(dc, rail, kPanel);
    fill_rect_solid(dc, RECT{rail.right - 1, rail.top, rail.right, rail.bottom}, kBorder);

    // New chat button (plus glyph).
    RECT add{10, rail.top + 12, rail.right - 10, rail.top + 48};
    bool add_hover = app->hover_id == CidNewChatTab;
    rounded_rect(dc, add, add_hover ? hover_fill() : kCard, kBorder, 12);
    draw(dc, "+", add, kText, app->font_heading, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    add_hit(add, CidNewChatTab);

    const int list_top = rail.top + 58;
    const int list_bottom = rail.bottom - 8;
    const int pitch = 58;  // 40 px tile + 18 px project tag
    const int total = static_cast<int>(app->sessions.size()) * pitch;
    const int view = std::max(1, list_bottom - list_top);
    const int max_scroll = std::max(0, total - view);
    if (app->sidebar_scroll_to_active && !app->sessions.empty()) {
        const int active_top = app->active_session * pitch;
        app->sidebar_scroll =
            std::max(0, std::min(max_scroll, active_top - (view - pitch)));
        app->sidebar_scroll_to_active = false;
    }
    app->sidebar_scroll = std::max(0, std::min(max_scroll, app->sidebar_scroll));

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, 0, list_top, rail.right, list_bottom);
    int y = list_top - app->sidebar_scroll;
    for (size_t i = 0; i < app->sessions.size(); ++i) {
        const auto& session = app->sessions[i];
        const int size = 40;
        if (y + size + 18 < list_top) {
            y += pitch;
            continue;
        }
        if (y > list_bottom) break;

        const int cx = (rail.right - size) / 2;
        RECT tile{cx, y, cx + size, y + size};
        const bool active = static_cast<int>(i) == app->active_session;
        const int chat_id = static_cast<int>(CidChatTab) + static_cast<int>(i);
        const bool hovered = app->hover_id == chat_id;
        rounded_rect(dc, tile, active ? kCard : (hovered ? hover_fill() : kPanel),
                     active ? kAccent : kBorder, 12);

        // Glyph: first letter of the title (falls back to a dot).
        std::string glyph = "•";
        for (char c : session.title) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                glyph = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                break;
            }
        }
        draw(dc, glyph, tile, active ? kAccent : kMuted, app->font_heading,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT hit_rect{tile.left, std::max(static_cast<int>(tile.top), list_top), tile.right,
                      std::min(static_cast<int>(tile.bottom), list_bottom)};
        if (hit_rect.bottom > hit_rect.top) add_hit(hit_rect, chat_id);

        // Short title under the tile so older chats are distinguishable; the
        // hover card shows the full title, project, and age.
        std::string tag = session.title.empty() ? project_label(session.project) : session.title;
        if (tag.size() > 6) tag = tag.substr(0, 6);
        draw(dc, tag, RECT{2, y + size + 1, rail.right - 2, y + size + 14}, kMuted,
             app->font_tiny, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        if (hovered) {
            const int tw = 286;
            const int th = 62;
            const int tx = rail.right + 10;
            const int min_y = kHeaderH + 8;
            int max_y = client.bottom - (app->tab == Tab::Chat ? kFooterH : 0) - th - 10;
            if (max_y < min_y) max_y = min_y;
            const int ty =
                std::max(min_y, std::min(static_cast<int>(tile.top) - 6, max_y));
            app->sidebar_tooltip = RECT{tx, ty, tx + tw, ty + th};
            app->sidebar_tooltip_visible = true;
        }
        y += pitch;
    }
    RestoreDC(dc, saved);

    if (max_scroll > 0) {
        const int track_top = list_top + 4;
        const int track_h = std::max(1, view - 8);
        const int thumb_h = std::max(22, track_h * view / std::max(1, total));
        const int thumb_y =
            track_top + (track_h - thumb_h) * app->sidebar_scroll / max_scroll;
        RECT thumb{rail.right - 4, thumb_y, rail.right - 1, thumb_y + thumb_h};
        rounded_rect(dc, thumb, blend_color(kPanel, kText, 25),
                     blend_color(kPanel, kText, 25), 2);
    }
    return rail.right;
}

void paint_sidebar_tooltip(HDC dc, RECT client) {
    AppState* app = g_app;
    if (!app->sidebar_tooltip_visible) return;
    const int index = app->hover_id - static_cast<int>(CidChatTab);
    if (index < 0 || index >= static_cast<int>(app->sessions.size())) return;
    const ChatSession& session = app->sessions[static_cast<size_t>(index)];

    RECT box = app->sidebar_tooltip;
    RECT shadow{box.left + 2, box.top + 3, box.right + 2, box.bottom + 3};
    rounded_rect(dc, shadow, blend_color(kBg, kText, 5), blend_color(kBg, kText, 5),
                 kRadiusCard);
    rounded_rect(dc, box, kCard, kBorder, kRadiusCard);

    draw(dc, session.title,
         RECT{box.left + 14, box.top + 9, box.right - 14, box.top + 31}, kText,
         app->font_ui, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    std::string meta = project_label(session.project);
    const std::string when = relative_time(session.updated_at);
    if (!when.empty()) meta += "   ·   " + when;
    draw(dc, meta, RECT{box.left + 14, box.top + 35, box.right - 14, box.bottom - 8}, kMuted,
         app->font_tiny, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    (void)client;
}

void paint_header(HDC dc, RECT client) {
    AppState* app = g_app;
    RECT header{0, 0, client.right, kHeaderH};
    if (g_palette.gradient) {
        paint_gradient(dc, header, gradient_color_at(0, 0, client.bottom),
                       gradient_color_at(kHeaderH, 0, client.bottom));
    } else {
        fill_rect_solid(dc, header, kBg);
    }
    fill_rect_solid(dc, RECT{0, kHeaderH - 1, client.right, kHeaderH}, kBorder);

    // Logo: the same diamond outline used for the application icon.
    {
        const int size = 28;
        const int cx = 26 + size / 2;
        const int cy = kHeaderH / 2;
        const int radius = size / 2 - 2;
        HPEN pen = CreatePen(PS_SOLID, 2, kText);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        POINT diamond[4] = {{cx, cy - radius}, {cx + radius, cy}, {cx, cy + radius},
                            {cx - radius, cy}};
        Polygon(dc, diamond, 4);
        HBRUSH dot = CreateSolidBrush(kAccent);
        HGDIOBJ old_dot = SelectObject(dc, dot);
        const int dot_r = 3;
        Ellipse(dc, cx - dot_r, cy - dot_r, cx + dot_r + 1, cy + dot_r + 1);
        SelectObject(dc, old_dot);
        DeleteObject(dot);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }
    const int word_left = 26 + 28 + 12;
    draw(dc, "Clam Build", RECT{word_left, 0, word_left + 200, kHeaderH}, kText, app->font_brand,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Rounded pill tabs keep the header compact and let the active tab read
    // clearly without a heavy underline.
    int x = word_left + text_width(dc, "Clam Build", app->font_brand) + 32;
    auto tab = [&](const std::string& label, int id, bool selected) {
        int w = text_width(dc, label, app->font_small) + 28;
        RECT area{x, 10, x + w, 40};
        if (selected) {
            rounded_rect(dc, area, kCard, kBorder, 10);
        } else if (app->hover_id == id) {
            rounded_rect(dc, area, hover_fill(), hover_fill(), 10);
        }
        draw(dc, label, area, selected ? kAccent : kMuted, app->font_small,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        add_hit(area, id);
        x += w + 4;
    };
    tab("Chat", CidChat, app->tab == Tab::Chat);
    tab("Activity", CidActivity, app->tab == Tab::Activity);
    tab("Settings", CidSettings, app->tab == Tab::Settings);

    // Right cluster: quiet text buttons. The model label shrinks and, if the
    // window is very narrow, the redundant New chat chip drops out so the
    // cluster never collides with the tabs.
    const int tabs_end = x - 4;
    int right = client.right - 26;
    std::string model = app->settings.model;
    const size_t slash = model.find('/');
    if (slash != std::string::npos) model = model.substr(slash + 1);

    struct QuietItem {
        std::string label;
        int id;
        COLORREF color;
    };
    std::vector<QuietItem> items;
    auto width_of = [&](const std::string& label) {
        return text_width(dc, label, app->font_tiny) + 18;
    };
    auto total_width = [&]() {
        int width = 0;
        for (const auto& item : items) width += width_of(item.label);
        if (!items.empty()) width += 6 * (static_cast<int>(items.size()) - 1);
        return width;
    };
    int model_chars = static_cast<int>(model.size());
    bool include_new_chat = true;
    bool include_effort = true;
    const int available = right - (tabs_end + 12);
    auto rebuild = [&] {
        items.clear();
        if (include_new_chat) items.push_back(QuietItem{"New chat", CidNewChat, kMuted});
        items.push_back(QuietItem{app->settings.allow_all ? "Allow all" : "Manual", CidPerm,
                                  app->settings.allow_all ? kAccent : kWarn});
        if (include_effort) items.push_back(QuietItem{app->settings.effort, CidEffort, kMuted});
        std::string shown = model;
        if (model_chars < static_cast<int>(shown.size())) {
            shown = shown.substr(0, static_cast<size_t>(std::max(0, model_chars - 1))) + "…";
        }
        items.push_back(QuietItem{shown + " ▾", CidModel, kText});
    };
    rebuild();
    while (model_chars > 8 && total_width() > available) {
        --model_chars;
        rebuild();
    }
    if (total_width() > available) {
        include_new_chat = false;
        rebuild();
    }
    if (total_width() > available) {
        include_effort = false;
        rebuild();
    }
    while (model_chars > 3 && total_width() > available) {
        --model_chars;
        rebuild();
    }

    auto quiet = [&](const QuietItem& item) {
        const int w = width_of(item.label);
        RECT area{right - w, 10, right, 40};
        if (app->hover_id == item.id) {
            rounded_rect(dc, area, hover_fill(), hover_fill(), 10);
        }
        draw(dc, item.label, area, item.color, app->font_tiny,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        add_hit(area, item.id);
        right = area.left - 6;
    };
    for (const auto& item : items) quiet(item);
}

void paint_composer(HDC dc, RECT client) {
    AppState* app = g_app;
    // Keep the rail intact: start the composer band to the right of it.
    const int side = sidebar_width();
    RECT footer{side, client.bottom - kFooterH, client.right, client.bottom};
    if (g_palette.gradient) {
        paint_gradient(dc, footer, gradient_color_at(footer.top, 0, client.bottom),
                       gradient_color_at(footer.bottom, 0, client.bottom));
    } else {
        fill_rect_solid(dc, footer, kBg);
    }

    // First run: float the composer in the middle of the window until a
    // conversation starts, then dock it to the bottom.
    const bool hero = app->transcript().empty() && !app->busy;
    const int content_left = side + 18;
    const int content_right = client.right - 22;
    RECT box;
    RECT send_btn;
    if (hero) {
        const int send_w = 74;
        const int width = std::min(760, content_right - content_left - send_w - 24);
        const int group = width + 10 + send_w;
        const int group_left = content_left + ((content_right - content_left) - group) / 2;
        const int cy = (client.bottom + kHeaderH) / 2 + 10;
        box = RECT{group_left, cy, group_left + width, cy + 84};
        send_btn = RECT{box.right + 10, box.top, box.right + 10 + send_w, box.bottom};
    } else {
        box = RECT{content_left, footer.top + 12, client.right - 118, footer.top + 12 + 66};
        send_btn = RECT{client.right - 100, box.top, client.right - 22, box.bottom};
    }
    rounded_rect(dc, box, kInputBg, kBorder, kRadiusLarge);

    const int text_left = box.left + 16;
    const int text_top = box.top + 12;
    const int line_h = 20;
    RECT body{text_left, text_top, box.right - 16, box.bottom - 10};
    const int saved_text = SaveDC(dc);
    IntersectClipRect(dc, body.left, body.top, body.right, body.bottom);
    if (app->input.empty()) {
        draw(dc, "Ask Clam to build something…",
             RECT{text_left, text_top, body.right, text_top + line_h}, kMuted, app->font_ui,
             DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        const std::vector<std::string> lines =
            wrap_text(dc, app->input, body.right - body.left, app->font_ui);
        if (app->sel_region == 0 && app->sel_end > app->sel_start) {
            paint_selection(dc, lines, body, line_h, app->font_ui, app->sel_start, app->sel_end);
        }
        for (size_t i = 0; i < lines.size(); ++i) {
            RECT line_rect{body.left, body.top + static_cast<int>(i) * line_h, body.right,
                           body.top + (static_cast<int>(i) + 1) * line_h};
            draw(dc, lines[i], line_rect, kText, app->font_ui,
                 DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }
    // Blinking caret at the wrapped position (only when this tab has focus).
    if (app->caret_on && app->tab == Tab::Chat) {
        std::vector<std::string> lines;
        if (!app->input.empty()) {
            lines = wrap_text(dc, app->input, body.right - body.left, app->font_ui);
        }
        int caret_line = 0;
        int caret_column = 0;
        line_for_offset(lines, app->caret_pos, caret_line, caret_column);
        const std::string caret_text =
            lines.empty() ? std::string() : lines[static_cast<size_t>(caret_line)];
        const int caret_x = text_left + prefix_width(dc, caret_text, caret_column, app->font_ui);
        const int caret_y = text_top + caret_line * line_h;
        HBRUSH caret_brush = CreateSolidBrush(kAccent);
        RECT caret{caret_x, caret_y, caret_x + 2, caret_y + 18};
        FillRect(dc, &caret, caret_brush);
        DeleteObject(caret_brush);
    }
    RestoreDC(dc, saved_text);
    // Region 0 = the composer draft. Clicking anywhere in the box positions the caret.
    g_app->text_regions.push_back(TextHit{0, body, app->input});
    g_app->composer_box = box;

    const bool can_send = !app->busy && (!trim(app->input).empty() || !app->attachments.empty());
    const bool send_hover = app->hover_id == CidSend;
    const COLORREF send_fill =
        can_send ? (send_hover ? blend_color(kAccent, RGB(255, 255, 255), 12) : kAccent)
                 : kCard;
    rounded_rect(dc, send_btn, send_fill, can_send ? kAccent : kBorder, kRadiusLarge);
    draw(dc, app->busy ? "…" : "↑", send_btn,
         can_send ? accent_text_color() : kMuted, app->font_ui,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    add_hit(send_btn, CidSend);

    // Attachment thumbnails: click one to remove it.
    int chip_x = hero ? box.left : content_left;
    const int accessory_row = hero ? box.bottom + 8 : footer.top + 84;
    for (size_t i = 0; i < app->attachments.size(); ++i) {
        const auto& attachment = app->attachments[i];
        RECT chip{chip_x, accessory_row, chip_x + 52, accessory_row + 34};
        bool hovered = g_app->hover_id == static_cast<int>(CidAttachmentChip) + static_cast<int>(i);
        rounded_rect(dc, chip, hovered ? hover_fill() : kCard, hovered ? kDanger : kBorder,
                     kRadiusControl);
        if (attachment.bitmap && !hovered) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old_bitmap = SelectObject(mem, attachment.bitmap);
            SetStretchBltMode(dc, HALFTONE);
            StretchBlt(dc, chip.left + 3, chip.top + 3, 46, 28, mem, 0, 0, attachment.width,
                       attachment.height, SRCCOPY);
            SelectObject(mem, old_bitmap);
            DeleteDC(mem);
        } else {
            draw(dc, "remove", chip, kDanger, app->font_tiny,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        add_hit(chip, static_cast<int>(CidAttachmentChip) + static_cast<int>(i));
        chip_x = chip.right + 6;
    }

    // Quiet action text, on the same row as attachment chips.
    int action_x = chip_x + (app->attachments.empty() ? 0 : 12);
    auto action = [&](const std::string& label, int id) {
        int w = text_width(dc, label, app->font_tiny) + 20;
        RECT area{action_x, accessory_row + 3, action_x + w, accessory_row + 31};
        bool hovered = g_app->hover_id == id;
        if (hovered) rounded_rect(dc, area, hover_fill(), hover_fill(), kRadiusControl);
        draw(dc, label, area, kMuted, app->font_tiny, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        add_hit(area, id);
    };
    action("Attach", CidAttach);
}

// Draws the model chooser as an overlay so it works from any tab and always
// sits above other hit regions.
void paint_model_popup(HDC dc, RECT client) {
    AppState* app = g_app;
    if (!app->dropdown_open) return;

    const int row_h = 26;
    const int max_rows = 12;
    const int count = static_cast<int>(app->models.size());
    if (count == 0) {
        RECT box{client.right / 2 - 180, 90, client.right / 2 + 180, 150};
        rounded_rect(dc, box, kCard, kBorder, kRadiusCard);
        draw(dc, app->models_loading ? "Loading models\u2026" : "No models loaded. Press Refresh.",
             RECT{box.left + 16, box.top + 20, box.right - 16, box.bottom}, kMuted, app->font_small,
             DT_LEFT | DT_SINGLELINE);
        add_hit(box, CidNone);
        return;
    }

    const int visible = std::min(count, max_rows);
    const int width = std::min(560, static_cast<int>(client.right) - 80);
    const int left = (client.right - width) / 2;
    const int top = 74;
    const int height = visible * row_h + 74;
    RECT box{left, top, left + width, top + height};

    // Dim backdrop for focus.
    HDC screen = GetDC(nullptr);
    HDC dim = CreateCompatibleDC(screen);
    HBITMAP dim_bmp = CreateCompatibleBitmap(screen, client.right, client.bottom);
    HGDIOBJ dim_old = SelectObject(dim, dim_bmp);
    BitBlt(dim, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 110, 0};
    AlphaBlend(dc, 0, 0, client.right, client.bottom, dim, 0, 0, client.right, client.bottom, blend);
    SelectObject(dim, dim_old);
    DeleteObject(dim_bmp);
    DeleteDC(dim);
    ReleaseDC(nullptr, screen);

    rounded_rect(dc, box, kCard, kBorder, kRadiusCard);
    draw(dc, "SELECT MODEL", RECT{box.left + 16, box.top + 12, box.right - 140, box.top + 30},
         kMuted, app->font_tiny, DT_LEFT | DT_SINGLELINE);
    draw(dc, std::to_string(count) + " available",
         RECT{box.left + 120, box.top + 12, box.right - 16, box.top + 30}, kMuted,
         app->font_tiny, DT_RIGHT | DT_SINGLELINE);

    int y = box.top + 48;
    int first = app->model_scroll;
    int last = std::min(count, first + visible);
    for (int i = first; i < last; ++i) {
        RECT row{box.left + 6, y, box.right - 6, y + row_h};
        app->model_rows.push_back(row);
        bool selected = app->models[i] == app->settings.model;
        if (app->dropdown_hover == i || selected) {
            RECT pill{row.left + 2, row.top + 1, row.right - 2, row.bottom - 1};
            rounded_rect(dc, pill, kInputBg, selected ? kAccent : kBorder, 6);
        }
        std::string label = app->models[i];
        if (model_supports_vision(label)) label += "   \u25C9";
        draw(dc, (selected ? "\u2713  " : "    ") + label,
             RECT{row.left + 10, row.top + 4, row.right - 10, row.bottom}, selected ? kAccent : kText,
             app->font_small, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += row_h;
    }

    std::string hint = "\u2191\u2193 scroll \u00B7 Enter select \u00B7 Esc close";
    draw(dc, hint, RECT{box.left + 16, box.bottom - 22, box.right - 16, box.bottom - 4}, kMuted,
         app->font_tiny, DT_CENTER | DT_SINGLELINE);

    add_hit(box, CidModelPick);
}

void paint(HWND hwnd, HDC target, RECT client) {
    HDC dc = CreateCompatibleDC(target);
    HBITMAP buffer = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ old = SelectObject(dc, buffer);
    if (g_palette.gradient) {
        paint_gradient(dc, client, g_palette.grad_top, g_palette.grad_bottom);
    } else {
        fill_rect_solid(dc, client, kBg);
    }

    RECT client_rect;
    GetClientRect(g_app->window, &client_rect);
    const SettingsLayout layout = settings_layout(client_rect, g_app->scroll);
    auto place_field = [&](HWND control, RECT field) {
        if (!control) return;
        // Child windows cannot be clipped by the header, so hide a field that is
        // scrolled underneath it or below the window instead of letting it float.
        const bool visible = g_app->tab == Tab::Settings && field.top >= kHeaderH &&
                             field.bottom <= client_rect.bottom;
        ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
        if (visible) {
            g_app->layout_edit(control, field.left + 2, field.top + 2,
                               field.right - field.left - 4, field.bottom - field.top - 4,
                               kRadiusControl);
        }
    };
    place_field(g_app->key_field_edit, layout.key_field);
    place_field(g_app->project_edit, layout.project_field);

    reset_hits();
    paint_header(dc, client);

    // Scrolling content lives below the header; clip it so cards and text never
    // paint over the wordmark or tabs.
    const int content_dc = SaveDC(dc);
    IntersectClipRect(dc, 0, kHeaderH, client.right, client.bottom);
    g_hit_content = true;
    switch (g_app->tab) {
        case Tab::Chat: paint_transcript(dc, client); break;
        case Tab::Activity: paint_activity(dc, client); break;
        case Tab::Settings: paint_settings(dc, client); break;
    }
    paint_sidebar(dc, client);
    g_hit_content = false;
    RestoreDC(dc, content_dc);

    if (g_app->tab == Tab::Chat) paint_composer(dc, client);
    paint_sidebar_tooltip(dc, client);
    paint_model_popup(dc, client);

    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(buffer);
    DeleteDC(dc);
    (void)hwnd;
}

// ---------------------------------------------------------------- input
void handle_click(HWND hwnd, POINT point) {
    AppState* app = g_app;

    // Model popup takes priority over everything beneath it.
    if (app->dropdown_open) {
        for (size_t i = 0; i < app->model_rows.size(); ++i) {
            if (hit(app->model_rows[i], point)) {
                size_t index = static_cast<size_t>(app->model_scroll) + i;
                if (index < app->models.size()) {
                    app->settings.model = app->models[index];
                    save();
                }
                app->dropdown_open = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }
        }
    }

    if (hit_test(point) == CidNone) {
        RECT client{};
        GetClientRect(hwnd, &client);
        if (app->tab == Tab::Chat) {
            SetFocus(hwnd);
            return;
        }
        if (app->tab == Tab::Settings) {
            SettingsLayout layout = settings_layout(client, app->scroll);
            if (hit(layout.key_field, point) && app->key_field_edit) {
                SetFocus(app->key_field_edit);
                return;
            }
            if (hit(layout.project_field, point) && app->project_edit) {
                SetFocus(app->project_edit);
                return;
            }
        }
    }
    int id = hit_test(point);
    if (id == CidSidebarReveal) {
        app->sidebar_open = true;
        app->sidebar_hide_armed = false;
        app->hover_id = CidNone;
        KillTimer(hwnd, 2);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (id == CidNewChatTab) {
        if (app->busy) {
            app->error = "Finish or cancel the current turn before switching chats.";
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        // New tab inherits the current project; a prompt is optional.
        add_session(app->root, "New chat");
        app->settings.project = app->root;
        app->tab = Tab::Chat;
        app->dropdown_open = false;
        app->sel_region = kSelNone;
        app->error.clear();
        save();
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }
    if (id >= CidChatTab && id < CidChatClose) {
        if (app->busy) {
            app->error = "Finish or cancel the current turn before switching chats.";
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        size_t index = static_cast<size_t>(id - CidChatTab);
        if (index < app->sessions.size()) {
            app->active_session = static_cast<int>(index);
            app->root = app->sessions[index].project;
            app->settings.project = app->root;
            app->tab = Tab::Chat;
            app->dropdown_open = false;
            app->scroll = 0;
            app->sel_region = kSelNone;
            app->error.clear();
            save();
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return;
    }
    if (id >= CidChatClose && id < CidActivityRow) {
        if (app->busy) {
            app->error = "Finish or cancel the current turn before closing a chat.";
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        size_t index = static_cast<size_t>(id - CidChatClose);
        if (index < app->sessions.size() && app->sessions.size() > 1) {
            delete_chat_file(app->sessions[index]);
            app->sessions.erase(app->sessions.begin() + static_cast<long>(index));
            if (app->active_session >= static_cast<int>(app->sessions.size())) {
                app->active_session = static_cast<int>(app->sessions.size()) - 1;
            }
            app->root = app->sessions[static_cast<size_t>(app->active_session)].project;
            app->settings.project = app->root;
            app->scroll = 0;
            save();
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return;
    }
    if (id >= CidUserToggle && id < CidNewChatTab) {
        const size_t index = static_cast<size_t>(id - CidUserToggle);
        auto& transcript = app->transcript();
        if (index < transcript.size()) {
            UiMessage& message = transcript[index];
            if (message.role == Role::User &&
                count_words(strip_markdown(message.text)) > kUserPreviewWords) {
                message.expanded = !message.expanded;
                app->sel_region = kSelNone;
                save_active_chat();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return;
    }
    if (id >= CidAttachmentChip && id < CidAttachmentChipEnd) {
        remove_attachment(id - CidAttachmentChip);
        return;
    }
    if (id >= CidActivityRow) {
        size_t index = static_cast<size_t>(id - CidActivityRow);
        if (index < app->activity().size()) {
            app->activity()[index].expanded = !app->activity()[index].expanded;
            save_active_chat();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return;
    }
    if (id >= CidTheme && id < CidTheme + kThemeCount) {
        app->theme = kThemeOptions[id - CidTheme].theme;
        apply_theme(app->theme);
        update_window_frame(hwnd);
        app->settings.theme = theme_to_string(app->theme);
        save();
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }

    switch (id) {
        case CidChat:
        case CidActivity:
        case CidSettings:
            app->tab = id == CidChat ? Tab::Chat : (id == CidActivity ? Tab::Activity : Tab::Settings);
            app->scroll = 0;
            app->dropdown_open = false;
            break;
        case CidNewChat:
            if (!app->busy) {
                add_session(app->root, "New chat");
                app->tab = Tab::Chat;
                app->dropdown_open = false;
                save();
                for (auto& attachment : app->attachments) {
                    if (attachment.bitmap) DeleteObject(attachment.bitmap);
                }
                app->attachments.clear();
                app->error.clear();
                app->scroll = 0;
            }
            break;
        case CidEffort:
            app->settings.effort = next_effort(app->settings.effort);
            save();
            break;
        case CidPerm:
            app->settings.allow_all = !app->settings.allow_all;
            save();
            break;
        case CidModel:
        case CidModelPick:
            if (app->dropdown_open) {
                app->dropdown_open = false;
            } else {
                app->dropdown_open = true;
                app->model_scroll = 0;
                int index = 0;
                for (size_t i = 0; i < app->models.size(); ++i) {
                    if (app->models[i] == app->settings.model) index = static_cast<int>(i);
                }
                app->dropdown_hover = index;
                if (index > 8) app->model_scroll = index - 8;
                if (app->models.empty()) refresh_models();
            }
            break;
        case CidRefresh:
            refresh_models();
            break;
        case CidSend:
            if (start_turn(trim(app->input))) app->input.clear();
            break;
        case CidAttach:
            attach_dialog(hwnd);
            break;
        case CidPaste:
            attach_clipboard(hwnd);
            break;
        case CidKeySave: {
            std::string key = trim(get_window_text(app->key_field_edit));
            if (key.empty() && !app->key_editing) {
                app->key_editing = true;
                app->error.clear();
                SetWindowTextW(app->key_field_edit, L"");
                SetFocus(app->key_field_edit);
                break;
            }
            if (key.size() < 12) {
                app->error = "That does not look like an NVIDIA API key (expected nvapi-...).";
                SetFocus(app->key_field_edit);
                break;
            }
            std::string error;
            if (save_api_key(key, error)) {
                app->api_key = key;
                app->key_editing = false;
                app->error.clear();
                SetWindowTextW(app->key_field_edit, L"");
                refresh_models();
            } else {
                app->error = error;
            }
            break;
        }
        case CidProjectOpen: {
            if (app->busy) {
                app->error = "Finish or cancel the current turn before opening another project.";
                break;
            }
            std::string typed = trim(get_window_text(app->project_edit));
            if (typed.empty()) typed = trim(app->project_buffer);
            if (typed.empty()) {
                app->error = "Choose a project folder with Browse, or type its path.";
                break;
            }
            if (!is_directory(typed)) {
                app->error = "Project folder not found: " + typed;
                break;
            }
            app->root = typed;
            app->settings.project = typed;
            app->error.clear();
            app->tab = Tab::Chat;
            // Reuse the active tab when it is still empty, otherwise open a new chat.
            ChatSession& current = app->sessions[app->active_index()];
            bool reusable = current.transcript.empty() && current.activity.empty();
            if (reusable) {
                current.title = "New chat";
                current.project = typed;
            } else {
                add_session(typed, "New chat");
            }
            SetWindowTextW(app->project_edit, L"");
            app->project_buffer.clear();
            save();
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        }
        case CidProjectBrowse: {
            BROWSEINFOW info{};
            info.hwndOwner = hwnd;
            info.lpszTitle = L"Choose a project folder";
            info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
            LPITEMIDLIST item = SHBrowseForFolderW(&info);
            if (item) {
                wchar_t path[MAX_PATH];
                if (SHGetPathFromIDListW(item, path)) {
                    std::string chosen = wide_to_utf8(path);
                    // Browse only stages the path; Open commits it and creates the chat.
                    SetWindowTextW(app->project_edit, path);
                    app->project_buffer = chosen;
                    app->error.clear();
                }
                CoTaskMemFree(item);
            }
            break;
        }
        default: break;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void scroll_by(int delta);

void handle_key(HWND hwnd, WPARAM key, bool ctrl, bool shift) {
    AppState* app = g_app;

    // The model chooser owns the keyboard while it is open.
    if (app->dropdown_open && !app->models.empty()) {
        const int count = static_cast<int>(app->models.size());
        const int visible = std::min(count, 12);
        if (key == VK_ESCAPE) {
            app->dropdown_open = false;
        } else if (key == VK_UP) {
            app->dropdown_hover = std::max(0, app->dropdown_hover - 1);
            if (app->dropdown_hover < app->model_scroll) app->model_scroll = app->dropdown_hover;
        } else if (key == VK_DOWN) {
            app->dropdown_hover = std::min(count - 1, app->dropdown_hover + 1);
            if (app->dropdown_hover >= app->model_scroll + visible) {
                app->model_scroll = app->dropdown_hover - visible + 1;
            }
        } else if (key == VK_PRIOR) {
            app->dropdown_hover = std::max(0, app->dropdown_hover - visible);
            app->model_scroll = std::max(0, app->model_scroll - visible);
        } else if (key == VK_NEXT) {
            app->dropdown_hover = std::min(count - 1, app->dropdown_hover + visible);
            app->model_scroll = std::min(std::max(0, count - visible), app->model_scroll + visible);
        } else if (key == VK_RETURN) {
            if (app->dropdown_hover >= 0 && app->dropdown_hover < count) {
                app->settings.model = app->models[static_cast<size_t>(app->dropdown_hover)];
                save();
            }
            app->dropdown_open = false;
        } else if (key == VK_HOME) {
            app->dropdown_hover = 0;
            app->model_scroll = 0;
        } else if (key == VK_END) {
            app->dropdown_hover = count - 1;
            app->model_scroll = std::max(0, count - visible);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (ctrl && (key == '1' || key == VK_TAB)) {
        app->tab = Tab::Chat;
        app->dropdown_open = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (ctrl && key == '2') {
        app->tab = Tab::Activity;
        app->dropdown_open = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (ctrl && key == '3') {
        app->tab = Tab::Settings;
        app->dropdown_open = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (ctrl && key == 'V') {
        attach_clipboard(hwnd);
        return;
    }
    if (ctrl && key == 'A') {
        if (!app->input.empty()) {
            app->sel_region = 0;
            app->sel_start = 0;
            app->sel_end = static_cast<int>(utf8_to_wide(app->input).size());
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return;
    }
    if (ctrl && key == 'C') {
        if (app->sel_region != kSelNone && app->sel_end > app->sel_start) {
            const std::string* raw = nullptr;
            for (const auto& region : app->text_regions) {
                if (region.region == app->sel_region) raw = &region.text;
            }
            if (raw) {
                std::string slice = app->sel_region == 0
                                        ? slice_utf16(app->input, app->sel_start, app->sel_end)
                                        : selected_text(*raw, app->sel_start, app->sel_end);
                if (!slice.empty() && OpenClipboard(hwnd)) {
                    EmptyClipboard();
                    size_t bytes = (slice.size() + 1) * sizeof(wchar_t);
                    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (mem) {
                        void* dst = GlobalLock(mem);
                        std::wstring wide_slice = utf8_to_wide(slice);
                        memcpy(dst, wide_slice.c_str(), (wide_slice.size() + 1) * sizeof(wchar_t));
                        GlobalUnlock(mem);
                        SetClipboardData(CF_UNICODETEXT, mem);
                    }
                    CloseClipboard();
                }
            }
        }
        return;
    }
    if (ctrl && key == 'N') {
        if (!app->busy) {
            add_session(app->root, "New chat");
            save();
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }
    if (key == VK_ESCAPE) {
        if (app->dropdown_open) {
            app->dropdown_open = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (app->busy) {
            app->cancel = true;
        }
        return;
    }
    if (key == VK_RETURN && !shift) {
        if (start_turn(trim(app->input))) app->input.clear();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (key == VK_RETURN && shift) {
        app->input += "\n";
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (key == VK_BACK) {
        if (!app->input.empty()) app->input.pop_back();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (key == VK_UP) {
        scroll_by(-60);
        return;
    }
    if (key == VK_DOWN) {
        scroll_by(60);
        return;
    }
}

// Clamps scrolling to the painted content so you can always reach the top and bottom.
int max_scroll() {
    RECT client{};
    GetClientRect(g_app->window, &client);
    int viewport = client.bottom - kHeaderH - (g_app->tab == Tab::Chat ? kFooterH : 0);
    int total = g_app->content_height;
    return std::max(0, total - viewport + 16);
}

void scroll_by(int delta) {
    int limit = max_scroll();
    int next = std::min(limit, std::max(0, g_app->scroll + delta));
    if (next == g_app->scroll) return;
    g_app->scroll = next;
    InvalidateRect(g_app->window, nullptr, FALSE);
}

void arm_sidebar_hide(HWND hwnd) {
    if (!g_app->sidebar_open || g_app->sidebar_hide_armed) return;
    SetTimer(hwnd, 2, 1000, nullptr);
    g_app->sidebar_hide_armed = true;
}

void cancel_sidebar_hide(HWND hwnd) {
    if (!g_app->sidebar_hide_armed) return;
    KillTimer(hwnd, 2);
    g_app->sidebar_hide_armed = false;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            SetTimer(hwnd, 1, 530, nullptr);
            // The rail starts open, then settles out of the way if the pointer
            // is not resting on it.
            SetTimer(hwnd, 2, 1000, nullptr);
            g_app->sidebar_hide_armed = true;
            return 0;
        case WM_TIMER:
            if (wparam == 1) {
                g_app->caret_on = !g_app->caret_on;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (wparam == 2) {
                g_app->sidebar_hide_armed = false;
                KillTimer(hwnd, 2);
                // Do not reflow the page out from under an active text drag.
                if (g_app->sel_dragging) return 0;
                POINT cursor{};
                bool over_rail = false;
                if (GetCursorPos(&cursor)) {
                    ScreenToClient(hwnd, &cursor);
                    RECT client{};
                    GetClientRect(hwnd, &client);
                    over_rail = cursor.x >= 0 && cursor.x < kSidebarW &&
                                cursor.y >= kHeaderH && cursor.y < client.bottom;
                }
                if (!over_rail && g_app->sidebar_open) {
                    g_app->sidebar_open = false;
                    g_app->hover_id = CidNone;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) != WA_INACTIVE) {
                HWND focus = GetFocus();
                if (focus != g_app->key_field_edit && focus != g_app->project_edit) {
                    SetFocus(hwnd);
                }
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize.x = 820;
            limits->ptMinTrackSize.y = 600;
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            paint(hwnd, dc, client);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCHITTEST: {
            LRESULT base = DefWindowProcW(hwnd, message, wparam, lparam);
            return base == HTCLIENT ? HTCLIENT : base;
        }
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (!g_app->sel_dragging) {
                const bool over_rail = g_app->sidebar_open && point.x >= 0 &&
                                       point.x < kSidebarW && point.y >= kHeaderH;
                if (g_app->sidebar_open) {
                    if (over_rail) {
                        cancel_sidebar_hide(hwnd);
                    } else {
                        arm_sidebar_hide(hwnd);
                    }
                } else if (point.x <= kSidebarRevealW && point.y >= kHeaderH) {
                    // Hovering the edge grip slides the rail back in.
                    g_app->sidebar_open = true;
                    cancel_sidebar_hide(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            if (g_app->sel_dragging && g_app->sel_region == 0) {
                RECT box = g_app->composer_box;
                HDC measure_dc = GetDC(hwnd);
                const int offset = composer_offset_from_point(measure_dc, box, point);
                ReleaseDC(hwnd, measure_dc);
                if (offset != g_app->sel_end) {
                    g_app->sel_end = offset;
                    g_app->caret_pos = offset;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (g_app->sel_dragging && g_app->sel_region != kSelNone &&
                g_app->sel_region != 0) {
                const TextHit* target = nullptr;
                for (const auto& region : g_app->text_regions) {
                    if (region.region == g_app->sel_region) target = &region;
                }
                if (target) {
                    HDC measure_dc = GetDC(hwnd);
                    std::vector<std::string> lines = wrap_text(
                        measure_dc, strip_markdown(target->text),
                        target->box.right - target->box.left, g_app->font_ui);
                    g_app->sel_end = offset_at_point(measure_dc, lines, target->box, 20,
                                                     g_app->font_ui, point);
                    ReleaseDC(hwnd, measure_dc);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            int id = hit_test(point);
            int hover = id;
            if (g_app->dropdown_open) {
                for (size_t i = 0; i < g_app->model_rows.size(); ++i) {
                    if (hit(g_app->model_rows[i], point)) {
                        g_app->dropdown_hover = g_app->model_scroll + static_cast<int>(i);
                    }
                }
            }
            if (hover != g_app->hover_id) {
                g_app->hover_id = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            g_app->hover_id = CidNone;
            g_app->dropdown_hover = -1;
            arm_sidebar_hide(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            RECT client{};
            GetClientRect(hwnd, &client);

            // A composer click is handled specially below, so dismiss the model
            // chooser first; otherwise that early return would leave it open.
            if (g_app->dropdown_open && g_app->tab == Tab::Chat &&
                hit(g_app->composer_box, point)) {
                g_app->dropdown_open = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            // Composer (region 0): place the caret wherever the user clicked.
            if (g_app->tab == Tab::Chat) {
                RECT box = g_app->composer_box;
                if (box.right > box.left && hit(box, point)) {
                    HDC measure_dc = GetDC(hwnd);
                    const int offset = composer_offset_from_point(measure_dc, box, point);
                    ReleaseDC(hwnd, measure_dc);
                    g_app->caret_pos = offset;
                    g_app->sel_region = 0;
                    g_app->sel_start = offset;
                    g_app->sel_end = offset;
                    g_app->sel_dragging = true;
                    g_app->caret_on = true;
                    SetTimer(hwnd, 1, 530, nullptr);
                    SetFocus(hwnd);
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }

            // Any other painted text: begin a drag selection.
            for (const auto& region : g_app->text_regions) {
                if (region.region == 0) continue;
                if (!hit(region.box, point)) continue;
                HDC measure_dc = GetDC(hwnd);
                std::vector<std::string> lines =
                    wrap_text(measure_dc, strip_markdown(region.text), region.box.right - region.box.left,
                              g_app->font_ui);
                int offset = offset_at_point(measure_dc, lines, region.box, 20, g_app->font_ui, point);
                ReleaseDC(hwnd, measure_dc);
                g_app->sel_region = region.region;
                g_app->sel_start = offset;
                g_app->sel_end = offset;
                g_app->sel_dragging = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // Clicking elsewhere clears the selection.
            if (g_app->sel_region != kSelNone) {
                g_app->sel_region = kSelNone;
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            if (g_app->tab == Tab::Settings) {
                SettingsLayout layout = settings_layout(client, g_app->scroll);
                if (hit(layout.key_field, point) && g_app->key_field_edit) {
                    SetFocus(g_app->key_field_edit);
                    return 0;
                }
                if (hit(layout.project_field, point) && g_app->project_edit) {
                    SetFocus(g_app->project_edit);
                    return 0;
                }
            }

            g_app->pressed_id = hit_test(point);
            SetCapture(hwnd);
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ReleaseCapture();
            if (g_app->sel_dragging) {
                g_app->sel_dragging = false;
                if (g_app->sel_end < g_app->sel_start) std::swap(g_app->sel_start, g_app->sel_end);
                InvalidateRect(hwnd, nullptr, FALSE);
                arm_sidebar_hide(hwnd);
                return 0;
            }
            const bool popup_was_open = g_app->dropdown_open;
            if (hit_test(point) == g_app->pressed_id && g_app->pressed_id != CidNone) {
                handle_click(hwnd, point);
            }
            // Clicking outside the chooser dismisses it.
            if (g_app->dropdown_open && popup_was_open) {
                bool inside_rows = false;
                for (const auto& row : g_app->model_rows) {
                    if (hit(row, point)) inside_rows = true;
                }
                if (!inside_rows) {
                    g_app->dropdown_open = false;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            g_app->pressed_id = CidNone;
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (g_app->dropdown_open && !g_app->models.empty()) {
                int count = static_cast<int>(g_app->models.size());
                int visible = std::min(count, 12);
                int delta = -GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
                g_app->model_scroll = std::max(0, std::min(std::max(0, count - visible),
                                                           g_app->model_scroll + delta));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &point);
            if (g_app->sidebar_open && point.x >= 0 && point.x < kSidebarW &&
                point.y >= kHeaderH) {
                RECT client{};
                GetClientRect(hwnd, &client);
                const int view =
                    std::max(1, static_cast<int>(client.bottom) - 8 - (kHeaderH + 58));
                const int total = static_cast<int>(g_app->sessions.size()) * 58;
                const int max_scroll = std::max(0, total - view);
                const int step = -static_cast<int>(GET_WHEEL_DELTA_WPARAM(wparam)) /
                                 WHEEL_DELTA * 58;
                g_app->sidebar_scroll =
                    std::max(0, std::min(max_scroll, g_app->sidebar_scroll + step));
                g_app->sidebar_scroll_to_active = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            scroll_by(-(int)GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * 60);
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC control_dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(control_dc, kText);
            SetBkColor(control_dc, kInputBg);
            static HBRUSH edit_brush = nullptr;
            static COLORREF edit_brush_colour = 0xFFFFFFFF;
            if (!edit_brush || edit_brush_colour != kInputBg) {
                if (edit_brush) DeleteObject(edit_brush);
                edit_brush = CreateSolidBrush(kInputBg);
                edit_brush_colour = kInputBg;
            }
            return reinterpret_cast<LRESULT>(edit_brush);
        }
        case WM_CTLCOLORSTATIC: {
            HDC control_dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(control_dc, kMuted);
            SetBkColor(control_dc, kBg);
            static HBRUSH bg_brush = nullptr;
            static COLORREF bg_brush_colour = 0xFFFFFFFF;
            if (!bg_brush || bg_brush_colour != kBg) {
                if (bg_brush) DeleteObject(bg_brush);
                bg_brush = CreateSolidBrush(kBg);
                bg_brush_colour = kBg;
            }
            return reinterpret_cast<LRESULT>(bg_brush);
        }
        case WM_KEYDOWN: {
            HWND focus = GetFocus();
            const bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            // Tab shortcuts win over text input.
            if (ctrl_down && (wparam == '1' || wparam == '2' || wparam == '3')) {
                g_app->tab = wparam == '1' ? Tab::Chat
                                           : (wparam == '2' ? Tab::Activity : Tab::Settings);
                g_app->dropdown_open = false;
                g_app->scroll = 0;
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            // Text controls own their keyboard input entirely.
            if (focus == g_app->key_field_edit) {
                return static_cast<LRESULT>(SendMessageW(focus, message, wparam, lparam));
            }
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            // Enter in the composer sends; Shift+Enter inserts a newline.
            if (wparam == VK_RETURN && !ctrl) {
                if (shift) {
                    g_app->input += "\n";
                    g_app->caret_pos = static_cast<int>(g_app->input.size());
                    g_app->sel_region = kSelNone;
                } else if (start_turn(trim(g_app->input))) {
                    g_app->input.clear();
                    g_app->caret_pos = 0;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_LEFT) {
                g_app->caret_pos = std::max(0, g_app->caret_pos - 1);
                g_app->sel_region = kSelNone;
                g_app->caret_on = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_RIGHT) {
                g_app->caret_pos =
                    std::min(static_cast<int>(g_app->input.size()), g_app->caret_pos + 1);
                g_app->sel_region = kSelNone;
                g_app->caret_on = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_HOME) {
                g_app->caret_pos = 0;
                g_app->sel_region = kSelNone;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_END) {
                g_app->caret_pos = static_cast<int>(g_app->input.size());
                g_app->sel_region = kSelNone;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_DELETE) {
                if (g_app->caret_pos < static_cast<int>(g_app->input.size())) {
                    g_app->input.erase(static_cast<size_t>(g_app->caret_pos), 1);
                }
                g_app->caret_on = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_BACK) {
                if (g_app->caret_pos > 0 &&
                    g_app->caret_pos <= static_cast<int>(g_app->input.size())) {
                    g_app->input.erase(static_cast<size_t>(g_app->caret_pos - 1), 1);
                    g_app->caret_pos -= 1;
                }
                g_app->caret_on = true;
                SetTimer(hwnd, 1, 530, nullptr);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            handle_key(hwnd, wparam, ctrl, shift);
            return 0;
        }
        case WM_CHAR: {
            if (GetFocus() == g_app->key_field_edit) {
                return static_cast<LRESULT>(
                    SendMessageW(g_app->key_field_edit, message, wparam, lparam));
            }
            if (GetFocus() == g_app->project_edit) {
                return static_cast<LRESULT>(
                    SendMessageW(g_app->project_edit, message, wparam, lparam));
            }
            wchar_t ch = static_cast<wchar_t>(wparam);
            if (ch >= 0x20 || ch == L'\t') {
                std::string typed = wide_to_utf8(std::wstring(1, ch));
                if (g_app->sel_region == 0 && g_app->sel_end > g_app->sel_start) {
                    int from = std::min(g_app->sel_start, g_app->sel_end);
                    int to = std::max(g_app->sel_start, g_app->sel_end);
                    g_app->input.erase(static_cast<size_t>(from), static_cast<size_t>(to - from));
                    g_app->caret_pos = from;
                    g_app->sel_start = g_app->sel_end = from;
                }
                size_t at = static_cast<size_t>(
                    std::min<int>(g_app->caret_pos, static_cast<int>(g_app->input.size())));
                g_app->input.insert(at, typed);
                g_app->caret_pos = static_cast<int>(at + typed.size());
                g_app->caret_on = true;
                SetTimer(hwnd, 1, 530, nullptr);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_COMMAND:
            if (HIWORD(wparam) == EN_CHANGE && reinterpret_cast<HWND>(lparam) == g_app->key_field_edit) {
                if (GetWindowTextLengthW(g_app->key_field_edit) > 0) g_app->key_editing = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_APP_TURN:
            handle_async(reinterpret_cast<AsyncEvent*>(lparam));
            return 0;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wparam);
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::string> paths;
            for (UINT i = 0; i < count; ++i) {
                wchar_t path[MAX_PATH * 2];
                if (DragQueryFileW(drop, i, path, MAX_PATH * 2)) paths.push_back(wide_to_utf8(path));
            }
            DragFinish(drop);
            attach_paths(paths);
            return 0;
        }
        case WM_DESTROY:
            save_active_chat();
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

int RunClamBuildApp(HINSTANCE instance, int show_command) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);
    OleInitialize(nullptr);  // handles CoInitialize + OLE; do not call both

    AppState state;
    g_app = &state;
    state.instance = instance;
    state.settings = load_settings();
    load_api_key(state.api_key);
    state.root = state.settings.project.empty() ? current_directory() : state.settings.project;
    state.sessions = load_chat_sessions();
    if (state.sessions.empty()) {
        ChatSession session;
        session.title = "New chat";
        session.project = state.root;
        session.id = make_session_id();
        session.updated_at = now_seconds();
        state.sessions.push_back(std::move(session));
    } else {
        state.active_session = static_cast<int>(state.sessions.size()) - 1;
        const std::string& project = state.sessions.back().project;
        if (!project.empty()) state.root = project;
        state.settings.project = state.root;
    }
    state.sidebar_scroll_to_active = true;
    state.theme = theme_from_string(state.settings.theme);
    apply_theme(state.theme);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = L"ClamBuildWindow";
    wc.hIcon = LoadIconW(instance, L"IDI_CLAM");
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = CreateSolidBrush(kBg);
    RegisterClassExW(&wc);

    state.window = CreateWindowExW(WS_EX_ACCEPTFILES, L"ClamBuildWindow", L"Clam Build",
                                   WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1200, 860,
                                   nullptr, nullptr, instance, nullptr);
    if (!state.window) return 1;
    update_window_frame(state.window);

    state.font_ui = make_ui_font(15, FW_NORMAL);
    state.font_title = make_ui_font(26, FW_SEMIBOLD);
    state.font_brand = make_ui_font(16, FW_SEMIBOLD);
    state.font_heading = make_ui_font(20, FW_SEMIBOLD);
    state.font_small = make_ui_font(13, FW_NORMAL);
    state.font_tiny = make_ui_font(12, FW_NORMAL);
    state.font_mono = make_mono_font(13, FW_NORMAL);

    // Hidden edit control: owns keyboard focus so typing flows into the custom composer.
    state.key_field_edit = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | ES_PASSWORD | ES_AUTOHSCROLL, 0, 0, 10, 24, state.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(2002)), instance, nullptr);
    SendMessageW(state.key_field_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(8, 8));
    SendMessageW(state.key_field_edit, EM_SETCUEBANNER, FALSE,
                 reinterpret_cast<LPARAM>(L"Paste a new NVIDIA API key"));
    SendMessageW(state.key_field_edit, WM_SETFONT, reinterpret_cast<WPARAM>(state.font_small), TRUE);
    ShowWindow(state.key_field_edit, SW_HIDE);

    state.project_edit = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 24, state.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(2003)), instance, nullptr);
    SendMessageW(state.project_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(8, 8));
    SendMessageW(state.project_edit, EM_SETCUEBANNER, FALSE,
                 reinterpret_cast<LPARAM>(L"Path to your project folder"));
    SendMessageW(state.project_edit, WM_SETFONT, reinterpret_cast<WPARAM>(state.font_small), TRUE);
    ShowWindow(state.project_edit, SW_HIDE);

    ShowWindow(state.window, show_command);
    UpdateWindow(state.window);
    SetForegroundWindow(state.window);
    SetActiveWindow(state.window);
    SetFocus(state.window);
    refresh_models();

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace clam
