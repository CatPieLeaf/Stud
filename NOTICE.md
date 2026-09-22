# Stud legal notice

Stud is a free, unofficial, non-commercial project that runs the real
Roblox Android application on a Linux desktop. It is experimental
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
- Stud is **not affiliated with Google, Android, or the Android Open
  Source Project**, nor with Khronos, the Vulkan working group, or any
  other project whose software it uses.
- Stud **contains no Roblox code and distributes none**. It does not ship,
  mirror, host or download the Roblox application package. You supply that
  file yourself, from a source of your own choosing, and your use of it is
  between you and Roblox.
- Stud **does not modify the Roblox engine**. It loads the application
  exactly as published and supplies the Android interfaces it expects.
  Refusing to patch that binary is a rule the project holds itself to, not
  an accident of how it turned out.
- Stud is a launcher and a runtime. It is not a distributor, not a modding
  tool, and not an emulator.

## 2. Trademarks

Roblox is a trademark of Roblox Corporation. Android and Google are
trademarks of Google LLC. Vulkan is a trademark of the Khronos Group.
Other names are the property of their owners. They are used here only to
say truthfully what Stud works with. No endorsement is claimed or implied,
and Stud uses no logo, artwork or branding belonging to any of them.

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
  Roblox's own engine, to run the application: sign-in, settings, the
  experiences you open.
- Your login is stored on disk, encrypted with AES-256-GCM. Only the
  encryption key is held in the system keyring, through the Secret
  Service interface, the same arrangement Chromium calls safe storage.
  The keyring therefore holds one opaque application key rather than a
  readable `.ROBLOSECURITY` value, and deleting that entry makes the
  stored cookie permanently unreadable.
- If, and only if, you switch on the server-region notification in
  Settings, off by default, the address of the **game server** you
  joined is sent over HTTPS to `ipwho.is` to name its country. Your own address is
  not sent. Leaving the setting off means the request never happens.
- Logs are written under `~/.local/state/stud/logs/` and stay on your
  machine. Nothing uploads them.

## 7. No warranty, and no liability

Stud is provided **as is**, with no warranty of any kind. Sections 15, 16
and 17 of the AGPL say this in full and in the terms that bind; in plain
words: this is experimental software, running an application it was never
designed to run, and you use it at your own risk. The author is not liable
for damage, data loss, or anything Roblox or any other service does in
response to your use of it.

## 8. If a rights holder objects

If Roblox Corporation, Google, or any other rights holder sends a
verifiable, direct communication asking this project to stop or to change
something, it will be honoured. Write to catpieleaf@proton.me, or open an
issue at https://github.com/CatPieLeaf/Stud/issues.

## 9. Third-party software

Stud redistributes ANGLE, SwiftShader, the Vulkan loader and validation
layers, and Android's own libc, libm, libdl and dynamic linker from AOSP.
Every one of those is under a permissive licence and ships with its own
licence text and copyright notice, installed to
`/usr/share/licenses/stud/` and kept beside the libraries themselves.

Stud also builds three shaders from vendored references. The upscaler is
RAVU-Zoom from mpv-prescalers, LGPL-3.0-or-later, beside SGSR
(BSD-3-Clause) and FSR1's RCAS (MIT). LGPLv3 incorporates the GPLv3
terms and permits conveying a copy under them, and AGPLv3 section 13
permits combining a covered work with GPLv3 work. The hooks those
shaders are generated from ship in `third_party/`, as the Corresponding
Source that section requires.

The full credits are in the README.
