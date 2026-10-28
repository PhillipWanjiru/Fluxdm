/* FluxDM — engine/include/MetadataParser.hpp
 * Phase 2: Extract download metadata from HTTP headers and URLs.
 * Handles Content-Disposition, MIME type mapping, filename sanitisation,
 * ETag/Last-Modified for conditional requests, and redirect tracking.
 */
#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <cstdint>

namespace fluxdm {

/* ── Parsed metadata ─────────────────────────────────────────────────────── */
struct DownloadMeta {
    std::string filename;           /* best guess at the target filename      */
    std::string mime_type;          /* e.g. "video/mp4"                       */
    std::string extension;          /* e.g. ".mp4"                            */
    std::string etag;               /* for conditional If-None-Match          */
    std::string last_modified;      /* for conditional If-Modified-Since      */
    std::string content_encoding;   /* gzip / br / identity                   */
    uint64_t    content_length { 0 };
    bool        chunked        { false };
    bool        accept_ranges  { false };
    bool        is_attachment  { false };

    /* Redirect chain (in order) */
    std::vector<std::string> redirect_chain;
    std::string final_url;

    bool has_filename() const { return !filename.empty(); }
};

/* ── MetadataParser ──────────────────────────────────────────────────────── */
class MetadataParser {
public:
    MetadataParser() = default;

    /* Parse all relevant headers from a raw HTTP header block.
     * header_block must be the full response up to and including \r\n\r\n. */
    DownloadMeta parse(const std::string& header_block,
                       const std::string& url) const;

    /* Parse a single Content-Disposition header value.
     * Returns the filename if found, empty otherwise. */
    static std::string parse_content_disposition(const std::string& value);

    /* Derive a filename from the URL path component. */
    static std::string filename_from_url(const std::string& url);

    /* Map a MIME type string to a file extension (with leading dot). */
    static std::string mime_to_extension(const std::string& mime);

    /* Sanitise a filename: strip path traversal, illegal chars, etc. */
    static std::string sanitise_filename(const std::string& raw);

    /* URL-decode a percent-encoded string. */
    static std::string url_decode(const std::string& encoded);

    /* Extract header value by name (case-insensitive). */
    static std::string get_header(const std::string& block,
                                  const std::string& name);

    /* Parse all headers into a map (lowercase keys). */
    static std::unordered_map<std::string,std::string>
        parse_headers(const std::string& block);

    /* Build a conditional GET header block from previous metadata. */
    static std::string make_conditional_headers(const DownloadMeta& meta);
};

} /* namespace fluxdm */
