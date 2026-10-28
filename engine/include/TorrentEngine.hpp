/* FluxDM — engine/include/TorrentEngine.hpp
 * Phase 2: BitTorrent support — .torrent parsing, magnet URIs,
 * piece management, SHA-1 verification, multi-file layouts.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include <array>

namespace fluxdm {

/* ── Bencode types ───────────────────────────────────────────────────────── */
struct BencodeValue;
using BencodeDict = std::unordered_map<std::string, std::shared_ptr<BencodeValue>>;
using BencodeList = std::vector<std::shared_ptr<BencodeValue>>;

struct BencodeValue {
    enum class Type { Int, Str, List, Dict };
    Type        type;
    int64_t     int_val  { 0 };
    std::string str_val;
    BencodeList list_val;
    BencodeDict dict_val;

    static std::shared_ptr<BencodeValue> parse(const std::string& data,
                                                size_t& pos);
    std::string to_string() const;
};

/* ── Piece ───────────────────────────────────────────────────────────────── */
struct Piece {
    uint32_t index;
    uint64_t offset;          /* byte offset in the logical file span */
    uint32_t length;
    std::array<uint8_t,20> sha1_hash;
    bool     verified { false };
    bool     downloaded { false };
};

/* ── TorrentFile (within a multi-file torrent) ───────────────────────────── */
struct TorrentFile {
    std::string path;         /* relative path including subdirectories */
    uint64_t    length;
    uint64_t    offset;       /* offset within the logical concatenation */
};

/* ── TorrentInfo ─────────────────────────────────────────────────────────── */
struct TorrentInfo {
    std::string              name;
    std::string              info_hash;      /* 20-byte SHA-1, hex-encoded  */
    std::string              info_hash_raw;  /* 20-byte raw bytes           */
    uint32_t                 piece_length;
    uint64_t                 total_size;
    std::vector<Piece>       pieces;
    std::vector<TorrentFile> files;          /* empty = single-file torrent */
    bool                     is_private { false };

    /* Tracker announce URLs */
    std::string              announce;
    std::vector<std::string> announce_list;

    /* DHT nodes (for trackerless) */
    std::vector<std::pair<std::string,uint16_t>> nodes;

    /* Comment, creation date */
    std::string comment;
    int64_t     creation_date { 0 };

    bool is_multi_file() const { return !files.empty(); }
    bool is_valid()      const { return !info_hash.empty() && piece_length > 0; }
};

/* ── Magnet URI ─────────────────────────────────────────────────────────── */
struct MagnetURI {
    std::string info_hash;         /* 40-char hex or 32-char base32 */
    std::string info_hash_hex;     /* always 40-char hex            */
    std::string display_name;
    std::vector<std::string> trackers;
    std::vector<std::string> web_seeds;
    uint64_t    size_hint { 0 };

    bool is_valid() const { return !info_hash_hex.empty(); }
};

/* ── Peer ────────────────────────────────────────────────────────────────── */
struct Peer {
    std::string ip;
    uint16_t    port;
    std::string peer_id;   /* 20-byte, may be empty for compact peers */
};

/* ── TorrentEngine ───────────────────────────────────────────────────────── */
class TorrentEngine {
public:
    static constexpr uint32_t BLOCK_SIZE       = 16 * 1024;  /* 16 KB blocks  */
    static constexpr uint32_t DEFAULT_PIECE_LEN = 256 * 1024;

    TorrentEngine();
    ~TorrentEngine();

    /* ── Parsing ───────────────────────────────────────────────────────────── */

    /* Load a .torrent file from disk. Returns false on parse error. */
    bool load_file(const std::string& path);

    /* Load a .torrent from raw bytes. */
    bool load_bytes(const std::vector<uint8_t>& data);

    /* Parse a magnet: URI. */
    static MagnetURI parse_magnet(const std::string& uri);

    /* ── Piece management ──────────────────────────────────────────────────── */

    /* Returns the next piece to download using "rarest first" strategy.
     * available_pieces is a bitmask from a peer's BITFIELD message. */
    int32_t next_piece(const std::vector<bool>& available_pieces) const;

    /* Mark a piece as downloaded and verify its SHA-1. */
    bool verify_piece(uint32_t piece_idx, const uint8_t* data, size_t len);

    /* Returns a bitmask of which pieces we have. */
    std::vector<bool> have_pieces() const;

    /* Percentage complete (0.0 – 100.0) */
    double completion_pct() const;

    /* ── Queries ───────────────────────────────────────────────────────────── */

    const TorrentInfo&  info()   const { return info_; }
    bool                loaded() const { return loaded_; }
    uint32_t            pieces_total()    const;
    uint32_t            pieces_have()     const;
    uint32_t            pieces_missing()  const;

    /* Map a byte offset to the TorrentFile it belongs to */
    const TorrentFile*  file_at_offset(uint64_t offset) const;

    /* Piece → file byte range (handles pieces that span file boundaries) */
    struct FileRange { std::string path; uint64_t offset; uint64_t length; };
    std::vector<FileRange> piece_file_ranges(uint32_t piece_idx) const;

    /* ── Bencode helpers (public so PeerDiscovery can use them) ────────────── */
    static std::shared_ptr<BencodeValue> bencode_parse(const std::string& data);
    static std::string                   bencode_encode(const BencodeValue& v);

    /* SHA-1 of a buffer (20 bytes) */
    static std::array<uint8_t,20> sha1(const uint8_t* data, size_t len);
    static std::string             sha1_hex(const uint8_t* data, size_t len);

    /* Base32 decode (for magnet URIs using base32 info-hash) */
    static std::string base32_decode(const std::string& encoded);

    /* Percent-encode a 20-byte binary string */
    static std::string url_encode_hash(const std::string& raw);

    /* Callbacks */
    using OnPieceVerified = std::function<void(uint32_t idx, bool ok)>;
    void set_on_piece_verified(OnPieceVerified cb) { on_piece_verified_ = std::move(cb); }

private:
    TorrentInfo      info_;
    bool             loaded_ { false };
    mutable std::mutex mu_;
    OnPieceVerified  on_piece_verified_;

    /* Piece availability hints (populated as peers connect) */
    std::vector<uint32_t> piece_availability_;

    bool parse_torrent(const std::string& raw);
    bool extract_info(const BencodeValue& root);
    void build_piece_list(const std::string& pieces_str);
    void compute_file_offsets();
};

} /* namespace fluxdm */
