#include "note_store.h"

#include <objbase.h>
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace raster {

namespace {

constexpr char kMagic[] = "RASTERNOTE/1";

std::int64_t NowUtcSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }

    const int size =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }

    const int size =
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size);
    return out;
}

std::wstring Trim(std::wstring_view value) {
    std::size_t start = 0;
    while (start < value.size() && iswspace(value[start]) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && iswspace(value[end - 1]) != 0) {
        --end;
    }

    return std::wstring(value.substr(start, end - start));
}

std::wstring CollapseWhitespace(std::wstring_view value, std::size_t limit) {
    std::wstring out;
    out.reserve(std::min(limit, value.size()));
    bool previous_space = false;

    for (wchar_t ch : value) {
        const bool is_space = (ch == L'\r') || (ch == L'\n') || (ch == L'\t') || (ch == L' ');
        if (is_space) {
            if (!out.empty() && !previous_space) {
                out.push_back(L' ');
            }
            previous_space = true;
            continue;
        }

        out.push_back(ch);
        previous_space = false;
        if (out.size() >= limit) {
            break;
        }
    }

    return Trim(out);
}

std::wstring DerivePreview(std::wstring_view text) {
    auto preview = CollapseWhitespace(text, 72);
    if (preview.empty()) {
        return L"EMPTY NOTE";
    }
    return preview;
}

std::wstring MakeGuidString() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        return L"note-fallback";
    }

    wchar_t buffer[64] = {};
    StringFromGUID2(guid, buffer, _countof(buffer));
    std::wstring out(buffer);
    out.erase(std::remove(out.begin(), out.end(), L'{'), out.end());
    out.erase(std::remove(out.begin(), out.end(), L'}'), out.end());
    return out;
}

std::string SerializeBoldRanges(const std::vector<TextRange>& ranges) {
    std::ostringstream out;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << ranges[i].start << ':' << ranges[i].length;
    }
    return out.str();
}

std::vector<TextRange> ParseBoldRanges(std::string_view serialized) {
    std::vector<TextRange> out;
    std::size_t cursor = 0;

    while (cursor < serialized.size()) {
        const auto comma = serialized.find(',', cursor);
        const auto token = serialized.substr(cursor, comma == std::string_view::npos ? serialized.size() - cursor
                                                                                    : comma - cursor);
        const auto colon = token.find(':');
        if (colon != std::string_view::npos) {
            try {
                const auto start = static_cast<std::uint32_t>(
                    std::stoul(std::string(token.substr(0, colon))));
                const auto length = static_cast<std::uint32_t>(
                    std::stoul(std::string(token.substr(colon + 1))));
                if (length != 0) {
                    out.push_back({start, length});
                }
            } catch (...) {
            }
        }

        if (comma == std::string_view::npos) {
            break;
        }
        cursor = comma + 1;
    }

    return out;
}

bool DeserializeRecord(const std::string& raw, NoteRecord& out) {
    const auto divider = raw.find("\n---\n");
    if (divider == std::string::npos) {
        return false;
    }

    const auto header = raw.substr(0, divider);
    const auto body = raw.substr(divider + 5);

    std::istringstream lines(header);
    std::string line;
    if (!std::getline(lines, line) || line != kMagic) {
        return false;
    }

    out.modified_utc = 0;
    out.bold_ranges.clear();

    while (std::getline(lines, line)) {
        if (line.rfind("modified=", 0) == 0) {
            try {
                out.modified_utc = std::stoll(line.substr(9));
            } catch (...) {
                out.modified_utc = 0;
            }
        } else if (line.rfind("bold=", 0) == 0) {
            out.bold_ranges = ParseBoldRanges(line.substr(5));
        }
    }

    out.text = Utf8ToWide(body);
    return true;
}

std::string SerializeRecord(const NoteRecord& note) {
    std::ostringstream out;
    out << kMagic << '\n';
    out << "modified=" << note.modified_utc << '\n';
    out << "bold=" << SerializeBoldRanges(note.bold_ranges) << '\n';
    out << "---\n";
    out << WideToUtf8(note.text);
    return out.str();
}

}  // namespace

