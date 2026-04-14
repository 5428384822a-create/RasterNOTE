#include "note_app.h"

#include "theme.h"

#include <dwmapi.h>
#include <imm.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <sstream>
#include <windowsx.h>

namespace raster {

namespace {

constexpr wchar_t kLauncherClassName[] = L"RasterNoteNativeLauncherWindow";
constexpr wchar_t kMainClassName[] = L"RasterNoteNativeMainWindow";
constexpr UINT_PTR kAutoSaveTimerId = 41;
constexpr UINT_PTR kCaretBlinkTimerId = 42;
constexpr UINT_PTR kUiAnimationTimerId = 43;

std::int64_t NowUtcSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

int DipToPx(float dip, float scale) {
    return static_cast<int>(std::lround(dip * scale));
}

float MoveToward(float current, float target, float delta) {
    if (current < target) {
        return std::min(current + delta, target);
    }
    return std::max(current - delta, target);
}

COLORREF ToColorRef(const D2D1_COLOR_F& color) {
    const auto red = static_cast<BYTE>(std::clamp(color.r * 255.0f, 0.0f, 255.0f));
    const auto green = static_cast<BYTE>(std::clamp(color.g * 255.0f, 0.0f, 255.0f));
    const auto blue = static_cast<BYTE>(std::clamp(color.b * 255.0f, 0.0f, 255.0f));
    return RGB(red, green, blue);
}

D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& color, float alpha) {
    return D2D1::ColorF(color.r, color.g, color.b, alpha);
}

D2D1_COLOR_F BlendColor(const D2D1_COLOR_F& from, const D2D1_COLOR_F& to, float amount) {
    const auto t = std::clamp(amount, 0.0f, 1.0f);
    return D2D1::ColorF(from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t, from.b + (to.b - from.b) * t,
                        from.a + (to.a - from.a) * t);
}

bool EnsureRenderTarget(HWND hwnd, ID2D1Factory* factory,
                        Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget>& target,
                        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>& brush) {
    if (target) {
        return true;
    }

    RECT client{};
    GetClientRect(hwnd, &client);
    const auto width = std::max<LONG>(1, client.right - client.left);
    const auto height = std::max<LONG>(1, client.bottom - client.top);

    const auto render_props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE));
    const auto hwnd_props = D2D1::HwndRenderTargetProperties(
        hwnd,
        D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)),
        D2D1_PRESENT_OPTIONS_IMMEDIATELY);

    if (FAILED(factory->CreateHwndRenderTarget(render_props, hwnd_props, &target))) {
        return false;
    }

    const auto dpi = static_cast<float>(GetDpiForWindow(hwnd));
    target->SetDpi(dpi, dpi);
    if (FAILED(target->CreateSolidColorBrush(theme::Ink(), &brush))) {
        target.Reset();
        return false;
    }

    return true;
}

void HitTestCaret(IDWriteTextLayout* layout, std::uint32_t caret_position, std::uint32_t text_length, float* x,
                  float* y, DWRITE_HIT_TEST_METRICS* metrics) {
    const auto clamped = std::min(caret_position, text_length);
    const BOOL trailing = clamped > 0;
    const UINT32 text_pos = trailing ? clamped - 1U : 0U;
    layout->HitTestTextPosition(text_pos, trailing, x, y, metrics);
}

std::wstring FormatTimestamp(std::int64_t utc_seconds) {
    if (utc_seconds <= 0) {
        return L"UNSAVED";
    }

    std::time_t raw_time = static_cast<std::time_t>(utc_seconds);
    std::tm local_time{};
    localtime_s(&local_time, &raw_time);

    wchar_t buffer[64] = {};
    wcsftime(buffer, _countof(buffer), L"%Y-%m-%d %H:%M", &local_time);
    return buffer;
}

std::wstring ReadCompositionString(HIMC context, DWORD index) {
    const auto bytes = ImmGetCompositionStringW(context, index, nullptr, 0);
    if (bytes <= 0) {
        return {};
    }

    std::wstring out(static_cast<std::size_t>(bytes / sizeof(wchar_t)), L'\0');
    ImmGetCompositionStringW(context, index, out.data(), bytes);
    return out;
}

void DrawButton(ID2D1HwndRenderTarget* target, ID2D1SolidColorBrush* brush, IDWriteTextFormat* format,
                const D2D1_RECT_F& rect, std::wstring_view label, bool active, bool hovered = false,
                bool pressed = false, bool focus = false) {
    auto fill = active ? theme::Accent() : theme::Card();
    auto border = active ? theme::Accent() : theme::Border();
    auto text = active ? theme::InverseText() : theme::Ink();

    if (hovered) {
        fill = active ? D2D1::ColorF(0x262626, 1.0f) : BlendColor(theme::Card(), theme::Selection(), 0.55f);
        border = theme::Ink();
    }
    if (pressed) {
        fill = active ? D2D1::ColorF(0x050505, 1.0f) : BlendColor(theme::Selection(), theme::Chrome(), 0.30f);
        border = theme::Accent();
    }

    brush->SetColor(fill);
    target->FillRectangle(rect, brush);
    brush->SetColor(border);
    target->DrawRectangle(rect, brush, 1.0f);
    if (focus) {
        brush->SetColor(WithAlpha(theme::Accent(), active ? 0.34f : 0.18f));
        target->DrawRectangle(D2D1::RectF(rect.left + 2.0f, rect.top + 2.0f, rect.right - 2.0f, rect.bottom - 2.0f),
                              brush, 1.0f);
    }
    brush->SetColor(text);
    target->DrawText(label.data(), static_cast<UINT32>(label.size()), format, rect, brush);
}

}  // namespace

int NoteApp::Run(HINSTANCE instance, int show_command) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int exit_code = -1;
    if (Initialize(instance, show_command)) {
        exit_code = MessageLoop();
    } else {
        MessageBoxW(nullptr, L"RasterNoteNative failed to start.", L"RasterNoteNative", MB_ICONERROR | MB_OK);
    }

    CoUninitialize();
    return exit_code;
}

bool NoteApp::Initialize(HINSTANCE instance, int show_command) {
    instance_ = instance;
    (void)show_command;

    if (!store_.Initialize()) {
        return false;
    }
    if (!CreateFactories()) {
        return false;
    }
    if (!RegisterWindowClasses()) {
        return false;
    }
    if (!CreateWindows()) {
        return false;
    }

    RefreshRecentNotes();
    ShowWindow(launcher_hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(launcher_hwnd_);

    if (!recent_notes_.empty()) {
        OpenNote(recent_notes_.front().id);
    } else {
        CreateNewNote();
    }

    return true;
}

bool NoteApp::CreateFactories() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory_.GetAddressOf()))) {
        return false;
    }

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf())))) {
        return false;
    }

    if (FAILED(dwrite_factory_->CreateTextFormat(L"Bahnschrift", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.0f,
                                                 L"", &label_format_))) {
        return false;
    }
    label_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    if (FAILED(dwrite_factory_->CreateTextFormat(L"Bahnschrift", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f,
                                                 L"", &section_format_))) {
        return false;
    }
    section_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    if (FAILED(dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_REGULAR,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                 theme::kEditorFontSize, L"", &editor_format_))) {
        return false;
    }
    editor_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    if (FAILED(dwrite_factory_->CreateTextFormat(L"Bahnschrift", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.0f,
                                                 L"", &meta_format_))) {
        return false;
    }
    meta_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    if (FAILED(dwrite_factory_->CreateTextFormat(L"Bahnschrift", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.0f,
                                                 L"", &meta_right_format_))) {
        return false;
    }
    meta_right_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    meta_right_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

    if (FAILED(dwrite_factory_->CreateTextFormat(L"Bahnschrift", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.0f,
                                                 L"", &button_format_))) {
        return false;
    }
    button_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    button_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    button_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    return true;
}

bool NoteApp::RegisterWindowClasses() const {
    WNDCLASSEXW launcher_class{};
    launcher_class.cbSize = sizeof(launcher_class);
    launcher_class.style = CS_HREDRAW | CS_VREDRAW;
    launcher_class.lpfnWndProc = WindowProc;
    launcher_class.hInstance = instance_;
    launcher_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    launcher_class.lpszClassName = kLauncherClassName;

    if (RegisterClassExW(&launcher_class) == 0) {
        return false;
    }

    WNDCLASSEXW main_class{};
    main_class.cbSize = sizeof(main_class);
    main_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    main_class.lpfnWndProc = WindowProc;
    main_class.hInstance = instance_;
    main_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    main_class.lpszClassName = kMainClassName;

    return RegisterClassExW(&main_class) != 0;
}

bool NoteApp::CreateWindows() {
    RECT work_area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    const auto scale = static_cast<float>(GetDpiForSystem()) / 96.0f;

    const auto launcher_width = DipToPx(theme::kLauncherWidth, scale);
    const auto launcher_height = DipToPx(theme::kLauncherCollapsedHeight, scale);
    const auto launcher_margin = DipToPx(20.0f, scale);

    launcher_hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kLauncherClassName, L"RasterNoteNative",
                                     WS_POPUP, work_area.right - launcher_width - launcher_margin,
                                     work_area.top + launcher_margin, launcher_width, launcher_height, nullptr,
                                     nullptr, instance_, this);

    if (!launcher_hwnd_) {
        return false;
    }

    const auto desired_main_width = DipToPx(theme::kMainWidth, scale);
    const auto desired_main_height = DipToPx(theme::kMainHeight, scale);
    const int screen_width = static_cast<int>(work_area.right - work_area.left);
    const int screen_height = static_cast<int>(work_area.bottom - work_area.top);
    const auto main_margin = DipToPx(28.0f, scale);
    const int main_width =
        std::min<int>(desired_main_width, std::max<int>(DipToPx(760.0f, scale), screen_width - main_margin * 2));
    const int main_height =
        std::min<int>(desired_main_height, std::max<int>(DipToPx(560.0f, scale), screen_height - main_margin * 2));
    const auto main_x = work_area.left + ((work_area.right - work_area.left) - main_width) / 2;
    const auto main_y = work_area.top + ((work_area.bottom - work_area.top) - main_height) / 2;

    constexpr DWORD kMainWindowStyle =
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;

    main_hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kMainClassName, L"RasterNoteNative", kMainWindowStyle, main_x,
                                 main_y, main_width, main_height, nullptr, nullptr, instance_, this);

    if (!main_hwnd_) {
        DestroyWindow(launcher_hwnd_);
        launcher_hwnd_ = nullptr;
        return false;
    }

    ApplyWindowStyle(main_hwnd_);
    return true;
}

int NoteApp::MessageLoop() const {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK NoteApp::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create_struct = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        auto* app = reinterpret_cast<NoteApp*>(create_struct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        return TRUE;
    }

    auto* app = reinterpret_cast<NoteApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    return app->HandleMessage(hwnd, message, wparam, lparam);
}

LRESULT NoteApp::HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (hwnd == launcher_hwnd_) {
        return HandleLauncherMessage(message, wparam, lparam);
    }
    return HandleMainMessage(message, wparam, lparam);
}

