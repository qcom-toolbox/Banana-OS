#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""rebuild.py REFBUILD OUT - compile Mesa's libOSMesa again for Banana OS.

REFBUILD: the reference Linux build (meson + ninja) of the same source and
options. Every object that libOSMesa.so is linked from is compiled again
from REFBUILD/compile_commands.json with the Banana OS SDK's headers in
place of the system's; the static archives are made again from those
objects and libOSMesa.so is linked the way ninja links it, without the
system's C library (its functions come from the app: the SDK's dlopen).
Objects go to OUT/objs, errors to OUT/logs/<object>.log; a summary is
printed. EXTRA (environment): more compiler flags; JOBS: parallel jobs;
KEEP=1: reuse objects whose compiler arguments, source and headers match."""
import collections, hashlib, json, os, re, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REF, OUT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
ONLY = sys.argv[3:]
SDK = os.path.normpath(HERE + "/../../sdk")
GCC = subprocess.run(["gcc", "-dumpversion"], capture_output=True, text=True).stdout.strip().split(".")[0]
GCCINC = subprocess.run(["gcc", "-print-file-name=include"], capture_output=True, text=True).stdout.strip()
PRE = ["-nostdinc", "-isystem", SDK + "/include", "-isystem", HERE + "/include", "-fdiagnostics-color=never",
       "-isystem", GCCINC, "-D__BANANA_OS__", "-mno-red-zone", "-fno-stack-protector"]
TRIPLE = subprocess.check_output(["gcc", "-dumpmachine"], text=True).strip()
CXX_DIRS = ["/usr/include/c++/" + GCC, "/usr/include/c++/" + GCC + "/" + TRIPLE,
            "/usr/include/" + TRIPLE + "/c++/" + GCC, "/usr/include/x86_64-linux-gnu/c++/" + GCC]
PRE_CXX = ["-nostdinc++"] + [arg for directory in CXX_DIRS if os.path.isdir(directory) for arg in ["-isystem", directory]]

LLVM_LIBRARY = os.environ.get("LLVM_LIBRARY")
if LLVM_LIBRARY:
    llvm_include = os.path.join(HERE, "../llvm/include")
    PRE += ["-isystem", llvm_include]
    PRE_CXX += ["-include", os.path.join(llvm_include, "cxx_compat.h")]

# what the build machine had and Banana OS has not: zlib (Mesa then uses its
# own CRC-32), SPIRV-Tools (only for printing SPIR-V); plain diagnostics
POST = ["-UHAVE_ZLIB", "-UHAVE_COMPRESSION", "-UHAVE_SPIRV_TOOLS", "-UHAVE_LIBDRM", "-DNO_REGEX", "-fdiagnostics-color=never",
        "-mtls-dialect=gnu"]                 # (the SDK loader has no TLS descriptors)
POST_CXX = ["-fno-exceptions"]               # (nor a C++ unwinder)
# ...except Mesa's Intel code and drivers: they ask libdrm which GPU this is
# (drmGetDevice2, drmGetVersion), which banana/drm_shim.c answers from the
# kernel's i915 interface. Without HAVE_LIBDRM, util/libdrm.h gives them
# stubs that always fail ("Failed to query drm device"): crocus and iris
# then turned every real GPU down and apps drew on the processor. (r600 and
# the radeon winsys likewise: drmGetVersion, drmCommandWriteRead.)
LIBDRM_USERS = re.compile(r"(^|/)src/(intel/|gallium/drivers/(crocus|iris|r600)/|gallium/winsys/(crocus|iris|radeon)/)")
# r600's shader compiler (sfn) is C++ with iostreams: libstdc++'s headers with
# the LLVM port's compatibility (glibc's C-locale layout, what the SDK's C
# library lacks), and libstdc++.a linked in as ports/llvm/link.py links it
CXX_STREAMS = re.compile(r"(^|/)src/gallium/drivers/r600/")
LLVM_INC = os.path.normpath(HERE + "/../llvm/include")
STREAMS_CXX = ["-isystem", LLVM_INC, "-include", LLVM_INC + "/cxx_compat.h"]
EXTRA = os.environ.get("EXTRA", "").split()
JOBS = int(os.environ.get("JOBS", os.cpu_count() or 2))
KEEP = os.environ.get("KEEP") == "1"         # objects already there are kept
os.makedirs(OUT + "/objs", exist_ok=True)
os.makedirs(OUT + "/logs", exist_ok=True)


def ninja(*args):
    return subprocess.run(["ninja", "-C", REF] + list(args), capture_output=True, text=True, check=True).stdout


def objname(o):
    return o.replace("/", "_")


# the link of libOSMesa.so and the archives (with their objects) it takes
link = None
for line in ninja("-t", "commands", "src/gallium/targets/osmesa/libOSMesa.so.8.0.0").splitlines():
    if "-o src/gallium/targets/osmesa/libOSMesa.so.8.0.0" in line:
        link = line
if not link:
    sys.exit("rebuild.py: the link of libOSMesa.so was not found in " + REF)
largs = shlex.split(link)


def inputs(target):                          # what ninja builds target from
    q = ninja("-t", "query", target)
    ins = re.search(r"input: \S+\n((?:    .*\n)*)", q)
    return [x.strip() for x in ins.group(1).splitlines() if not x.strip().startswith("|")] if ins else []


archives = [a for a in largs if a.endswith(".a") and not a.startswith("/")]
members = {a: [x for x in inputs(a) if x.endswith(".o") and not x.endswith("renderonly_renderonly.c.o")] for a in archives}   # archive -> its objects
direct = [a for a in largs if a.endswith(".o") and not a.startswith("/")]
# Mesa's own shared libraries (libglapi.so): their objects go into libOSMesa.so
# itself (Banana OS loads one library)
shared = [a for a in largs[1:] if re.search(r"\.so(\.[0-9.]+)?$", a) and not a.startswith(("/", "-"))
          and a != "src/gallium/targets/osmesa/libOSMesa.so.8.0.0"]
for s in shared:
    for x in inputs(s):
        if x.endswith(".o"):
            direct.append(x)
        elif x.endswith(".a") and not x.startswith("/") and x not in archives:
            archives.append(x)
            members[x] = [y for y in inputs(x) if y.endswith(".o")]

cmds = {}
for e in json.load(open(REF + "/compile_commands.json")):
    out = e.get("output") or re.search(r"-o (\S+)", e["command"]).group(1)
    cmds[out] = e
wanted = [o for a in archives for o in members[a]] + direct
if ONLY:
    wanted = [o for o in wanted if any(s in o for s in ONLY)]


def rewrite(cmd, out, directory="", source=""):
    a = shlex.split(cmd)
    while os.path.basename(a[0]) in ("ccache", "sccache"): a.pop(0)
    cxx = a[0].endswith("++")
    pre = PRE[:1] + (PRE_CXX if cxx else []) + PRE[1:]
    res, i = [a[0]] + pre, 1
    while i < len(a):
        x = a[i]
        if x in ("-MD", "-pipe", "-pthread") or x.startswith("-fstack-protector") or x.startswith("-fstack-clash") \
                or x.startswith("-I/usr/include/libdrm"):        # (the host's libdrm: ours is ports/mesa/include)
            i += 1
            continue
        if x in ("-MQ", "-MF"):
            i += 2
            continue
        if x == "-o":
            res += ["-o", OUT + "/objs/" + objname(out)]
            i += 2
            continue
        res.append(x)
        i += 1
    post = POST
    if LIBDRM_USERS.search(os.path.normpath(os.path.join(directory, source))):
        post = [x for x in POST if x != "-UHAVE_LIBDRM"] + ["-DHAVE_LIBDRM"]
    if cxx and not LLVM_LIBRARY and CXX_STREAMS.search(os.path.normpath(os.path.join(directory, source))):
        res = res[:1] + STREAMS_CXX + res[1:]
    return res + post + (POST_CXX if cxx else []) + EXTRA


# The reference build targets host headers. SDK or port header changes must
# also invalidate the target cache, even when the source file stayed put.
roots = [SDK + "/include", HERE + "/include", HERE + "/../llvm/include", os.path.abspath(REF + "/../../src"), REF]
header_hash = hashlib.sha256()
for base in roots:
    if not os.path.isdir(base): continue
    for directory, dirs, files in os.walk(base):
        dirs.sort()
        for filename in sorted(files):
            if filename.endswith((".h", ".inc")):
                path = os.path.join(directory, filename)
                header_hash.update(path.encode()); header_hash.update(open(path, "rb").read())
header_stamp = header_hash.hexdigest()
compiler_id = subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0]

def build(o):
    e = cmds.get(o)
    if not e:
        return o, False, "no compile command"
    artifact = OUT + "/objs/" + objname(o)
    source = os.path.join(e["directory"], e["file"])
    argv = rewrite(e["command"], o, e["directory"], e["file"])
    signature = hashlib.sha256((json.dumps(argv) + str(header_stamp) + compiler_id).encode()).hexdigest()
    stamp = artifact + ".signature"
    if KEEP and os.path.exists(artifact) and os.path.exists(stamp) and open(stamp).read() == signature and os.path.getmtime(artifact) >= os.path.getmtime(source):
        return o, True, ""
    r = subprocess.run(argv, cwd=e["directory"], capture_output=True, text=True)
    log = OUT + "/logs/" + objname(o) + ".log"
    if r.returncode == 0:
        open(stamp, "w").write(signature)
        if os.path.exists(log):
            os.remove(log)
        return o, True, ""
    open(log, "w").write(r.stderr)
    return o, False, r.stderr


with ThreadPoolExecutor(JOBS) as ex:
    res = list(ex.map(build, wanted))
bad = [r for r in res if not r[1]]
print("ok %d, failed %d" % (len(res) - len(bad), len(bad)))
if bad:
    missing, errs = collections.Counter(), collections.Counter()
    for _, _, e in bad:
        for m in re.findall(r"fatal error: ([^:]+): No such file", e):
            missing[m] += 1
        for m in re.findall(r"error: (.*)", e):
            errs[re.sub(r"[‘’'\"][^‘’'\"]*[‘’'\"]", "X", m)[:90]] += 1
    print("missing headers: " + ", ".join("%s(%d)" % kv for kv in missing.most_common(60)))
    print("errors:")
    for m, n in errs.most_common(40):
        print("  %4d %s" % (n, m))
    print("failed: " + " ".join(os.path.basename(r[0]) for r in bad[:80]))
    sys.exit(1)
if ONLY:
    sys.exit(0)

# the archives again, then the library
os.makedirs(OUT + "/lib", exist_ok=True)
for x in archives:
    a = OUT + "/lib/" + objname(x)
    if os.path.exists(a):
        os.remove(a)
    subprocess.run(["ar", "rcs", a] + [OUT + "/objs/" + objname(o) for o in members[x]], check=True)
newargs, skip = [], False
for x in largs:
    if skip:
        skip = False
        continue
    if x in archives:
        newargs.append(OUT + "/lib/" + objname(x))
    elif x in direct:
        newargs.append(OUT + "/objs/" + objname(x))
    elif x in shared:
        newargs += [OUT + "/objs/" + objname(o) for o in inputs(x) if o.endswith(".o")]
        newargs += [OUT + "/lib/" + objname(o) for o in inputs(x) if o in archives and o not in largs]
    elif x == "-o":
        newargs += ["-o", OUT + "/libOSMesa.so"]
        skip = True
    elif (x.startswith("-l") or x in ("-pthread", "-Wl,--no-undefined") or x.startswith("-Wl,--as-needed")
          or x.startswith("-Wl,-rpath") or (x.startswith("/") and ".so" in x)):
        continue                             # (the C library and the others: the app's, at dlopen)
    elif x.startswith("-Wl,-soname"):
        newargs.append("-Wl,-soname,libOSMesa.so")
    else:
        newargs.append(x)
cc = newargs[0]
crt = [subprocess.run([cc, "-print-file-name=" + f], capture_output=True, text=True).stdout.strip() for f in ("crtbeginS.o", "crtendS.o")]
libgcc = subprocess.run([cc, "-print-libgcc-file-name"], capture_output=True, text=True).stdout.strip()
# std::unordered_map's growth policy: libstdc++'s own object (it needs nothing
# else; the GCC Runtime Library Exception allows linking it in)
libstdcxx = subprocess.run([cc, "-print-file-name=libstdc++.a"], capture_output=True, text=True).stdout.strip()
subprocess.run(["ar", "x", "--output=" + OUT + "/lib", libstdcxx, "hashtable_c++0x.o"], check=True)
final = [newargs[0], "-nostdlib", crt[0]] + newargs[1:] + [OUT + "/lib/hashtable_c++0x.o", libgcc, crt[1]]
if LLVM_LIBRARY: final += [os.path.abspath(LLVM_LIBRARY)]
streams = not LLVM_LIBRARY and any(CXX_STREAMS.search(o) and o.endswith(".cpp.o") for o in wanted)
if streams:
    # libstdc++.a for r600's iostreams. The SDK's own symbols (malloc, operator
    # new, __cxa_atexit...) come first from a link-only stand-in for it, so that
    # nothing of a second runtime is pulled out of libstdc++.a for them; the
    # stand-in is taken out of the library's needs afterwards (as link.py).
    sdk_objs = [os.path.join(HERE, "gl3test/build/x86_64", f) for f in sorted(os.listdir(HERE + "/gl3test/build/x86_64"))
                if f.endswith(".o") and f not in ("__exports.o", "gl3test.o", "gl3.o", "sdk_crt0.o")] \
        if os.path.isdir(HERE + "/gl3test/build/x86_64") else []
    if not sdk_objs:
        sys.exit("rebuild.py: build ports/mesa/gl3test first (the SDK's symbols, for r600's C++ runtime)")
    syms = {}
    for f in sdk_objs:
        for line in subprocess.check_output(["nm", "-g", "--defined-only", f], text=True).splitlines():
            x = line.split()
            if len(x) == 3 and not x[2].startswith("_ZTVN10__cxxabiv1"):
                syms[x[2]] = x[1]
    open(OUT + "/lib/imports.S", "w").write("".join(
        ".globl %s\n.type %s,@function\n%s: ret\n" % (n, n, n) if t.upper() in ("T", "W") else
        ".globl %s\n.type %s,@object\n.size %s,8\n%s: .quad 0\n" % (n, n, n, n) for n, t in sorted(syms.items()))
        + '.section .note.GNU-stack,"",@progbits\n')
    subprocess.run(["gcc", "-shared", "-nostdlib", "-Wl,-soname,libbanana-import.so", "-o", OUT + "/lib/libbanana-import.so",
                    OUT + "/lib/imports.S"], check=True)
    gccinc = subprocess.check_output(["gcc", "-print-file-name=include"], text=True).strip()
    subprocess.run(["gcc", "-c", "-fPIC", "-O2", "-mno-red-zone", "-nostdinc", "-isystem", SDK + "/include", "-isystem", LLVM_INC,
                    "-isystem", gccinc, HERE + "/../llvm/compat.c", "-o", OUT + "/lib/cxx_compat.o"], check=True)
    subprocess.run(["gcc", "-c", "-fPIC", "-O2", "-mno-red-zone", "-nostdinc", "-isystem", SDK + "/include", "-isystem", LLVM_INC,
                    "-isystem", gccinc, HERE + "/banana/cxx_streams.c", "-o", OUT + "/lib/cxx_streams.o"], check=True)
    lib = lambda f: subprocess.check_output(["g++", "-print-file-name=" + f], text=True).strip()
    final = final[:-2] + ["-Wl,--no-as-needed", OUT + "/lib/libbanana-import.so", OUT + "/lib/cxx_compat.o", OUT + "/lib/cxx_streams.o",
                          "-Wl,--start-group", lib("libstdc++.a"), lib("libgcc_eh.a"), libgcc, "-Wl,--end-group"] + final[-1:]
r = subprocess.run(final, cwd=REF, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-4000:])
    sys.exit(1)
if streams:
    # take the stand-in out of DT_NEEDED: its symbols stay the app's (the SDK loader's) to give
    import struct
    p = OUT + "/libOSMesa.so"
    b = bytearray(open(p, "rb").read())
    phoff = struct.unpack_from("<Q", b, 32)[0]
    phsize, phnum = struct.unpack_from("<HH", b, 54)
    segs = [struct.unpack_from("<IIQQQQQQ", b, phoff + i * phsize) for i in range(phnum)]
    dyn = next(ph for ph in segs if ph[0] == 2)
    ents = [(j, *struct.unpack_from("<QQ", b, j)) for j in range(dyn[2], dyn[2] + dyn[5], 16)]
    straddr = next(v for _, t, v in ents if t == 5)
    load = next(ph for ph in segs if ph[0] == 1 and ph[3] <= straddr < ph[3] + ph[5])
    strings = load[2] + straddr - load[3]
    removed = 0
    for off, tag, val in ents:
        if tag == 1:
            name = bytes(b[strings + val:]).split(b"\0", 1)[0]
            if name != b"libbanana-import.so":
                sys.exit("rebuild.py: libOSMesa.so needs " + name.decode())
            struct.pack_into("<QQ", b, off, 21, 0)                 # (DT_DEBUG: nothing)
            removed += 1
    if removed != 1:
        sys.exit("rebuild.py: the SDK stand-in was not linked")
    open(p, "wb").write(b)
print("linked " + OUT + "/libOSMesa.so")
