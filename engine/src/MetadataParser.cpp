/* FluxDM — engine/src/MetadataParser.cpp */

#include "../include/MetadataParser.hpp"
#include <algorithm>
#include <cctype>
#include <regex>
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
        if (c == '/' || c == '\\' || c == '\0' || c == ':' ||
            c == '*'  || c == '?' || c == '"'  || c == '<' ||
            c == '>'  || c == '|')
            safe += '_';
        else
            safe += c;
    }
    /* Remove leading dots (hidden files) */
    size_t start = safe.find_first_not_of('.');
    if (start == std::string::npos) return "download";
    safe = safe.substr(start);
    /* Truncate at 255 chars (common FS limit) */
    if (safe.size() > 255) safe.resize(255);
    return safe.empty() ? "download" : safe;
}

std::string MetadataParser::parse_content_disposition(const std::string& value) {
    /* Try RFC 5987 extended notation first: filename*=UTF-8''name */
    std::regex rfc5987(R"(filename\*\s*=\s*(?:[Uu][Tt][Ff]-8''|)([^;]+))",
                        std::regex::icase);
    std::smatch m;
    if (std::regex_search(value, m, rfc5987))
        return sanitise_filename(url_decode(trim(m[1].str())));

    /* Fall back to filename="..." */
    std::regex basic(R"(filename\s*=\s*([^;]+))", std::regex::icase);
    if (std::regex_search(value, m, basic))
        return sanitise_filename(trim(m[1].str()));

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
