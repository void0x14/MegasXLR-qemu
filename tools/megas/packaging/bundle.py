#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build-time only: collect binaries, their actual imports, firmware and checksums.

No Python is shipped or required by the installed application. A bundle is tied
to its target CPU/OS. Linux includes its loader to avoid host library collisions.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def copied(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination, follow_symlinks=True)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def imports_linux(binary):
    output = subprocess.run(["ldd", str(binary)], text=True, capture_output=True,
                            check=True).stdout
    if "not found" in output:
        raise RuntimeError(f"Unresolved shared library for {binary}: {output}")
    return [Path(match) for match in re.findall(r"(?:=>\s+)?(/[^\s()]+)", output)]


def imports_windows(binary, search):
    output = subprocess.run(["objdump", "-p", str(binary)], text=True,
                            capture_output=True, check=True).stdout
    names = re.findall(r"DLL Name:\s*(\S+)", output, re.IGNORECASE)
    result = []
    # OS DLLs stay supplied by Windows. Application DLLs are private to the bundle.
    os_dlls = {"kernel32.dll", "user32.dll", "gdi32.dll", "ws2_32.dll",
               "advapi32.dll", "bcrypt.dll", "crypt32.dll", "dnsapi.dll",
               "iphlpapi.dll", "ntdll.dll", "ole32.dll", "oleaut32.dll",
               "secur32.dll", "setupapi.dll", "shell32.dll", "shlwapi.dll",
               "version.dll", "winmm.dll", "winspool.drv", "wtsapi32.dll",
               "msvcrt.dll", "ucrtbase.dll", "comdlg32.dll", "comctl32.dll",
               "dwmapi.dll", "imm32.dll", "netapi32.dll", "normaliz.dll",
               "psapi.dll", "rpcrt4.dll", "userenv.dll", "winhttp.dll",
               "wininet.dll", "powrprof.dll", "dwrite.dll", "dxgi.dll",
               "d3d11.dll", "opengl32.dll", "winhvplatform.dll", "cfgmgr32.dll",
               "msimg32.dll", "uxtheme.dll", "shcore.dll"}
    available = {}
    for directory in search:
        if directory.is_dir():
            available.update({entry.name.lower(): entry for entry in directory.iterdir()
                              if entry.is_file()})
    for name in names:
        lower = name.lower()
        if lower in os_dlls or lower.startswith(("api-ms-", "ext-ms-")):
            continue
        if lower not in available:
            raise RuntimeError(f"Unresolved application DLL {name} imported by {binary}")
        result.append(available[lower])
    return result


def dependency_closure(binary, windows, search):
    pending = [Path(binary).resolve()]
    visited = set()
    dependencies = {}
    while pending:
        current = pending.pop()
        current_key = str(current.resolve())
        if current_key in visited:
            continue
        visited.add(current_key)
        imports = imports_windows(current, search) if windows else imports_linux(current)
        for dependency in imports:
            resolved = dependency.resolve()
            key = str(resolved)
            if key not in visited:
                dependencies[key] = resolved
                pending.append(resolved)
    return list(dependencies.values())


def launcher(path, relative_binary, root_up=".", library_dirs=("lib", "qemu/lib"),
             runtime_vars=True, tessdata=False):
    library = ":".join(f"$base/{directory}" for directory in library_dirs)
    body = f'''#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")/{root_up}" && pwd)
'''
    if runtime_vars:
        body += f'''export MEGAS_QEMU="$base/qemu/bin/qemu-system-x86_64"
export MEGAS_QEMU_IMG="$base/qemu/bin/qemu-img"
export MEGAS_QEMU_DATA_DIR="$base/qemu/share/qemu"
export MEGAS_PACKAGE_DIR="$base"
export LD_LIBRARY_PATH="{library}${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}"
if [ -x "$base/ocr/tesseract" ]; then
    export MEGAS_TESSERACT="$base/ocr/tesseract"
    export TESSDATA_PREFIX="$base/ocr/tessdata"
fi
export QEMU_MODULE_DIR="$base/qemu/lib/qemu"
'''
    elif tessdata:
        body += '''export TESSDATA_PREFIX="$base/ocr/tessdata"
'''
    body += f'''exec "$base/lib/ld-linux-x86-64.so.2" --library-path "{library}" "$base/{relative_binary}" "$@"
'''
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body)
    path.chmod(0o755)


