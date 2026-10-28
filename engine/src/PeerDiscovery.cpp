/* FluxDM — engine/src/PeerDiscovery.cpp */

#include "../include/PeerDiscovery.hpp"
#include "../include/TorrentEngine.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <random>

namespace fluxdm {

/* ══════════════════════════════════════════════════════════════════════════
 * Static helpers
 * ══════════════════════════════════════════════════════════════════════════ */

int64_t PeerDiscovery::now_unix() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

NodeID PeerDiscovery::random_node_id() {
    NodeID id;
    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    for (auto& b : id) b = dist(rng);
    return id;
}

std::string PeerDiscovery::make_peer_id() {
    /* FluxDM client identifier: -FD0100-<12 random digits> */
    std::string pid = "-FD0100-";
    std::mt19937 rng(static_cast<uint32_t>(std::time(nullptr)));
    std::uniform_int_distribution<int> d(0, 9);
    for (int i = 0; i < 12; ++i) pid += static_cast<char>('0' + d(rng));
    return pid;
}

TrackerType PeerDiscovery::infer_tracker_type(const std::string& url) {
    if (url.substr(0, 3) == "udp") return TrackerType::UDP;
    if (url.substr(0, 2) == "ws")  return TrackerType::WS;
    return TrackerType::HTTP;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Constructor / destructor
 * ══════════════════════════════════════════════════════════════════════════ */

PeerDiscovery::PeerDiscovery(uint16_t listen_port)
    : listen_port_(listen_port),
      my_node_id_(random_node_id()) {}

PeerDiscovery::~PeerDiscovery() { stop(); }

/* ══════════════════════════════════════════════════════════════════════════
 * Lifecycle
 * ══════════════════════════════════════════════════════════════════════════ */

bool PeerDiscovery::start() {
    if (running_) return true;
    running_ = true;
    tracker_thread_ = std::thread(&PeerDiscovery::tracker_loop, this);
    dht_thread_     = std::thread(&PeerDiscovery::dht_loop, this);
    return true;
}

void PeerDiscovery::stop() {
    running_ = false;
    if (tracker_thread_.joinable()) tracker_thread_.join();
    if (dht_thread_.joinable())     dht_thread_.join();
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tracker management
 * ══════════════════════════════════════════════════════════════════════════ */

void PeerDiscovery::add_tracker(const std::string& url) {
    std::lock_guard<std::mutex> lock(tracker_mu_);
    for (const auto& t : trackers_)
        if (t.url == url) return;   /* dedup */
    Tracker t;
    t.url           = url;
    t.type          = infer_tracker_type(url);
    t.next_announce = now_unix();   /* announce immediately */
    trackers_.push_back(t);
}

void PeerDiscovery::add_trackers(const TorrentInfo& info) {
    if (!info.announce.empty()) add_tracker(info.announce);
    for (const auto& url : info.announce_list) add_tracker(url);
}

void PeerDiscovery::announce(const AnnounceRequest& req) {
    std::lock_guard<std::mutex> lock(tracker_mu_);
    int64_t now = now_unix();
    for (auto& t : trackers_) {
        if (!t.is_due(now) || !t.reachable) continue;
        bool ok = (t.type == TrackerType::UDP)
                  ? udp_announce(t, req)
                  : http_announce(t, req);
        if (on_tracker_) on_tracker_(t, ok);
    }
}

void PeerDiscovery::announce_all(const AnnounceRequest& req) {
    std::lock_guard<std::mutex> lock(tracker_mu_);
    for (auto& t : trackers_) {
        bool ok = (t.type == TrackerType::UDP)
                  ? udp_announce(t, req)
                  : http_announce(t, req);
        if (on_tracker_) on_tracker_(t, ok);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * HTTP Tracker announce
 * ══════════════════════════════════════════════════════════════════════════ */

static std::string build_announce_url(const std::string& base,
                                       const AnnounceRequest& req) {
    /* Encode info_hash and peer_id */
    auto url_enc = [](const std::string& s) {
        std::ostringstream ss;
        for (unsigned char c : s)
            ss << '%' << std::hex << std::uppercase
               << std::setw(2) << std::setfill('0') << (int)c;
        return ss.str();
    };

    std::string event_str;
    switch (req.event) {
    case AnnounceEvent::Started:   event_str = "&event=started";   break;
    case AnnounceEvent::Stopped:   event_str = "&event=stopped";   break;
    case AnnounceEvent::Completed: event_str = "&event=completed"; break;
    default: break;
    }

    std::ostringstream url;
    url << base
        << "?info_hash=" << url_enc(req.info_hash)
        << "&peer_id="   << url_enc(req.peer_id)
        << "&port="      << req.port
        << "&uploaded="  << req.uploaded
        << "&downloaded="<< req.downloaded
        << "&left="      << req.left
        << "&compact=1"
        << "&numwant="   << req.want
        << event_str;
    return url.str();
}

bool PeerDiscovery::http_announce(Tracker& t, const AnnounceRequest& req) {
    /* Build URL */
    std::string url = build_announce_url(t.url, req);

    /* Parse host and path from URL */
    std::string host, path;
    uint16_t port = 80;
    [[maybe_unused]] bool use_tls = false;  /* reserved: HTTPS trackers use plain
                                              * HTTP fallback for now — TLS wiring
                                              * arrives when https.c is shared here */

    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    std::string scheme = url.substr(0, scheme_end);
    if (scheme == "https") { use_tls = true; port = 443; }
    std::string rest = url.substr(scheme_end + 3);
    size_t slash = rest.find('/');
    std::string host_part = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
    path = (slash != std::string::npos) ? rest.substr(slash) : "/";

    /* host:port split */
    size_t colon = host_part.find(':');
    if (colon != std::string::npos) {
        host = host_part.substr(0, colon);
        try { port = static_cast<uint16_t>(std::stoi(host_part.substr(colon+1))); }
        catch (...) {}
    } else {
        host = host_part;
    }

    /* DNS resolve */
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) {
        t.reachable  = false;
        t.error_msg  = "DNS failed for " + host;
        return false;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); t.reachable = false; return false; }

    struct timeval tv{ 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    bool ok = false;
    if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
        /* Send HTTP GET */
        std::ostringstream req_hdr;
        req_hdr << "GET " << path << " HTTP/1.0\r\n"
                << "Host: " << host << "\r\n"
                << "User-Agent: FluxDM/1.0\r\n"
                << "Connection: close\r\n\r\n";
        std::string req_str = req_hdr.str();
        send(fd, req_str.data(), req_str.size(), MSG_NOSIGNAL);

        /* Read response */
        std::string response;
        char buf[4096];
        ssize_t n;
        while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
            response.append(buf, static_cast<size_t>(n));

        /* Find body after \r\n\r\n */
        auto body_pos = response.find("\r\n\r\n");
        if (body_pos != std::string::npos) {
            std::string body = response.substr(body_pos + 4);
            try {
                auto parsed = TorrentEngine::bencode_parse(body);
                if (parsed && parsed->type == BencodeValue::Type::Dict) {
                    /* interval */
                    auto iv = parsed->dict_val.find("interval");
                    if (iv != parsed->dict_val.end())
                        t.interval = iv->second->int_val;
                    t.next_announce = now_unix() + t.interval;

                    /* seeders / leechers */
                    auto sd = parsed->dict_val.find("complete");
                    if (sd != parsed->dict_val.end())
                        t.seeders = static_cast<uint32_t>(sd->second->int_val);
                    auto lc = parsed->dict_val.find("incomplete");
                    if (lc != parsed->dict_val.end())
                        t.leechers = static_cast<uint32_t>(lc->second->int_val);

                    /* peers (compact) */
                    auto peers_it = parsed->dict_val.find("peers");
                    std::vector<Peer> new_peers;
                    if (peers_it != parsed->dict_val.end()) {
                        if (peers_it->second->type == BencodeValue::Type::Str) {
                            new_peers = parse_compact_peers(peers_it->second->str_val);
                        } else {
                            new_peers = parse_peer_list(*peers_it->second);
                        }
                    }
                    if (!new_peers.empty())
                        add_peers(new_peers, t.url);

                    t.reachable = true;
                    t.error_msg.clear();
                    ok = true;
                }
                /* failure reason */
                auto fr = parsed ? parsed->dict_val.find("failure reason")
                                 : parsed->dict_val.end();
                if (parsed && fr != parsed->dict_val.end()) {
                    t.error_msg = fr->second->str_val;
                    ok = false;
                }
            } catch (...) {
                t.error_msg = "bencode parse error";
            }
        }
    }
    close(fd);
    freeaddrinfo(res);
    if (!ok) t.reachable = false;
    return ok;
}

/* ══════════════════════════════════════════════════════════════════════════
 * UDP Tracker announce (BEP-15)
 * ══════════════════════════════════════════════════════════════════════════ */

bool PeerDiscovery::udp_announce(Tracker& t, const AnnounceRequest& req) {
    /* Parse udp://host:port/announce */
    std::string url = t.url;
    if (url.substr(0,6) == "udp://") url = url.substr(6);
    size_t slash = url.find('/');
    std::string host_port = (slash != std::string::npos) ? url.substr(0,slash) : url;

    std::string host;
    uint16_t    port = 6969;
    size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        host = host_port.substr(0, colon);
        try { port = static_cast<uint16_t>(std::stoi(host_port.substr(colon+1))); }
        catch (...) {}
    } else { host = host_port; }

    /* Resolve */
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) {
        t.error_msg = "UDP DNS failed"; t.reachable = false; return false;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { freeaddrinfo(res); return false; }

    struct timeval tv{ 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* ── Step 1: Connect request ─────────────────────────────────── */
    /* Magic connection ID: 0x41727101980 */
    std::mt19937 rng(std::random_device{}());
    uint32_t transaction_id = rng();
    uint8_t connect_req[16]{};
    uint64_t magic = 0x41727101980ULL;
    /* Pack big-endian */
    for (int i = 7; i >= 0; --i) { connect_req[i] = magic & 0xFF; magic >>= 8; }
    /* action = 0 (connect) */
    connect_req[11] = 0;
    /* transaction_id */
    connect_req[12] = (transaction_id >> 24) & 0xFF;
    connect_req[13] = (transaction_id >> 16) & 0xFF;
    connect_req[14] = (transaction_id >>  8) & 0xFF;
    connect_req[15] =  transaction_id        & 0xFF;

    sendto(fd, connect_req, sizeof(connect_req), 0, res->ai_addr, res->ai_addrlen);

    /* ── Step 2: Connect response ────────────────────────────────── */
    uint8_t connect_resp[16]{};
    ssize_t n = recvfrom(fd, connect_resp, sizeof(connect_resp), 0, nullptr, nullptr);
    if (n < 16) {
        close(fd); freeaddrinfo(res);
        t.error_msg = "UDP connect timeout"; t.reachable = false; return false;
    }

    /* Validate action (0) and transaction_id */
    uint32_t resp_action = ((uint32_t)connect_resp[0]<<24)|((uint32_t)connect_resp[1]<<16)|
                           ((uint32_t)connect_resp[2]<<8)| connect_resp[3];
    uint32_t resp_tid    = ((uint32_t)connect_resp[4]<<24)|((uint32_t)connect_resp[5]<<16)|
                           ((uint32_t)connect_resp[6]<<8)| connect_resp[7];
    if (resp_action != 0 || resp_tid != transaction_id) {
        close(fd); freeaddrinfo(res); t.error_msg = "UDP bad response"; return false;
    }

    /* Extract connection_id */
    uint64_t conn_id = 0;
    for (int i = 0; i < 8; ++i) conn_id = (conn_id << 8) | connect_resp[8+i];

    /* ── Step 3: Announce request ────────────────────────────────── */
    uint8_t announce_req[98]{};
    /* connection_id */
    for (int i = 7; i >= 0; --i) { announce_req[i] = conn_id & 0xFF; conn_id >>= 8; }
    /* action = 1 (announce) */
    announce_req[11] = 1;
    /* transaction_id */
    transaction_id = rng();
    announce_req[12] = (transaction_id >> 24) & 0xFF;
    announce_req[13] = (transaction_id >> 16) & 0xFF;
    announce_req[14] = (transaction_id >>  8) & 0xFF;
    announce_req[15] =  transaction_id        & 0xFF;
    /* info_hash (20 bytes) */
    if (req.info_hash.size() >= 20)
        std::memcpy(announce_req + 16, req.info_hash.data(), 20);
    /* peer_id (20 bytes) */
    if (req.peer_id.size() >= 20)
        std::memcpy(announce_req + 36, req.peer_id.data(), 20);
    /* downloaded, left, uploaded (int64 each) */
    auto pack64 = [&](size_t off, uint64_t v) {
        for (int i = 7; i >= 0; --i) { announce_req[off+i] = v&0xFF; v>>=8; }
    };
    pack64(56, req.downloaded);
    pack64(64, req.left);
    pack64(72, req.uploaded);
    /* event */
    uint32_t ev = 0;
    switch (req.event) {
    case AnnounceEvent::Completed: ev = 1; break;
    case AnnounceEvent::Started:   ev = 2; break;
    case AnnounceEvent::Stopped:   ev = 3; break;
    default: break;
    }
    announce_req[83] = ev;
    /* port */
    announce_req[96] = (req.port >> 8) & 0xFF;
    announce_req[97] =  req.port       & 0xFF;

    sendto(fd, announce_req, sizeof(announce_req), 0, res->ai_addr, res->ai_addrlen);

    /* ── Step 4: Announce response ───────────────────────────────── */
    uint8_t announce_resp[1024]{};
    n = recvfrom(fd, announce_resp, sizeof(announce_resp), 0, nullptr, nullptr);
    close(fd); freeaddrinfo(res);

    if (n < 20) { t.error_msg = "UDP announce timeout"; t.reachable = false; return false; }

    resp_action = ((uint32_t)announce_resp[0]<<24)|((uint32_t)announce_resp[1]<<16)|
                  ((uint32_t)announce_resp[2]<<8)|announce_resp[3];
    if (resp_action != 1) { t.error_msg = "UDP bad announce action"; return false; }

    uint32_t interval_s = ((uint32_t)announce_resp[8]<<24)|((uint32_t)announce_resp[9]<<16)|
                          ((uint32_t)announce_resp[10]<<8)|announce_resp[11];
    t.interval      = static_cast<int64_t>(interval_s);
    t.next_announce = now_unix() + t.interval;

    t.leechers = ((uint32_t)announce_resp[12]<<24)|((uint32_t)announce_resp[13]<<16)|
                 ((uint32_t)announce_resp[14]<<8)|announce_resp[15];
    t.seeders  = ((uint32_t)announce_resp[16]<<24)|((uint32_t)announce_resp[17]<<16)|
                 ((uint32_t)announce_resp[18]<<8)|announce_resp[19];

    /* Parse compact peer list */
    if (n > 20) {
        std::string compact(reinterpret_cast<char*>(announce_resp + 20),
                            static_cast<size_t>(n - 20));
        auto peers = parse_compact_peers(compact);
        add_peers(peers, t.url);
    }

    t.reachable = true;
    t.error_msg.clear();
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Peer parsing
 * ══════════════════════════════════════════════════════════════════════════ */

std::vector<Peer> PeerDiscovery::parse_compact_peers(const std::string& compact) const {
    std::vector<Peer> result;
    /* BEP-23: 6 bytes per peer (4 IP + 2 port) */
    for (size_t i = 0; i + 5 < compact.size(); i += 6) {
        Peer p;
        struct in_addr addr;
        std::memcpy(&addr.s_addr, compact.data() + i, 4);
        p.ip   = inet_ntoa(addr);
        p.port = (static_cast<uint16_t>(
                      static_cast<uint8_t>(compact[i+4])) << 8) |
                  static_cast<uint8_t>(compact[i+5]);
        result.push_back(p);
    }
    return result;
}

std::vector<Peer> PeerDiscovery::parse_peer_list(const BencodeValue& list) const {
    std::vector<Peer> result;
    for (const auto& pv : list.list_val) {
        if (pv->type != BencodeValue::Type::Dict) continue;
        Peer p;
        auto ip   = pv->dict_val.find("ip");
        auto port = pv->dict_val.find("port");
        auto pid  = pv->dict_val.find("peer id");
        if (ip   != pv->dict_val.end()) p.ip      = ip->second->str_val;
        if (port != pv->dict_val.end()) p.port     = static_cast<uint16_t>(port->second->int_val);
        if (pid  != pv->dict_val.end()) p.peer_id  = pid->second->str_val;
        if (!p.ip.empty() && p.port > 0) result.push_back(p);
    }
    return result;
}

void PeerDiscovery::add_compact_peers(const std::string& compact) {
    auto peers = parse_compact_peers(compact);
    add_peers(peers, "compact");
}

void PeerDiscovery::add_peers(const std::vector<Peer>& new_peers,
                               const std::string& source) {
    std::lock_guard<std::mutex> lock(peers_mu_);
    for (const auto& p : new_peers) {
        std::string key = p.ip + ":" + std::to_string(p.port);
        if (peer_keys_.count(key)) continue;
        if (peers_.size() >= MAX_PEERS) break;
        peer_keys_.insert(key);
        peers_.push_back(p);
    }
    if (on_peers_ && !new_peers.empty()) on_peers_(new_peers, source);
}

/* ══════════════════════════════════════════════════════════════════════════
 * DHT
 * ══════════════════════════════════════════════════════════════════════════ */

void PeerDiscovery::bootstrap(const std::vector<std::pair<std::string,uint16_t>>& nodes) {
    std::lock_guard<std::mutex> lock(dht_mu_);
    for (const auto& [ip, port] : nodes) {
        DHTNode node;
        node.id   = random_node_id();  /* unknown until ping response */
        node.ip   = ip;
        node.port = port;
        node.last_seen = std::chrono::steady_clock::now();
        bootstrap_nodes_.push_back(node);
    }
}

void PeerDiscovery::dht_announce(const std::string& info_hash_hex, uint16_t port) {
    /* Placeholder: send announce_peer to K closest nodes */
    (void)info_hash_hex; (void)port;
}

void PeerDiscovery::dht_get_peers(const std::string& info_hash_hex) {
    /* Placeholder: iterative get_peers lookup */
    (void)info_hash_hex;
}

void PeerDiscovery::dht_ping(const std::string& ip, uint16_t port) {
    /* Send a BEP-5 ping query */
    (void)ip; (void)port;
}

/* ══════════════════════════════════════════════════════════════════════════
 * PEX
 * ══════════════════════════════════════════════════════════════════════════ */

void PeerDiscovery::pex_add(const std::vector<Peer>& peers) {
    add_peers(peers, "pex");
}

std::string PeerDiscovery::pex_generate() const {
    /* Generate compact peer list (BEP-11) */
    std::lock_guard<std::mutex> lock(peers_mu_);
    std::string compact;
    compact.reserve(peers_.size() * 6);
    for (const auto& p : peers_) {
        struct in_addr addr{};
        if (inet_aton(p.ip.c_str(), &addr) == 0) continue;
        compact.append(reinterpret_cast<char*>(&addr.s_addr), 4);
        compact += static_cast<char>((p.port >> 8) & 0xFF);
        compact += static_cast<char>( p.port       & 0xFF);
    }
    return compact;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Background loops
 * ══════════════════════════════════════════════════════════════════════════ */

void PeerDiscovery::tracker_loop() {
    while (running_) {
        /* Check for due announces every 30 seconds */
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (!running_) break;

        std::vector<Tracker*> due;
        {
            std::lock_guard<std::mutex> lock(tracker_mu_);
            int64_t now = now_unix();
            for (auto& t : trackers_)
                if (t.is_due(now) && t.reachable) due.push_back(&t);
        }

        /* Announce with empty request (periodic re-announce) */
        AnnounceRequest req;
        req.peer_id = make_peer_id();
        req.port    = listen_port_;
        req.want    = 50;

        for (auto* t : due) {
            bool ok = (t->type == TrackerType::UDP)
                      ? udp_announce(*t, req)
                      : http_announce(*t, req);
            if (on_tracker_) on_tracker_(*t, ok);
        }
    }
}

void PeerDiscovery::dht_loop() {
    /* Ping bootstrap nodes on startup, then maintain routing table */
    std::this_thread::sleep_for(std::chrono::seconds(2));

    {
        std::lock_guard<std::mutex> lock(dht_mu_);
        for (const auto& node : bootstrap_nodes_)
            dht_ping(node.ip, node.port);
    }

    while (running_) {
        /* Refresh buckets every 15 minutes */
        std::this_thread::sleep_for(std::chrono::minutes(15));
        if (!running_) break;

        /* Ping nodes that haven't been seen recently */
        std::lock_guard<std::mutex> lock(dht_mu_);
        auto now = std::chrono::steady_clock::now();
        for (auto& [bucket, nodes] : routing_table_) {
            for (auto& node : nodes) {
                auto age = std::chrono::duration_cast<std::chrono::minutes>(
                    now - node.last_seen).count();
                if (age > 15) dht_ping(node.ip, node.port);
            }
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Queries
 * ══════════════════════════════════════════════════════════════════════════ */

std::vector<Peer> PeerDiscovery::get_peers(size_t n) const {
    std::lock_guard<std::mutex> lock(peers_mu_);
    size_t take = std::min(n, peers_.size());
    return std::vector<Peer>(peers_.begin(), peers_.begin() + static_cast<ptrdiff_t>(take));
}

size_t PeerDiscovery::peer_count() const {
    std::lock_guard<std::mutex> lock(peers_mu_);
    return peers_.size();
}

size_t PeerDiscovery::tracker_count() const {
    std::lock_guard<std::mutex> lock(tracker_mu_);
    return trackers_.size();
}

size_t PeerDiscovery::dht_node_count() const {
    std::lock_guard<std::mutex> lock(dht_mu_);
    size_t n = 0;
    for (const auto& [k,v] : routing_table_) n += v.size();
    return n + bootstrap_nodes_.size();
}

} /* namespace fluxdm */
