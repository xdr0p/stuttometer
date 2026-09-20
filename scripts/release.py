#!/usr/bin/env python3
"""
Stuttometer Automated Release & Version Management Tool
======================================================
Usage:
    python scripts/release.py <version> [options]
    python scripts/release.py --bump {patch,minor,major} [options]

Examples:
    python scripts/release.py 0.5.1 --title "Audio Glitch Timing Polish"
    python scripts/release.py --bump patch --dry-run
    python scripts/release.py 0.5.1 --skip-push
"""

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# All 10 files that define or validate version strings in Stuttometer
VERSION_TARGETS = [
    {
        "file": "CMakeLists.txt",
        "pattern": r'(VERSION\s+)\d+\.\d+\.\d+',
        "template": r'\g<1>{version}'
    },
    {
        "file": "include/stuttometer/correlator.hpp",
        "pattern": r'(std::string\s+tool_version\{")\d+\.\d+\.\d+("\};)',
        "template": r'\g<1>{version}\g<2>'
    },
    {
        "file": "src/cli_parser.cpp",
        "pattern": r'(out\s*<<\s*"Stuttometer\s+v)\d+\.\d+\.\d+(\\n";)',
        "template": r'\g<1>{version}\g<2>'
    },
    {
        "file": "src/json_reporter.cpp",
        "pattern": r'(root\["tool_version"\]\s*=\s*report\.tool_version\.empty\(\)\s*\?\s*")\d+\.\d+\.\d+("\s*:\s*report\.tool_version;)',
        "template": r'\g<1>{version}\g<2>'
    },
    {
        "file": "src/main.cpp",
        "pattern": r'(\[STUTTOMETER\]\s+Initializing\s+Stuttometer\s+v)\d+\.\d+\.\d+(\s+\(Elevated Mode\)\.\.\.\\n";)',
        "template": r'\g<1>{version}\g<2>'
    },
    {
        "file": "src/gui/card_renderer.cpp",
        "pattern": r'(std::string\s+ver\s*=\s*report\.tool_version\.empty\(\)\s*\?\s*")\d+\.\d+\.\d+("\s*:\s*report\.tool_version;)',
        "template": r'\g<1>{version}\g<2>'
    },
    {
        "file": "src/gui/resources.rc",
        "multi": [
            {
                "pattern": r'(FILEVERSION\s+)\d+,\d+,\d+,\d+',
                "template": r'\g<1>{major},{minor},{patch},0'
            },
            {
                "pattern": r'(PRODUCTVERSION\s+)\d+,\d+,\d+,\d+',
                "template": r'\g<1>{major},{minor},{patch},0'
            },
            {
                "pattern": r'(VALUE\s+"FileVersion",\s*")\d+\.\d+\.\d+\.\d+(")',
                "template": r'\g<1>{quad}\g<2>'
            },
            {
                "pattern": r'(VALUE\s+"ProductVersion",\s*")\d+\.\d+\.\d+\.\d+(")',
                "template": r'\g<1>{quad}\g<2>'
            }
        ]
    },
    {
        "file": "src/gui/stuttometer_gui.manifest",
        "pattern": r'(version=")\d+\.\d+\.\d+\.\d+(")',
        "template": r'\g<1>{quad}\g<2>'
    },
    {
        "file": "tests/test_cli_args.cpp",
        "pattern": r'(STUTTO_ASSERT\(out\.str\(\)\.find\("Stuttometer v)\d+\.\d+\.\d+("\)\s*!=\s*std::string::npos\);)',
        "template": r'\g<1>{version}\g<2>'
    },
    {
        "file": "tests/test_card_renderer.cpp",
        "pattern": r'(report\.tool_version\s*=\s*")\d+\.\d+\.\d+(";)',
        "template": r'\g<1>{version}\g<2>'
    }
]


def run_cmd(cmd, check=True, cwd=ROOT_DIR):
    """Executes a shell command and streams output."""
    print(f"\n[EXEC] {cmd} (cwd={cwd})")
    res = subprocess.run(cmd, shell=True, cwd=cwd, text=True, capture_output=True)
    if res.stdout:
        print(res.stdout.strip())
    if res.stderr:
        print(res.stderr.strip(), file=sys.stderr)
    if check and res.returncode != 0:
        raise RuntimeError(f"Command failed with exit code {res.returncode}: {cmd}")
    return res


def get_current_version():
    """Reads current project version from CMakeLists.txt."""
    cmake_path = os.path.join(ROOT_DIR, "CMakeLists.txt")
    with open(cmake_path, "r", encoding="utf-8") as f:
        content = f.read()
    m = re.search(r'VERSION\s+(\d+\.\d+\.\d+)', content)
    if not m:
        raise ValueError("Could not determine current version from CMakeLists.txt")
    return m.group(1)


