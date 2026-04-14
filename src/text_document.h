#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace raster {

struct TextRange {
    std::uint32_t start = 0;
    std::uint32_t length = 0;
};

class GapBuffer {
public:
    GapBuffer();
    explicit GapBuffer(std::wstring text);

    void SetText(std::wstring text);
    [[nodiscard]] std::wstring Snapshot() const;
    [[nodiscard]] std::uint32_t Size() const;

    void Insert(std::uint32_t position, std::wstring_view text);
    void Erase(std::uint32_t position, std::uint32_t count);

private:
    std::vector<wchar_t> data_;
    std::uint32_t gap_start_ = 0;
    std::uint32_t gap_end_ = 0;

    void EnsureGap(std::uint32_t minimum_size);
    void MoveGap(std::uint32_t position);
};

class TextDocument {
public:
    TextDocument();

    void Load(std::wstring text, std::vector<TextRange> bold_ranges);

    [[nodiscard]] std::wstring Text() const;
    [[nodiscard]] const std::vector<TextRange>& BoldRanges() const;
    [[nodiscard]] std::uint32_t Length() const;
    [[nodiscard]] std::uint64_t Revision() const;

    [[nodiscard]] std::uint32_t Caret() const;
    [[nodiscard]] std::uint32_t Anchor() const;
    [[nodiscard]] std::uint32_t SelectionStart() const;
    [[nodiscard]] std::uint32_t SelectionEnd() const;
    [[nodiscard]] bool HasSelection() const;
    [[nodiscard]] bool PendingBold() const;

    void SetCaret(std::uint32_t position, bool extend_selection);
    void SetSelection(std::uint32_t anchor, std::uint32_t caret);
    void SelectAll();

    void MoveCaretLeft(bool extend_selection);
    void MoveCaretRight(bool extend_selection);
    void MoveCaretHome(bool extend_selection);
    void MoveCaretEnd(bool extend_selection);

    void InsertText(std::wstring_view text);
    void DeleteBackward();
    void DeleteForward();
    void DeleteSelection();
    void ToggleBold();

    [[nodiscard]] bool IsRangeFullyBold(std::uint32_t start, std::uint32_t length) const;
    [[nodiscard]] bool StyleAt(std::uint32_t position) const;

private:
    GapBuffer buffer_;
    std::vector<TextRange> bold_ranges_;
    std::uint32_t anchor_ = 0;
    std::uint32_t caret_ = 0;
    bool pending_bold_ = false;
    std::uint64_t revision_ = 1;

    void InsertAt(std::uint32_t position, std::wstring_view text, bool bold);
    void DeleteRange(std::uint32_t start, std::uint32_t length);
    void ApplyBold(std::uint32_t start, std::uint32_t length, bool enabled);
    void NormalizeBoldRanges();
    void SyncPendingStyle();
    void Touch();
    [[nodiscard]] std::uint32_t Clamp(std::uint32_t value) const;
};

}  // namespace raster
