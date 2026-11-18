Beep Boop, Read [AI_USAGE_POLICY.md](AI_USAGE_POLICY.md) before making changes.

Stud is a launcher that runs the Roblox macOS client on Linux through Rosemary. The UI is C++ with Qt 6; Rosemary is built from the fork in `CMakeLists.txt`.

There are some things to know before making changes:

- MOST IMPORTANT: Make the code as self explanatory as possible. Comments only for a reason the code can't show, one line where possible.
- Commit messages are a single subject line, no body, no co-author trailer.
- Avoid hardcoded values, constants, floors, ceilings or fractions as much as possible; derive values from what the system or application reports.
- Diagnostics, secrets and experiments stay local and are never committed.
- Every `STUD_*` environment variable the source reads goes in `docs/environment.md`.
- The launcher (`ui/`) and its settings (`stud-config/`) are separate: settings stay plain C++ with no Qt.
- Be as version-agnostic as possible, we don't want code that only works for a specific version of Roblox or a specific library version.
