/* FluxDM — engine/src/TorrentEngine.cpp */

#include "../include/TorrentEngine.hpp"
#include <openssl/sha.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <regex>
#include <cctype>
#include <charconv>

namespace fluxdm {

/* ══════════════════════════════════════════════════════════════════════════
 * Bencode parser
 * ══════════════════════════════════════════════════════════════════════════ */

/* The decimal number in data[first, last): optional '-', then digits only (no sign, no spaces, no trailing text),
 * at most 19 of them, so that it fits an int64. Anything else is malformed. */
static int64_t bencode_number(const std::string& data, size_t first, size_t last) {
    int64_t n = 0;
    const char* b = data.data() + first;
    const char* e = data.data() + last;
    if (b == e) throw std::runtime_error("bencode: empty number");
    auto r = std::from_chars(b, e, n);
    if (r.ec != std::errc() || r.ptr != e) throw std::runtime_error("bencode: bad number");
    return n;
}

std::shared_ptr<BencodeValue> BencodeValue::parse(const std::string& data,
                                                   size_t& pos, unsigned depth) {
    if (pos >= data.size()) return nullptr;
    if (depth > MAX_DEPTH) throw std::runtime_error("bencode: nested too deeply");

    auto v = std::make_shared<BencodeValue>();
    v->begin = pos;

    char c = data[pos];

    if (c == 'i') {
        /* Integer: i<digits>e */
        ++pos;
        size_t end = data.find('e', pos);
        if (end == std::string::npos) throw std::runtime_error("bencode: bad int");
        v->type    = Type::Int;
        v->int_val = bencode_number(data, pos, end);
        pos = end + 1;

    } else if (c == 'l') {
        /* List: l<items>e */
        ++pos;
        v->type = Type::List;
        while (pos < data.size() && data[pos] != 'e') {
            auto item = parse(data, pos, depth + 1);
            if (!item) break;
            v->list_val.push_back(item);
        }
        if (pos < data.size()) ++pos; /* consume 'e' */

    } else if (c == 'd') {
        /* Dict: d<key><value>...e */
        ++pos;
        v->type = Type::Dict;
        while (pos < data.size() && data[pos] != 'e') {
            auto key = parse(data, pos, depth + 1);
            if (!key || key->type != Type::Str) break;
            auto val = parse(data, pos, depth + 1);
            if (!val) break;
            v->dict_val[key->str_val] = val;
        }
        if (pos < data.size()) ++pos;

    } else if (std::isdigit(static_cast<unsigned char>(c))) {
        /* Byte string: <len>:<bytes> */
        size_t colon = data.find(':', pos);
        if (colon == std::string::npos) throw std::runtime_error("bencode: bad str");
        int64_t len = bencode_number(data, pos, colon);
        pos = colon + 1;
        /* compared as "len > what is left", not "pos + len > size", which wraps for a huge len */
        if (len < 0 || static_cast<uint64_t>(len) > data.size() - pos) throw std::runtime_error("bencode: str truncated");
        v->type    = Type::Str;
        v->str_val = data.substr(pos, static_cast<size_t>(len));
        pos += static_cast<size_t>(len);

    } else {
        throw std::runtime_error(std::string("bencode: unexpected char '") + c + "'");
    }

    v->end = pos;
    return v;
}

std::string BencodeValue::to_string() const {
    switch (type) {
    case Type::Int:  return std::to_string(int_val);
    case Type::Str:  return str_val;
    case Type::List: {
        std::string s = "[";
        for (size_t i = 0; i < list_val.size(); ++i) {
            if (i) s += ",";
            s += list_val[i]->to_string();
        }
        return s + "]";
    }
    case Type::Dict: {
        std::string s = "{";
        bool first = true;
        for (const auto& [k,vv] : dict_val) {
            if (!first) s += ",";
            s += k + ":" + vv->to_string();
            first = false;
        }
        return s + "}";
    }
    }
    return {};
}

/* ══════════════════════════════════════════════════════════════════════════
 * SHA-1 helpers
 * ══════════════════════════════════════════════════════════════════════════ */

std::array<uint8_t,20> TorrentEngine::sha1(const uint8_t* data, size_t len) {
    std::array<uint8_t,20> digest{};
    SHA1(data, len, digest.data());
    return digest;
}

std::string TorrentEngine::sha1_hex(const uint8_t* data, size_t len) {
    auto digest = sha1(data, len);
    std::ostringstream ss;
    for (auto b : digest) ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    return ss.str();
}

std::string TorrentEngine::url_encode_hash(const std::string& raw) {
    std::ostringstream ss;
    for (unsigned char c : raw) {
        ss << '%' << std::hex << std::uppercase
           << std::setw(2) << std::setfill('0') << (int)c;
    }
    return ss.str();
}

/* ══════════════════════════════════════════════════════════════════════════
 * Base32 decode
 * ══════════════════════════════════════════════════════════════════════════ */

std::string TorrentEngine::base32_decode(const std::string& encoded) {
    static const std::string alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string result;
    uint32_t buffer = 0;   /* unsigned, and cut back to the bits not yet used: a signed int overflowed here (UB) */
    int      bits   = 0;
    for (char c : encoded) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (c == '=') break;
        auto pos = alpha.find(c);
        if (pos == std::string::npos) continue;
        buffer = (buffer << 5) | static_cast<uint32_t>(pos);
        bits  += 5;
        if (bits >= 8) {
            result += static_cast<char>((buffer >> (bits - 8)) & 0xFFu);
            bits   -= 8;
            buffer &= (1u << bits) - 1u;
        }
    }
    return result;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Magnet URI parser
 * ══════════════════════════════════════════════════════════════════════════ */

static std::string url_decode_str(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i+2 < s.size()) {
            int val;
            std::istringstream ss(s.substr(i+1,2));
            ss >> std::hex >> val;
            r += static_cast<char>(val);
            i += 2;
        } else if (s[i] == '+') { r += ' '; }
        else { r += s[i]; }
    }
    return r;
}