LRESULT NoteApp::HandleLauncherMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            RenderLauncher();
            return 0;
        case WM_SIZE:
            ResizeRenderTarget(launcher_hwnd_);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE && launcher_expanded_) {
                ToggleLauncherExpanded(false);
            }
            return 0;
        case WM_MOUSEMOVE: {
            const auto point = PointFromLParamDip(launcher_hwnd_, lparam);
            ResetCursor(launcher_hwnd_, point.x, point.y);
            return 0;
        }
        case WM_LBUTTONUP: {
            const auto point = PointFromLParamDip(launcher_hwnd_, lparam);
            for (const auto& hit : launcher_hits_) {
                if (!Hit(hit.rect, point.x, point.y)) {
                    continue;
                }

                switch (hit.role) {
                    case HitRole::LauncherToggle:
                        ToggleLauncherExpanded(!launcher_expanded_);
                        return 0;
                    case HitRole::LauncherNew:
                        CreateNewNote();
                        ToggleLauncherExpanded(false);
                        return 0;
                    case HitRole::LauncherQuit:
                        SaveNow();
                        DestroyWindow(launcher_hwnd_);
                        return 0;
                    case HitRole::LauncherNote:
                        OpenNote(hit.payload);
                        ToggleLauncherExpanded(false);
                        return 0;
                    default:
                        break;
                }
            }
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(launcher_hwnd_, HWND_TOPMOST, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOACTIVATE);
            launcher_target_.Reset();
            launcher_brush_.Reset();
            return 0;
        }
        case WM_CLOSE:
            SaveNow();
            DestroyWindow(launcher_hwnd_);
            return 0;
        case WM_DESTROY:
            if (main_hwnd_) {
                DestroyWindow(main_hwnd_);
                main_hwnd_ = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(launcher_hwnd_, message, wparam, lparam);
    }
}