def bump_version_string(current, bump_type):
    """Computes next semantic version."""
    parts = [int(p) for p in current.split(".")]
    if bump_type == "patch":
        parts[2] += 1
    elif bump_type == "minor":
        parts[1] += 1
        parts[2] = 0
    elif bump_type == "major":
        parts[0] += 1
        parts[1] = 0
        parts[2] = 0
    else:
        raise ValueError(f"Unknown bump type: {bump_type}")
    return ".".join(str(p) for p in parts)


def update_file_version(target_def, new_version, dry_run=False):
    """Updates version patterns in a specific file."""
    rel_path = target_def["file"]
    full_path = os.path.join(ROOT_DIR, rel_path)
    if not os.path.exists(full_path):
        raise FileNotFoundError(f"Target file not found: {full_path}")

    with open(full_path, "r", encoding="utf-8") as f:
        content = f.read()

    major, minor, patch = new_version.split(".")
    quad = f"{major}.{minor}.{patch}.0"

    subs = target_def.get("multi", [target_def])
    new_content = content
    replaced_count = 0

    for sub in subs:
        pat = sub["pattern"]
        tmpl = sub["template"].format(
            version=new_version,
            major=major,
            minor=minor,
            patch=patch,
            quad=quad
        )
        matches = len(re.findall(pat, new_content))
        if matches == 0:
            raise RuntimeError(f"Pattern not found in {rel_path}: {pat}")
        new_content, count = re.subn(pat, tmpl, new_content)
        replaced_count += count

    if not dry_run and new_content != content:
        with open(full_path, "w", encoding="utf-8", newline="") as f:
            f.write(new_content)

    print(f"  [OK] {rel_path} ({replaced_count} replacement{'s' if replaced_count > 1 else ''})")


