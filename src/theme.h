#pragma once

#include <d2d1.h>

namespace raster::theme {

inline constexpr float kLauncherWidth = 380.0f;
inline constexpr float kLauncherCollapsedHeight = 132.0f;
inline constexpr float kLauncherExpandedHeight = 480.0f;

inline constexpr float kMainWidth = 1120.0f;
inline constexpr float kMainHeight = 800.0f;

inline constexpr float kOuterPadding = 18.0f;
inline constexpr float kHeaderHeight = 112.0f;
inline constexpr float kEditorPadding = 12.0f;
inline constexpr float kButtonHeight = 24.0f;
inline constexpr float kButtonWidth = 56.0f;
inline constexpr float kListRowHeight = 58.0f;
inline constexpr float kCaretWidth = 2.0f;
inline constexpr float kEditorFontSize = 18.0f;
inline constexpr float kWindowControlWidth = 56.0f;
inline constexpr float kWindowControlHeight = 24.0f;
inline constexpr float kResizeBorder = 8.0f;
inline constexpr float kRootBorderThickness = 1.0f;
inline constexpr float kHistoryDrawerWidth = 336.0f;
inline constexpr float kHistoryDrawerHeaderHeight = 48.0f;
inline constexpr float kHistoryRowHeight = 74.0f;
inline constexpr float kHistoryRowGap = 8.0f;
inline constexpr float kHistoryDeleteButtonWidth = 64.0f;
inline constexpr float kUiAnimationTickMs = 16.0f;

inline D2D1_COLOR_F Paper() {
    return D2D1::ColorF(0xF2F0EA, 1.0f);
}

inline D2D1_COLOR_F Panel() {
    return D2D1::ColorF(0xF7F5EF, 1.0f);
}

inline D2D1_COLOR_F Chrome() {
    return D2D1::ColorF(0xECE8DE, 1.0f);
}

inline D2D1_COLOR_F Card() {
    return D2D1::ColorF(0xFBFAF5, 1.0f);
}

inline D2D1_COLOR_F Ink() {
    return D2D1::ColorF(0x151515, 1.0f);
}

inline D2D1_COLOR_F Dim() {
    return D2D1::ColorF(0x5C5C5C, 1.0f);
}

inline D2D1_COLOR_F Border() {
    return D2D1::ColorF(0xC9C3B7, 1.0f);
}

inline D2D1_COLOR_F SoftBorder() {
    return D2D1::ColorF(0xDDD7CB, 1.0f);
}

inline D2D1_COLOR_F Accent() {
    return D2D1::ColorF(0x0E0E0E, 1.0f);
}

inline D2D1_COLOR_F InverseText() {
    return D2D1::ColorF(0xF6F4EE, 1.0f);
}

inline D2D1_COLOR_F Selection() {
    return D2D1::ColorF(0xDDD5C7, 1.0f);
}

inline D2D1_COLOR_F Hint() {
    return D2D1::ColorF(0x9A9387, 1.0f);
}

}  // namespace raster::theme