LRESULT NoteApp::HandleMainMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCACTIVATE:
            return TRUE;
        case WM_NCHITTEST: {
            POINT screen_point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const auto hit = HitTestMainNc(screen_point);
            if (hit != HTNOWHERE) {
                return hit;
            }
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            RenderMain();
            return 0;
        case WM_SIZE:
            ResizeRenderTarget(main_hwnd_);
            InvalidateRect(main_hwnd_, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            SaveNow();
            ClearMainHover();
            ShowWindow(main_hwnd_, SW_HIDE);
            if (launcher_hwnd_) {
                ToggleLauncherExpanded(false);
                ShowWindow(launcher_hwnd_, SW_SHOWNOACTIVATE);
            }
            return 0;
        case WM_SETFOCUS:
            StartCaretBlink();
            UpdateImeWindow();
            InvalidateRect(main_hwnd_, nullptr, FALSE);
            return 0;
        case WM_KILLFOCUS:
            StopCaretBlink();
            selecting_ = false;
            if (GetCapture() == main_hwnd_) {
                ReleaseCapture();
            }
            SaveNow();
            InvalidateRect(main_hwnd_, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (wparam == kAutoSaveTimerId) {
                KillTimer(main_hwnd_, kAutoSaveTimerId);
                SaveNow();
                return 0;
            }
            if (wparam == kCaretBlinkTimerId) {
                caret_visible_ = !caret_visible_;
                InvalidateRect(main_hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == kUiAnimationTimerId) {
                TickUiAnimation();
                return 0;
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            const auto scale = ScaleFor(main_hwnd_);
            info->ptMinTrackSize.x = DipToPx(760.0f, scale);
            info->ptMinTrackSize.y = DipToPx(520.0f, scale);

            MONITORINFO monitor_info{};
            monitor_info.cbSize = sizeof(monitor_info);
            if (const auto monitor = MonitorFromWindow(main_hwnd_, MONITOR_DEFAULTTONEAREST);
                GetMonitorInfoW(monitor, &monitor_info)) {
                const auto& monitor_rect = monitor_info.rcMonitor;
                const auto& work_rect = monitor_info.rcWork;
                info->ptMaxPosition.x = work_rect.left - monitor_rect.left;
                info->ptMaxPosition.y = work_rect.top - monitor_rect.top;
                info->ptMaxSize.x = work_rect.right - work_rect.left;
                info->ptMaxSize.y = work_rect.bottom - work_rect.top;
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            const auto point = PointFromLParamDip(main_hwnd_, lparam);
            if (selecting_) {
                document_.SetCaret(HitTestEditor(point.x, point.y), true);
                ResetCaretBlink();
                preferred_caret_x_ = -1.0f;
                EnsureCaretVisible();
                UpdateImeWindow();
                InvalidateRect(main_hwnd_, nullptr, FALSE);
            } else {
                UpdateMainHover(point.x, point.y);
                ResetCursor(main_hwnd_, point.x, point.y);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            ClearMainHover();
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(main_hwnd_);
            const auto point = PointFromLParamDip(main_hwnd_, lparam);
            const auto layout = BuildMainChromeLayout();

            for (const auto& hit : main_hits_) {
                if (!Hit(hit.rect, point.x, point.y)) {
                    continue;
                }

                BeginPressedFeedback(hit.role, hit.payload);
                switch (hit.role) {
                    case HitRole::MainNew:
                        CreateNewNote();
                        return 0;
                    case HitRole::MainBold: {
                        const auto had_selection = document_.HasSelection();
                        document_.ToggleBold();
                        if (had_selection) {
                            note_dirty_ = true;
                            ScheduleSave();
                            UpdateMainCaption();
                        }
                        InvalidateLayout();
                        InvalidateRect(main_hwnd_, nullptr, FALSE);
                        return 0;
                    }
                    case HitRole::MainSave:
                        SaveNow();
                        InvalidateRect(main_hwnd_, nullptr, FALSE);
                        return 0;
                    case HitRole::MainLauncher:
                        ToggleHistoryDrawer(history_drawer_target_ <= 0.0f);
                        return 0;
                    case HitRole::MainMinimize:
                        ShowWindow(main_hwnd_, SW_MINIMIZE);
                        return 0;
                    case HitRole::MainMaximize:
                        ToggleMainMaximized();
                        return 0;
                    case HitRole::MainClose:
                        SaveNow();
                        ClearMainHover();
                        ShowWindow(main_hwnd_, SW_HIDE);
                        if (launcher_hwnd_) {
                            ToggleLauncherExpanded(false);
                            ShowWindow(launcher_hwnd_, SW_SHOWNOACTIVATE);
                        }
                        return 0;
                    case HitRole::MainHistoryNote:
                        OpenNote(hit.payload);
                        ToggleHistoryDrawer(false);
                        return 0;
                    case HitRole::MainHistoryDelete:
                        BeginDeleteConfirmation(hit.payload);
                        return 0;
                    case HitRole::MainHistoryDeleteConfirm:
                        DeleteNoteAndAdvance(hit.payload);
                        return 0;
                    case HitRole::MainHistoryDeleteCancel:
                        ClearDeleteConfirmation();
                        InvalidateRect(main_hwnd_, nullptr, FALSE);
                        return 0;
                    default:
                        break;
                }
            }

            if (history_drawer_progress_ > 0.0f && !Hit(history_drawer_rect_, point.x, point.y)) {
                ClearDeleteConfirmation();
                ClearMainHover();
                ToggleHistoryDrawer(false);
                return 0;
            }

            if (Hit(layout.drag_rect, point.x, point.y)) {
                POINT screen_point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ClientToScreen(main_hwnd_, &screen_point);
                ReleaseCapture();
                SendMessageW(main_hwnd_, WM_NCLBUTTONDOWN, HTCAPTION,
                             MAKELPARAM(screen_point.x, screen_point.y));
                return 0;
            }

            if (Hit(editor_rect_, point.x, point.y)) {
                const auto extend = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                document_.SetCaret(HitTestEditor(point.x, point.y), extend);
                ResetCaretBlink();
                preferred_caret_x_ = -1.0f;
                selecting_ = true;
                ClearMainHover();
                SetCapture(main_hwnd_);
                EnsureCaretVisible();
                UpdateImeWindow();
                InvalidateRect(main_hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (selecting_) {
                selecting_ = false;
                if (GetCapture() == main_hwnd_) {
                    ReleaseCapture();
                }
            }
            return 0;
        case WM_MOUSEWHEEL: {
            if (history_drawer_progress_ > 0.0f) {
                POINT screen_point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                const auto point = PointFromScreenDip(main_hwnd_, screen_point);
                if (Hit(history_list_rect_, point.x, point.y)) {
                    history_scroll_y_ -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 48.0f;
                    ClampHistoryScroll();
                    InvalidateRect(main_hwnd_, nullptr, FALSE);
                    return 0;
                }
            }

            EnsureTextLayout();
            const auto inner_height =
                std::max(0.0f, editor_rect_.bottom - editor_rect_.top - theme::kEditorPadding * 2.0f);
            const auto max_scroll = std::max(0.0f, layout_metrics_.height - inner_height + 4.0f);
            scroll_y_ -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 40.0f;
            scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll);
            InvalidateRect(main_hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_CHAR: {
            const auto now = GetTickCount64();
            if (!ime_dedup_chars_.empty()) {
                if ((now - ime_dedup_tick_) < 250 && ime_dedup_chars_.front() == static_cast<wchar_t>(wparam)) {
                    ime_dedup_chars_.erase(ime_dedup_chars_.begin());
                    return 0;
                }
                ime_dedup_chars_.clear();
            }

            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                return 0;
            }

            if (wparam == VK_RETURN) {
                InsertCommittedText(L"\n");
                return 0;
            }
            if (wparam == VK_TAB) {
                InsertCommittedText(L"    ");
                return 0;
            }
            if (wparam >= 32) {
                wchar_t text[2] = {static_cast<wchar_t>(wparam), L'\0'};
                InsertCommittedText(text);
                return 0;
            }
            return 0;
        }
        case WM_KEYDOWN: {
            const auto ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const auto shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

            if (ctrl) {
                switch (wparam) {
                    case 'N':
                        CreateNewNote();
                        return 0;
                    case 'S':
                        SaveNow();
                        InvalidateRect(main_hwnd_, nullptr, FALSE);
                        return 0;
                    case 'B': {
                        const auto had_selection = document_.HasSelection();
                        document_.ToggleBold();
                        if (had_selection) {
                            note_dirty_ = true;
                            ScheduleSave();
                            UpdateMainCaption();
                        }
                        InvalidateLayout();
                        InvalidateRect(main_hwnd_, nullptr, FALSE);
                        return 0;
                    }
                    case 'H':
                        ToggleHistoryDrawer(history_drawer_target_ <= 0.0f);
                        return 0;
                    case 'A':
                        document_.SelectAll();
                        ResetCaretBlink();
                        InvalidateRect(main_hwnd_, nullptr, FALSE);
                        return 0;
                    case 'C':
                        CopySelectionToClipboard();
                        return 0;
                    case 'X':
                        CutSelectionToClipboard();
                        return 0;
                    case 'V':
                        PasteFromClipboard();
                        return 0;
                    default:
                        break;
                }
            }

            switch (wparam) {
                case VK_LEFT:
                    document_.MoveCaretLeft(shift);
                    ResetCaretBlink();
                    preferred_caret_x_ = -1.0f;
                    EnsureCaretVisible();
                    UpdateImeWindow();
                    InvalidateRect(main_hwnd_, nullptr, FALSE);
                    return 0;
                case VK_RIGHT:
                    document_.MoveCaretRight(shift);
                    ResetCaretBlink();
                    preferred_caret_x_ = -1.0f;
                    EnsureCaretVisible();
                    UpdateImeWindow();
                    InvalidateRect(main_hwnd_, nullptr, FALSE);
                    return 0;
                case VK_HOME:
                    document_.MoveCaretHome(shift);
                    ResetCaretBlink();
                    preferred_caret_x_ = -1.0f;
                    EnsureCaretVisible();
                    UpdateImeWindow();
                    InvalidateRect(main_hwnd_, nullptr, FALSE);
                    return 0;
                case VK_END:
                    document_.MoveCaretEnd(shift);
                    ResetCaretBlink();
                    preferred_caret_x_ = -1.0f;
                    EnsureCaretVisible();
                    UpdateImeWindow();
                    InvalidateRect(main_hwnd_, nullptr, FALSE);
                    return 0;
                case VK_UP:
                    ResetCaretBlink();
                    MoveCaretVertical(-1, shift);
                    return 0;
                case VK_DOWN:
                    ResetCaretBlink();
                    MoveCaretVertical(1, shift);
                    return 0;
                case VK_BACK: {
                    const auto before = document_.Revision();
                    document_.DeleteBackward();
                    if (document_.Revision() != before) {
                        note_dirty_ = true;
                        ScheduleSave();
                        ResetCaretBlink();
                        InvalidateLayout();
                        EnsureCaretVisible();
                        UpdateImeWindow();
                        UpdateMainCaption();
                        InvalidateRect(main_hwnd_, nullptr, FALSE);
                    }
                    return 0;
                }
                case VK_DELETE: {
                    const auto before = document_.Revision();
                    document_.DeleteForward();
                    if (document_.Revision() != before) {
                        note_dirty_ = true;
                        ScheduleSave();
                        ResetCaretBlink();
                        InvalidateLayout();
                        EnsureCaretVisible();
                        UpdateImeWindow();
                        UpdateMainCaption();
                        InvalidateRect(main_hwnd_, nullptr, FALSE);
                    }
                    return 0;
                }
                case VK_PRIOR:
                    ResetCaretBlink();
                    scroll_y_ = std::max(0.0f, scroll_y_ - 280.0f);
                    InvalidateRect(main_hwnd_, nullptr, FALSE);
                    return 0;
                case VK_NEXT:
                    ResetCaretBlink();
                    scroll_y_ += 280.0f;
                    EnsureCaretVisible();
                    InvalidateRect(main_hwnd_, nullptr, FALSE);
                    return 0;
                case VK_ESCAPE:
                    if (!pending_delete_id_.empty()) {
                        ClearDeleteConfirmation();
                    } else if (history_drawer_open_ || history_drawer_progress_ > 0.0f) {
                        ToggleHistoryDrawer(false);
                    } else if (launcher_expanded_) {
                        ToggleLauncherExpanded(false);
                    } else {
                        ShowWindow(main_hwnd_, SW_HIDE);
                        if (launcher_hwnd_) {
                            ToggleLauncherExpanded(false);
                            ShowWindow(launcher_hwnd_, SW_SHOWNOACTIVATE);
                        }
                    }
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_IME_STARTCOMPOSITION:
            ime_preview_.clear();
            ResetCaretBlink();
            InvalidateLayout();
            UpdateImeWindow();
            InvalidateRect(main_hwnd_, nullptr, FALSE);
            return 0;
        case WM_IME_COMPOSITION:
            HandleImeComposition(lparam);
            return 0;
        case WM_IME_ENDCOMPOSITION:
            ime_preview_.clear();
            ResetCaretBlink();
            InvalidateLayout();
            InvalidateRect(main_hwnd_, nullptr, FALSE);
            return 0;
        case WM_IME_CHAR:
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(main_hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyWindowStyle(main_hwnd_);
            main_target_.Reset();
            main_brush_.Reset();
            InvalidateLayout();
            return 0;
        }
        default:
            break;
    }

    return DefWindowProcW(main_hwnd_, message, wparam, lparam);
}

void NoteApp::RenderLauncher() {
    PAINTSTRUCT paint{};
    BeginPaint(launcher_hwnd_, &paint);

    if (!EnsureRenderTarget(launcher_hwnd_, d2d_factory_.Get(), launcher_target_, launcher_brush_)) {
        EndPaint(launcher_hwnd_, &paint);
        return;
    }

    launcher_hits_.clear();
    const auto size = ClientSizeDip(launcher_hwnd_);
    const auto root = D2D1::RectF(0.0f, 0.0f, size.width, size.height);
    const auto header = D2D1::RectF(0.0f, 0.0f, size.width, 54.0f);
    const auto toggle_rect = D2D1::RectF(size.width - 92.0f, 12.0f, size.width - 14.0f, 44.0f);

    launcher_target_->BeginDraw();
    launcher_target_->Clear(theme::Paper());

    launcher_brush_->SetColor(theme::Border());
    launcher_target_->DrawRectangle(root, launcher_brush_.Get(), 1.0f);
    launcher_target_->DrawLine(D2D1::Point2F(0.0f, header.bottom), D2D1::Point2F(size.width, header.bottom),
                               launcher_brush_.Get(), 1.0f);

    launcher_brush_->SetColor(theme::Ink());
    const auto label_rect = D2D1::RectF(16.0f, 10.0f, size.width - 110.0f, 26.0f);
    launcher_target_->DrawText(L"RASTER NOTE FLOAT", _countof(L"RASTER NOTE FLOAT") - 1, label_format_.Get(),
                               label_rect, launcher_brush_.Get());

    DrawButton(launcher_target_.Get(), launcher_brush_.Get(), meta_format_.Get(), toggle_rect,
               launcher_expanded_ ? L"CLOSE" : L"OPEN", launcher_expanded_);
    launcher_hits_.push_back({toggle_rect, HitRole::LauncherToggle, {}});

    if (!launcher_expanded_) {
        const std::wstring preview =
            recent_notes_.empty() ? L"OPEN TO REVEAL RECENT NOTES" : recent_notes_.front().title;
        launcher_brush_->SetColor(theme::Dim());
        const auto preview_rect = D2D1::RectF(16.0f, 50.0f, size.width - 16.0f, size.height - 10.0f);
        launcher_target_->DrawText(preview.c_str(), static_cast<UINT32>(preview.size()), meta_format_.Get(),
                                   preview_rect, launcher_brush_.Get());
    } else {
        launcher_brush_->SetColor(theme::Dim());
        launcher_target_->DrawText(L"RECENT", _countof(L"RECENT") - 1, label_format_.Get(),
                                   D2D1::RectF(16.0f, 64.0f, 120.0f, 80.0f), launcher_brush_.Get());

        float top = 84.0f;
        if (recent_notes_.empty()) {
            launcher_brush_->SetColor(theme::Hint());
            const auto empty_rect = D2D1::RectF(16.0f, top + 12.0f, size.width - 16.0f, top + 80.0f);
            launcher_target_->DrawText(L"NO NOTES YET. CREATE THE FIRST ONE.",
                                       _countof(L"NO NOTES YET. CREATE THE FIRST ONE.") - 1, meta_format_.Get(),
                                       empty_rect, launcher_brush_.Get());
        } else {
            const auto visible_count = std::min<std::size_t>(recent_notes_.size(), 6);
            for (std::size_t i = 0; i < visible_count; ++i) {
                const auto& note = recent_notes_[i];
                const auto row = D2D1::RectF(16.0f, top, size.width - 16.0f, top + theme::kListRowHeight);
                launcher_brush_->SetColor(note.id == current_note_.id ? theme::Selection() : theme::Panel());
                launcher_target_->FillRectangle(row, launcher_brush_.Get());
                launcher_brush_->SetColor(theme::Border());
                launcher_target_->DrawRectangle(row, launcher_brush_.Get(), 1.0f);

                launcher_brush_->SetColor(theme::Ink());
                const auto title_rect = D2D1::RectF(row.left + 10.0f, row.top + 6.0f, row.right - 10.0f,
                                                    row.top + 28.0f);
                launcher_target_->DrawText(note.title.c_str(), static_cast<UINT32>(note.title.size()),
                                           meta_format_.Get(), title_rect, launcher_brush_.Get());

                launcher_brush_->SetColor(theme::Dim());
                const auto preview_rect =
                    D2D1::RectF(row.left + 10.0f, row.top + 24.0f, row.right - 100.0f, row.bottom - 8.0f);
                launcher_target_->DrawText(note.preview.c_str(), static_cast<UINT32>(note.preview.size()),
                                           meta_format_.Get(), preview_rect, launcher_brush_.Get());

                const auto time_string = FormatTimestamp(note.modified_utc);
                const auto time_rect =
                    D2D1::RectF(row.right - 92.0f, row.top + 24.0f, row.right - 10.0f, row.bottom - 8.0f);
                launcher_target_->DrawText(time_string.c_str(), static_cast<UINT32>(time_string.size()),
                                           meta_format_.Get(), time_rect, launcher_brush_.Get());

                launcher_hits_.push_back({row, HitRole::LauncherNote, note.id});
                top += theme::kListRowHeight + 8.0f;
            }
        }

        const auto new_rect = D2D1::RectF(16.0f, size.height - 52.0f, 132.0f, size.height - 16.0f);
        DrawButton(launcher_target_.Get(), launcher_brush_.Get(), meta_format_.Get(), new_rect, L"NEW NOTE", false);
        launcher_hits_.push_back({new_rect, HitRole::LauncherNew, {}});

        const auto quit_rect = D2D1::RectF(size.width - 96.0f, size.height - 52.0f, size.width - 16.0f,
                                           size.height - 16.0f);
        DrawButton(launcher_target_.Get(), launcher_brush_.Get(), meta_format_.Get(), quit_rect, L"EXIT", false);
        launcher_hits_.push_back({quit_rect, HitRole::LauncherQuit, {}});
    }

    if (launcher_target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        launcher_target_.Reset();
        launcher_brush_.Reset();
    }

    EndPaint(launcher_hwnd_, &paint);
}

void NoteApp::RenderMain() {
    PAINTSTRUCT paint{};
    BeginPaint(main_hwnd_, &paint);

    if (!EnsureRenderTarget(main_hwnd_, d2d_factory_.Get(), main_target_, main_brush_)) {
        EndPaint(main_hwnd_, &paint);
        return;
    }

    main_target_->BeginDraw();
    main_target_->Clear(theme::Paper());
    main_hits_.clear();

    const auto layout = BuildMainChromeLayout();
    const auto title = NoteStore::DeriveTitle(document_.Text());
    const auto bold_active = document_.HasSelection()
                                 ? document_.IsRangeFullyBold(document_.SelectionStart(),
                                                              document_.SelectionEnd() - document_.SelectionStart())
                                 : document_.PendingBold();
    const auto maximized = IsMainMaximized();
    const std::wstring subtitle =
        note_dirty_ ? L"LOCAL NOTE STORE / UNSAVED / CTRL+S TO COMMIT"
                    : L"LOCAL NOTE STORE / AUTOSAVE READY / CTRL+B TOGGLE";
    const std::wstring editor_meta_left = L"PLAIN TEXT / BOLD ONLY";
    const std::wstring editor_meta_right = L"CTRL+H NOTES / CTRL+B / CTRL+S";
    const std::wstring footer_left = L"LOCAL FILE";
    std::wstringstream footer_right_stream;
    footer_right_stream << L"CHARS " << document_.Length() << L"    SAVED "
                        << FormatTimestamp(current_note_.modified_utc);
    const auto footer_right = footer_right_stream.str();
    const auto main_focused = GetFocus() == main_hwnd_;

    editor_rect_ = layout.editor_rect;
    history_drawer_rect_ = {};
    history_list_rect_ = {};
    history_content_height_ = 0.0f;

    main_brush_->SetColor(theme::Chrome());
    main_target_->FillRectangle(layout.chrome_rect, main_brush_.Get());
    main_brush_->SetColor(theme::Card());
    main_target_->FillRectangle(layout.editor_card_rect, main_brush_.Get());
    main_brush_->SetColor(theme::Panel());
    main_target_->FillRectangle(layout.editor_band_rect, main_brush_.Get());
    main_target_->FillRectangle(layout.footer_band_rect, main_brush_.Get());

    main_brush_->SetColor(theme::Border());
    main_target_->DrawRectangle(layout.window_rect, main_brush_.Get(), theme::kRootBorderThickness);
    main_target_->DrawRectangle(layout.editor_card_rect, main_brush_.Get(), 1.0f);
    if (main_focused) {
        main_brush_->SetColor(WithAlpha(theme::Accent(), 0.14f));
        main_target_->DrawRectangle(D2D1::RectF(layout.editor_card_rect.left + 3.0f, layout.editor_card_rect.top + 3.0f,
                                                layout.editor_card_rect.right - 3.0f,
                                                layout.editor_card_rect.bottom - 3.0f),
                                    main_brush_.Get(), 1.0f);
        main_brush_->SetColor(theme::Border());
    }
    main_target_->DrawLine(D2D1::Point2F(layout.header_rect.left, layout.header_rect.bottom),
                           D2D1::Point2F(layout.header_rect.right, layout.header_rect.bottom), main_brush_.Get(),
                           1.0f);
    main_target_->DrawLine(D2D1::Point2F(layout.editor_band_rect.left, layout.editor_band_rect.bottom),
                           D2D1::Point2F(layout.editor_band_rect.right, layout.editor_band_rect.bottom),
                           main_brush_.Get(), 1.0f);
    main_target_->DrawLine(D2D1::Point2F(layout.footer_band_rect.left, layout.footer_band_rect.top),
                           D2D1::Point2F(layout.footer_band_rect.right, layout.footer_band_rect.top),
                           main_brush_.Get(), 1.0f);

    main_brush_->SetColor(theme::Dim());
    main_target_->DrawText(L"MAIN WINDOW / FRAMELESS / NATIVE",
                           _countof(L"MAIN WINDOW / FRAMELESS / NATIVE") - 1, label_format_.Get(),
                           layout.utility_rect, main_brush_.Get());

    main_brush_->SetColor(theme::Ink());
    main_target_->DrawText(title.c_str(), static_cast<UINT32>(title.size()), section_format_.Get(),
                           layout.title_rect, main_brush_.Get());
    main_brush_->SetColor(theme::Dim());
    main_target_->DrawText(subtitle.c_str(), static_cast<UINT32>(subtitle.size()), meta_format_.Get(),
                           layout.subtitle_rect, main_brush_.Get());

    DrawButton(main_target_.Get(), main_brush_.Get(), button_format_.Get(), layout.new_button, L"NEW", false,
               IsMainHovered(HitRole::MainNew), IsPressedFeedback(HitRole::MainNew));
    DrawButton(main_target_.Get(), main_brush_.Get(), button_format_.Get(), layout.bold_button, L"BOLD", bold_active,
               IsMainHovered(HitRole::MainBold), IsPressedFeedback(HitRole::MainBold));
    DrawButton(main_target_.Get(), main_brush_.Get(), button_format_.Get(), layout.save_button, L"SAVE", note_dirty_,
               IsMainHovered(HitRole::MainSave), IsPressedFeedback(HitRole::MainSave), note_dirty_ && main_focused);
    DrawButton(main_target_.Get(), main_brush_.Get(), button_format_.Get(), layout.launcher_button, L"NOTES",
               history_drawer_target_ > 0.0f || history_drawer_progress_ > 0.0f, IsMainHovered(HitRole::MainLauncher),
               IsPressedFeedback(HitRole::MainLauncher),
               main_focused && (history_drawer_target_ > 0.0f || history_drawer_progress_ > 0.0f));
    DrawButton(main_target_.Get(), main_brush_.Get(), button_format_.Get(), layout.minimize_button, L"MIN", false,
               IsMainHovered(HitRole::MainMinimize), IsPressedFeedback(HitRole::MainMinimize));
    DrawButton(main_target_.Get(), main_brush_.Get(), button_format_.Get(), layout.maximize_button,
               maximized ? L"REST" : L"MAX", false, IsMainHovered(HitRole::MainMaximize),
               IsPressedFeedback(HitRole::MainMaximize));
    DrawButton(main_target_.Get(), main_brush_.Get(), button_format_.Get(), layout.close_button, L"HIDE", false,
               IsMainHovered(HitRole::MainClose), IsPressedFeedback(HitRole::MainClose));

    main_hits_.push_back({layout.new_button, HitRole::MainNew, {}});
    main_hits_.push_back({layout.bold_button, HitRole::MainBold, {}});
    main_hits_.push_back({layout.save_button, HitRole::MainSave, {}});
    main_hits_.push_back({layout.launcher_button, HitRole::MainLauncher, {}});
    main_hits_.push_back({layout.minimize_button, HitRole::MainMinimize, {}});
    main_hits_.push_back({layout.maximize_button, HitRole::MainMaximize, {}});
    main_hits_.push_back({layout.close_button, HitRole::MainClose, {}});

    main_brush_->SetColor(theme::Dim());
    const auto editor_meta_left_rect =
        D2D1::RectF(layout.editor_band_rect.left + 12.0f, layout.editor_band_rect.top + 7.0f,
                    layout.editor_band_rect.left + 260.0f, layout.editor_band_rect.bottom - 4.0f);
    const auto editor_meta_right_rect =
        D2D1::RectF(layout.editor_band_rect.right - 260.0f, layout.editor_band_rect.top + 7.0f,
                    layout.editor_band_rect.right - 12.0f, layout.editor_band_rect.bottom - 4.0f);
    main_target_->DrawText(editor_meta_left.c_str(), static_cast<UINT32>(editor_meta_left.size()), label_format_.Get(),
                           editor_meta_left_rect, main_brush_.Get());
    main_target_->DrawText(editor_meta_right.c_str(), static_cast<UINT32>(editor_meta_right.size()),
                           meta_right_format_.Get(), editor_meta_right_rect, main_brush_.Get());

    EnsureTextLayout();

    const auto content_origin =
        D2D1::Point2F(editor_rect_.left + 12.0f, editor_rect_.top + 10.0f - scroll_y_);

    main_target_->PushAxisAlignedClip(editor_rect_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (document_.HasSelection() && text_layout_) {
        const auto display_start = MapDocToDisplay(document_.SelectionStart());
        const auto display_end = MapDocToDisplay(document_.SelectionEnd());
        const auto display_length = display_end > display_start ? display_end - display_start : 0U;
        if (display_length != 0U) {
            UINT32 actual_count = 0;
            text_layout_->HitTestTextRange(display_start, display_length, 0.0f, 0.0f, nullptr, 0, &actual_count);
            if (actual_count != 0U) {
                std::vector<DWRITE_HIT_TEST_METRICS> boxes(actual_count);
                text_layout_->HitTestTextRange(display_start, display_length, 0.0f, 0.0f, boxes.data(), actual_count,
                                               &actual_count);
                main_brush_->SetColor(theme::Selection());
                for (UINT32 i = 0; i < actual_count; ++i) {
                    const auto rect = D2D1::RectF(content_origin.x + boxes[i].left, content_origin.y + boxes[i].top,
                                                  content_origin.x + boxes[i].left + boxes[i].width,
                                                  content_origin.y + boxes[i].top + boxes[i].height);
                    main_target_->FillRectangle(rect, main_brush_.Get());
                }
            }
        }
    }

    if (text_layout_) {
        main_target_->SetTransform(D2D1::Matrix3x2F::Translation(content_origin.x, content_origin.y));
        main_brush_->SetColor(theme::Ink());
        main_target_->DrawTextLayout(D2D1::Point2F(0.0f, 0.0f), text_layout_.Get(), main_brush_.Get());
        main_target_->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    if (layout_text_.empty() && ime_preview_.empty()) {
        main_brush_->SetColor(theme::Hint());
        const auto hint_rect = D2D1::RectF(editor_rect_.left + 12.0f, editor_rect_.top + 38.0f,
                                           editor_rect_.right - 12.0f, editor_rect_.top + 58.0f);
        main_target_->DrawText(L"Press Ctrl+B for bold. Autosave is enabled.",
                               _countof(L"Press Ctrl+B for bold. Autosave is enabled.") - 1, meta_format_.Get(),
                               hint_rect, main_brush_.Get());
    }

    if (main_focused && caret_visible_ && text_layout_) {
        float caret_x = 0.0f;
        float caret_y = 0.0f;
        DWRITE_HIT_TEST_METRICS metrics{};
        HitTestCaret(text_layout_.Get(), MapDocToDisplay(document_.Caret()),
                     static_cast<std::uint32_t>(layout_text_.size()), &caret_x, &caret_y, &metrics);

        const auto caret_rect = D2D1::RectF(content_origin.x + caret_x,
                                            content_origin.y + caret_y,
                                            content_origin.x + caret_x + theme::kCaretWidth,
                                            content_origin.y + caret_y + metrics.height);
        main_brush_->SetColor(theme::Accent());
        main_target_->FillRectangle(caret_rect, main_brush_.Get());
    }

    main_target_->PopAxisAlignedClip();

    main_brush_->SetColor(theme::Dim());
    const auto footer_left_rect =
        D2D1::RectF(layout.footer_rect.left, layout.footer_rect.top, layout.footer_rect.left + 180.0f,
                    layout.footer_rect.bottom);
    const auto footer_right_rect =
        D2D1::RectF(layout.footer_rect.right - 340.0f, layout.footer_rect.top, layout.footer_rect.right,
                    layout.footer_rect.bottom);
    main_target_->DrawText(footer_left.c_str(), static_cast<UINT32>(footer_left.size()), meta_format_.Get(),
                           footer_left_rect, main_brush_.Get());
    main_target_->DrawText(footer_right.c_str(), static_cast<UINT32>(footer_right.size()),
                           meta_right_format_.Get(), footer_right_rect, main_brush_.Get());

    if (history_drawer_progress_ > 0.0f) {
        const auto drawer_width = theme::kHistoryDrawerWidth * history_drawer_progress_;
        const auto drawer_left = layout.editor_card_rect.right - drawer_width;
        history_drawer_rect_ =
            D2D1::RectF(drawer_left, layout.chrome_rect.bottom + 1.0f, layout.editor_card_rect.right,
                        layout.editor_card_rect.bottom);
        const auto scrim_rect =
            D2D1::RectF(layout.editor_card_rect.left, history_drawer_rect_.top, history_drawer_rect_.left,
                        history_drawer_rect_.bottom);
        const auto drawer_header_rect =
            D2D1::RectF(history_drawer_rect_.left + 1.0f, history_drawer_rect_.top + 1.0f, history_drawer_rect_.right - 1.0f,
                        history_drawer_rect_.top + theme::kHistoryDrawerHeaderHeight);
        history_list_rect_ = D2D1::RectF(history_drawer_rect_.left + 12.0f, drawer_header_rect.bottom + 10.0f,
                                         history_drawer_rect_.right - 12.0f, history_drawer_rect_.bottom - 12.0f);

        main_brush_->SetColor(D2D1::ColorF(0x151515, 0.10f * history_drawer_progress_));
        main_target_->FillRectangle(scrim_rect, main_brush_.Get());

        main_brush_->SetColor(theme::Chrome());
        main_target_->FillRectangle(history_drawer_rect_, main_brush_.Get());
        main_brush_->SetColor(theme::Card());
        main_target_->FillRectangle(drawer_header_rect, main_brush_.Get());
        main_brush_->SetColor(WithAlpha(theme::Accent(), 0.08f * history_drawer_progress_));
        main_target_->FillRectangle(
            D2D1::RectF(history_drawer_rect_.left, history_drawer_rect_.top, history_drawer_rect_.left + 4.0f,
                        history_drawer_rect_.bottom),
            main_brush_.Get());
        main_brush_->SetColor(theme::Border());
        main_target_->DrawRectangle(history_drawer_rect_, main_brush_.Get(), 1.0f);
        main_target_->DrawLine(D2D1::Point2F(drawer_header_rect.left, drawer_header_rect.bottom),
                               D2D1::Point2F(drawer_header_rect.right, drawer_header_rect.bottom), main_brush_.Get(),
                               1.0f);

        const std::wstring drawer_title = L"RECENT / LOCAL";
        const std::wstring drawer_hint = L"ESC CLOSE";
        std::wstringstream drawer_count_stream;
        drawer_count_stream << recent_notes_.size() << L" NOTES";
        const auto drawer_count = drawer_count_stream.str();

        main_brush_->SetColor(theme::Ink());
        main_target_->DrawText(drawer_title.c_str(), static_cast<UINT32>(drawer_title.size()), label_format_.Get(),
                               D2D1::RectF(drawer_header_rect.left + 12.0f, drawer_header_rect.top + 10.0f,
                                           drawer_header_rect.left + 180.0f, drawer_header_rect.bottom - 6.0f),
                               main_brush_.Get());
        main_brush_->SetColor(theme::Dim());
        main_target_->DrawText(drawer_hint.c_str(), static_cast<UINT32>(drawer_hint.size()), meta_format_.Get(),
                               D2D1::RectF(drawer_header_rect.right - 220.0f, drawer_header_rect.top + 10.0f,
                                           drawer_header_rect.right - 116.0f, drawer_header_rect.bottom - 6.0f),
                               main_brush_.Get());
        main_target_->DrawText(drawer_count.c_str(), static_cast<UINT32>(drawer_count.size()),
                               meta_right_format_.Get(),
                               D2D1::RectF(drawer_header_rect.right - 112.0f, drawer_header_rect.top + 10.0f,
                                           drawer_header_rect.right - 12.0f, drawer_header_rect.bottom - 6.0f),
                               main_brush_.Get());

        history_content_height_ = 0.0f;
        if (!recent_notes_.empty()) {
            history_content_height_ =
                recent_notes_.size() * (theme::kHistoryRowHeight + theme::kHistoryRowGap) - theme::kHistoryRowGap;
        }
        ClampHistoryScroll();

        main_target_->PushAxisAlignedClip(history_list_rect_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (recent_notes_.empty()) {
            main_brush_->SetColor(theme::Hint());
            main_target_->DrawText(L"NO PREVIOUS NOTES.", _countof(L"NO PREVIOUS NOTES.") - 1, meta_format_.Get(),
                                   D2D1::RectF(history_list_rect_.left, history_list_rect_.top + 12.0f,
                                               history_list_rect_.right, history_list_rect_.top + 34.0f),
                                   main_brush_.Get());
        } else {
            for (std::size_t i = 0; i < recent_notes_.size(); ++i) {
                const auto& note = recent_notes_[i];
                const auto row_top = history_list_rect_.top +
                                     static_cast<float>(i) * (theme::kHistoryRowHeight + theme::kHistoryRowGap) -
                                     history_scroll_y_;
                const auto row_bottom = row_top + theme::kHistoryRowHeight;
                if (row_bottom < history_list_rect_.top || row_top > history_list_rect_.bottom) {
                    continue;
                }

                const auto row_rect =
                    D2D1::RectF(history_list_rect_.left, row_top, history_list_rect_.right, row_bottom);
                const auto is_current = note.id == current_note_.id;
                const auto is_pending = note.id == pending_delete_id_;
                const auto row_hovered = IsMainHovered(HitRole::MainHistoryNote, note.id) ||
                                         IsMainHovered(HitRole::MainHistoryDelete, note.id) ||
                                         IsMainHovered(HitRole::MainHistoryDeleteConfirm, note.id) ||
                                         IsMainHovered(HitRole::MainHistoryDeleteCancel, note.id);
                const auto row_pressed = IsPressedFeedback(HitRole::MainHistoryNote, note.id) ||
                                         IsPressedFeedback(HitRole::MainHistoryDelete, note.id) ||
                                         IsPressedFeedback(HitRole::MainHistoryDeleteConfirm, note.id) ||
                                         IsPressedFeedback(HitRole::MainHistoryDeleteCancel, note.id);

                auto row_fill = is_current ? theme::Selection() : theme::Card();
                if (row_hovered) {
                    row_fill = is_current ? BlendColor(theme::Selection(), theme::Chrome(), 0.35f)
                                          : BlendColor(theme::Card(), theme::Panel(), 0.72f);
                }
                if (is_pending) {
                    row_fill = BlendColor(row_fill, theme::Panel(), 0.86f * pending_delete_progress_);
                }
                if (row_pressed) {
                    row_fill = BlendColor(row_fill, theme::Selection(), 0.52f);
                }

                main_brush_->SetColor(row_fill);
                main_target_->FillRectangle(row_rect, main_brush_.Get());
                if (is_current || row_hovered || is_pending) {
                    const auto marker_color =
                        is_pending ? WithAlpha(theme::Accent(), 0.92f)
                                   : (is_current ? WithAlpha(theme::Accent(), 0.32f) : WithAlpha(theme::Ink(), 0.12f));
                    main_brush_->SetColor(marker_color);
                    main_target_->FillRectangle(
                        D2D1::RectF(row_rect.left, row_rect.top, row_rect.left + 3.0f, row_rect.bottom),
                        main_brush_.Get());
                }
                main_brush_->SetColor(is_pending ? WithAlpha(theme::Accent(), 0.70f)
                                                 : (row_hovered ? theme::Ink() : theme::Border()));
                main_target_->DrawRectangle(row_rect, main_brush_.Get(), 1.0f);

                const auto content_right = row_rect.right - (is_pending ? 12.0f : 88.0f);
                const auto title_rect =
                    D2D1::RectF(row_rect.left + 10.0f, row_rect.top + 8.0f, content_right, row_rect.top + 28.0f);
                const auto preview_rect =
                    D2D1::RectF(row_rect.left + 10.0f, row_rect.top + 29.0f, content_right, row_rect.top + 47.0f);
                const auto time_text = FormatTimestamp(note.modified_utc);

                main_brush_->SetColor(is_pending ? theme::Accent() : theme::Ink());
                main_target_->DrawText(note.title.c_str(), static_cast<UINT32>(note.title.size()), meta_format_.Get(),
                                       title_rect, main_brush_.Get());
                if (!is_pending) {
                    main_brush_->SetColor(theme::Dim());
                    main_target_->DrawText(note.preview.c_str(), static_cast<UINT32>(note.preview.size()),
                                           meta_format_.Get(), preview_rect, main_brush_.Get());
                }

                if (is_pending) {
                    const auto shift = (1.0f - pending_delete_progress_) * 12.0f;
                    const auto confirm_panel_rect =
                        D2D1::RectF(row_rect.left + 10.0f + shift, row_rect.top + 34.0f, row_rect.right - 10.0f,
                                    row_rect.bottom - 10.0f);
                    const auto confirm_hovered = IsMainHovered(HitRole::MainHistoryDeleteConfirm, note.id);
                    const auto confirm_pressed = IsPressedFeedback(HitRole::MainHistoryDeleteConfirm, note.id);
                    const auto cancel_hovered = IsMainHovered(HitRole::MainHistoryDeleteCancel, note.id);
                    const auto cancel_pressed = IsPressedFeedback(HitRole::MainHistoryDeleteCancel, note.id);
                    const auto button_top = confirm_panel_rect.top + 4.0f;
                    const auto cancel_rect = D2D1::RectF(confirm_panel_rect.right - 60.0f, button_top,
                                                         confirm_panel_rect.right - 4.0f, button_top + 22.0f);
                    const auto confirm_rect = D2D1::RectF(cancel_rect.left - 68.0f, button_top,
                                                          cancel_rect.left - 4.0f, button_top + 22.0f);
                    const auto prompt_rect =
                        D2D1::RectF(confirm_panel_rect.left + 8.0f, confirm_panel_rect.top + 4.0f, confirm_rect.left - 8.0f,
                                    confirm_panel_rect.top + 18.0f);
                    const auto caution_rect = D2D1::RectF(confirm_panel_rect.left + 8.0f, confirm_panel_rect.top + 18.0f,
                                                          confirm_rect.left - 8.0f, confirm_panel_rect.bottom - 3.0f);

                    main_brush_->SetColor(WithAlpha(theme::Card(), 0.94f));
                    main_target_->FillRectangle(confirm_panel_rect, main_brush_.Get());
                    main_brush_->SetColor(WithAlpha(theme::Accent(), 0.16f + 0.18f * pending_delete_progress_));
                    main_target_->DrawRectangle(confirm_panel_rect, main_brush_.Get(), 1.0f);
                    main_brush_->SetColor(theme::Accent());
                    main_target_->DrawText(L"DELETE LOCAL FILE?", _countof(L"DELETE LOCAL FILE?") - 1,
                                           label_format_.Get(), prompt_rect, main_brush_.Get());
                    main_brush_->SetColor(theme::Dim());
                    main_target_->DrawText(L"PERMANENT / NO UNDO", _countof(L"PERMANENT / NO UNDO") - 1,
                                           meta_format_.Get(), caution_rect, main_brush_.Get());
                    DrawButton(main_target_.Get(), main_brush_.Get(), button_format_.Get(), confirm_rect, L"CONFIRM",
                               true, confirm_hovered, confirm_pressed, true);
                    DrawButton(main_target_.Get(), main_brush_.Get(), button_format_.Get(), cancel_rect, L"CANCEL",
                               false, cancel_hovered, cancel_pressed);
                    main_hits_.push_back({confirm_rect, HitRole::MainHistoryDeleteConfirm, note.id});
                    main_hits_.push_back({cancel_rect, HitRole::MainHistoryDeleteCancel, note.id});
                } else {
                    const auto delete_hovered = IsMainHovered(HitRole::MainHistoryDelete, note.id);
                    const auto delete_pressed = IsPressedFeedback(HitRole::MainHistoryDelete, note.id);
                    const auto delete_rect =
                        D2D1::RectF(row_rect.right - theme::kHistoryDeleteButtonWidth - 10.0f, row_rect.top + 9.0f,
                                    row_rect.right - 10.0f, row_rect.top + 31.0f);
                    const auto open_rect =
                        D2D1::RectF(row_rect.left, row_rect.top, delete_rect.left - 8.0f, row_rect.bottom);
                    const auto time_rect =
                        D2D1::RectF(row_rect.right - 160.0f, row_rect.top + 50.0f, row_rect.right - 10.0f, row_rect.bottom - 8.0f);
                    main_brush_->SetColor(is_current ? BlendColor(theme::Dim(), theme::Ink(), 0.24f) : theme::Dim());
                    main_target_->DrawText(time_text.c_str(), static_cast<UINT32>(time_text.size()),
                                           meta_right_format_.Get(), time_rect, main_brush_.Get());
                    DrawButton(main_target_.Get(), main_brush_.Get(), button_format_.Get(), delete_rect, L"DELETE",
                               false, delete_hovered, delete_pressed);
                    main_hits_.push_back({open_rect, HitRole::MainHistoryNote, note.id});
                    main_hits_.push_back({delete_rect, HitRole::MainHistoryDelete, note.id});
                }
            }
        }
        main_target_->PopAxisAlignedClip();
    }

    if (main_target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        main_target_.Reset();
        main_brush_.Reset();
    }

    EndPaint(main_hwnd_, &paint);
}

void NoteApp::ResizeRenderTarget(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const auto size =
        D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(1, client.right - client.left)),
                    static_cast<UINT32>(std::max<LONG>(1, client.bottom - client.top)));

    if (hwnd == launcher_hwnd_ && launcher_target_) {
        launcher_target_->Resize(size);
    } else if (hwnd == main_hwnd_ && main_target_) {
        main_target_->Resize(size);
        InvalidateLayout();
    }
}

void NoteApp::InvalidateLayout() {
    text_layout_.Reset();
    layout_width_ = 0.0f;
    layout_revision_ = 0;
    layout_text_.clear();
    ZeroMemory(&layout_metrics_, sizeof(layout_metrics_));
}

void NoteApp::EnsureTextLayout() {
    if (!dwrite_factory_ || !editor_format_) {
        return;
    }

    const auto content_width =
        std::max(120.0f, editor_rect_.right - editor_rect_.left - theme::kEditorPadding * 2.0f);
    std::wstring display_text = document_.Text();
    if (!ime_preview_.empty()) {
        display_text.insert(display_text.begin() + document_.Caret(), ime_preview_.begin(), ime_preview_.end());
    }

    if (text_layout_ && layout_revision_ == document_.Revision() && std::fabs(layout_width_ - content_width) < 0.1f &&
        layout_text_ == display_text) {
        return;
    }

    layout_text_ = std::move(display_text);
    layout_width_ = content_width;
    layout_revision_ = document_.Revision();

    std::wstring render_text = layout_text_;
    if (render_text.empty()) {
        render_text = L" ";
    }

    dwrite_factory_->CreateTextLayout(render_text.c_str(), static_cast<UINT32>(render_text.size()),
                                      editor_format_.Get(), content_width, 100000.0f, &text_layout_);
    if (!text_layout_) {
        return;
    }

    const auto caret = document_.Caret();
    const auto preview_length = static_cast<std::uint32_t>(ime_preview_.size());
    for (const auto& range : document_.BoldRanges()) {
        const auto range_end = range.start + range.length;
        if (preview_length == 0U || range_end <= caret) {
            text_layout_->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, {range.start, range.length});
            continue;
        }
        if (range.start >= caret) {
            text_layout_->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, {range.start + preview_length, range.length});
            continue;
        }

        text_layout_->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, {range.start, caret - range.start});
        text_layout_->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD,
                                    {caret + preview_length, range_end - caret});
    }

    if (preview_length != 0U) {
        const DWRITE_TEXT_RANGE comp_range{caret, preview_length};
        text_layout_->SetUnderline(TRUE, comp_range);
        if (document_.PendingBold()) {
            text_layout_->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, comp_range);
        }
    }

    text_layout_->GetMetrics(&layout_metrics_);
    const auto inner_height =
        std::max(0.0f, editor_rect_.bottom - editor_rect_.top - theme::kEditorPadding * 2.0f);
    const auto max_scroll = std::max(0.0f, layout_metrics_.height - inner_height + 4.0f);
    scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll);
}

void NoteApp::EnsureCaretVisible() {
    EnsureTextLayout();
    if (!text_layout_) {
        return;
    }

    float caret_x = 0.0f;
    float caret_y = 0.0f;
    DWRITE_HIT_TEST_METRICS metrics{};
    HitTestCaret(text_layout_.Get(), MapDocToDisplay(document_.Caret()),
                 static_cast<std::uint32_t>(layout_text_.size()), &caret_x, &caret_y, &metrics);

    const auto inner_height =
        std::max(0.0f, editor_rect_.bottom - editor_rect_.top - theme::kEditorPadding * 2.0f);
    const auto max_scroll = std::max(0.0f, layout_metrics_.height - inner_height + 4.0f);

    if (caret_y < scroll_y_) {
        scroll_y_ = std::max(0.0f, caret_y - 6.0f);
    } else if ((caret_y + metrics.height) > (scroll_y_ + inner_height)) {
        scroll_y_ = std::min(max_scroll, caret_y + metrics.height - inner_height + 6.0f);
    }
}

void NoteApp::MoveCaretVertical(int direction, bool extend_selection) {
    EnsureTextLayout();
    if (!text_layout_) {
        return;
    }

    float caret_x = 0.0f;
    float caret_y = 0.0f;
    DWRITE_HIT_TEST_METRICS metrics{};
    HitTestCaret(text_layout_.Get(), MapDocToDisplay(document_.Caret()),
                 static_cast<std::uint32_t>(layout_text_.size()), &caret_x, &caret_y, &metrics);

    if (preferred_caret_x_ < 0.0f) {
        preferred_caret_x_ = caret_x;
    }

    const auto probe_y = direction < 0 ? std::max(0.0f, caret_y - 4.0f) : caret_y + metrics.height + 4.0f;
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS hit{};
    text_layout_->HitTestPoint(preferred_caret_x_, probe_y, &trailing, &inside, &hit);

    auto display_position = hit.textPosition + (trailing ? 1U : 0U);
    display_position = std::min(display_position, static_cast<std::uint32_t>(layout_text_.size()));
    document_.SetCaret(MapDisplayToDoc(display_position), extend_selection);
    EnsureCaretVisible();
    UpdateImeWindow();
    ResetCaretBlink();
    InvalidateRect(main_hwnd_, nullptr, FALSE);
}

std::uint32_t NoteApp::HitTestEditor(float x_dip, float y_dip) {
    EnsureTextLayout();
    if (!text_layout_) {
        return 0;
    }

    const auto local_x = std::max(0.0f, x_dip - editor_rect_.left - theme::kEditorPadding);
    const auto local_y = std::max(0.0f, y_dip - editor_rect_.top - theme::kEditorPadding + scroll_y_);

    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS hit{};
    text_layout_->HitTestPoint(local_x, local_y, &trailing, &inside, &hit);

    auto display_position = hit.textPosition + (trailing ? 1U : 0U);
    display_position = std::min(display_position, static_cast<std::uint32_t>(layout_text_.size()));
    return MapDisplayToDoc(display_position);
}

void NoteApp::UpdateImeWindow() {
    if (!main_hwnd_ || GetFocus() != main_hwnd_) {
        return;
    }

    EnsureTextLayout();
    if (!text_layout_) {
        return;
    }

    float caret_x = 0.0f;
    float caret_y = 0.0f;
    DWRITE_HIT_TEST_METRICS metrics{};
    HitTestCaret(text_layout_.Get(), MapDocToDisplay(document_.Caret()),
                 static_cast<std::uint32_t>(layout_text_.size()), &caret_x, &caret_y, &metrics);

    const auto scale = ScaleFor(main_hwnd_);
    POINT caret_point{
        DipToPx(editor_rect_.left + theme::kEditorPadding + caret_x, scale),
        DipToPx(editor_rect_.top + theme::kEditorPadding + caret_y - scroll_y_ + metrics.height, scale)};

    if (auto* context = ImmGetContext(main_hwnd_)) {
        COMPOSITIONFORM composition{};
        composition.dwStyle = CFS_POINT;
        composition.ptCurrentPos = caret_point;
        ImmSetCompositionWindow(context, &composition);

        CANDIDATEFORM candidate{};
        candidate.dwStyle = CFS_CANDIDATEPOS;
        candidate.dwIndex = 0;
        candidate.ptCurrentPos = caret_point;
        ImmSetCandidateWindow(context, &candidate);
        ImmReleaseContext(main_hwnd_, context);
    }
}

void NoteApp::ScheduleSave() {
    if (!main_hwnd_) {
        return;
    }
    SetTimer(main_hwnd_, kAutoSaveTimerId, 650, nullptr);
}

void NoteApp::SaveNow() {
    if (!note_dirty_ || current_note_.id.empty()) {
        return;
    }

    if (main_hwnd_) {
        KillTimer(main_hwnd_, kAutoSaveTimerId);
    }

    SyncCurrentNoteFromDocument();
    current_note_.modified_utc = NowUtcSeconds();
    if (store_.SaveNote(current_note_)) {
        note_dirty_ = false;
        RefreshRecentNotes();
        UpdateMainCaption();
        InvalidateRect(launcher_hwnd_, nullptr, FALSE);
        InvalidateRect(main_hwnd_, nullptr, FALSE);
    }
}

void NoteApp::RefreshRecentNotes() {
    recent_notes_ = store_.ListRecentNotes(24);
    ClampHistoryScroll();
}

void NoteApp::OpenNote(const std::wstring& id) {
    SaveNow();

    NoteRecord loaded;
    if (!store_.LoadNote(id, loaded)) {
        CreateNewNote();
        return;
    }

    current_note_ = std::move(loaded);
    document_.Load(current_note_.text, current_note_.bold_ranges);
    note_dirty_ = false;
    scroll_y_ = 0.0f;
    preferred_caret_x_ = -1.0f;
    ime_preview_.clear();
    ime_dedup_chars_.clear();
    InvalidateLayout();
    UpdateMainCaption();
    ClearDeleteConfirmation();
    ClearMainHover();
    ToggleHistoryDrawer(false);
    if (launcher_hwnd_) {
        ToggleLauncherExpanded(false);
        ShowWindow(launcher_hwnd_, SW_HIDE);
    }
    ShowWindow(main_hwnd_, SW_SHOW);
    SetForegroundWindow(main_hwnd_);
    SetFocus(main_hwnd_);
    UpdateImeWindow();
    ResetCaretBlink();
    InvalidateRect(main_hwnd_, nullptr, FALSE);
    InvalidateRect(launcher_hwnd_, nullptr, FALSE);
}

void NoteApp::CreateNewNote() {
    SaveNow();

    current_note_ = store_.CreateNote();
    document_.Load(L"", {});
    note_dirty_ = true;
    scroll_y_ = 0.0f;
    preferred_caret_x_ = -1.0f;
    ime_preview_.clear();
    ime_dedup_chars_.clear();
    InvalidateLayout();
    UpdateMainCaption();
    ClearDeleteConfirmation();
    ClearMainHover();
    ToggleHistoryDrawer(false);
    if (launcher_hwnd_) {
        ToggleLauncherExpanded(false);
        ShowWindow(launcher_hwnd_, SW_HIDE);
    }
    ShowWindow(main_hwnd_, SW_SHOW);
    SetForegroundWindow(main_hwnd_);
    SetFocus(main_hwnd_);
    SaveNow();
    UpdateImeWindow();
    ResetCaretBlink();
    InvalidateRect(main_hwnd_, nullptr, FALSE);
}

void NoteApp::SyncCurrentNoteFromDocument() {
    current_note_.text = document_.Text();
    current_note_.bold_ranges = document_.BoldRanges();
}

void NoteApp::ToggleLauncherExpanded(bool expanded) {
    if (!launcher_hwnd_) {
        return;
    }

    launcher_expanded_ = expanded;

    RECT work_area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    const auto scale = ScaleFor(launcher_hwnd_);
    const auto width = DipToPx(theme::kLauncherWidth, scale);
    const auto height = DipToPx(expanded ? theme::kLauncherExpandedHeight : theme::kLauncherCollapsedHeight, scale);
    const auto margin = DipToPx(20.0f, scale);

    SetWindowPos(launcher_hwnd_, HWND_TOPMOST, work_area.right - width - margin, work_area.top + margin, width,
                 height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    InvalidateRect(launcher_hwnd_, nullptr, FALSE);
}

void NoteApp::ToggleHistoryDrawer(bool open) {
    history_drawer_open_ = open;
    history_drawer_target_ = open ? 1.0f : 0.0f;
    if (open) {
        RefreshRecentNotes();
    } else {
        ClearDeleteConfirmation();
    }
    StartUiAnimation();
}

void NoteApp::StartUiAnimation() {
    if (!main_hwnd_) {
        return;
    }

    animation_last_tick_ = GetTickCount64();
    SetTimer(main_hwnd_, kUiAnimationTimerId, static_cast<UINT>(theme::kUiAnimationTickMs), nullptr);
    InvalidateRect(main_hwnd_, nullptr, FALSE);
}

void NoteApp::TickUiAnimation() {
    if (!main_hwnd_) {
        return;
    }

    const auto now = GetTickCount64();
    const auto delta_ms = static_cast<float>(std::max<ULONGLONG>(1, now - animation_last_tick_));
    animation_last_tick_ = now;

    const auto drawer_step = delta_ms / 170.0f;
    const auto delete_step = delta_ms / 130.0f;

    const auto old_drawer_progress = history_drawer_progress_;
    const auto old_delete_progress = pending_delete_progress_;
    const auto old_pressed_feedback_valid = pressed_feedback_valid_;
    history_drawer_progress_ = MoveToward(history_drawer_progress_, history_drawer_target_, drawer_step);
    pending_delete_progress_ = MoveToward(pending_delete_progress_, pending_delete_target_, delete_step);
    if (pressed_feedback_valid_ && now >= pressed_feedback_until_tick_) {
        pressed_feedback_valid_ = false;
        pressed_feedback_payload_.clear();
    }

    if (pending_delete_target_ == 0.0f && pending_delete_progress_ == 0.0f) {
        pending_delete_id_.clear();
    }

    const auto running = (history_drawer_progress_ != history_drawer_target_) ||
                         (pending_delete_progress_ != pending_delete_target_) || pressed_feedback_valid_;
    if (!running) {
        KillTimer(main_hwnd_, kUiAnimationTimerId);
    }

    if (old_drawer_progress != history_drawer_progress_ || old_delete_progress != pending_delete_progress_ ||
        old_pressed_feedback_valid != pressed_feedback_valid_) {
        InvalidateRect(main_hwnd_, nullptr, FALSE);
    }
}

void NoteApp::BeginDeleteConfirmation(const std::wstring& id) {
    if (id.empty()) {
        return;
    }

    if (pending_delete_id_ != id) {
        pending_delete_id_ = id;
        pending_delete_progress_ = 0.0f;
    }
    pending_delete_target_ = 1.0f;
    StartUiAnimation();
}

void NoteApp::ClearDeleteConfirmation() {
    if (pending_delete_id_.empty() && pending_delete_progress_ == 0.0f) {
        return;
    }

    pending_delete_target_ = 0.0f;
    if (pending_delete_progress_ == 0.0f) {
        pending_delete_id_.clear();
        InvalidateRect(main_hwnd_, nullptr, FALSE);
        return;
    }

    StartUiAnimation();
}

void NoteApp::DeleteNoteAndAdvance(const std::wstring& id) {
    if (id.empty()) {
        return;
    }

    const auto deleting_current = id == current_note_.id;
    if (!store_.DeleteNote(id)) {
        ClearDeleteConfirmation();
        RefreshRecentNotes();
        InvalidateRect(main_hwnd_, nullptr, FALSE);
        return;
    }

    ClearDeleteConfirmation();
    RefreshRecentNotes();

    if (deleting_current) {
        note_dirty_ = false;
        if (!recent_notes_.empty()) {
            OpenNote(recent_notes_.front().id);
        } else {
            CreateNewNote();
        }
        return;
    }

    InvalidateRect(main_hwnd_, nullptr, FALSE);
}

void NoteApp::ClampHistoryScroll() {
    const auto visible_height = std::max(0.0f, history_list_rect_.bottom - history_list_rect_.top);
    const auto max_scroll = std::max(0.0f, history_content_height_ - visible_height);
    history_scroll_y_ = std::clamp(history_scroll_y_, 0.0f, max_scroll);
}

void NoteApp::UpdateMainHover(float x_dip, float y_dip) {
    if (!main_hwnd_) {
        return;
    }

    if (!tracking_main_mouse_) {
        TRACKMOUSEEVENT event{};
        event.cbSize = sizeof(event);
        event.dwFlags = TME_LEAVE;
        event.hwndTrack = main_hwnd_;
        TrackMouseEvent(&event);
        tracking_main_mouse_ = true;
    }

    bool found = false;
    HitRole next_role = HitRole::LauncherToggle;
    std::wstring next_payload;
    for (const auto& hit : main_hits_) {
        if (!Hit(hit.rect, x_dip, y_dip)) {
            continue;
        }
        found = true;
        next_role = hit.role;
        next_payload = hit.payload;
        break;
    }

    if (main_hover_valid_ == found && (!found || (main_hover_role_ == next_role && main_hover_payload_ == next_payload))) {
        return;
    }

    main_hover_valid_ = found;
    if (found) {
        main_hover_role_ = next_role;
        main_hover_payload_ = std::move(next_payload);
    } else {
        main_hover_payload_.clear();
    }
    InvalidateRect(main_hwnd_, nullptr, FALSE);
}

void NoteApp::ClearMainHover() {
    tracking_main_mouse_ = false;
    if (!main_hover_valid_ && main_hover_payload_.empty()) {
        return;
    }

    main_hover_valid_ = false;
    main_hover_payload_.clear();
    InvalidateRect(main_hwnd_, nullptr, FALSE);
}

void NoteApp::BeginPressedFeedback(HitRole role, std::wstring_view payload) {
    pressed_feedback_valid_ = true;
    pressed_feedback_role_ = role;
    pressed_feedback_payload_.assign(payload.begin(), payload.end());
    pressed_feedback_until_tick_ = GetTickCount64() + 110;
    StartUiAnimation();
}

bool NoteApp::IsMainHovered(HitRole role, std::wstring_view payload) const {
    return main_hover_valid_ && main_hover_role_ == role && main_hover_payload_ == payload;
}

bool NoteApp::IsPressedFeedback(HitRole role, std::wstring_view payload) const {
    return pressed_feedback_valid_ && pressed_feedback_role_ == role && pressed_feedback_payload_ == payload;
}

void NoteApp::UpdateMainCaption() {
    if (!main_hwnd_) {
        return;
    }

    const auto title = NoteStore::DeriveTitle(document_.Text());
    std::wstring caption = title + L"  |  RasterNoteNative";
    SetWindowTextW(main_hwnd_, caption.c_str());
}

NoteApp::MainChromeLayout NoteApp::BuildMainChromeLayout() const {
    MainChromeLayout layout;
    if (!main_hwnd_) {
        return layout;
    }

    const auto size = ClientSizeDip(main_hwnd_);
    const auto left = theme::kOuterPadding;
    const auto right = std::max(left + 10.0f, size.width - theme::kOuterPadding);
    const auto chrome_left = left;
    const auto chrome_top = left;
    const auto chrome_right = right;
    const auto chrome_bottom = chrome_top + theme::kHeaderHeight;
    const auto inner_left = chrome_left + 16.0f;
    const auto inner_right = chrome_right - 16.0f;
    const auto row_top = chrome_top + 10.0f;
    constexpr float kControlGap = 6.0f;
    constexpr float kEditorBandHeight = 28.0f;
    constexpr float kFooterBandHeight = 28.0f;

    layout.window_rect = D2D1::RectF(0.5f, 0.5f, std::max(0.5f, size.width - 0.5f),
                                     std::max(0.5f, size.height - 0.5f));
    layout.chrome_rect = D2D1::RectF(chrome_left, chrome_top, chrome_right, chrome_bottom);
    layout.header_rect = layout.chrome_rect;
    layout.control_band_rect = D2D1::RectF(chrome_left, chrome_top, chrome_right, chrome_top + 42.0f);

    auto button_right = inner_right;
    layout.close_button = D2D1::RectF(button_right - theme::kWindowControlWidth, row_top, button_right,
                                      row_top + theme::kWindowControlHeight);
    button_right = layout.close_button.left - kControlGap;
    layout.maximize_button = D2D1::RectF(button_right - theme::kWindowControlWidth, row_top, button_right,
                                         row_top + theme::kWindowControlHeight);
    button_right = layout.maximize_button.left - kControlGap;
    layout.minimize_button = D2D1::RectF(button_right - theme::kWindowControlWidth, row_top, button_right,
                                         row_top + theme::kWindowControlHeight);
    button_right = layout.minimize_button.left - kControlGap;
    layout.launcher_button = D2D1::RectF(button_right - theme::kButtonWidth, row_top, button_right,
                                         row_top + theme::kButtonHeight);
    button_right = layout.launcher_button.left - kControlGap;
    layout.save_button = D2D1::RectF(button_right - theme::kButtonWidth, row_top, button_right,
                                     row_top + theme::kButtonHeight);
    button_right = layout.save_button.left - kControlGap;
    layout.bold_button = D2D1::RectF(button_right - theme::kButtonWidth, row_top, button_right,
                                     row_top + theme::kButtonHeight);
    button_right = layout.bold_button.left - kControlGap;
    layout.new_button = D2D1::RectF(button_right - theme::kButtonWidth, row_top, button_right,
                                    row_top + theme::kButtonHeight);

    const auto utility_right = std::max(inner_left + 180.0f, layout.new_button.left - 16.0f);
    const auto title_right = std::max(inner_left + 220.0f, layout.new_button.left - 16.0f);
    layout.utility_rect = D2D1::RectF(inner_left, row_top + 1.0f, utility_right, row_top + 16.0f);
    layout.title_rect = D2D1::RectF(inner_left, chrome_top + 40.0f, title_right, chrome_top + 64.0f);
    layout.subtitle_rect = D2D1::RectF(inner_left, chrome_top + 62.0f, title_right, chrome_bottom - 8.0f);
    layout.toolbar_rect = D2D1::RectF(layout.new_button.left, layout.new_button.top, layout.close_button.right,
                                      layout.close_button.bottom);
    layout.drag_rect = D2D1::RectF(inner_left, chrome_top, std::max(inner_left + 80.0f, layout.new_button.left - 12.0f),
                                   chrome_bottom);

    layout.editor_card_rect =
        D2D1::RectF(theme::kOuterPadding, layout.chrome_rect.bottom + 14.0f, size.width - theme::kOuterPadding,
                    size.height - theme::kOuterPadding);
    layout.editor_band_rect = D2D1::RectF(layout.editor_card_rect.left + 1.0f, layout.editor_card_rect.top + 1.0f,
                                          layout.editor_card_rect.right - 1.0f,
                                          layout.editor_card_rect.top + kEditorBandHeight);
    layout.footer_band_rect = D2D1::RectF(layout.editor_card_rect.left + 1.0f,
                                          layout.editor_card_rect.bottom - kFooterBandHeight - 1.0f,
                                          layout.editor_card_rect.right - 1.0f, layout.editor_card_rect.bottom - 1.0f);
    layout.footer_rect = D2D1::RectF(layout.footer_band_rect.left + 12.0f, layout.footer_band_rect.top + 6.0f,
                                     layout.footer_band_rect.right - 12.0f, layout.footer_band_rect.bottom - 4.0f);
    layout.editor_rect = D2D1::RectF(layout.editor_card_rect.left + 12.0f, layout.editor_band_rect.bottom + 10.0f,
                                     layout.editor_card_rect.right - 12.0f, layout.footer_band_rect.top - 10.0f);
    return layout;
}

LRESULT NoteApp::HitTestMainNc(POINT screen_point) const {
    if (!main_hwnd_) {
        return HTNOWHERE;
    }

    const auto point = PointFromScreenDip(main_hwnd_, screen_point);
    const auto size = ClientSizeDip(main_hwnd_);
    if (point.x < 0.0f || point.y < 0.0f || point.x > size.width || point.y > size.height) {
        return HTNOWHERE;
    }

    if (!IsMainMaximized()) {
        const auto border = theme::kResizeBorder;
        const auto left = point.x < border;
        const auto right = point.x >= (size.width - border);
        const auto top = point.y < border;
        const auto bottom = point.y >= (size.height - border);

        if (top && left) {
            return HTTOPLEFT;
        }
        if (top && right) {
            return HTTOPRIGHT;
        }
        if (bottom && left) {
            return HTBOTTOMLEFT;
        }
        if (bottom && right) {
            return HTBOTTOMRIGHT;
        }
        if (left) {
            return HTLEFT;
        }
        if (right) {
            return HTRIGHT;
        }
        if (top) {
            return HTTOP;
        }
        if (bottom) {
            return HTBOTTOM;
        }
    }

    const auto layout = BuildMainChromeLayout();
    if (Hit(layout.drag_rect, point.x, point.y)) {
        return HTCAPTION;
    }
    if (Hit(layout.minimize_button, point.x, point.y) || Hit(layout.maximize_button, point.x, point.y) ||
        Hit(layout.close_button, point.x, point.y) || Hit(layout.new_button, point.x, point.y) ||
        Hit(layout.bold_button, point.x, point.y) || Hit(layout.save_button, point.x, point.y) ||
        Hit(layout.launcher_button, point.x, point.y)) {
        return HTCLIENT;
    }

    return HTCLIENT;
}

void NoteApp::ToggleMainMaximized() {
    if (!main_hwnd_) {
        return;
    }

    ShowWindow(main_hwnd_, IsMainMaximized() ? SW_RESTORE : SW_MAXIMIZE);
    InvalidateRect(main_hwnd_, nullptr, FALSE);
}

bool NoteApp::IsMainMaximized() const {
    return main_hwnd_ && IsZoomed(main_hwnd_) != FALSE;
}

void NoteApp::ApplyWindowStyle(HWND hwnd) const {
    const auto caption = ToColorRef(theme::Paper());
    const auto text = ToColorRef(theme::Ink());
    const auto border = ToColorRef(theme::Border());
    (void)caption;
    (void)text;
    (void)border;
    MARGINS shadow{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(hwnd, &shadow);
#ifdef DWMWA_CAPTION_COLOR
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
#endif
#ifdef DWMWA_TEXT_COLOR
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
#endif
#ifdef DWMWA_BORDER_COLOR
#ifdef DWMWA_COLOR_NONE
    const auto window_border = static_cast<COLORREF>(DWMWA_COLOR_NONE);
#else
    const auto window_border = border;
#endif
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &window_border, sizeof(window_border));
#endif
#ifdef DWMWA_WINDOW_CORNER_PREFERENCE
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
#endif
}

void NoteApp::ResetCursor(HWND hwnd, float x_dip, float y_dip) {
    if (hwnd == launcher_hwnd_) {
        for (const auto& hit : launcher_hits_) {
            if (Hit(hit.rect, x_dip, y_dip)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return;
            }
        }
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return;
    }

    for (const auto& hit : main_hits_) {
        if (Hit(hit.rect, x_dip, y_dip)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return;
        }
    }

    if (Hit(editor_rect_, x_dip, y_dip)) {
        SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        return;
    }

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

bool NoteApp::Hit(const D2D1_RECT_F& rect, float x_dip, float y_dip) const {
    return x_dip >= rect.left && x_dip <= rect.right && y_dip >= rect.top && y_dip <= rect.bottom;
}

bool NoteApp::CopySelectionToClipboard() {
    if (!document_.HasSelection()) {
        return false;
    }

    const auto text = document_.Text();
    const auto start = document_.SelectionStart();
    const auto length = document_.SelectionEnd() - start;
    const auto selected = text.substr(start, length);

    if (!OpenClipboard(main_hwnd_)) {
        return false;
    }

    EmptyClipboard();
    const auto bytes = (selected.size() + 1U) * sizeof(wchar_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }

    auto* buffer = static_cast<wchar_t*>(GlobalLock(memory));
    memcpy(buffer, selected.c_str(), bytes);
    GlobalUnlock(memory);
    SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    return true;
}

bool NoteApp::CutSelectionToClipboard() {
    if (!CopySelectionToClipboard()) {
        return false;
    }

    document_.DeleteSelection();
    note_dirty_ = true;
    ScheduleSave();
    preferred_caret_x_ = -1.0f;
    InvalidateLayout();
    EnsureCaretVisible();
    UpdateImeWindow();
    UpdateMainCaption();
    ResetCaretBlink();
    InvalidateRect(main_hwnd_, nullptr, FALSE);
    return true;
}

bool NoteApp::PasteFromClipboard() {
    if (!OpenClipboard(main_hwnd_)) {
        return false;
    }

    const auto handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
        CloseClipboard();
        return false;
    }

    const auto* data = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!data) {
        CloseClipboard();
        return false;
    }

    const std::wstring text(data);
    GlobalUnlock(handle);
    CloseClipboard();

    if (text.empty()) {
        return false;
    }

    InsertCommittedText(text);
    return true;
}

void NoteApp::InsertCommittedText(std::wstring_view text) {
    if (text.empty()) {
        return;
    }

    document_.InsertText(text);
    note_dirty_ = true;
    ScheduleSave();
    preferred_caret_x_ = -1.0f;
    ime_preview_.clear();
    InvalidateLayout();
    EnsureCaretVisible();
    UpdateMainCaption();
    UpdateImeWindow();
    ResetCaretBlink();
    InvalidateRect(main_hwnd_, nullptr, FALSE);
}

void NoteApp::HandleImeComposition(LPARAM lparam) {
    if (!main_hwnd_) {
        return;
    }

    auto* context = ImmGetContext(main_hwnd_);
    if (!context) {
        return;
    }

    if ((lparam & GCS_RESULTSTR) != 0) {
        const auto result = ReadCompositionString(context, GCS_RESULTSTR);
        ime_preview_.clear();
        ime_dedup_chars_ = result;
        ime_dedup_tick_ = GetTickCount64();
        if (!result.empty()) {
            InsertCommittedText(result);
        }
    }

    if ((lparam & GCS_COMPSTR) != 0) {
        ime_preview_ = ReadCompositionString(context, GCS_COMPSTR);
        InvalidateLayout();
        EnsureCaretVisible();
        UpdateImeWindow();
        InvalidateRect(main_hwnd_, nullptr, FALSE);
    }

    ImmReleaseContext(main_hwnd_, context);
}

void NoteApp::StartCaretBlink() {
    if (!main_hwnd_) {
        return;
    }

    caret_visible_ = true;
    KillTimer(main_hwnd_, kCaretBlinkTimerId);

    const auto blink_time = GetCaretBlinkTime();
    if (blink_time == INFINITE) {
        return;
    }

    SetTimer(main_hwnd_, kCaretBlinkTimerId, std::max<UINT>(200U, blink_time == 0U ? 530U : blink_time), nullptr);
}

void NoteApp::StopCaretBlink() {
    caret_visible_ = false;
    if (main_hwnd_) {
        KillTimer(main_hwnd_, kCaretBlinkTimerId);
    }
}

void NoteApp::ResetCaretBlink() {
    if (GetFocus() != main_hwnd_) {
        caret_visible_ = true;
        return;
    }

    StartCaretBlink();
    InvalidateRect(main_hwnd_, nullptr, FALSE);
}

float NoteApp::ScaleFor(HWND hwnd) const {
    const auto dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
    return static_cast<float>(dpi) / 96.0f;
}

D2D1_SIZE_F NoteApp::ClientSizeDip(HWND hwnd) const {
    RECT client{};
    GetClientRect(hwnd, &client);
    const auto scale = ScaleFor(hwnd);
    return D2D1::SizeF((client.right - client.left) / scale, (client.bottom - client.top) / scale);
}

D2D1_POINT_2F NoteApp::PointFromLParamDip(HWND hwnd, LPARAM lparam) const {
    const auto scale = ScaleFor(hwnd);
    return D2D1::Point2F(GET_X_LPARAM(lparam) / scale, GET_Y_LPARAM(lparam) / scale);
}

D2D1_POINT_2F NoteApp::PointFromScreenDip(HWND hwnd, POINT screen_point) const {
    ScreenToClient(hwnd, &screen_point);
    const auto scale = ScaleFor(hwnd);
    return D2D1::Point2F(screen_point.x / scale, screen_point.y / scale);
}

std::uint32_t NoteApp::MapDocToDisplay(std::uint32_t position) const {
    if (!ime_preview_.empty() && position >= document_.Caret()) {
        return position + static_cast<std::uint32_t>(ime_preview_.size());
    }
    return position;
}

std::uint32_t NoteApp::MapDisplayToDoc(std::uint32_t position) const {
    if (ime_preview_.empty()) {
        return std::min(position, document_.Length());
    }

    const auto caret = document_.Caret();
    const auto preview_end = caret + static_cast<std::uint32_t>(ime_preview_.size());
    if (position <= caret) {
        return position;
    }
    if (position <= preview_end) {
        return caret;
    }
    return std::min(position - static_cast<std::uint32_t>(ime_preview_.size()), document_.Length());
}

}  // namespace raster