def collect(binary, destination, libraries, windows, search):
    copied(binary, destination)
    pending = [binary]
    seen = set()
    while pending:
        current = pending.pop()
        deps = imports_windows(current, search) if windows else imports_linux(current)
        for dependency in deps:
            key = str(dependency.resolve())
            if key in seen:
                continue
            seen.add(key)
            copied(dependency, libraries / dependency.name)
            pending.append(dependency)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--headless", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qemu", type=Path)
    parser.add_argument("--qemu-img", type=Path,
                        help="QEMU disk utility from the same runtime build")
    parser.add_argument("--qemu-data", type=Path)
    parser.add_argument("--qemu-modules", type=Path,
                        help="QEMU dynamic module directory, if this build uses modules")
    parser.add_argument("--tesseract", type=Path,
                        help="Optional Tesseract executable to bundle with its imports")
    parser.add_argument("--tesseract-data", type=Path,
                        help="Optional Tesseract tessdata directory; English trained data is included")
    parser.add_argument("--release-base", help="Trusted HTTPS release directory; emits pinned bootstrap installers")
    args = parser.parse_args()
    windows = args.binary.suffix.lower() == ".exe"
    suffix = ".exe" if windows else ""
    qemu = args.qemu or shutil.which("qemu-system-x86_64" + suffix)
    qemu_img = args.qemu_img or shutil.which("qemu-img" + suffix)
    if qemu and not qemu_img:
        sibling = Path(qemu).resolve().with_name("qemu-img" + suffix)
        if sibling.is_file():
            qemu_img = sibling
    if not qemu or not qemu_img:
        raise RuntimeError("QEMU and qemu-img must be available on the build runner to produce a complete bundle")
    qemu = Path(qemu).resolve()
    qemu_img = Path(qemu_img).resolve()
    search = [qemu.parent, args.binary.resolve().parent]
    tesseract = args.tesseract or shutil.which("tesseract" + suffix)
    tessdata = args.tesseract_data
    tesseract_skip_reason = "Tesseract or English trained data was not selected or found."
    ocr_runtime_bytes = 0
    if tesseract:
        tesseract = Path(tesseract).resolve()
        search.append(tesseract.parent)
        if tessdata is None:
            result = subprocess.run([str(tesseract), "--print-tessdata-dir"],
                                    text=True, capture_output=True)
            if result.returncode == 0 and result.stdout.strip():
                tessdata = Path(result.stdout.strip().splitlines()[-1])
            else:
                result = subprocess.run([str(tesseract), "--list-langs"],
                                        text=True, capture_output=True)
                match = re.search(r' in ["\']([^"\']+)["\']',
                                  result.stdout + result.stderr)
                if match:
                    tessdata = Path(match.group(1))
                else:
                    candidates = ([Path(os.environ["TESSDATA_PREFIX"])]
                                  if os.environ.get("TESSDATA_PREFIX") else [])
                    candidates.extend((tesseract.parent.parent / "share" / "tessdata",
                                       Path("/usr/share/tesseract-ocr/5/tessdata"),
                                       Path("/usr/share/tesseract-ocr/4.00/tessdata")))
                    tessdata = next((candidate for candidate in candidates
                                     if (candidate / "eng.traineddata").is_file()), None)
        if tessdata is None or not (Path(tessdata) / "eng.traineddata").is_file():
            if args.tesseract:
                raise RuntimeError("Selected Tesseract needs a tessdata directory containing eng.traineddata")
            tesseract_skip_reason = "Tesseract was found but English trained data was unavailable."
            tesseract = None
            tessdata = None
        if tesseract:
            tessdata = Path(tessdata).resolve()
            try:
                tesseract_dependencies = dependency_closure(tesseract, windows, search)
            except RuntimeError as error:
                if args.tesseract:
                    raise RuntimeError(f"Selected Tesseract has an unresolved runtime dependency: {error}") from error
                tesseract_skip_reason = f"Tesseract runtime dependencies were unresolved: {error}"
                tesseract = None
                tessdata = None
            if tesseract:
                ocr_runtime_bytes = (tesseract.stat().st_size +
                                     (tessdata / "eng.traineddata").stat().st_size +
                                     sum(path.stat().st_size for path in tesseract_dependencies))
                tesseract_skip_reason = ""
    output = args.output.resolve()
    if output.exists():
        if not (output / ".megas-bundle").is_file():
            raise RuntimeError("Output exists without the bundle marker; refusing to replace another directory")
        shutil.rmtree(output)
    output.mkdir(parents=True)
    (output / ".megas-bundle").write_text("Megas generated bundle\n")
    appdir = output if windows else output / "libexec"
    app_libdir = output if windows else output / "lib"
    collect(args.binary.resolve(), appdir / ("megas" + suffix), app_libdir, windows, search)
    collect(args.headless.resolve(), appdir / ("megas-headless" + suffix), app_libdir, windows, search)
    qemu_dir = output / "qemu" / ("bin" if windows else "libexec")
    qemu_libdir = output / "qemu" / "bin" if windows else output / "qemu" / "lib"
    collect(qemu, qemu_dir / qemu.name, qemu_libdir, windows, search)
    collect(qemu_img, qemu_dir / qemu_img.name, qemu_libdir, windows, search)
    if tesseract:
        ocr_directory = output / "ocr"
        ocr_libdir = ocr_directory if windows else ocr_directory / "lib"
        ocr_binary = ocr_directory / ("tesseract.exe" if windows else "bin/tesseract")
        collect(tesseract, ocr_binary, ocr_libdir,
                windows, search)
        tessdata_output = ocr_directory / "tessdata"
        tessdata_output.mkdir(parents=True, exist_ok=True)
        copied(Path(tessdata) / "eng.traineddata", tessdata_output / "eng.traineddata")
    if not windows:
        # QEMU and qemu-img use the same private runtime loader as the controller.
        for name in ("megas", "megas-headless"):
            launcher(output / name, f"libexec/{name}", library_dirs=("lib", "qemu/lib", "ocr/lib"))
        for name in ("qemu-system-x86_64", "qemu-img"):
            launcher(output / "qemu" / "bin" / name, f"qemu/libexec/{name}",
                     root_up="../..", library_dirs=("qemu/lib", "lib"))
        if tesseract:
            launcher(output / "ocr" / "tesseract", "ocr/bin/tesseract",
                     root_up="..", library_dirs=("ocr/lib",), runtime_vars=False,
                     tessdata=True)
        if not (output / "lib" / "ld-linux-x86-64.so.2").exists():
            raise RuntimeError("Linux x86-64 glibc loader unavailable; this bundle target requires x86-64 glibc")
    data = args.qemu_data
    if data is None:
        candidates = [qemu.parent.parent / "share" / "qemu", Path("/usr/share/qemu")]
        data = next((path for path in candidates if path.is_dir()), None)
    if data is None:
        raise RuntimeError("QEMU firmware directory missing; refusing an incomplete bundle")
    shutil.copytree(data, output / "qemu" / "share" / "qemu", symlinks=False)
    explicit_module_dir = args.qemu_modules or (
        Path(os.environ["QEMU_MODULE_DIR"]) if os.environ.get("QEMU_MODULE_DIR") else None)
    if explicit_module_dir:
        module_candidates = [explicit_module_dir]
    else:
        module_prefix = qemu.parent.parent / "lib"
        module_candidates = [module_prefix / "qemu", *sorted(module_prefix.glob("*/qemu")),
                             Path("/usr/lib/x86_64-linux-gnu/qemu"),
                             Path("/usr/lib/qemu")]
    seen_module_directories = set()
    skipped_modules = []
    for module_directory in module_candidates:
        module_directory = Path(module_directory).resolve()
        if module_directory in seen_module_directories:
            continue
        seen_module_directories.add(module_directory)
        if module_directory.is_dir():
            for module in module_directory.iterdir():
                if module.is_file() and module.suffix in (".so", ".dll"):
                    try:
                        collect(module, output / "qemu" / "lib" / "qemu" / module.name,
                                qemu_libdir, windows, search)
                    except RuntimeError as error:
                        if "Unresolved shared library" not in str(error) and "Unresolved application DLL" not in str(error):
                            raise
                        (output / "qemu" / "lib" / "qemu" / module.name).unlink(missing_ok=True)
                        skipped_modules.append(f"{module.name}: {error}")
            break
        if explicit_module_dir:
            raise RuntimeError(f"Selected QEMU module directory does not exist: {module_directory}")
    def version_line(binary):
        result = subprocess.run([str(binary), "--version"], text=True,
                                capture_output=True, check=True)
        return result.stdout.splitlines()[0] if result.stdout.splitlines() else "version unavailable"
    runtime_metadata = ["Megas runtime inputs (build-time provenance)",
                        f"Megas controller: {args.binary.name}",
                        f"QEMU executable: {qemu.name}",
                        f"QEMU version: {version_line(qemu)}",
                        f"qemu-img executable: {qemu_img.name}",
                        f"qemu-img version: {version_line(qemu_img)}",
                        f"OCR runtime: {'Tesseract plus English trained data included; private runtime size ' + format(ocr_runtime_bytes / (1024 * 1024), '.1f') + ' MiB.' if tesseract else 'Tesseract not included; OCR may use a system installation if available. ' + tesseract_skip_reason}",
                        *([f"Tesseract executable: {tesseract.name}",
                           f"Tesseract version: {version_line(tesseract)}"] if tesseract else []),
                        "Runtime origin: imported from the selected build runner/toolchain; this bundle step does not build QEMU from the source checkout.",
                        "Use --qemu, --qemu-img, --qemu-data, and --qemu-modules to select a matching QEMU build and its assets.",
                        *(f"Optional QEMU module omitted because a build-time dependency was unavailable: {line}" for line in skipped_modules),
                        ""]
    (output / "licenses" / "runtime-source.txt").parent.mkdir(parents=True, exist_ok=True)
    (output / "licenses" / "runtime-source.txt").write_text("\n".join(runtime_metadata), encoding="utf-8")
    # Linux distribution packages can put SeaBIOS outside the QEMU data directory.
    for directory in (Path("/usr/share/seabios"), Path("/usr/share/ipxe/qemu")):
        if not windows and directory.is_dir():
            for path in directory.iterdir():
                if path.is_file():
                    target = output / "qemu" / "share" / "qemu" / path.name
                    if not target.exists():
                        copied(path, target)
    for name in ("packaging", "guest", "docs"):
        ignored = ["__pycache__", "*.pyc"]
        if name == "docs":
            # Maintainer validation records the archive hash and stays source-only.
            ignored.append("packaging-validation.md")
        shutil.copytree(ROOT / name, output / name, ignore=shutil.ignore_patterns(*ignored))
    if not windows:
        # The ELF controller runs from libexec; keep the source helpers next to
        # it as well as in the install-facing root-level guest directory.
        shutil.copytree(ROOT / "guest", output / "libexec" / "guest",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    copied(ROOT / "vendor" / "LICENSE", output / "licenses" / "Nuklear.txt")
    copied(ROOT.parents[1] / "COPYING", output / "licenses" / "QEMU-COPYING.txt")
    if windows:
        license_directory = qemu.parent.parent / "share" / "licenses"
        if license_directory.is_dir():
            shutil.copytree(license_directory, output / "licenses" / "runtime")
    else:
        versions = []
        runtime_binaries = [qemu, qemu_img, *([tesseract] if tesseract else [])]
        runtime_libraries = list((output / "lib").iterdir()) + list((output / "qemu" / "lib").iterdir())
        if (output / "ocr" / "lib").is_dir():
            runtime_libraries.extend((output / "ocr" / "lib").iterdir())
        for dependency in [*runtime_binaries, *runtime_libraries]:
            query = subprocess.run(["dpkg-query", "-S", str(dependency if dependency in runtime_binaries else dependency.name)],
                                   text=True, capture_output=True)
            if query.returncode == 0:
                package = query.stdout.split(": ", 1)[0].splitlines()[0]
                copyright_file = Path("/usr/share/doc") / package.split(":", 1)[0] / "copyright"
                if copyright_file.is_file():
                    copied(copyright_file, output / "licenses" / "runtime" / (package.replace(":", "_") + ".txt"))
                version = subprocess.run(["dpkg-query", "-W", "-f=${Package} ${Version}\\n", package],
                                         text=True, capture_output=True)
                if version.returncode == 0:
                    versions.append(version.stdout)
        (output / "licenses" / "runtime-versions.txt").write_text("".join(sorted(set(versions))))
    if not windows:
        installer = output / "packaging" / "install.sh"
        installer.chmod(0o755)
        if not installer.stat().st_mode & 0o111:
            raise RuntimeError("Linux bundle installer is not executable")
    files = sorted(path for path in output.rglob("*") if path.is_file())
    manifest = "".join(f"{sha256(path)}  {path.relative_to(output).as_posix()}\n" for path in files)
    (output / "SHA256SUMS").write_text(manifest, encoding="ascii")
    archive = output.with_suffix(".zip" if windows else ".tar.gz")
    if windows:
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as writer:
            for path in output.rglob("*"):
                if path.is_file():
                    writer.write(path, path.relative_to(output.parent))
    else:
        with tarfile.open(archive, "w:gz") as writer:
            writer.add(output, arcname=output.name)
    digest = sha256(archive)
    archive.with_name(archive.name + ".sha256").write_text(f"{digest}  {archive.name}\n")
    if args.release_base:
        if not args.release_base.startswith("https://github.com/void0x14/MegasXLR-qemu/releases/download/"):
            raise RuntimeError("Bootstrap URL must use this fork's GitHub release directory")
        url = args.release_base.rstrip("/") + "/" + archive.name
        template = "bootstrap.ps1.in" if windows else "bootstrap.sh.in"
        content = (ROOT / "packaging" / template).read_text()
        escaped_url = url.replace("'", "''") if windows else shlex.quote(url)
        content = content.replace("@URL@", escaped_url).replace("@SHA256@", digest).replace("@BUNDLE@", output.name)
        bootstrap = archive.with_name("install-megas.ps1" if windows else "install-megas.sh")
        bootstrap.write_text(content)
        if not windows:
            bootstrap.chmod(0o755)
    print(f"Bundle ready: {archive}\nSHA256: {digest}\nQEMU: {qemu}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Build runner repair prompt: Repair tools/megas/packaging/bundle.py on this runner. {error}. Resolve the missing build input and rerun; do not publish a partial bundle.", file=sys.stderr)
        sys.exit(1)
