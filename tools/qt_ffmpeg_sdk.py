"""Stage an FFmpeg SDK that links against the LGPL FFmpeg DLLs Qt Multimedia already ships.

Qt 6.8.3 bundles FFmpeg 7.1 (Lavc 61.19) as DLLs without headers or import libraries. Rather than
shipping a second FFmpeg, we take the public headers from the matching official source release and
generate MSVC import libraries from Qt's DLL export tables. One FFmpeg copy, one set of LGPL duties.

Run (Windows, MSVC tools on disk):
  python tools/qt_ffmpeg_sdk.py <ffmpeg-7.1 source dir> <Qt bin dir> <out dir>
Produces <out>/include, <out>/lib/*.lib, <out>/bin/*.dll (FFMPEG_ROOT layout for CMake).
"""
import glob, os, pathlib, re, shutil, subprocess, sys

LIBS = ["avcodec", "avformat", "avutil", "swscale", "swresample"]


def msvc_tool(name):
    hits = sorted(glob.glob(r"C:\Program Files*\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\Hostx64\x64\\" + name))
    if not hits:
        sys.exit(f"{name} not found; install VS 2022 Build Tools")
    return hits[-1]


def main(src, qt_bin, out):
    src, qt_bin, out = map(pathlib.Path, (src, qt_bin, out))
    inc, lib, binr = out / "include", out / "lib", out / "bin"
    for d in (inc, lib, binr):
        d.mkdir(parents=True, exist_ok=True)
    for name in LIBS:
        shutil.copytree(src / f"lib{name}", inc / f"lib{name}", dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("*.c", "*.asm", "*.S", "tests", "x86", "aarch64", "arm", "*.o"))
    # Headers normally written by ./configure.
    (inc / "libavutil" / "avconfig.h").write_text(
        "#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n#endif\n")
    (inc / "libavutil" / "ffversion.h").write_text(
        "#ifndef AVUTIL_FFVERSION_H\n#define AVUTIL_FFVERSION_H\n#define FFMPEG_VERSION \"7.1\"\n#endif\n")
    dumpbin, libexe = msvc_tool("dumpbin.exe"), msvc_tool("lib.exe")
    for dll in sorted(qt_bin.glob("*.dll")):
        m = re.match(r"(avcodec|avformat|avutil|swscale|swresample)-\d+\.dll$", dll.name)
        if not m:
            continue
        exports = subprocess.run([dumpbin, "/exports", str(dll)], capture_output=True, text=True, check=True).stdout
        names = re.findall(r"^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)", exports, re.M)
        deff = lib / f"{m.group(1)}.def"
        deff.write_text(f"LIBRARY {dll.stem}\nEXPORTS\n" + "".join(f"  {n}\n" for n in names))
        subprocess.run([libexe, "/nologo", f"/def:{deff}", f"/out:{lib / (m.group(1) + '.lib')}", "/machine:x64"], check=True,
                       capture_output=True)
        shutil.copy2(dll, binr / dll.name)
        print(f"{dll.name}: {len(names)} exports -> {m.group(1)}.lib")


if __name__ == "__main__":
    main(*sys.argv[1:4])
