// LoucuraXXXX GPU - Win32 GUI front end.
//
// A native window with a tab control: Info, Config, Render and Log. All logic
// is in GuiModel; this file only creates controls and marshals their values.
// It builds against the plain Win32 API (user32/gdi32/comctl32) so it needs no
// external toolkit and cross-compiles cleanly with mingw-w64.
#include "gui/frontend.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace lx::gui {
namespace {

constexpr int kIdTab = 1000;
constexpr int kIdRefresh = 1001;
constexpr int kIdValidate = 1002;
constexpr int kIdRender = 1003;
constexpr int kIdSave = 1004;
constexpr int kIdLoad = 1005;
constexpr int kIdRamEdit = 1010;
constexpr int kIdCoresEdit = 1011;
constexpr int kIdWidthEdit = 1012;
constexpr int kIdHeightEdit = 1013;
constexpr int kIdFramesEdit = 1014;
constexpr int kIdBackendCombo = 1015;
constexpr int kIdFilterCombo = 1016;
constexpr int kIdFilterEnableCheck = 1017;
constexpr int kIdFilterScopeCombo = 1018;
constexpr int kIdFilterIntensityEdit = 1019;
constexpr int kIdLog = 1020;
constexpr int kIdInfo = 1021;

struct AppContext {
    GuiModel* model = nullptr;
    HWND tab = nullptr;
    HWND info = nullptr;
    HWND log = nullptr;
    HWND ram_edit = nullptr;
    HWND cores_edit = nullptr;
    HWND width_edit = nullptr;
    HWND height_edit = nullptr;
    HWND frames_edit = nullptr;
    HWND backend_combo = nullptr;
    HWND filter_combo = nullptr;
    HWND filter_enable_check = nullptr;
    HWND filter_scope_combo = nullptr;
    HWND filter_intensity_edit = nullptr;
};

AppContext g_app;

std::wstring widen(const std::string& text) {
    if (text.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                  nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), len);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
}

std::wstring get_window_text(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"";
    std::wstring text(static_cast<std::size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<std::size_t>(len));
    return text;
}

// Parses a non-negative integer from an edit control. Returns `fallback` when
// the field is empty or malformed, so a stray character never crashes the app.
unsigned long long parse_edit_u64(HWND hwnd, unsigned long long fallback) {
    const std::string text = narrow(get_window_text(hwnd));
    if (text.empty()) return fallback;
    try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed, 10);
        if (consumed != text.size()) return fallback;
        return value;
    } catch (const std::exception&) {
        return fallback;
    }
}

void set_edit_u64(HWND hwnd, unsigned long long value) {
    SetWindowTextW(hwnd, widen(std::to_string(value)).c_str());
}

void refresh_info_text() {
    const GuiState& s = g_app.model->state();
    std::ostringstream ss;
    ss << "CPU: " << s.cpu_model << "\r\n";
    ss << "Cores: " << s.logical_cores << " logical, " << s.physical_cores << " physical";
    if (s.hybrid_cpu) {
        ss << " (" << s.performance_cores << " performance, " << s.efficiency_cores
           << " efficiency)";
    }
    ss << "\r\nOS: " << s.os_summary << "\r\n";
    ss << "Memory: " << (s.total_memory_bytes / kMiB) << " MiB total, "
       << (s.available_memory_bytes / kMiB) << " MiB available (" << s.memory_topology << ")\r\n";
    ss << "GPUs:\r\n";
    if (s.gpus.empty()) ss << "  (none)\r\n";
    for (const auto& gpu : s.gpus) ss << "  - " << gpu << "\r\n";
    ss << "Backends:\r\n";
    for (const auto& b : s.backends) {
        ss << "  - " << b.name << " [" << b.kind
           << (b.is_hardware ? ", hardware" : ", software") << "]\r\n";
    }
    ss << "Filters:\r\n";
    for (const auto& f : s.filters) ss << "  - " << f.name << ": " << f.description << "\r\n";
    ss << "Safe limits: RAM <= " << (s.max_ram_bytes / kMiB) << " MiB, cores <= "
       << s.max_cpu_cores << "\r\n";
    SetWindowTextW(g_app.info, widen(ss.str()).c_str());
}

