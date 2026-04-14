#pragma once

#include "text_document.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace raster {

struct NoteRecord {
    std::wstring id;
    std::wstring text;
    std::vector<TextRange> bold_ranges;
    std::int64_t modified_utc = 0;
};

struct NoteSummary {
    std::wstring id;
    std::wstring title;
    std::wstring preview;
    std::int64_t modified_utc = 0;
};

class NoteStore {
public:
    bool Initialize();

    [[nodiscard]] NoteRecord CreateNote() const;
    bool SaveNote(const NoteRecord& note) const;
    bool LoadNote(const std::wstring& id, NoteRecord& out) const;
    bool DeleteNote(const std::wstring& id) const;
    [[nodiscard]] std::vector<NoteSummary> ListRecentNotes(std::size_t limit) const;

    [[nodiscard]] std::wstring RootPath() const;
    [[nodiscard]] static std::wstring DeriveTitle(std::wstring_view text);

private:
    [[nodiscard]] std::wstring PathFor(const std::wstring& id) const;

    std::wstring root_path_;
    std::wstring notes_path_;
};

}  // namespace raster