MagnetURI TorrentEngine::parse_magnet(const std::string& uri) {
    MagnetURI m;
    if (uri.substr(0,8) != "magnet:?") return m;

    std::string params = uri.substr(8);
    std::istringstream ss(params);
    std::string param;

    while (std::getline(ss, param, '&')) {
        auto eq = param.find('=');
        if (eq == std::string::npos) continue;
        std::string key = param.substr(0, eq);
        std::string val = url_decode_str(param.substr(eq + 1));

        if (key == "xt") {
            /* urn:btih:<hash> */
            auto prefix = val.find("urn:btih:");
            if (prefix != std::string::npos) {
                m.info_hash = val.substr(prefix + 9);
                if (m.info_hash.size() == 32) {
                    /* Base32 */
                    std::string raw = base32_decode(m.info_hash);
                    std::ostringstream hex;
                    for (unsigned char c : raw)
                        hex << std::hex << std::setw(2) << std::setfill('0') << (int)c;
                    m.info_hash_hex = hex.str();
                } else if (m.info_hash.size() == 40) {
                    m.info_hash_hex = m.info_hash;
                }
            }
        } else if (key == "dn") {
            m.display_name = val;
        } else if (key == "tr") {
            m.trackers.push_back(val);
        } else if (key == "ws") {
            m.web_seeds.push_back(val);
        } else if (key == "xl") {
            try { m.size_hint = std::stoull(val); } catch(...) {}
        }
    }
    return m;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Bencode encoder (for tracker requests)
 * ══════════════════════════════════════════════════════════════════════════ */

std::string TorrentEngine::bencode_encode(const BencodeValue& v) {
    std::ostringstream ss;
    switch (v.type) {
    case BencodeValue::Type::Int:
        ss << 'i' << v.int_val << 'e'; break;
    case BencodeValue::Type::Str:
        ss << v.str_val.size() << ':' << v.str_val; break;
    case BencodeValue::Type::List:
        ss << 'l';
        for (const auto& item : v.list_val) ss << bencode_encode(*item);
        ss << 'e'; break;
    case BencodeValue::Type::Dict: {
        /* Keys must be sorted */
        ss << 'd';
        std::vector<std::string> keys;
        keys.reserve(v.dict_val.size());
        for (const auto& [k,_] : v.dict_val) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys) {
            ss << k.size() << ':' << k;
            ss << bencode_encode(*v.dict_val.at(k));
        }
        ss << 'e'; break;
    }
    }
    return ss.str();
}