void refresh_log_text() {
    std::ostringstream ss;
    for (const auto& line : g_app.model->state().log) {
        ss << (line.is_error ? "[err] " : "      ") << line.text << "\r\n";
    }
    SetWindowTextW(g_app.log, widen(ss.str()).c_str());
}

void refresh_filter_controls();

void refresh_controls() {
    const GuiState& s = g_app.model->state();
    set_edit_u64(g_app.ram_edit, s.config.reserved_ram_bytes / kMiB);
    set_edit_u64(g_app.cores_edit, s.config.reserved_cpu_cores);

    SendMessageW(g_app.backend_combo, CB_RESETCONTENT, 0, 0);
    for (const auto& b : s.backends) {
        SendMessageW(g_app.backend_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(b.kind.c_str()));
    }
    if (!s.backends.empty()) SendMessageW(g_app.backend_combo, CB_SETCURSEL, 0, 0);

    SendMessageW(g_app.filter_combo, CB_RESETCONTENT, 0, 0);
    for (const auto& f : s.filters) {
        SendMessageW(g_app.filter_combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(f.name.c_str()));
    }
    if (!s.filters.empty()) SendMessageW(g_app.filter_combo, CB_SETCURSEL, 0, 0);

    refresh_filter_controls();
    refresh_info_text();
    refresh_log_text();
}

