#include <algorithm>
#include <charconv>
#include <chrono>

#include "coring5/http/Handlers.hpp"

namespace Coring5 {

namespace {

using FileStatus = StaticFileStep::Status;

std::string_view trim_ows(std::string_view value) noexcept {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

constexpr std::array<std::string_view, 12> months{
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
constexpr std::array<std::string_view, 7> weekdays{
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr std::array<std::string_view, 7> full_weekdays{
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

template <std::size_t N>
std::size_t date_word(const std::array<std::string_view, N>& words,
                      std::string_view word) noexcept {
    return static_cast<std::size_t>(std::ranges::find(words, word) - words.begin());
}

int date_number(std::string_view text) noexcept {
    int value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return -1;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

std::optional<int64_t> parse_http_date(std::string_view date,
                                       std::chrono::sys_seconds now) noexcept {
    using namespace std::chrono;
    date = trim_ows(date);
    const auto comma = date.find(',');
    std::size_t weekday = weekdays.size();
    std::string_view day_text, month_text, year_text, time;
    if (date.size() == 29 && comma == 3 && date[4] == ' ' &&
        date[7] == ' ' && date[11] == ' ' && date[16] == ' ' && date.ends_with(" GMT")) {
        weekday = date_word(weekdays, date.substr(0, 3));
        day_text = date.substr(5, 2);
        month_text = date.substr(8, 3);
        year_text = date.substr(12, 4);
        time = date.substr(17, 8);
    } else if (comma != date.npos) {
        weekday = date_word(full_weekdays, date.substr(0, comma));
        date.remove_prefix(comma + 1);
        if (date.size() != 23 || date[0] != ' ' || date[3] != '-' ||
            date[7] != '-' || date[10] != ' ' || !date.ends_with(" GMT")) {
            return std::nullopt;
        }
        day_text = date.substr(1, 2);
        month_text = date.substr(4, 3);
        year_text = date.substr(8, 2);
        time = date.substr(11, 8);
    } else if (date.size() == 24 && date[3] == ' ' && date[7] == ' ' &&
               date[10] == ' ' && date[19] == ' ') {
        weekday = date_word(weekdays, date.substr(0, 3));
        day_text = date.substr(date[8] == ' ' ? 9 : 8, date[8] == ' ' ? 1 : 2);
        month_text = date.substr(4, 3);
        year_text = date.substr(20, 4);
        time = date.substr(11, 8);
    } else {
        return std::nullopt;
    }

    const auto month = date_word(months, month_text) + 1;
    const int day = date_number(day_text);
    int year = date_number(year_text);
    const int hour = date_number(time.substr(0, 2));
    const int minute = date_number(time.substr(3, 2));
    const int second = date_number(time.substr(6, 2));
    if (weekday >= weekdays.size() || month > months.size() || day < 1 || year < 0 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60 ||
        time[2] != ':' || time[5] != ':') {
        return std::nullopt;
    }
    const auto tod = hours(hour) + minutes(minute) + seconds(second);
    if (year_text.size() == 2) {
        const auto today = year_month_day(floor<days>(now));
        const auto cutoff_date = today + years(50);
        const auto cutoff = sys_days(cutoff_date) + (now - floor<days>(now));
        year += (static_cast<int>(cutoff_date.year()) / 100) * 100;
        if (sys_days(std::chrono::year(year) / static_cast<int>(month) / day) + tod > cutoff) {
            year -= 100;
        }
    }
    const auto ymd = std::chrono::year(year) / static_cast<int>(month) / day;
    if (!ymd.ok() || std::chrono::weekday(sys_days(ymd)).c_encoding() != weekday) {
        return std::nullopt;
    }
    return (sys_days(ymd) + tod).time_since_epoch().count();
}

void format_http_date(std::chrono::sys_seconds point, std::array<char, 30>& out) noexcept {
    using namespace std::chrono;
    constexpr auto first = sys_days(year(0) / January / 1);
    constexpr auto end = sys_days(year(10000) / January / 1);
    if (point < first || point >= end) {
        return;
    }
    const auto date = floor<days>(point);
    const auto ymd = year_month_day(date);
    const auto time = hh_mm_ss(point - date);
    constexpr std::string_view shape = "   , 00     0000 00:00:00 GMT";
    std::ranges::copy(shape, out.begin());
    std::ranges::copy(weekdays[weekday(date).c_encoding()], out.begin());
    std::ranges::copy(months[static_cast<unsigned>(ymd.month()) - 1], out.begin() + 8);
    const auto write_number = [&](std::size_t offset, std::size_t width, unsigned value) {
        for (std::size_t end_digit = offset + width; end_digit > offset; value /= 10) {
            out[--end_digit] = static_cast<char>('0' + value % 10);
        }
    };
    write_number(5, 2, static_cast<unsigned>(ymd.day()));
    write_number(12, 4, static_cast<unsigned>(static_cast<int>(ymd.year())));
    write_number(17, 2, static_cast<unsigned>(time.hours().count()));
    write_number(20, 2, static_cast<unsigned>(time.minutes().count()));
    write_number(23, 2, static_cast<unsigned>(time.seconds().count()));
}

bool none_match(std::string_view value, std::string_view weak_etag) noexcept {
    value = trim_ows(value);
    if (value == "*") {
        return true;
    }

    bool matched = false;
    while (!value.empty()) {
        if (value.front() == ',') {
            value = trim_ows(value.substr(1));
            continue;
        }
        if (value.starts_with("W/")) {
            value.remove_prefix(2);
        }
        if (value.empty() || value.front() != '"') {
            return false;
        }
        const auto end = value.find('"', 1);
        if (end == std::string_view::npos ||
            !std::ranges::all_of(value.substr(1, end - 1),
                                 [](unsigned char c) { return c >= 0x21 && c != 0x7f; })) {
            return false;
        }
        matched |= value.substr(0, end + 1) == weak_etag.substr(2);
        value = trim_ows(value.substr(end + 1));
        if (!value.empty() && value.front() != ',') {
            return false;
        }
    }
    return matched;
}

std::optional<uint64_t> decimal(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    uint64_t number = 0;
    const char* const end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(value.data(), end, number);
    if (ptr != end) {
        return std::nullopt;
    }
    // Out-of-range start offsets are unsatisfiable. End offsets and suffix
    // lengths are clamped to the representation length below.
    return ec == std::errc::result_out_of_range ? UINT64_MAX : number;
}

struct Selection {
    uint16_t status;
    uint64_t offset;
    uint64_t length;
};

Selection select_range(std::string_view value, uint64_t size) noexcept {
    constexpr Selection rejected{416, 0, 0};
    const Selection full{200, 0, size};
    value = trim_ows(value);
    const auto eq = value.find('=');
    if (eq == std::string_view::npos || !valid_static_header_name(value.substr(0, eq))) {
        return rejected;
    }
    constexpr std::string_view bytes = "bytes";
    const auto unit = value.substr(0, eq);
    if (!std::ranges::equal(
            unit, bytes, [](char a, char b) { return a == b || a == b - ('a' - 'A'); })) {
        return full;
    }
    value.remove_prefix(eq + 1);
    if (value.find(',') != std::string_view::npos) {
        return full;  // multipart/byteranges is unsupported
    }
    const auto dash = value.find('-');
    if (dash == std::string_view::npos) {
        return rejected;
    }
    const auto last_text = value.substr(dash + 1);
    const auto last = decimal(last_text);
    if (dash == 0) {
        if (!last || *last == 0) {
            return rejected;
        }
        const auto count = std::min(size, *last);
        return size == 0 ? full : Selection{206, size - count, count};
    }
    const auto first = decimal(value.substr(0, dash));
    if (!first || *first >= size || (!last_text.empty() && (!last || *last < *first))) {
        return rejected;
    }
    const auto end = std::min(size - 1, last.value_or(UINT64_MAX));
    return {206, *first, end - *first + 1};
}

}  // namespace

StaticFile StaticFile::prepare(const StaticFileMetadata& file,
                               bool head_only,
                               StaticFileConditions conditions) noexcept {
    StaticFile response;
    response.head_only_ = head_only;

    auto out = std::ranges::copy(std::string_view{"W/\""}, response.etag_.data()).out;
    for (const uint64_t field : {static_cast<uint64_t>(file.device_major),
                                 static_cast<uint64_t>(file.device_minor),
                                 file.inode,
                                 file.size,
                                 static_cast<uint64_t>(file.modified.seconds),
                                 static_cast<uint64_t>(file.modified.nanoseconds),
                                 static_cast<uint64_t>(file.changed.seconds),
                                 static_cast<uint64_t>(file.changed.nanoseconds)}) {
        out = std::to_chars(out, response.etag_.data() + response.etag_.size(), field, 16).ptr;
        *out++ = '-';
    }
    out[-1] = '"';
    response.etag_size_ = static_cast<std::size_t>(out - response.etag_.data());

    using namespace std::chrono;
    const auto now = floor<seconds>(system_clock::now());
    const auto modified = std::min(sys_seconds(seconds(file.modified.seconds)), now);
    format_http_date(modified, response.last_modified_);
    const auto since = !conditions.if_none_match && conditions.if_modified_since
                           ? parse_http_date(*conditions.if_modified_since, now)
                           : std::nullopt;
    if ((conditions.if_none_match && none_match(*conditions.if_none_match, response.etag())) ||
        (since && !response.last_modified().empty() && modified.time_since_epoch().count() <= *since)) {
        response.status_ = 304;
        return response;
    }
    const auto selection = !head_only && conditions.range && !conditions.has_if_range
                               ? select_range(*conditions.range, file.size)
                               : Selection{200, 0, file.size};
    response.status_ = selection.status;
    response.offset_ = selection.offset;
    response.length_ = selection.length;

    if (selection.status == 206 || selection.status == 416) {
        auto* const start = response.content_range_.data();
        auto* const end = start + response.content_range_.size();
        auto* p = std::ranges::copy(std::string_view{"bytes "}, start).out;
        if (selection.status == 206) {
            p = std::to_chars(p, end, selection.offset).ptr;
            *p++ = '-';
            p = std::to_chars(p, end, selection.offset + (selection.length - 1)).ptr;
        } else {
            *p++ = '*';
        }
        *p++ = '/';
        p = std::to_chars(p, end, file.size).ptr;
        response.content_range_size_ = static_cast<std::size_t>(p - start);
    }
    return response;
}

Result<StaticFileStep> StaticFile::apply(Exchange& ex,
                                         ApplyCursor& cur,
                                         uint32_t read_capacity, std::span<const ResponseField> fields) const noexcept {
    constexpr auto stream = StreamId::Res;
    const auto& body = ex.stream(stream);
    const bool no_content = content_forbidden(status_, head_only_);
    if (ex.request_meta().head_only != head_only_ || cur.sent > length_ ||
        (!no_content && cur.sent < length_ && read_capacity == 0) || body.claimed() != 0) {
        return unexpected_error(Errc::InvalidArgument);
    }
    if (ex.phase() == Phase::Closing) {
        return unexpected_error(Errc::Canceled);
    }
    if (body.state() == StreamState::Aborted) {
        return unexpected_error(body.aborted_with());
    }
    if (!cur.head_done) {
        if (ex.phase() == Phase::Routing && !ex.attach_responder(SideRef{})) {
            return unexpected_error(Errc::Canceled);
        }
        ResponseMeta meta;
        meta.status = status_;
        meta.no_content = no_content;
        meta.declared_known = status_ != 304;
        meta.declared = length_;
        if (ex.publish_final(meta, fields) != HeadStatus::Ok) {
            return unexpected_error(Errc::Canceled);
        }
        cur.head_done = true;
    }
    if (body.state() == StreamState::Finished) {
        return StaticFileStep{FileStatus::Done};
    }
    if (cur.sent == length_) {
        if (ex.finish(stream) != WriteStatus::Ok) {
            return unexpected_error(body.aborted_with());
        }
        return StaticFileStep{FileStatus::Done};
    }

    if (body.want() != 0) {
        return StaticFileStep{FileStatus::Blocked};
    }
    const auto want = static_cast<uint32_t>(std::min<uint64_t>(length_ - cur.sent, read_capacity));
    const auto claim = ex.claim(stream, want);
    if (claim.status == ClaimStatus::Blocked) {
        return StaticFileStep{FileStatus::Blocked};
    }
    if (claim.status != ClaimStatus::Granted) {
        return unexpected_error(Errc::Canceled);
    }
    return StaticFileStep{FileStatus::Read, offset_ + cur.sent, claim.granted};
}

Result<> StaticFile::on_read(Exchange& ex,
                             ApplyCursor& cur,
                             Result<std::span<const std::byte>> completion) const noexcept {
    constexpr auto stream = StreamId::Res;
    const auto& body = ex.stream(stream);
    if (body.state() != StreamState::Open) {
        return unexpected_error(Errc::Canceled);
    }
    if (!cur.head_done || body.claimed() == 0) {
        return unexpected_error(Errc::InvalidArgument);
    }
    if (!completion || completion->empty() || completion->size() > body.claimed()) {
        const Error error = !completion           ? completion.error()
                            : completion->empty() ? Error{Errc::Eof, 0}
                                                  : Error{Errc::InvalidArgument, 0};
        ex.abort(stream, error.code);
        return std::unexpected(error);
    }
    if (ex.push(stream, *completion) != WriteStatus::Ok) {
        return unexpected_error(body.aborted_with());
    }
    cur.sent += completion->size();
    return {};
}

}  // namespace Coring5
