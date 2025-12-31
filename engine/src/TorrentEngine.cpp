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

namespace fluxdm {

/* ══════════════════════════════════════════════════════════════════════════
 * Bencode parser
 * ══════════════════════════════════════════════════════════════════════════ */

std::shared_ptr<BencodeValue> BencodeValue::parse(const std::string& data,
                                                   size_t& pos) {
    if (pos >= data.size()) return nullptr;

    auto v = std::make_shared<BencodeValue>();

    char c = data[pos];

    if (c == 'i') {
        /* Integer: i<digits>e */
        ++pos;
        size_t end = data.find('e', pos);
        if (end == std::string::npos) throw std::runtime_error("bencode: bad int");
        v->type    = Type::Int;
        v->int_val = std::stoll(data.substr(pos, end - pos));
        pos = end + 1;

    } else if (c == 'l') {
        /* List: l<items>e */
        ++pos;
        v->type = Type::List;
        while (pos < data.size() && data[pos] != 'e') {
            auto item = parse(data, pos);
            if (!item) break;
            v->list_val.push_back(item);
        }
        if (pos < data.size()) ++pos; /* consume 'e' */

    } else if (c == 'd') {
        /* Dict: d<key><value>...e */
        ++pos;
        v->type = Type::Dict;
        while (pos < data.size() && data[pos] != 'e') {
            auto key = parse(data, pos);
            if (!key || key->type != Type::Str) break;
            auto val = parse(data, pos);
            if (!val) break;
            v->dict_val[key->str_val] = val;
        }
        if (pos < data.size()) ++pos;

    } else if (std::isdigit(static_cast<unsigned char>(c))) {
        /* Byte string: <len>:<bytes> */
        size_t colon = data.find(':', pos);
        if (colon == std::string::npos) throw std::runtime_error("bencode: bad str");
        size_t len = std::stoull(data.substr(pos, colon - pos));
        pos = colon + 1;
        if (pos + len > data.size()) throw std::runtime_error("bencode: str truncated");
        v->type    = Type::Str;
        v->str_val = data.substr(pos, len);
        pos += len;

    } else {
        throw std::runtime_error(std::string("bencode: unexpected char '") + c + "'");
    }

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
    int buffer = 0, bits = 0;
    for (char c : encoded) {
        c = std::toupper(static_cast<unsigned char>(c));
        if (c == '=') break;
        auto pos = alpha.find(c);
        if (pos == std::string::npos) continue;
        buffer = (buffer << 5) | static_cast<int>(pos);
        bits  += 5;
        if (bits >= 8) {
            result += static_cast<char>((buffer >> (bits - 8)) & 0xFF);
            bits -= 8;
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
    catch (const std::exception& e) { return false; }

    if (!root || root->type != BencodeValue::Type::Dict) return false;
    return extract_info(*root);
}

bool TorrentEngine::extract_info(const BencodeValue& root) {
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
    if (info_it == d.end()) return false;
    const auto& info = info_it->second->dict_val;

    auto nm = info.find("name");
    if (nm != info.end()) info_.name = nm->second->str_val;

    auto pl = info.find("piece length");
    if (pl == info.end()) return false;
    info_.piece_length = static_cast<uint32_t>(pl->second->int_val);

    auto pcs = info.find("pieces");
    if (pcs == info.end()) return false;
    build_piece_list(pcs->second->str_val);

    auto priv = info.find("private");
    if (priv != info.end()) info_.is_private = priv->second->int_val != 0;

    /* Single-file vs multi-file */
    auto files = info.find("files");
    if (files != info.end()) {
        /* Multi-file */
        for (const auto& file : files->second->list_val) {
            TorrentFile tf;
            auto flen = file->dict_val.find("length");
            if (flen != file->dict_val.end())
                tf.length = static_cast<uint64_t>(flen->second->int_val);
            auto fpath = file->dict_val.find("path");
            if (fpath != file->dict_val.end()) {
                bool first = true;
                for (const auto& comp : fpath->second->list_val) {
                    if (!first) tf.path += "/";
                    tf.path += comp->str_val;
                    first = false;
                }
            }
            info_.files.push_back(tf);
        }
        compute_file_offsets();
    } else {
        /* Single file */
        auto flen = info.find("length");
        if (flen != info.end())
            info_.total_size = static_cast<uint64_t>(flen->second->int_val);
    }

    /* Compute info_hash — SHA1 of the bencoded 'info' dictionary */
    const BencodeValue info_val{ BencodeValue::Type::Dict, 0, {}, {}, info_it->second->dict_val };
    std::string encoded_info = bencode_encode(info_val);
    info_.info_hash_raw = std::string(
        reinterpret_cast<const char*>(
            sha1(reinterpret_cast<const uint8_t*>(encoded_info.data()),
                 encoded_info.size()).data()), 20);
    info_.info_hash     = sha1_hex(
        reinterpret_cast<const uint8_t*>(encoded_info.data()),
        encoded_info.size());

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
        /* Last piece may be shorter */
        if (info_.total_size > 0 && i == n - 1)
            p.length = static_cast<uint32_t>(
                info_.total_size - p.offset);
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
        if (i < available.size() && !available[i]) continue;
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