// The name of the filter currently selected in the combo, or "" when there is
// none. Used by the per-filter toggles.
std::string selected_filter_name() {
    const GuiState& s = g_app.model->state();
    const int sel = static_cast<int>(SendMessageW(g_app.filter_combo, CB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(s.filters.size())) return "";
    return s.filters[static_cast<std::size_t>(sel)].name;
}

// Reflects the selected filter's enabled/scope/intensity into the toggle row.
void refresh_filter_controls() {
    const GuiState& s = g_app.model->state();
    const int sel = static_cast<int>(SendMessageW(g_app.filter_combo, CB_GETCURSEL, 0, 0));
    const bool have = sel >= 0 && sel < static_cast<int>(s.filters.size());
    EnableWindow(g_app.filter_enable_check, have);
    EnableWindow(g_app.filter_scope_combo, have);
    EnableWindow(g_app.filter_intensity_edit, have);
    if (!have) return;

    const FilterChoice& choice = s.filters[static_cast<std::size_t>(sel)];
    SendMessageW(g_app.filter_enable_check, BM_SETCHECK,
                 choice.enabled ? BST_CHECKED : BST_UNCHECKED, 0);

    // The scope combo holds the three fixed choices; select the current one.
    const wchar_t* scopes[] = {L"any", L"game", L"system"};
    int scope_index = 0;
    for (int i = 0; i < 3; ++i) {
        if (choice.scope == narrow(scopes[i])) scope_index = i;
    }
    SendMessageW(g_app.filter_scope_combo, CB_SETCURSEL, scope_index, 0);

    std::ostringstream intensity;
    intensity << choice.default_intensity;
    SetWindowTextW(g_app.filter_intensity_edit, widen(intensity.str()).c_str());
}

void collect_config_from_controls() {
    GuiState& s = g_app.model->state();
    const unsigned long long ram_mib = parse_edit_u64(g_app.ram_edit, s.config.reserved_ram_bytes / kMiB);
    s.config.reserved_ram_bytes = ram_mib * kMiB;
    const unsigned long long cores = parse_edit_u64(g_app.cores_edit, s.config.reserved_cpu_cores);
    s.config.reserved_cpu_cores = static_cast<u32>(std::min<unsigned long long>(cores, 0xFFFFFFFFu));

    const int backend_sel = static_cast<int>(SendMessageW(g_app.backend_combo, CB_GETCURSEL, 0, 0));
    if (backend_sel >= 0 && backend_sel < static_cast<int>(s.backends.size())) {
        s.config.preferred_backend = s.backends[static_cast<std::size_t>(backend_sel)].kind;
    }
    // The filter list is owned by the per-filter toggles (set_filter_enabled /
    // set_filter_scope / set_filter_intensity), not by this combo, so it is left
    // untouched here.
}

HWND make_child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y,
                int w, int h, int id) {
    return CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

void create_controls(HWND hwnd) {
    const int margin = 10;
    const int control_h = 24;
    const int label_w = 110;
    const int edit_w = 120;
    int y = margin;

    // Config tab controls (top area).
    make_child(hwnd, L"STATIC", L"Reserved RAM (MiB)", WS_VISIBLE, margin, y + 4, label_w,
               control_h, 0);
    g_app.ram_edit = make_child(hwnd, L"EDIT", L"", WS_BORDER | ES_NUMBER, margin + label_w, y,
                                edit_w, control_h, kIdRamEdit);
    y += control_h + 6;

    make_child(hwnd, L"STATIC", L"CPU cores", WS_VISIBLE, margin, y + 4, label_w, control_h, 0);
    g_app.cores_edit = make_child(hwnd, L"EDIT", L"", WS_BORDER | ES_NUMBER, margin + label_w, y,
                                  edit_w, control_h, kIdCoresEdit);
    y += control_h + 6;

    make_child(hwnd, L"STATIC", L"Backend", WS_VISIBLE, margin, y + 4, label_w, control_h, 0);
    g_app.backend_combo = make_child(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                                     margin + label_w, y, edit_w, 120, kIdBackendCombo);
    y += control_h + 6;

    make_child(hwnd, L"STATIC", L"Filter", WS_VISIBLE, margin, y + 4, label_w, control_h, 0);
    g_app.filter_combo = make_child(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                                    margin + label_w, y, edit_w, 120, kIdFilterCombo);
    y += control_h + 6;

    // Per-filter toggles: enable, content scope and intensity. The scope is what
    // makes "reality" filters apply to game content only, leaving the desktop
    // untouched.
    g_app.filter_enable_check = make_child(hwnd, L"BUTTON", L"Enabled",
                                           BS_AUTOCHECKBOX | WS_TABSTOP, margin + label_w, y,
                                           90, control_h, kIdFilterEnableCheck);
    g_app.filter_scope_combo = make_child(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                                          margin + label_w + 96, y, 100, 120, kIdFilterScopeCombo);
    SendMessageW(g_app.filter_scope_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"any"));
    SendMessageW(g_app.filter_scope_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"game"));
    SendMessageW(g_app.filter_scope_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"system"));
    g_app.filter_intensity_edit = make_child(hwnd, L"EDIT", L"1", WS_BORDER, margin + label_w + 202,
                                             y, 60, control_h, kIdFilterIntensityEdit);
    y += control_h + 6;

    make_child(hwnd, L"STATIC", L"Width / Height / Frames", WS_VISIBLE, margin, y + 4, label_w + 40,
               control_h, 0);
    g_app.width_edit = make_child(hwnd, L"EDIT", L"160", WS_BORDER | ES_NUMBER, margin + label_w + 40,
                                  y, 60, control_h, kIdWidthEdit);
    g_app.height_edit = make_child(hwnd, L"EDIT", L"120", WS_BORDER | ES_NUMBER,
                                   margin + label_w + 106, y, 60, control_h, kIdHeightEdit);
    g_app.frames_edit = make_child(hwnd, L"EDIT", L"3", WS_BORDER | ES_NUMBER,
                                   margin + label_w + 172, y, 60, control_h, kIdFramesEdit);
    y += control_h + 10;

    make_child(hwnd, L"BUTTON", L"Refresh host", WS_TABSTOP, margin, y, 110, 28, kIdRefresh);
    make_child(hwnd, L"BUTTON", L"Validate", WS_TABSTOP, margin + 116, y, 90, 28, kIdValidate);
    make_child(hwnd, L"BUTTON", L"Render", WS_TABSTOP, margin + 212, y, 90, 28, kIdRender);
    make_child(hwnd, L"BUTTON", L"Save config", WS_TABSTOP, margin + 308, y, 110, 28, kIdSave);
    make_child(hwnd, L"BUTTON", L"Load config", WS_TABSTOP, margin + 424, y, 110, 28, kIdLoad);
    y += 38;

    // Info and Log panes.
    g_app.info = make_child(hwnd, L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY |
                                                   WS_VSCROLL | ES_AUTOVSCROLL,
                            margin, y, 640, 150, kIdInfo);
    y += 158;
    make_child(hwnd, L"STATIC", L"Log", WS_VISIBLE, margin, y, 200, 20, 0);
    y += 22;
    g_app.log = make_child(hwnd, L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY |
                                                  WS_VSCROLL | ES_AUTOVSCROLL,
                           margin, y, 640, 120, kIdLog);
}

