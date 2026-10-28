/* FluxDM — engine/include/PeerDiscovery.hpp
 * Phase 2: DHT Kademlia, HTTP/UDP tracker announce, PEX.
 * Discovers peers for BitTorrent downloads without relying solely
 * on centralised trackers.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <array>
#include <chrono>
#include "TorrentEngine.hpp"

namespace fluxdm {

/* ── Node ID (160-bit Kademlia node identifier) ──────────────────────────── */
using NodeID = std::array<uint8_t, 20>;

/* ── DHT node ────────────────────────────────────────────────────────────── */
struct DHTNode {
    NodeID      id;
    std::string ip;
    uint16_t    port;
    std::chrono::steady_clock::time_point last_seen;
    bool        good { true };
};

/* ── Tracker type ────────────────────────────────────────────────────────── */
enum class TrackerType { HTTP, UDP, WS };

/* ── Tracker ────────────────────────────────────────────────────────────── */
struct Tracker {
    std::string  url;
    TrackerType  type;
    int64_t      interval      { 1800 };  /* seconds between announces */
    int64_t      min_interval  { 900 };
    int64_t      next_announce { 0 };     /* unix timestamp            */
    uint32_t     seeders       { 0 };
    uint32_t     leechers      { 0 };
    uint32_t     completed     { 0 };
    bool         reachable     { true };
    std::string  error_msg;

    bool is_due(int64_t now) const { return now >= next_announce; }
};

/* ── Announce event ──────────────────────────────────────────────────────── */
enum class AnnounceEvent { Started, Stopped, Completed, Empty };

/* ── Announce request ────────────────────────────────────────────────────── */
struct AnnounceRequest {
    std::string    info_hash;       /* 20-byte raw                 */
    std::string    peer_id;         /* 20-byte raw                 */
    uint16_t       port;
    uint64_t       uploaded;
    uint64_t       downloaded;
    uint64_t       left;
    AnnounceEvent  event  { AnnounceEvent::Empty };
    uint32_t       want   { 50 };   /* max peers requested         */
};

/* ── PeerDiscovery ───────────────────────────────────────────────────────── */
class PeerDiscovery {
public:
    static constexpr size_t   K_BUCKET_SIZE     = 8;   /* Kademlia k          */
    static constexpr size_t   ALPHA             = 3;   /* parallel lookups    */
    static constexpr uint16_t DEFAULT_DHT_PORT  = 6881;
    static constexpr size_t   MAX_PEERS         = 200;

    explicit PeerDiscovery(uint16_t listen_port = DEFAULT_DHT_PORT);
    ~PeerDiscovery();

    /* No copy */
    PeerDiscovery(const PeerDiscovery&)            = delete;
    PeerDiscovery& operator=(const PeerDiscovery&) = delete;

    /* ── Lifecycle ─────────────────────────────────────────────────────────── */

    /* Start DHT + tracker threads */
    bool start();
    void stop();
    bool running() const { return running_; }

    /* ── Tracker announce ──────────────────────────────────────────────────── */

    /* Register trackers from a TorrentInfo */
    void add_trackers(const TorrentInfo& info);
    void add_tracker(const std::string& url);

    /* Trigger an announce on all due trackers */
    void announce(const AnnounceRequest& req);

    /* Force an immediate announce to all trackers */
    void announce_all(const AnnounceRequest& req);

    /* ── DHT ───────────────────────────────────────────────────────────────── */

    /* ── DHT ───────────────────────────────────────────────────────────────── */
    /* NOT IMPLEMENTED. bootstrap() below does populate bootstrap_nodes_
     * with real data, and the background dht_loop() does call dht_ping()
     * on them — but dht_ping()/dht_announce()/dht_get_peers() are all
     * empty bodies (see PeerDiscovery.cpp) that silence their unused
     * parameters and do nothing else. No BEP-5 query/response encoding,
     * no routing table, no iterative lookups exist. Peer discovery
     * currently works ONLY through HTTP/UDP tracker announce (both of
     * those ARE fully implemented and tested) and PEX. Implementing real
     * Kademlia DHT is a substantial, separate piece of work — routing
     * table maintenance, iterative node lookups, and UDP wire encoding —
     * not a small addition to what's here. */

    /* Bootstrap from known nodes (from .torrent or hardcoded) */
    void bootstrap(const std::vector<std::pair<std::string,uint16_t>>& nodes);

    /* Announce our presence for an info_hash to the DHT. NOT IMPLEMENTED. */
    void dht_announce(const std::string& info_hash_hex, uint16_t port);

    /* Look up peers in the DHT for an info_hash. NOT IMPLEMENTED. */
    void dht_get_peers(const std::string& info_hash_hex);

    /* Add a compact peer list (BEP-23) to our peer set */
    void add_compact_peers(const std::string& compact);

    /* ── PEX (Peer Exchange) ───────────────────────────────────────────────── */

    /* Register peers received from a connected peer (PEX extension) */
    void pex_add(const std::vector<Peer>& peers);

    /* Generate a PEX message for our current peer set */
    std::string pex_generate() const;

    /* ── Peer access ───────────────────────────────────────────────────────── */

    /* Get up to n peers we know about */
    std::vector<Peer> get_peers(size_t n = 50) const;

    /* Number of known peers */
    size_t peer_count()   const;
    size_t tracker_count()const;
    size_t dht_node_count()const;

    /* Callbacks */
    using OnPeers   = std::function<void(const std::vector<Peer>&, const std::string& source)>;
    using OnTracker = std::function<void(const Tracker&, bool success)>;

    void set_on_peers(OnPeers cb)     { on_peers_   = std::move(cb); }
    void set_on_tracker(OnTracker cb) { on_tracker_ = std::move(cb); }

private:
    uint16_t       listen_port_;
    NodeID         my_node_id_;
    std::atomic<bool> running_ { false };

    mutable std::mutex               peers_mu_;
    std::vector<Peer>                peers_;
    std::unordered_set<std::string>  peer_keys_;   /* "ip:port" set for dedup */

    mutable std::mutex               tracker_mu_;
    std::vector<Tracker>             trackers_;

    mutable std::mutex               dht_mu_;
    std::unordered_map<std::string, std::vector<DHTNode>> routing_table_; /* bucket_prefix → nodes */
    std::vector<DHTNode>             bootstrap_nodes_;

    std::thread  tracker_thread_;
    std::thread  dht_thread_;

    OnPeers   on_peers_;
    OnTracker on_tracker_;

    /* ── Internal ──────────────────────────────────────────────────────────── */
    void    tracker_loop();
    void    dht_loop();

    bool    http_announce(Tracker& t, const AnnounceRequest& req);
    bool    udp_announce(Tracker& t,  const AnnounceRequest& req);

    std::vector<Peer> parse_compact_peers(const std::string& compact) const;
    std::vector<Peer> parse_peer_list(const BencodeValue& list) const;

    void    add_peers(const std::vector<Peer>& new_peers, const std::string& source);
    void    dht_ping(const std::string& ip, uint16_t port);

    static NodeID   random_node_id();
    static TrackerType infer_tracker_type(const std::string& url);
    static std::string make_peer_id();
    static int64_t  now_unix();
};

} /* namespace fluxdm */
