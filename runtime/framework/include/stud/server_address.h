#pragma once

#include <string>

// The public address of the game server this process is actually talking
// to.
//
// The engine's own join line names an address, and it is the wrong one:
//
//   ! Joining game '<jobId>' place <placeId> at 10.34.8.86
//
// 10.x is Roblox's internal UDMUX addressing, so it locates nothing. The
// real, routable server is only visible on the engine's own UDP socket --
// which is how this project's earlier ping investigation identified a
// session's server as 128.116.44.33 in Frankfurt.
//
// Process B shares the host network namespace (no --unshare-net), so its
// sockets appear in the ordinary /proc/net tables and can be read without
// any privilege.

namespace stud::jni_bridge {

// The remote address of this process's game-server UDP socket, as a
// printable IPv4 or IPv6 string, or empty when there is not exactly one
// obvious candidate.
//
// Deliberately conservative: it returns a candidate only when the socket
// is connected to a PUBLIC address, so a lookup is never attempted on a
// private or link-local peer, and it does not guess between several. An
// empty answer means "do not report a region", which is the honest
// outcome when the engine has not connected yet or is talking to
// something this cannot identify.
std::string game_server_address();

}  // namespace stud::jni_bridge
