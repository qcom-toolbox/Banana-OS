#!/usr/bin/env python3
"""Regenerates net/httpd_demo.c from web/test/cases/demo.php (run from the repo root)."""
src = open("web/test/cases/demo.php").read()
out = ["/* /var/www/demo.php, created by httpd next to the default index page",
       " * (generated from web/test/cases/demo.php by web/test/embed.py) */",
       '#include "httpd_demo.h"', "", "const char HTTPD_DEMO_PHP[] ="]
for line in src.split("\n")[:-1]:
    esc = line.replace("\\", "\\\\").replace('"', '\\"')
    out.append('    "' + esc + '\\n"')
out += ["    ;", "", "const unsigned int HTTPD_DEMO_PHP_LEN = sizeof(HTTPD_DEMO_PHP) - 1;", ""]
open("net/httpd_demo.c", "w").write("\n".join(out))
