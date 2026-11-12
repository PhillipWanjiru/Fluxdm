/* FluxDM — engine/src/MetadataParser.cpp */

#include "../include/MetadataParser.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

namespace fluxdm {

/* ── MIME → extension table ──────────────────────────────────────────────── */
static const std::unordered_map<std::string,std::string> MIME_MAP = {
    /* Video */
    {"video/mp4",              ".mp4"},
    {"video/x-matroska",       ".mkv"},
    {"video/webm",             ".webm"},
    {"video/x-msvideo",        ".avi"},
    {"video/quicktime",        ".mov"},
    {"video/x-flv",            ".flv"},
    {"video/3gpp",             ".3gp"},
    /* Audio */
    {"audio/mpeg",             ".mp3"},
    {"audio/flac",             ".flac"},
    {"audio/wav",              ".wav"},
    {"audio/x-wav",            ".wav"},
    {"audio/aac",              ".aac"},
    {"audio/ogg",              ".ogg"},
    {"audio/opus",             ".opus"},
    /* Images */
    {"image/jpeg",             ".jpg"},
    {"image/png",              ".png"},
    {"image/gif",              ".gif"},
    {"image/webp",             ".webp"},
    {"image/svg+xml",          ".svg"},
    /* Documents */
    {"application/pdf",        ".pdf"},
    {"application/epub+zip",   ".epub"},
    {"application/msword",     ".doc"},
    {"application/vnd.openxmlformats-officedocument.wordprocessingml.document", ".docx"},
    {"text/plain",             ".txt"},
    {"text/html",              ".html"},
    {"text/csv",               ".csv"},
    /* Archives */
    {"application/zip",        ".zip"},
    {"application/x-tar",      ".tar"},
    {"application/gzip",       ".gz"},
    {"application/x-bzip2",    ".bz2"},
    {"application/x-xz",       ".xz"},
    {"application/x-7z-compressed", ".7z"},
    {"application/x-rar-compressed",".rar"},
    /* Disk images */
    {"application/x-iso9660-image", ".iso"},
    /* Executables */
    {"application/x-debian-package", ".deb"},
    {"application/x-rpm",      ".rpm"},
    {"application/vnd.android.package-archive", ".apk"},
    {"application/java-archive", ".jar"},
    /* Data */
    {"application/json",       ".json"},
    {"application/xml",        ".xml"},
    {"application/octet-stream",""},    /* unknown binary — no extension */
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n\"'");
    size_t end   = s.find_last_not_of(" \t\r\n\"'");
    if (start == std::string::npos) return {};
    return s.substr(start, end - start + 1);
}

/* Whitespace-only trim — used for generic header values where quotes are
 * semantically meaningful (e.g. ETag per RFC 7232). The quote-stripping
 * trim() above is reserved for Content-Disposition filename extraction,
 * where quotes are just a delimiter, not part of the value. */
static std::string trim_ws(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end   = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    return s.substr(start, end - start + 1);
}

/* ── MetadataParser implementation ──────────────────────────────────────── */

std::unordered_map<std::string,std::string>
MetadataParser::parse_headers(const std::string& block) {
    std::unordered_map<std::string,std::string> result;
    std::istringstream ss(block);
    std::string line;
    bool first = true;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) { first = false; continue; }  /* skip status line */
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = to_lower(trim(line.substr(0, colon)));
        std::string val = trim_ws(line.substr(colon + 1));
        result[key] = val;
    }
    return result;
}

std::string MetadataParser::get_header(const std::string& block,
                                        const std::string& name) {
    auto headers = parse_headers(block);
    auto it      = headers.find(to_lower(name));
    return it != headers.end() ? it->second : std::string{};
}

std::string MetadataParser::url_decode(const std::string& enc) {
    std::string result;
    result.reserve(enc.size());
    for (size_t i = 0; i < enc.size(); ++i) {
        if (enc[i] == '%' && i + 2 < enc.size()) {
            int val = 0;
            std::istringstream hex(enc.substr(i + 1, 2));
            hex >> std::hex >> val;
            result += static_cast<char>(val);
            i += 2;
        } else if (enc[i] == '+') {
            result += ' ';
        } else {
            result += enc[i];
        }
    }
    return result;
}

std::string MetadataParser::sanitise_filename(const std::string& raw) {
    /* Strip path separators and dangerous sequences */
    std::string safe;
    safe.reserve(raw.size());
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        /* control characters too: a newline or an escape sequence in a name reaches logs, terminals and the UI */
        if (c == '/' || c == '\\' || c == ':' ||
            c == '*'  || c == '?' || c == '"'  || c == '<' ||
            c == '>'  || c == '|' || u < 0x20 || u == 0x7f)
            safe += '_';
        else
            safe += c;
    }
    /* Remove leading dots (hidden files) */
    size_t start = safe.find_first_not_of('.');
    if (start == std::string::npos) return "download";
    safe = safe.substr(start);
    /* Truncate at 255 bytes (the usual file-system limit), not in the middle of a UTF-8 sequence */
    if (safe.size() > 255) {
        size_t cut = 255;
        while (cut > 0 && (static_cast<unsigned char>(safe[cut]) & 0xC0) == 0x80) --cut;
        safe.resize(cut);
    }
    return safe.empty() ? "download" : safe;
}

