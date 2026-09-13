#include "stud/game_instance.h"

#include <cstring>
#include <mutex>

namespace stud::jni_bridge {

namespace {

std::mutex& instance_mutex() {
    static std::mutex m;
    return m;
}

std::string& instance_id() {
    static std::string id;
    return id;
}

}  // namespace

void note_engine_log_line(const char* message, size_t length) {
    if (message == nullptr || length == 0) return;
    static constexpr char kNeedle[] = "Joining game '";
    const std::string line(message, length);
    const auto start = line.find(kNeedle);
    if (start == std::string::npos) return;
    const auto id_start = start + sizeof(kNeedle) - 1;
    const auto id_end = line.find('\'', id_start);
    if (id_end == std::string::npos) return;
    std::lock_guard<std::mutex> lock(instance_mutex());
    instance_id() = line.substr(id_start, id_end - id_start);
}

std::string current_game_instance_id() {
    std::lock_guard<std::mutex> lock(instance_mutex());
    return instance_id();
}

void clear_game_instance_id() {
    std::lock_guard<std::mutex> lock(instance_mutex());
    instance_id().clear();
}

}  // namespace stud::jni_bridge
