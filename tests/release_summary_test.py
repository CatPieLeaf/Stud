#!/usr/bin/env python3
"""release-summary.py keeps a release's ## SUMMARY: text and nothing else.

Both shapes below are real release descriptions, cut down: 1.2.5 had image
tables under its bullets, and 1.2.14 a collapsed full changelog. Both once
ended up in the metainfo and the Terra changelog as raw HTML.
"""
import importlib.util
import sys
from pathlib import Path

tool = Path(sys.argv[1]) / "tools" / "release-summary.py"
spec = importlib.util.spec_from_file_location("release_summary", tool)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

tables = """## Windows Graphical Parity (Beta)!\r
\r
## SUMMARY:\r
- New setting, on by default\r
- Highlight outlines are drawn with High quality MSAA, as they are on Windows\r
\r
<table align="center">\r
  <tr>\r
    <td><a href="1"><img src="https://github.com/user-attachments/assets/a" width="260"></a></td>\r
  </tr>\r
</table>\r
\r
### Windows Graphical Parity (Beta):\r
- not part of the summary\r
"""
assert module.summary(tables) == ([], [
    "New setting, on by default",
    "Highlight outlines are drawn with High quality MSAA, as they are on Windows",
]), module.summary(tables)

details = """## SUMMARY:
<details>
<summary>Full changelog</summary>

- Fixed: Stud froze for about 8 seconds at every launch
  on every GPU
- Improved: faster loading on AMD GPUs
</details>

## Everything else
- not part of the summary
"""
assert module.summary(details) == ([], [
    "Fixed: Stud froze for about 8 seconds at every launch on every GPU",
    "Improved: faster loading on AMD GPUs",
]), module.summary(details)
print("release summary: ok")
