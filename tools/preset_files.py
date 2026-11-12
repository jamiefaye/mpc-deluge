#!/usr/bin/env python3
"""List the sample files a Deluge preset refers to.

    tools/preset_files.py <preset.XML> [card root]      one path per line, relative to the card
    tools/preset_files.py --armtest <preset.XML> <card root>
        the same as "--file host=guest" arguments for tools/armtest/run.py (existing files only)

The firmware writes a sample's path either as a fileName="..." attribute or as a <fileName>
element, depending on the firmware version that saved the preset.
"""
import os
import re
import sys
from xml.sax.saxutils import unescape

args = [a for a in sys.argv[1:] if a != "--armtest"]
text = open(args[0], encoding="utf-8", errors="replace").read()
names = set(re.findall(r'fileName="([^"]+)"', text)) | set(re.findall(r"<fileName>([^<]+)</fileName>", text))
names = sorted(unescape(n, {"&quot;": '"', "&apos;": "'"}).strip() for n in names)
root = args[1] if len(args) > 1 else None
for n in names:
    if "--armtest" in sys.argv:
        host = os.path.join(root, n)
        if os.path.isfile(host):
            print("--file")
            print("%s=/sd/%s" % (host, n))
    else:
        missing = root and not os.path.isfile(os.path.join(root, n))
        print(n + ("   (missing)" if missing else ""))
