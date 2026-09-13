#pragma once

#include <string>

// Which SERVER the player is in, as opposed to which experience.
//
// The place id arrives through a real JNI callback
// (`gameActivity_onGameLoaded`), but nothing in the Java-facing surface
// ever reports the instance -- a normal join has no instance id until
// the backend picks a server, and the only place that answer is stated
// is the engine's own log:
//
//   ! Joining game '<jobId>' place <placeId> at <ip>
//
// So it is read from there. This is engine log text, not a byte offset:
// a future build changing the wording costs the server half of a link,
// nothing more, and the fallback (a link to the experience) still works.
namespace stud::jni_bridge {

// Every engine log line passes through here. Cheap: a substring check.
void note_engine_log_line(const char* message, size_t length);

// The instance the player is in, empty when that is not known.
std::string current_game_instance_id();

// Leaving an experience: the instance is gone before the next one has
// one, and a stale id would link to a server the player already left.
void clear_game_instance_id();

}  // namespace stud::jni_bridge