bool NoteStore::Initialize() {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data))) {
        return false;
    }

    root_path_ = std::wstring(local_app_data) + L"\\RasterNoteNative";
    notes_path_ = root_path_ + L"\\notes";
    CoTaskMemFree(local_app_data);

    try {
        std::filesystem::create_directories(std::filesystem::path(notes_path_));
    } catch (...) {
        return false;
    }

    return true;
}

NoteRecord NoteStore::CreateNote() const {
    NoteRecord note;
    note.id = MakeGuidString();
    note.modified_utc = NowUtcSeconds();
    return note;
}

bool NoteStore::SaveNote(const NoteRecord& note) const {
    if (notes_path_.empty() || note.id.empty()) {
        return false;
    }

    try {
        std::filesystem::create_directories(std::filesystem::path(notes_path_));
        std::ofstream out(std::filesystem::path(PathFor(note.id)), std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        const auto serialized = SerializeRecord(note);
        out.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        return static_cast<bool>(out);
    } catch (...) {
        return false;
    }
}

bool NoteStore::LoadNote(const std::wstring& id, NoteRecord& out) const {
    if (notes_path_.empty() || id.empty()) {
        return false;
    }

    try {
        std::ifstream in(std::filesystem::path(PathFor(id)), std::ios::binary);
        if (!in) {
            return false;
        }

        std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!DeserializeRecord(raw, out)) {
            return false;
        }

        out.id = id;
        return true;
    } catch (...) {
        return false;
    }
}

bool NoteStore::DeleteNote(const std::wstring& id) const {
    if (notes_path_.empty() || id.empty()) {
        return false;
    }

    try {
        std::error_code error;
        const std::filesystem::path path(PathFor(id));
        const auto removed = std::filesystem::remove(path, error);
        if (error) {
            return false;
        }
        return removed || !std::filesystem::exists(path);
    } catch (...) {
        return false;
    }
}

std::vector<NoteSummary> NoteStore::ListRecentNotes(std::size_t limit) const {
    std::vector<NoteSummary> notes;
    if (notes_path_.empty()) {
        return notes;
    }

    try {
        const std::filesystem::path notes_dir(notes_path_);
        if (!std::filesystem::exists(notes_dir)) {
            return notes;
        }

        for (const auto& entry : std::filesystem::directory_iterator(notes_dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != L".rnote") {
                continue;
            }

            std::ifstream in(entry.path(), std::ios::binary);
            if (!in) {
                continue;
            }

            std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            NoteRecord record;
            if (!DeserializeRecord(raw, record)) {
                continue;
            }

            NoteSummary summary;
            summary.id = entry.path().stem().wstring();
            summary.title = DeriveTitle(record.text);
            summary.preview = DerivePreview(record.text);
            summary.modified_utc = record.modified_utc;
            notes.push_back(std::move(summary));
        }
    } catch (...) {
        return {};
    }

    std::sort(notes.begin(), notes.end(), [](const NoteSummary& left, const NoteSummary& right) {
        return left.modified_utc > right.modified_utc;
    });

    if (notes.size() > limit) {
        notes.resize(limit);
    }
    return notes;
}

std::wstring NoteStore::RootPath() const {
    return root_path_;
}

std::wstring NoteStore::DeriveTitle(std::wstring_view text) {
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const auto next = text.find(L'\n', cursor);
        const auto length = next == std::wstring_view::npos ? text.size() - cursor : next - cursor;
        auto line = Trim(text.substr(cursor, length));
        if (!line.empty()) {
            if (line.size() > 42) {
                line.resize(42);
            }
            return line;
        }
        if (next == std::wstring_view::npos) {
            break;
        }
        cursor = next + 1;
    }

    return L"UNTITLED";
}

std::wstring NoteStore::PathFor(const std::wstring& id) const {
    return notes_path_ + L"\\" + id + L".rnote";
}

}  // namespace raster