/* The value of the parameter `name` ("filename" or "filename*") in a Content-Disposition value, up to the next ';', or
 * "" if it is absent or empty. Names match without regard to case. This was a pair of std::regex objects, built on
 * every call; libstdc++'s regex matcher recurses once per character, so a header value of 100 KB or so overflowed the
 * stack and took the process down. A scan cannot. */
static std::string disposition_parameter(const std::string& v, const std::string& lower, const std::string& name) {
    for (size_t at = lower.find(name); at != std::string::npos; at = lower.find(name, at + 1)) {
        size_t i = at + name.size();
        if (name == "filename" && i < lower.size() && lower[i] == '*') continue;    /* the extended form is another parameter */
        while (i < lower.size() && std::isspace(static_cast<unsigned char>(lower[i]))) ++i;
        if (i >= lower.size() || lower[i] != '=') continue;
        ++i;
        while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
        size_t end = v.find(';', i);
        std::string val = v.substr(i, end == std::string::npos ? std::string::npos : end - i);
        if (!val.empty()) return val;
    }
    return {};
}

std::string MetadataParser::parse_content_disposition(const std::string& value) {
    /* A file name is at most 255 bytes, 765 percent-encoded; no honest header value comes anywhere near this. */
    if (value.size() > 8192) return {};
    const std::string lower = to_lower(value);

    /* Try RFC 5987 extended notation first: filename*=UTF-8''name */
    std::string ext = disposition_parameter(value, lower, "filename*");
    if (!ext.empty()) {
        std::string t = trim(ext);
        if (t.size() > 7 && to_lower(t.substr(0, 7)) == "utf-8''") t.erase(0, 7);
        return sanitise_filename(url_decode(t.empty() ? trim(ext) : t));
    }

    /* Fall back to filename="..." */
    std::string basic = disposition_parameter(value, lower, "filename");
    if (!basic.empty()) return sanitise_filename(trim(basic));

    return {};
}

std::string MetadataParser::filename_from_url(const std::string& url) {
    /* Strip query string and fragment */
    std::string path = url;
    auto q = path.find('?');
    if (q != std::string::npos) path.resize(q);
    auto f = path.find('#');
    if (f != std::string::npos) path.resize(f);

    /* Last path component */
    auto slash = path.rfind('/');
    std::string name = (slash != std::string::npos)
                       ? path.substr(slash + 1) : path;

    return sanitise_filename(url_decode(name));
}

std::string MetadataParser::mime_to_extension(const std::string& mime) {
    /* Strip parameters (e.g. "video/mp4; codecs=avc1") */
    std::string base = mime;
    auto semi = base.find(';');
    if (semi != std::string::npos) base.resize(semi);
    base = trim(to_lower(base));

    auto it = MIME_MAP.find(base);
    return (it != MIME_MAP.end()) ? it->second : std::string{};
}

DownloadMeta MetadataParser::parse(const std::string& header_block,
                                    const std::string& url) const {
    DownloadMeta meta;
    meta.final_url = url;

    auto headers = parse_headers(header_block);

    /* Content-Length */
    auto cl = headers.find("content-length");
    if (cl != headers.end()) {
        try { meta.content_length = std::stoull(cl->second); }
        catch (...) {}
    }

    /* Accept-Ranges */
    auto ar = headers.find("accept-ranges");
    meta.accept_ranges = (ar != headers.end() &&
                          to_lower(ar->second).find("bytes") != std::string::npos);

    /* Transfer-Encoding */
    auto te = headers.find("transfer-encoding");
    meta.chunked = (te != headers.end() &&
                    to_lower(te->second).find("chunked") != std::string::npos);

    /* Content-Encoding */
    auto ce = headers.find("content-encoding");
    if (ce != headers.end()) meta.content_encoding = ce->second;

    /* ETag */
    auto etag = headers.find("etag");
    if (etag != headers.end()) meta.etag = etag->second;

    /* Last-Modified */
    auto lm = headers.find("last-modified");
    if (lm != headers.end()) meta.last_modified = lm->second;

    /* MIME type */
    auto ct = headers.find("content-type");
    if (ct != headers.end()) {
        meta.mime_type = ct->second;
        auto semi = meta.mime_type.find(';');
        std::string base_mime = (semi != std::string::npos)
                                ? meta.mime_type.substr(0, semi) : meta.mime_type;
        meta.extension = mime_to_extension(trim(base_mime));
    }

    /* Filename from Content-Disposition */
    auto cd = headers.find("content-disposition");
    if (cd != headers.end()) {
        meta.is_attachment = to_lower(cd->second).find("attachment") != std::string::npos;
        std::string fn     = parse_content_disposition(cd->second);
        if (!fn.empty()) { meta.filename = fn; return meta; }
    }

    /* Fall back to URL-derived filename */
    std::string url_name = filename_from_url(url);
    if (!url_name.empty() && url_name != "download") {
        /* Append extension if URL has none but MIME tells us what it is */
        if (url_name.find('.') == std::string::npos && !meta.extension.empty())
            url_name += meta.extension;
        meta.filename = url_name;
    } else {
        meta.filename = "download" + meta.extension;
    }

    return meta;
}

std::string MetadataParser::make_conditional_headers(const DownloadMeta& meta) {
    std::string hdrs;
    if (!meta.etag.empty())
        hdrs += "If-None-Match: " + meta.etag + "\r\n";
    if (!meta.last_modified.empty())
        hdrs += "If-Modified-Since: " + meta.last_modified + "\r\n";
    return hdrs;
}

} /* namespace fluxdm */