def get_github_token():
    """Retrieves GitHub token from Git Credential Manager."""
    proc = subprocess.Popen(
        ["git", "credential", "fill"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    out, err = proc.communicate("protocol=https\nhost=github.com\n")
    for line in out.splitlines():
        if line.startswith("password="):
            return line.split("=", 1)[1]
    raise RuntimeError("Could not find GitHub token from git credential manager.")


def package_release_zip(version):
    """Packages stuttometer.exe and stuttometer_gui.exe into release zip."""
    zip_name = f"stuttometer-v{version}-windows-x64.zip"
    zip_path = os.path.join(ROOT_DIR, "build", "Release", zip_name)
    exe_cli = os.path.join(ROOT_DIR, "build", "Release", "stuttometer.exe")
    exe_gui = os.path.join(ROOT_DIR, "build", "Release", "stuttometer_gui.exe")

    for exe in [exe_cli, exe_gui]:
        if not os.path.exists(exe):
            raise FileNotFoundError(f"Build artifact not found: {exe}")

    print(f"\n[PACKAGE] Compressing {zip_name}...")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.write(exe_cli, arcname="stuttometer.exe")
        zf.write(exe_gui, arcname="stuttometer_gui.exe")
    size_mb = os.path.getsize(zip_path) / (1024 * 1024)
    print(f"  -> Created {zip_path} ({size_mb:.2f} MB)")
    return zip_path


def publish_github_release(version, title, body_text):
    """Publishes release and uploads assets to GitHub via REST API."""
    token = get_github_token()
    headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github.v3+json",
        "User-Agent": "Stuttometer-Release-Script"
    }

    # Verify user
    req = urllib.request.Request("https://api.github.com/user", headers=headers)
    with urllib.request.urlopen(req) as resp:
        user_data = json.loads(resp.read().decode())
        print(f"[AUTH] Authenticated as GitHub user: {user_data.get('login')}")

    tag_name = f"v{version}"
    release_name = f"Stuttometer {tag_name}" if not title else f"Stuttometer {tag_name} - {title}"

    payload = {
        "tag_name": tag_name,
        "target_commitish": "main",
        "name": release_name,
        "body": body_text or f"## {release_name}\n\nAutomated release for {tag_name}.",
        "draft": False,
        "prerelease": False
    }

    print(f"\n[GITHUB] Creating Release {tag_name}...")
    req = urllib.request.Request(
        "https://api.github.com/repos/xdr0p/stuttometer/releases",
        data=json.dumps(payload).encode(),
        headers=headers,
        method="POST"
    )

    try:
        with urllib.request.urlopen(req) as resp:
            rel_data = json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        err = e.read().decode()
        if e.code == 422:
            print(f"Release already exists for {tag_name}, fetching existing release...")
            req_get = urllib.request.Request(
                f"https://api.github.com/repos/xdr0p/stuttometer/releases/tags/{tag_name}",
                headers=headers
            )
            with urllib.request.urlopen(req_get) as resp_get:
                rel_data = json.loads(resp_get.read().decode())
        else:
            raise RuntimeError(f"GitHub release creation failed ({e.code}): {err}")

    upload_url_base = rel_data["upload_url"].split("{")[0]
    html_url = rel_data["html_url"]
    print(f"  -> GitHub Release: {html_url}")

    zip_path = os.path.join(ROOT_DIR, "build", "Release", f"stuttometer-v{version}-windows-x64.zip")
    assets = [
        (zip_path, f"stuttometer-v{version}-windows-x64.zip", "application/zip"),
        (os.path.join(ROOT_DIR, "build", "Release", "stuttometer.exe"), "stuttometer.exe", "application/octet-stream"),
        (os.path.join(ROOT_DIR, "build", "Release", "stuttometer_gui.exe"), "stuttometer_gui.exe", "application/octet-stream")
    ]

    for fpath, aname, ctype in assets:
        if not os.path.exists(fpath):
            print(f"WARNING: Asset not found: {fpath}")
            continue
        size = os.path.getsize(fpath)
        print(f"[ASSET] Uploading {aname} ({size} bytes)...")
        upload_url = f"{upload_url_base}?name={urllib.parse.quote(aname)}"
        up_headers = {
            "Authorization": f"Bearer {token}",
            "Content-Type": ctype,
            "Content-Length": str(size),
            "User-Agent": "Stuttometer-Release-Script"
        }
        with open(fpath, "rb") as f:
            body = f.read()

        up_req = urllib.request.Request(upload_url, data=body, headers=up_headers, method="POST")
        try:
            with urllib.request.urlopen(up_req) as resp:
                asset_data = json.loads(resp.read().decode())
                print(f"  -> Uploaded: {asset_data.get('browser_download_url')}")
        except urllib.error.HTTPError as e:
            print(f"  -> Asset upload notice ({e.code}): {e.read().decode()}")

    print(f"\n[DONE] Successfully published {tag_name} release to GitHub!")


def main():
    parser = argparse.ArgumentParser(description="Stuttometer Automated Release & Version Tool")
    parser.add_argument("version", nargs="?", help="Explicit target semantic version (e.g. 0.5.1)")
    parser.add_argument("--bump", choices=["patch", "minor", "major"], help="Bump version automatically")
    parser.add_argument("--title", "-t", default="", help="Release title summary")
    parser.add_argument("--notes", "-n", default="", help="Custom markdown release notes or path to notes file")
    parser.add_argument("--dry-run", action="store_true", help="Perform version replacement check without writing files or pushing")
    parser.add_argument("--skip-build", action="store_true", help="Skip CMake build and tests (e.g. for testing version replacement)")
    parser.add_argument("--skip-push", action="store_true", help="Skip git commit, tag, push, and GitHub release publication")

    args = parser.parse_args()

    current_ver = get_current_version()
    print(f"[INFO] Current Stuttometer version: v{current_ver}")

    if args.bump:
        target_ver = bump_version_string(current_ver, args.bump)
    elif args.version:
        target_ver = args.version.lstrip("v")
    else:
        parser.error("Must provide a version argument (e.g. 0.5.1) or specify --bump {patch,minor,major}")

    if not re.match(r'^\d+\.\d+\.\d+$', target_ver):
        raise ValueError(f"Version must follow semantic versioning X.Y.Z (got: {target_ver})")

    print(f"[INFO] Target version: v{target_ver} ({'DRY-RUN' if args.dry_run else 'APPLYING'})")

    # Step 1: Update version in all 10 project files
    print("\n=== Step 1: Updating Version Across Codebase ===")
    for target in VERSION_TARGETS:
        update_file_version(target, target_ver, dry_run=args.dry_run)

    if args.dry_run:
        print("\n[DRY-RUN] Version substitutions validated successfully across all 10 files.")
        return

    # Step 2: Build and Test
    if not args.skip_build:
        print("\n=== Step 2: Compiling Release Configuration ===")
        run_cmd("cmake --build build --config Release")

        print("\n=== Step 3: Running Test Suite ===")
        run_cmd("ctest --test-dir build -C Release --output-on-failure")

    # Step 3: Package Release Zip
    package_release_zip(target_ver)

    if args.skip_push:
        print(f"\n[LOCAL DONE] v{target_ver} updated and tested locally (--skip-push).")
        return

    # Step 4: Git Commit & Tag
    print("\n=== Step 4: Git Commit & Tag ===")
    commit_msg = f"feat: Stuttometer {target_ver}"
    if args.title:
        commit_msg += f" - {args.title}"

    run_cmd("git add -u")
    run_cmd(f'git commit -m "{commit_msg}"')
    tag_msg = f"Stuttometer v{target_ver}" + (f" - {args.title}" if args.title else "")
    run_cmd(f'git tag -a v{target_ver} -m "{tag_msg}"')

    # Step 5: Git Push
    print("\n=== Step 5: Pushing Commit and Tag to Remote ===")
    run_cmd("git push origin main")
    run_cmd(f"git push origin v{target_ver}")

    # Step 6: Publish GitHub Release
    print("\n=== Step 6: Publishing GitHub Release ===")
    notes = args.notes
    if notes and os.path.exists(notes):
        with open(notes, "r", encoding="utf-8") as f:
            notes = f.read()

    publish_github_release(target_ver, args.title, notes)


if __name__ == "__main__":
    main()