std::shared_ptr<BencodeValue> TorrentEngine::bencode_parse(const std::string& data) {
    size_t pos = 0;
    return BencodeValue::parse(data, pos);
}

/* ══════════════════════════════════════════════════════════════════════════
 * TorrentEngine — load and parse
 * ══════════════════════════════════════════════════════════════════════════ */

TorrentEngine::TorrentEngine() = default;
TorrentEngine::~TorrentEngine() = default;

bool TorrentEngine::load_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string raw((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    return load_bytes(std::vector<uint8_t>(raw.begin(), raw.end()));
}

bool TorrentEngine::load_bytes(const std::vector<uint8_t>& data) {
    std::string raw(data.begin(), data.end());
    loaded_ = parse_torrent(raw);
    return loaded_;
}

bool TorrentEngine::parse_torrent(const std::string& raw) {
    std::shared_ptr<BencodeValue> root;
    try { root = bencode_parse(raw); }
    catch (const std::exception&) { return false; }

    if (!root || root->type != BencodeValue::Type::Dict) return false;
    info_ = TorrentInfo{};
    piece_availability_.clear();
    if (!extract_info(*root, raw)) { info_ = TorrentInfo{}; return false; }
    return true;
}

/* A name that becomes part of a path on disk: one plain component. A torrent is untrusted input, and a path such as
 * "../../.bashrc" or "/etc/cron.d/x" in it would put a file outside the download directory. */
static bool plain_component(const std::string& s) {
    if (s.empty() || s == "." || s == "..") return false;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

/* Sizes that keep every sum and product in this file inside 64 bits. */
static constexpr int64_t MAX_TORRENT_BYTES = int64_t{1} << 60;
static constexpr int64_t MAX_PIECE_BYTES   = int64_t{1} << 30;

bool TorrentEngine::extract_info(const BencodeValue& root, const std::string& raw) {
    const auto& d = root.dict_val;

    /* announce */
    auto ann = d.find("announce");
    if (ann != d.end()) info_.announce = ann->second->str_val;

    /* announce-list */
    auto anl = d.find("announce-list");
    if (anl != d.end()) {
        for (const auto& tier : anl->second->list_val)
            for (const auto& url : tier->list_val)
                info_.announce_list.push_back(url->str_val);
    }

    /* nodes (DHT) */
    auto nodes = d.find("nodes");
    if (nodes != d.end()) {
        for (const auto& node : nodes->second->list_val) {
            if (node->list_val.size() >= 2) {
                info_.nodes.push_back({
                    node->list_val[0]->str_val,
                    static_cast<uint16_t>(node->list_val[1]->int_val)
                });
            }
        }
    }

    /* comment / creation date */
    auto cm = d.find("comment");
    if (cm != d.end()) info_.comment = cm->second->str_val;
    auto cd = d.find("creation date");
    if (cd != d.end()) info_.creation_date = cd->second->int_val;

    /* info dict */
    auto info_it = d.find("info");
    if (info_it == d.end() || info_it->second->type != BencodeValue::Type::Dict) return false;
    const auto& info = info_it->second->dict_val;

    auto nm = info.find("name");
    if (nm != info.end()) {
        info_.name = nm->second->str_val;
        if (!plain_component(info_.name)) return false;
    }

    auto pl = info.find("piece length");
    if (pl == info.end() || pl->second->type != BencodeValue::Type::Int) return false;
    const int64_t piece_length = pl->second->int_val;
    if (piece_length < 1 || piece_length > MAX_PIECE_BYTES) return false;
    info_.piece_length = static_cast<uint32_t>(piece_length);

    auto pcs = info.find("pieces");
    if (pcs == info.end() || pcs->second->type != BencodeValue::Type::Str) return false;
    const std::string& pieces_str = pcs->second->str_val;
    if (pieces_str.empty() || pieces_str.size() % 20 != 0) return false;

    auto priv = info.find("private");
    if (priv != info.end()) info_.is_private = priv->second->int_val != 0;

    /* Single-file vs multi-file. The total size has to be known before the pieces can be laid out, and every
     * length, path and count is checked here: they all come from the torrent. */
    auto files = info.find("files");
    if (files != info.end()) {
        /* Multi-file */
        if (files->second->type != BencodeValue::Type::List || files->second->list_val.empty()) return false;
        int64_t total = 0;
        for (const auto& file : files->second->list_val) {
            if (file->type != BencodeValue::Type::Dict) return false;
            TorrentFile tf;
            auto flen = file->dict_val.find("length");
            if (flen == file->dict_val.end() || flen->second->type != BencodeValue::Type::Int) return false;
            const int64_t len = flen->second->int_val;
            if (len < 0 || len > MAX_TORRENT_BYTES) return false;
            total += len;                                   /* both terms are below 2^60 */
            if (total > MAX_TORRENT_BYTES) return false;
            tf.length = static_cast<uint64_t>(len);

            auto fpath = file->dict_val.find("path");
            if (fpath == file->dict_val.end() || fpath->second->type != BencodeValue::Type::List
                || fpath->second->list_val.empty()) return false;
            bool first = true;
            for (const auto& comp : fpath->second->list_val) {
                if (comp->type != BencodeValue::Type::Str || !plain_component(comp->str_val)) return false;
                if (!first) tf.path += "/";
                tf.path += comp->str_val;
                first = false;
            }
            info_.files.push_back(tf);
        }
        compute_file_offsets();
    } else {
        /* Single file */
        auto flen = info.find("length");
        if (flen == info.end() || flen->second->type != BencodeValue::Type::Int) return false;
        const int64_t len = flen->second->int_val;
        if (len < 0 || len > MAX_TORRENT_BYTES) return false;
        info_.total_size = static_cast<uint64_t>(len);
    }
    if (info_.total_size == 0) return false;

    /* There has to be exactly the number of pieces that the size and the piece length give. */
    const uint64_t expected = (info_.total_size + info_.piece_length - 1) / info_.piece_length;
    if (pieces_str.size() / 20 != expected) return false;
    build_piece_list(pieces_str);

    /* The info-hash is the SHA-1 of the 'info' dictionary exactly as it is in the file (BEP 3). A re-encoding of the
     * parsed dictionary is the same only when the creator wrote it in canonical form, and a different hash means a
     * different swarm. */
    const size_t from = info_it->second->begin;
    const size_t len  = info_it->second->end - from;
    const auto*  span = reinterpret_cast<const uint8_t*>(raw.data()) + from;
    const auto   digest = sha1(span, len);
    info_.info_hash_raw = std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
    info_.info_hash     = sha1_hex(span, len);

    /* Availability array */
    piece_availability_.assign(info_.pieces.size(), 0);

    return true;
}

void TorrentEngine::build_piece_list(const std::string& pieces_str) {
    if (pieces_str.size() % 20 != 0) return;
    size_t n = pieces_str.size() / 20;
    info_.pieces.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        Piece p;
        p.index  = static_cast<uint32_t>(i);
        p.offset = static_cast<uint64_t>(i) * info_.piece_length;
        /* The last piece is what is left of the content */
        if (i == n - 1)
            p.length = static_cast<uint32_t>(info_.total_size - p.offset);
        else
            p.length = info_.piece_length;
        std::memcpy(p.sha1_hash.data(), pieces_str.data() + i * 20, 20);
        info_.pieces.push_back(p);
    }
}

void TorrentEngine::compute_file_offsets() {
    uint64_t offset = 0;
    info_.total_size = 0;
    for (auto& f : info_.files) {
        f.offset = offset;
        offset  += f.length;
        info_.total_size += f.length;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Piece management
 * ══════════════════════════════════════════════════════════════════════════ */

int32_t TorrentEngine::next_piece(const std::vector<bool>& available) const {
    std::lock_guard<std::mutex> lock(mu_);
    /* Rarest-first: pick the piece with lowest availability that we
     * don't have and the peer does have */
    int32_t   best_idx  = -1;
    uint32_t  best_avail = UINT32_MAX;

    for (size_t i = 0; i < info_.pieces.size(); ++i) {
        if (info_.pieces[i].downloaded) continue;
        if (i >= available.size() || !available[i]) continue;   /* a bitfield that stops short says nothing about the rest */
        uint32_t avail = (i < piece_availability_.size())
                         ? piece_availability_[i] : 0;
        if (avail < best_avail) {
            best_avail = avail;
            best_idx   = static_cast<int32_t>(i);
        }
    }
    return best_idx;
}

bool TorrentEngine::verify_piece(uint32_t idx, const uint8_t* data, size_t len) {
    if (idx >= info_.pieces.size()) return false;
    Piece& p   = info_.pieces[idx];
    auto digest = sha1(data, len);
    bool ok     = (std::memcmp(digest.data(), p.sha1_hash.data(), 20) == 0);

    {
        std::lock_guard<std::mutex> lock(mu_);
        p.verified    = ok;
        p.downloaded  = ok;
    }

    if (on_piece_verified_) on_piece_verified_(idx, ok);
    return ok;
}

std::vector<bool> TorrentEngine::have_pieces() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<bool> bm(info_.pieces.size(), false);
    for (size_t i = 0; i < info_.pieces.size(); ++i)
        bm[i] = info_.pieces[i].downloaded;
    return bm;
}

double TorrentEngine::completion_pct() const {
    if (info_.pieces.empty()) return 0.0;
    uint32_t done = pieces_have();
    return 100.0 * done / info_.pieces.size();
}

uint32_t TorrentEngine::pieces_total()   const { return static_cast<uint32_t>(info_.pieces.size()); }
uint32_t TorrentEngine::pieces_have()    const {
    std::lock_guard<std::mutex> lock(mu_);
    uint32_t n = 0;
    for (const auto& p : info_.pieces) if (p.downloaded) ++n;
    return n;
}
uint32_t TorrentEngine::pieces_missing() const { return pieces_total() - pieces_have(); }

const TorrentFile* TorrentEngine::file_at_offset(uint64_t offset) const {
    for (const auto& f : info_.files)
        if (offset >= f.offset && offset < f.offset + f.length) return &f;
    return nullptr;
}

std::vector<TorrentEngine::FileRange>
TorrentEngine::piece_file_ranges(uint32_t piece_idx) const {
    std::vector<FileRange> result;
    if (piece_idx >= info_.pieces.size()) return result;

    const Piece& p      = info_.pieces[piece_idx];
    uint64_t piece_end  = p.offset + p.length;

    for (const auto& f : info_.files) {
        uint64_t f_end = f.offset + f.length;
        if (f_end <= p.offset)  continue;
        if (f.offset >= piece_end) break;

        uint64_t start  = std::max(p.offset,   f.offset);
        uint64_t end    = std::min(piece_end,  f_end);
        result.push_back({ f.path, start - f.offset, end - start });
    }
    return result;
}

} /* namespace fluxdm */
