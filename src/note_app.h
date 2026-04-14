#pragma once

#include "note_store.h"
#include "text_document.h"

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>
#include <string_view>
#include <vector>

namespace raster {

class NoteApp {
public:
    int Run(HINSTANCE instance, int show_command);

private:
    enum class HitRole {
        LauncherToggle,
        LauncherNew,
        LauncherQuit,
        LauncherNote,
        MainNew,
        MainBold,
        MainSave,
        MainLauncher,
        MainMinimize,
        MainMaximize,
        MainClose,
        MainHistoryNote,
        MainHistoryDelete,
        MainHistoryDeleteConfirm,
        MainHistoryDeleteCancel
    };

    struct HitRegion {
        D2D1_RECT_F rect{};
        HitRole role = HitRole::LauncherToggle;
        std::wstring payload;
    };

    struct MainChromeLayout {
        D2D1_RECT_F window_rect{};
        D2D1_RECT_F chrome_rect{};
        D2D1_RECT_F header_rect{};
        D2D1_RECT_F control_band_rect{};
        D2D1_RECT_F utility_rect{};
        D2D1_RECT_F title_rect{};
        D2D1_RECT_F subtitle_rect{};
        D2D1_RECT_F toolbar_rect{};
        D2D1_RECT_F drag_rect{};
        D2D1_RECT_F new_button{};
        D2D1_RECT_F bold_button{};
        D2D1_RECT_F save_button{};
        D2D1_RECT_F launcher_button{};
        D2D1_RECT_F minimize_button{};
        D2D1_RECT_F maximize_button{};
        D2D1_RECT_F close_button{};
        D2D1_RECT_F editor_card_rect{};
        D2D1_RECT_F editor_band_rect{};
        D2D1_RECT_F footer_band_rect{};
        D2D1_RECT_F editor_rect{};
        D2D1_RECT_F footer_rect{};
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    bool Initialize(HINSTANCE instance, int show_command);
    bool CreateFactories();
    bool RegisterWindowClasses() const;
    bool CreateWindows();
    int MessageLoop() const;

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleLauncherMessage(UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMainMessage(UINT message, WPARAM wparam, LPARAM lparam);

    void RenderLauncher();
    void RenderMain();
    void ResizeRenderTarget(HWND hwnd);
    void InvalidateLayout();
    void EnsureTextLayout();
    void EnsureCaretVisible();
    void MoveCaretVertical(int direction, bool extend_selection);
    std::uint32_t HitTestEditor(float x_dip, float y_dip);
    void UpdateImeWindow();
    void ScheduleSave();
    void SaveNow();
    void RefreshRecentNotes();
    void OpenNote(const std::wstring& id);
    void CreateNewNote();
    void SyncCurrentNoteFromDocument();
    void ToggleLauncherExpanded(bool expanded);
    void ToggleHistoryDrawer(bool open);
    void UpdateMainCaption();
    void ApplyWindowStyle(HWND hwnd) const;
    void ResetCursor(HWND hwnd, float x_dip, float y_dip);
    bool Hit(const D2D1_RECT_F& rect, float x_dip, float y_dip) const;
    [[nodiscard]] MainChromeLayout BuildMainChromeLayout() const;
    [[nodiscard]] LRESULT HitTestMainNc(POINT screen_point) const;
    void ToggleMainMaximized();
    [[nodiscard]] bool IsMainMaximized() const;

    bool CopySelectionToClipboard();
    bool CutSelectionToClipboard();
    bool PasteFromClipboard();
    void InsertCommittedText(std::wstring_view text);
    void HandleImeComposition(LPARAM lparam);
    void StartCaretBlink();
    void StopCaretBlink();
    void ResetCaretBlink();
    void StartUiAnimation();
    void TickUiAnimation();
    void BeginDeleteConfirmation(const std::wstring& id);
    void ClearDeleteConfirmation();
    void DeleteNoteAndAdvance(const std::wstring& id);
    void ClampHistoryScroll();
    void UpdateMainHover(float x_dip, float y_dip);
    void ClearMainHover();
    void BeginPressedFeedback(HitRole role, std::wstring_view payload);
    [[nodiscard]] bool IsMainHovered(HitRole role, std::wstring_view payload = {}) const;
    [[nodiscard]] bool IsPressedFeedback(HitRole role, std::wstring_view payload = {}) const;

    float ScaleFor(HWND hwnd) const;
    D2D1_SIZE_F ClientSizeDip(HWND hwnd) const;
    D2D1_POINT_2F PointFromLParamDip(HWND hwnd, LPARAM lparam) const;
    D2D1_POINT_2F PointFromScreenDip(HWND hwnd, POINT screen_point) const;
    std::uint32_t MapDocToDisplay(std::uint32_t position) const;
    std::uint32_t MapDisplayToDoc(std::uint32_t position) const;

    HINSTANCE instance_ = nullptr;
    HWND launcher_hwnd_ = nullptr;
    HWND main_hwnd_ = nullptr;

    NoteStore store_;
    NoteRecord current_note_;
    TextDocument document_;
    std::vector<NoteSummary> recent_notes_;

    bool note_dirty_ = false;
    bool launcher_expanded_ = false;
    bool history_drawer_open_ = false;
    bool selecting_ = false;
    float scroll_y_ = 0.0f;
    float history_scroll_y_ = 0.0f;
    float preferred_caret_x_ = -1.0f;
    D2D1_RECT_F editor_rect_{};
    D2D1_RECT_F history_drawer_rect_{};
    D2D1_RECT_F history_list_rect_{};
    float history_content_height_ = 0.0f;
    std::wstring ime_preview_;
    std::wstring ime_dedup_chars_;
    std::wstring pending_delete_id_;
    std::wstring main_hover_payload_;
    std::wstring pressed_feedback_payload_;
    ULONGLONG ime_dedup_tick_ = 0;
    ULONGLONG animation_last_tick_ = 0;
    ULONGLONG pressed_feedback_until_tick_ = 0;
    bool caret_visible_ = true;
    bool tracking_main_mouse_ = false;
    bool main_hover_valid_ = false;
    bool pressed_feedback_valid_ = false;
    HitRole main_hover_role_ = HitRole::LauncherToggle;
    HitRole pressed_feedback_role_ = HitRole::LauncherToggle;
    float history_drawer_progress_ = 0.0f;
    float history_drawer_target_ = 0.0f;
    float pending_delete_progress_ = 0.0f;
    float pending_delete_target_ = 0.0f;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> launcher_target_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> main_target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> launcher_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> main_brush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> label_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> section_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> editor_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> meta_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> meta_right_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> button_format_;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> text_layout_;
    std::wstring layout_text_;
    std::uint64_t layout_revision_ = 0;
    float layout_width_ = 0.0f;
    DWRITE_TEXT_METRICS layout_metrics_{};
    std::vector<HitRegion> launcher_hits_;
    std::vector<HitRegion> main_hits_;
};

}  // namespace raster