void on_command(HWND hwnd, int id, int notification) {
    switch (id) {
        case kIdRefresh:
            g_app.model->refresh_host();
            refresh_controls();
            break;
        case kIdFilterEnableCheck: {
            const std::string name = selected_filter_name();
            if (!name.empty()) {
                const bool enabled = SendMessageW(g_app.filter_enable_check, BM_GETCHECK, 0, 0) ==
                                     BST_CHECKED;
                g_app.model->set_filter_enabled(name, enabled);
            }
            refresh_filter_controls();
            break;
        }
        case kIdFilterCombo:
            if (notification == CBN_SELCHANGE) refresh_filter_controls();
            break;
        case kIdFilterScopeCombo:
            if (notification == CBN_SELCHANGE) {
                const std::string name = selected_filter_name();
                const int sel = static_cast<int>(SendMessageW(g_app.filter_scope_combo,
                                                              CB_GETCURSEL, 0, 0));
                const char* scopes[] = {"any", "game", "system"};
                if (!name.empty() && sel >= 0 && sel < 3) {
                    g_app.model->set_filter_scope(name, scopes[sel]);
                }
                refresh_filter_controls();
            }
            break;
        case kIdFilterIntensityEdit:
            if (notification == EN_KILLFOCUS) {
                const std::string name = selected_filter_name();
                const std::string text = narrow(get_window_text(g_app.filter_intensity_edit));
                if (!name.empty() && !text.empty()) {
                    try {
                        const float intensity = std::stof(text);
                        g_app.model->set_filter_intensity(name, intensity);
                    } catch (const std::exception&) {
                        // Ignore a malformed value; the field is re-synced below.
                    }
                }
                refresh_filter_controls();
            }
            break;
        case kIdValidate:
            collect_config_from_controls();
            g_app.model->validate();
            refresh_info_text();
            refresh_log_text();
            break;
        case kIdRender: {
            collect_config_from_controls();
            const u32 width = static_cast<u32>(parse_edit_u64(g_app.width_edit, 160));
            const u32 height = static_cast<u32>(parse_edit_u64(g_app.height_edit, 120));
            const u32 frames = static_cast<u32>(parse_edit_u64(g_app.frames_edit, 3));
            g_app.model->render_frames(width, height, frames);
            refresh_log_text();
            break;
        }
        case kIdSave:
            collect_config_from_controls();
            g_app.model->save_config();
            refresh_log_text();
            break;
        case kIdLoad:
            g_app.model->load_config();
            refresh_controls();
            break;
        default:
            break;
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE:
            create_controls(hwnd);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int notification = HIWORD(wparam);
            if (notification == BN_CLICKED || notification == CBN_SELCHANGE ||
                notification == EN_KILLFOCUS) {
                on_command(hwnd, id, notification);
                return 0;
            }
            break;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

int run_frontend(GuiModel& model) {
    g_app.model = &model;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"LoucuraXXXXGuiWindow";
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"LoucuraXXXX GPU", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 700, 720, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (hwnd == nullptr) return 1;

    // Detect the host before the window is shown so the first paint is real.
    model.refresh_host();

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    refresh_controls();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

}  // namespace lx::gui
