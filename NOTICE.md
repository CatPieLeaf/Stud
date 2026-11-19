# Stud legal notice

Stud is a free, unofficial, non-commercial project that runs the real
Roblox macOS client on a Linux desktop. It is experimental
software and it earns nobody any money.

**This is a notice, not a licence agreement.** Stud's licence is the GNU
Affero General Public License version 3 or later ([`LICENSE`](LICENSE)),
with one additional permission ([`LICENSE.exception`](LICENSE.exception)).
Nothing on this page takes away any right that licence gives you. It
cannot, and it is not trying to. What follows is what the project is,
what it is not, and what you should know before you use it.

## 1. What Stud is, and is not

- Stud is **not affiliated with Roblox Corporation**, and is not endorsed,
  sponsored, supported or approved by them. It is not an official Roblox
  product.
- Stud is **not affiliated with Apple**, nor with Khronos, the Vulkan
  working group, or any other project whose software it uses. It runs the
  client on Rosemary, an independent project Stud is not affiliated with
  either.
- Stud **contains no Roblox code and distributes none**. It downloads the
  macOS client from Roblox's own servers, onto your machine, the build
  Roblox itself serves; your use of it is between you and Roblox.
- Stud **does not modify the Roblox client**. It runs the application
  exactly as published. Refusing to patch it is a rule the project holds
  itself to, not an accident of how it turned out. The one file Stud writes
  inside it is `ClientAppSettings.json`, the file Roblox reads local
  settings from.
- Stud is a launcher. It is not a distributor, not a modding tool, and not
  an emulator.

## 2. Trademarks

Roblox is a trademark of Roblox Corporation. Apple and macOS are
trademarks of Apple Inc. Vulkan is a trademark of the Khronos Group.
GitHub is a trademark of GitHub, Inc., and Discord of Discord Inc.
Other names are the property of their owners. They are used here only to
say truthfully what Stud works with. No endorsement is claimed or implied.

The only logos Stud shows that are not its own are GitHub's and Discord's,
on the buttons that link to Stud's own repository and Discord server. The
drawings are from Simple Icons (CC0 1.0), shown in one colour.

## 3. Your Roblox account

Roblox does no testing or quality assurance for Stud, and using any
third-party client may be treated as a violation of Roblox's own terms.
Moderation action on your account is possible. That risk is yours to
weigh, and neither Stud nor its author is responsible for what Roblox
decides.

Your use of Roblox through Stud remains governed by Roblox's Terms of Use,
Privacy Policy and Community Standards, exactly as it would be anywhere
else.

## 4. What Stud is not for

Stud is built to play Roblox on Linux, and the project does not support
using it to cheat, to automate or bot gameplay, to read or modify the
memory of a running game, to scrape data, to run gameplay unattended on a
server, or to interfere with Roblox's own systems.

The AGPL gives you the right to modify and redistribute Stud, and that
right stands whatever this section says. What it does not give you is any
claim on the author's time, help or approval for any of the above.

## 5. Content and conduct inside Roblox

Stud draws the window. It does not control, monitor, moderate or have any
knowledge of what happens inside the Roblox application: chat, voice,
user-generated experiences or anything else. It cannot act on any of it.
Report anything you encounter to Roblox, whose moderation systems are
unaffected by Stud and continue to apply normally.

## 6. What leaves your machine

Stud sends nothing to its author, and there is no telemetry, no analytics
and no crash reporting of any kind.

- Requests to `roblox.com` and its subdomains are made by Stud and by
  Roblox's own client, to run the application: sign-in, settings, the
  experiences you open.
- Before each launch Stud asks `clientsettingscdn.roblox.com` which macOS
  build Roblox serves, and downloads it from `setup.rbxcdn.com` when it is
  not the one installed. Neither request carries your login.
- On first use, Rosemary downloads its MiniDarwin base system from
  `github.com/microlift/minidarwin`'s releases.
- Stud asks `api.github.com` whether a newer Stud is released, and fetches
  the release notes it shows on the welcome page. The request carries nothing
  about you.
- With the server-region notification on, Stud asks `ipwho.is` where the game
  server you joined is, by the server's address, never yours.
- When you choose a classic cursor, character sounds or avatar background in
  Settings, Stud downloads those files from Bloxstrap's repository on
  `raw.githubusercontent.com`. Nothing about you is sent.
- Your login is Roblox's own cookie file, inside Stud's Rosemary prefix
  (`~/.local/share/stud/prefix`). Between sessions Stud keeps it in your
  system keyring and removes the file; it is never sent anywhere by Stud.
- Logs are written under `~/.local/state/stud/logs/` and stay on your
  machine. Nothing uploads them. Join tickets are removed from them.

## 7. No warranty, and no liability

Stud is provided **as is**, with no warranty of any kind. Sections 15, 16
and 17 of the AGPL say this in full and in the terms that bind; in plain
words: this is experimental software, running an application on a system it
was never designed for, and you use it at your own risk. The author is not liable
for damage, data loss, or anything Roblox or any other service does in
response to your use of it.

## 8. If a rights holder objects

If Roblox Corporation, Apple, or any other rights holder sends a
verifiable, direct communication asking this project to stop or to change
something, it will be honoured. Write to catpieleaf@proton.me, or open an
issue at https://github.com/CatPieLeaf/Stud/issues.

## 9. Third-party software

Stud runs the client on Rosemary (https://github.com/microlift/rosemary,
GPL-3.0), built from Stud's fork of it and installed beside Stud's launcher.
AGPLv3 section 13 permits combining a covered work with a GPLv3 work and
conveying the result. The fork is https://github.com/CatPieLeaf/rosemary-stud,
branch `stud`: upstream Rosemary with Stud's changes as commits on top. That
branch is Rosemary's Corresponding Source for every Stud build, and Rosemary's
licence is installed to `/usr/share/licenses/stud/`.

The fork carries the upscale shaders Rosemary is built with: RAVU-Zoom from
mpv-prescalers (LGPL-3.0-or-later), SGSR (BSD-3-Clause) and FSR1's RCAS
(MIT). LGPLv3 incorporates the terms of GPLv3, and its section 2(b) permits
conveying a copy under the GPL with none of the LGPL's additional permissions
applying to that copy. The hook the RAVU-Zoom shader and its weight tables
come from is in the fork, `crates/metal/upscale/`, as the Corresponding
Source.

One more is compiled into Stud's launcher, so its notice is installed there
too:

 - miniz, which unpacks the client and writes the log export (MIT)

The full credits are in the README.
