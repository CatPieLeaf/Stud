#pragma once

#include <string>

#include "launch_uri.h"

namespace stud::ui {

// Hand a deep link to a Stud that is already playing.
//
// Clicking a game in a browser starts a SECOND stud-ui. That process
// cannot talk to the engine -- it is a fresh process, and Process B runs
// inside its own bwrap sandbox -- but it can reach render-host over the
// same socket Process B is already using. So the link is left there and
// Process B collects it.
//
// Returns false when nothing is listening, which is the ordinary case
// when no session is running; the caller then launches normally.
// The PARSED link travels, not the raw URI: parse_launch_uri() is the
// authoritative parser and it lives in this process, so sending the text
// would mean growing a second one inside the bionic sandbox.
bool hand_deep_link_to_running_stud(const LaunchUri& link);

}  // namespace stud::ui
