#include "stud/server_address.h"

#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace stud::jni_bridge {

namespace {

// The inode of every socket this process currently holds. /proc/net/udp
// lists every UDP socket in the whole network namespace -- which Process
// B shares with the host -- so the inode set is what narrows it to ours.
std::set<unsigned long long> own_socket_inodes() {
    std::set<unsigned long long> inodes;
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) return inodes;
    while (dirent* e = ::readdir(dir)) {
        char link[64];
        std::snprintf(link, sizeof(link), "/proc/self/fd/%s", e->d_name);
        char target[256];
        const ssize_t n = ::readlink(link, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = '\0';
        // "socket:[12345]"
        unsigned long long inode = 0;
        if (std::sscanf(target, "socket:[%llu]", &inode) == 1) inodes.insert(inode);
    }
    ::closedir(dir);
    return inodes;
}

bool is_public_v4(uint32_t host_order) {
    const uint8_t a = static_cast<uint8_t>(host_order >> 24);
    const uint8_t b = static_cast<uint8_t>((host_order >> 16) & 0xff);
    if (host_order == 0) return false;
    if (a == 10) return false;                        // 10/8
    if (a == 127) return false;                       // loopback
    if (a == 172 && b >= 16 && b <= 31) return false; // 172.16/12
    if (a == 192 && b == 168) return false;           // 192.168/16
    if (a == 169 && b == 254) return false;           // link-local
    if (a == 100 && b >= 64 && b <= 127) return false;  // carrier NAT
    if (a >= 224) return false;                       // multicast and above
    return true;
}

// One line of /proc/net/udp:
//   sl local_address rem_address st tx_queue:rx_queue tr:tm->when retrnsmt
//   uid timeout inode ...
// Addresses are hex, little-endian words: "0100007F:0035".
bool parse_v4_line(const std::string& line, uint32_t* remote_be, uint16_t* remote_port,
                    unsigned long long* inode) {
    std::istringstream in(line);
    std::string sl, local, remote, st, queues, timers, retr, uid, timeout, inode_text;
    if (!(in >> sl >> local >> remote >> st >> queues >> timers >> retr >> uid >> timeout >>
          inode_text)) {
        return false;
    }
    const auto colon = remote.find(':');
    if (colon == std::string::npos) return false;
    *remote_be = static_cast<uint32_t>(std::strtoul(remote.substr(0, colon).c_str(), nullptr, 16));
    *remote_port =
        static_cast<uint16_t>(std::strtoul(remote.substr(colon + 1).c_str(), nullptr, 16));
    *inode = std::strtoull(inode_text.c_str(), nullptr, 10);
    return true;
}

}  // namespace

std::string game_server_address() {
    const std::set<unsigned long long> mine = own_socket_inodes();
    if (mine.empty()) return {};

    std::ifstream udp("/proc/net/udp");
    if (!udp.is_open()) return {};

    std::string line;
    std::getline(udp, line);  // header
    std::vector<std::string> candidates;
    while (std::getline(udp, line)) {
        uint32_t remote_be = 0;
        uint16_t port = 0;
        unsigned long long inode = 0;
        if (!parse_v4_line(line, &remote_be, &port, &inode)) continue;
        if (mine.count(inode) == 0) continue;
        if (port == 0) continue;
        // Roblox game servers listen high; a peer on a well-known service
        // port is something else the engine talks to (and would be looked
        // up, and reported, as if it were the game). Narrow to the range
        // real game servers actually use rather than accepting any public
        // UDP peer.
        if (port < 1024) continue;
        // The table stores the address as a little-endian word, so the
        // host-order value is the byte-swapped one.
        const uint32_t host_order = ntohl(remote_be);
        if (!is_public_v4(host_order)) continue;
        in_addr addr{};
        addr.s_addr = remote_be;
        char text[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &addr, text, sizeof(text)) == nullptr) continue;
        candidates.push_back(text);
    }

    // Exactly one, or nothing. Several public UDP peers at once means this
    // cannot tell which is the game, and reporting the wrong server's
    // country is worse than reporting none.
    //
    // Note for anyone changing this: every address here is a socket's
    // REMOTE peer (rem_address), never the local end, so this cannot
    // report the user's own address. What it can get wrong is reporting
    // some other server the engine talks to, which is why the caller
    // samples only once, on the way into an experience.
    if (candidates.size() != 1) return {};
    return candidates.front();
}

}  // namespace stud::jni_bridge
