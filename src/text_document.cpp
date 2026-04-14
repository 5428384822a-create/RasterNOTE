#include "text_document.h"

#include <algorithm>

namespace raster {

namespace {

constexpr std::uint32_t kInitialGap = 64;

std::uint32_t RangeEnd(const TextRange& range) {
    return range.start + range.length;
}

}  // namespace

GapBuffer::GapBuffer() {
    data_.resize(kInitialGap, L'\0');
    gap_start_ = 0;
    gap_end_ = static_cast<std::uint32_t>(data_.size());
}

GapBuffer::GapBuffer(std::wstring text) : GapBuffer() {
    SetText(std::move(text));
}

void GapBuffer::SetText(std::wstring text) {
    const auto gap_size =
        std::max<std::uint32_t>(kInitialGap, static_cast<std::uint32_t>(text.size() / 2U + 16U));
    data_.assign(text.begin(), text.end());
    data_.resize(text.size() + gap_size, L'\0');
    gap_start_ = static_cast<std::uint32_t>(text.size());
    gap_end_ = static_cast<std::uint32_t>(data_.size());
}

std::wstring GapBuffer::Snapshot() const {
    std::wstring out;
    out.reserve(Size());
    out.append(data_.begin(), data_.begin() + gap_start_);
    out.append(data_.begin() + gap_end_, data_.end());
    return out;
}

std::uint32_t GapBuffer::Size() const {
    return static_cast<std::uint32_t>(data_.size()) - (gap_end_ - gap_start_);
}

void GapBuffer::EnsureGap(std::uint32_t minimum_size) {
    const auto current_gap = gap_end_ - gap_start_;
    if (current_gap >= minimum_size) {
        return;
    }

    const auto desired_gap =
        std::max<std::uint32_t>(minimum_size, static_cast<std::uint32_t>(data_.size() / 2U + 32U));
    const auto existing_size = Size();
    std::vector<wchar_t> expanded(existing_size + desired_gap, L'\0');
    const auto prefix_count = gap_start_;
    const auto suffix_count = static_cast<std::uint32_t>(data_.size()) - gap_end_;

    std::copy_n(data_.begin(), prefix_count, expanded.begin());
    std::copy_n(data_.begin() + gap_end_, suffix_count, expanded.begin() + prefix_count + desired_gap);

    data_ = std::move(expanded);
    gap_end_ = prefix_count + desired_gap;
}

void GapBuffer::MoveGap(std::uint32_t position) {
    position = std::min(position, Size());
    if (position == gap_start_) {
        return;
    }

    if (position < gap_start_) {
        const auto delta = gap_start_ - position;
        std::move_backward(data_.begin() + position, data_.begin() + gap_start_,
                           data_.begin() + gap_end_);
        gap_start_ -= delta;
        gap_end_ -= delta;
        return;
    }

    const auto delta = position - gap_start_;
    std::move(data_.begin() + gap_end_, data_.begin() + gap_end_ + delta, data_.begin() + gap_start_);
    gap_start_ += delta;
    gap_end_ += delta;
}

void GapBuffer::Insert(std::uint32_t position, std::wstring_view text) {
    if (text.empty()) {
        return;
    }

    MoveGap(position);
    EnsureGap(static_cast<std::uint32_t>(text.size()));
    std::copy(text.begin(), text.end(), data_.begin() + gap_start_);
    gap_start_ += static_cast<std::uint32_t>(text.size());
}

void GapBuffer::Erase(std::uint32_t position, std::uint32_t count) {
    if (count == 0 || position >= Size()) {
        return;
    }

    count = std::min(count, Size() - position);
    MoveGap(position);
    gap_end_ += count;
}

TextDocument::TextDocument() = default;

void TextDocument::Load(std::wstring text, std::vector<TextRange> bold_ranges) {
    buffer_.SetText(std::move(text));
    bold_ranges_ = std::move(bold_ranges);
    anchor_ = 0;
    caret_ = 0;
    pending_bold_ = false;
    revision_ = 1;
    NormalizeBoldRanges();
    SyncPendingStyle();
}

std::wstring TextDocument::Text() const {
    return buffer_.Snapshot();
}

const std::vector<TextRange>& TextDocument::BoldRanges() const {
    return bold_ranges_;
}

std::uint32_t TextDocument::Length() const {
    return buffer_.Size();
}

std::uint64_t TextDocument::Revision() const {
    return revision_;
}

std::uint32_t TextDocument::Caret() const {
    return caret_;
}

std::uint32_t TextDocument::Anchor() const {
    return anchor_;
}

std::uint32_t TextDocument::SelectionStart() const {
    return std::min(anchor_, caret_);
}

std::uint32_t TextDocument::SelectionEnd() const {
    return std::max(anchor_, caret_);
}

bool TextDocument::HasSelection() const {
    return anchor_ != caret_;
}

bool TextDocument::PendingBold() const {
    return pending_bold_;
}

void TextDocument::SetCaret(std::uint32_t position, bool extend_selection) {
    position = Clamp(position);
    if (!extend_selection) {
        anchor_ = position;
    }
    caret_ = position;
    if (!HasSelection()) {
        SyncPendingStyle();
    }
}

void TextDocument::SetSelection(std::uint32_t anchor, std::uint32_t caret) {
    anchor_ = Clamp(anchor);
    caret_ = Clamp(caret);
    if (!HasSelection()) {
        SyncPendingStyle();
    }
}

void TextDocument::SelectAll() {
    anchor_ = 0;
    caret_ = Length();
}

void TextDocument::MoveCaretLeft(bool extend_selection) {
    if (HasSelection() && !extend_selection) {
        SetCaret(SelectionStart(), false);
        return;
    }
    if (caret_ > 0) {
        SetCaret(caret_ - 1U, extend_selection);
    }
}

void TextDocument::MoveCaretRight(bool extend_selection) {
    if (HasSelection() && !extend_selection) {
        SetCaret(SelectionEnd(), false);
        return;
    }
    if (caret_ < Length()) {
        SetCaret(caret_ + 1U, extend_selection);
    }
}

void TextDocument::MoveCaretHome(bool extend_selection) {
    SetCaret(0, extend_selection);
}

void TextDocument::MoveCaretEnd(bool extend_selection) {
    SetCaret(Length(), extend_selection);
}

void TextDocument::InsertText(std::wstring_view text) {
    if (text.empty()) {
        return;
    }

    auto insert_at = caret_;
    if (HasSelection()) {
        insert_at = SelectionStart();
        DeleteRange(insert_at, SelectionEnd() - insert_at);
    }

    InsertAt(insert_at, text, pending_bold_);
    caret_ = insert_at + static_cast<std::uint32_t>(text.size());
    anchor_ = caret_;
    SyncPendingStyle();
    Touch();
}

void TextDocument::DeleteBackward() {
    if (HasSelection()) {
        DeleteSelection();
        return;
    }
    if (caret_ == 0) {
        return;
    }

    DeleteRange(caret_ - 1U, 1U);
    caret_ -= 1U;
    anchor_ = caret_;
    SyncPendingStyle();
    Touch();
}

void TextDocument::DeleteForward() {
    if (HasSelection()) {
        DeleteSelection();
        return;
    }
    if (caret_ >= Length()) {
        return;
    }

    DeleteRange(caret_, 1U);
    anchor_ = caret_;
    SyncPendingStyle();
    Touch();
}

void TextDocument::DeleteSelection() {
    if (!HasSelection()) {
        return;
    }

    const auto start = SelectionStart();
    DeleteRange(start, SelectionEnd() - start);
    caret_ = start;
    anchor_ = start;
    SyncPendingStyle();
    Touch();
}

void TextDocument::ToggleBold() {
    if (!HasSelection()) {
        pending_bold_ = !pending_bold_;
        Touch();
        return;
    }

    const auto start = SelectionStart();
    const auto length = SelectionEnd() - start;
    const auto enable = !IsRangeFullyBold(start, length);
    ApplyBold(start, length, enable);
    pending_bold_ = enable;
    Touch();
}

bool TextDocument::IsRangeFullyBold(std::uint32_t start, std::uint32_t length) const {
    if (length == 0) {
        return pending_bold_;
    }

    start = std::min(start, Length());
    const auto end = std::min(start + length, Length());
    auto covered_until = start;

    for (const auto& range : bold_ranges_) {
        const auto range_end = RangeEnd(range);
        if (range_end <= covered_until) {
            continue;
        }
        if (range.start > covered_until) {
            return false;
        }
        covered_until = std::min(range_end, end);
        if (covered_until >= end) {
            return true;
        }
    }

    return covered_until >= end;
}

bool TextDocument::StyleAt(std::uint32_t position) const {
    position = Clamp(position);
    for (const auto& range : bold_ranges_) {
        const auto end = RangeEnd(range);
        if (position >= range.start && position < end) {
            return true;
        }
        if (position > 0 && position == end) {
            return true;
        }
        if (range.start > position) {
            break;
        }
    }
    return false;
}

void TextDocument::InsertAt(std::uint32_t position, std::wstring_view text, bool bold) {
    const auto delta = static_cast<std::uint32_t>(text.size());
    buffer_.Insert(position, text);

    for (auto& range : bold_ranges_) {
        const auto end = RangeEnd(range);
        if (range.start >= position) {
            range.start += delta;
        } else if (end > position) {
            range.length += delta;
        }
    }

    if (bold) {
        bold_ranges_.push_back({position, delta});
    }

    NormalizeBoldRanges();
}

void TextDocument::DeleteRange(std::uint32_t start, std::uint32_t length) {
    if (length == 0 || start >= Length()) {
        return;
    }

    length = std::min(length, Length() - start);
    const auto end = start + length;
    buffer_.Erase(start, length);

    std::vector<TextRange> updated;
    updated.reserve(bold_ranges_.size());

    for (const auto& range : bold_ranges_) {
        const auto range_end = RangeEnd(range);
        if (range_end <= start) {
            updated.push_back(range);
            continue;
        }
        if (range.start >= end) {
            updated.push_back({range.start - length, range.length});
            continue;
        }

        if (range.start < start) {
            updated.push_back({range.start, start - range.start});
        }
        if (range_end > end) {
            updated.push_back({start, range_end - end});
        }
    }

    bold_ranges_ = std::move(updated);
    NormalizeBoldRanges();
}

void TextDocument::ApplyBold(std::uint32_t start, std::uint32_t length, bool enabled) {
    if (length == 0) {
        return;
    }

    start = std::min(start, Length());
    const auto end = std::min(start + length, Length());
    if (start >= end) {
        return;
    }

    if (enabled) {
        bold_ranges_.push_back({start, end - start});
        NormalizeBoldRanges();
        return;
    }

    std::vector<TextRange> updated;
    updated.reserve(bold_ranges_.size());
    for (const auto& range : bold_ranges_) {
        const auto range_end = RangeEnd(range);
        if (range_end <= start || range.start >= end) {
            updated.push_back(range);
            continue;
        }

        if (range.start < start) {
            updated.push_back({range.start, start - range.start});
        }
        if (range_end > end) {
            updated.push_back({end, range_end - end});
        }
    }

    bold_ranges_ = std::move(updated);
    NormalizeBoldRanges();
}

void TextDocument::NormalizeBoldRanges() {
    const auto max_length = Length();
    std::vector<TextRange> cleaned;
    cleaned.reserve(bold_ranges_.size());

    for (auto range : bold_ranges_) {
        if (range.length == 0 || range.start >= max_length) {
            continue;
        }
        range.length = std::min(range.length, max_length - range.start);
        if (range.length != 0) {
            cleaned.push_back(range);
        }
    }

    std::sort(cleaned.begin(), cleaned.end(),
              [](const TextRange& left, const TextRange& right) { return left.start < right.start; });

    bold_ranges_.clear();
    for (const auto& range : cleaned) {
        if (bold_ranges_.empty()) {
            bold_ranges_.push_back(range);
            continue;
        }

        auto& back = bold_ranges_.back();
        const auto back_end = RangeEnd(back);
        if (range.start <= back_end) {
            back.length = std::max(back_end, RangeEnd(range)) - back.start;
        } else {
            bold_ranges_.push_back(range);
        }
    }
}

void TextDocument::SyncPendingStyle() {
    if (!HasSelection()) {
        pending_bold_ = StyleAt(caret_);
    }
}

void TextDocument::Touch() {
    ++revision_;
}

std::uint32_t TextDocument::Clamp(std::uint32_t value) const {
    return std::min(value, Length());
}

}  // namespace raster
