#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Inventory the SDK dependency stage and retain matching added OSS sources.

Run only in the release-sources Docker stage. Dockerfile_release supplies all
source locations and checksums explicitly; this helper has no release-setting
defaults and does not install binary packages or configure the DAQIRI build.
Failure to find exact sources is fatal.
"""

import argparse
import csv
import ctypes
import hashlib
import importlib.metadata
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile


QUERY = "${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\n"

NOTICE_WORDS = ("license", "copying", "eula", "notice")
NON_OSS_PACKAGE_SOURCES = frozenset({"mft"})


def find_notice_files(doc, required=True):
    files = []
    copyright_file = doc / "copyright"
    if copyright_file.is_file():
        files.append(copyright_file)
    if doc.is_dir():
        files.extend(
            path
            for path in doc.rglob("*")
            if path.is_file()
            and path != copyright_file
            and any(word in path.name.lower() for word in NOTICE_WORDS)
        )
    files.sort(key=lambda path: (path != copyright_file, path.relative_to(doc).as_posix()))
    if not files and required:
        raise RuntimeError(f"No attribution material found in {doc}")
    return files


def find_package_notice_files(source, doc):
    return find_notice_files(doc, required=source not in NON_OSS_PACKAGE_SOURCES)


def render_third_party_notices(project_notice, packages):
    if not project_notice.strip():
        raise RuntimeError("Project attribution file is empty")
    sections = [project_notice.rstrip(), "", "ADDED RELEASE-CONTAINER PACKAGES", ""]
    for row, documents in sorted(packages, key=lambda item: item[0][0]):
        package, version, source, source_version = row
        sections.extend(
            [
                "=" * 72,
                f"Package: {package}",
                f"Version: {version}",
                f"Source package: {source}",
                f"Source version: {source_version}",
                "",
            ]
        )
        for name, content in sorted(
            documents, key=lambda item: (item[0] != "copyright", item[0])
        ):
            sections.extend(
                [
                    f"Document: {name}",
                    "-" * 72,
                    content.rstrip(),
                    "",
                ]
            )
    return "\n".join(sections).rstrip() + "\n"


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


def packages(text):
    return {row[0]: row for row in csv.reader(text.splitlines(), delimiter="\t")}


def enable_ubuntu_sources():
    """Mirror Ubuntu binary repositories, including security and updates."""
    entries = []
    for path in Path("/etc/apt/sources.list.d").glob("*.sources"):
        for block in path.read_text().split("\n\n"):
            if "ubuntu.com" in block and re.search(r"^Types:.*\bdeb\b", block, re.M):
                entries.append(re.sub(r"^Types:.*$", "Types: deb-src", block, flags=re.M))
    if entries:
        Path("/etc/apt/sources.list.d/daqiri-source.sources").write_text(
            "\n\n".join(entries) + "\n"
        )
    lines = []
    for path in [Path("/etc/apt/sources.list"), *Path("/etc/apt/sources.list.d").glob("*.list")]:
        if path.exists():
            lines.extend(
                line.replace("deb ", "deb-src ", 1)
                for line in path.read_text().splitlines()
                if line.startswith("deb ") and "ubuntu.com" in line
            )
    if lines:
        Path("/etc/apt/sources.list.d/daqiri-source.list").write_text("\n".join(lines) + "\n")
    if not entries and not lines:
        raise RuntimeError("No Ubuntu repositories found for matching source retrieval")
    run("apt-get", "update")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vendor-url", required=True)
    parser.add_argument("--vendor-sha256", required=True)
    parser.add_argument("--component-root", type=Path, required=True)
    parser.add_argument("--project-notices", type=Path, required=True)
    args = parser.parse_args()
    output = args.output
    yaml_cmake = (args.component_root / "third_party/yaml-cpp/CMakeLists.txt").read_text()
    spdlog_header = (args.component_root / "third_party/spdlog/include/spdlog/version.h").read_text()
    yaml_version = re.search(r"project\(YAML_CPP VERSION ([0-9.]+)", yaml_cmake)[1]
    spdlog_version = ".".join(
        re.search(rf"#define SPDLOG_VER_{part}\s+(\d+)", spdlog_header)[1]
        for part in ("MAJOR", "MINOR", "PATCH")
    )
    with (output / "components.tsv").open("w") as stream:
        writer = csv.writer(stream, delimiter="\t")
        writer.writerow(["component", "version", "source_artifact", "license_notice"])
        for name, version, notice in (
            ("daqiri", (args.component_root / "VERSION").read_text().strip(), "daqiri/LICENSE"),
            ("yaml-cpp", yaml_version, "daqiri/third_party/yaml-cpp/LICENSE"),
            ("spdlog", spdlog_version, "daqiri/third_party/spdlog/LICENSE"),
        ):
            writer.writerow([name, version, "daqiri-source.tar.gz", notice])
    current_text = run("dpkg-query", "-W", f"-f={QUERY}", capture_output=True).stdout
    (output / "packages.tsv").write_text(current_text)
    baseline = packages((output / "base-packages.tsv").read_text())
    current = packages(current_text)
    added = {name: row for name, row in current.items() if baseline.get(name) != row}
    # Reinstalled provider packages can have the same version as the base but
    # repair stripped NGC headers/symlinks. Include their sources regardless.
    for name, row in current.items():
        if row[2] in {"rdma-core", "mlnx-tools", "mlnx-ofed-kernel", "mft"}:
            added[name] = row
    source_dir = output / "sources"
    source_dir.mkdir()
    notices = output / "notices"
    notices.mkdir()
    sources = sorted({(row[2], row[3]) for row in added.values()})
    enable_ubuntu_sources()

    # The vendor does not publish a deb-src index. Its matching source bundle
    # contains the original source archives, including their packaging files.
    bundle = Path("/tmp/daqiri-ofed-source.tgz")
    run("curl", "-fsSL", args.vendor_url, "-o", str(bundle))
    digest = hashlib.sha256(bundle.read_bytes()).hexdigest()
    if digest != args.vendor_sha256:
        raise RuntimeError(f"Vendor source checksum mismatch: {digest}")
    provenance = []
    with tarfile.open(bundle) as archive:
        for source, version in sources:
            if source == "mft":
                # A proprietary Debian package can still bundle OSS outside
                # dpkg's dependency graph. Do not claim complete source coverage.
                library = Path("/usr/lib64/mft/ext_libs/libcrypto.so.3")
                crypto = ctypes.CDLL(str(library))
                crypto.OpenSSL_version.restype = ctypes.c_char_p
                review = {
                    "source": source, "version": version,
                    "kind": "proprietary package with bundled OSS",
                    "review_required": True,
                    "bundled_library": str(library),
                    "bundled_version": crypto.OpenSSL_version(0).decode(),
                    "reason": "Matching vendor sources and notices for bundled OSS are not supplied by the Debian package.",
                }
                provenance.append(review)
                (output / "source-review.json").write_text(json.dumps(review, indent=2) + "\n")
                continue
            destination = source_dir / source
            destination.mkdir()
            if source in {"rdma-core", "mlnx-tools", "mlnx-ofed-kernel"}:
                upstream = version.rsplit("-", 1)[0]
                filename = f"{source}_{upstream}.orig.tar.gz"
                members = [m for m in archive.getmembers() if m.name.endswith("/" + filename)]
                if len(members) != 1 or not members[0].isfile():
                    raise RuntimeError(f"Missing exact vendor source: {source}={version}")
                with archive.extractfile(members[0]) as src, (destination / filename).open("wb") as dst:
                    shutil.copyfileobj(src, dst)
                provenance.append({"source": source, "version": version, "url": args.vendor_url,
                                   "bundle_sha256": digest, "member": members[0].name})
            else:
                run("apt-get", "source", "--download-only", f"{source}={version}", cwd=destination)
                provenance.append({"source": source, "version": version, "kind": "Ubuntu source package"})

    package_notices = []
    rows = []
    for name, row in sorted(added.items()):
        package = name.split(":", 1)[0]
        doc = Path("/usr/share/doc") / package
        license_names = []
        documents = []
        notice_files = find_package_notice_files(row[2], doc)
        for path in notice_files:
            rel = path.relative_to(doc).as_posix()
            text = path.read_text(errors="replace")
            documents.append((rel, text))
            if path == doc / "copyright":
                shutil.copyfile(path, notices / f"{package}.copyright")
                license_names = sorted(set(re.findall(r"^License:\s*(.+)$", text, re.M)))
            else:
                target = notices / package / path.relative_to(doc)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
        if notice_files and row[2] not in NON_OSS_PACKAGE_SOURCES:
            package_notices.append((row, documents))
        rows.append([*row, "; ".join(license_names) or "See package notices and source archive"])
    with (output / "added-packages.tsv").open("w") as stream:
        writer = csv.writer(stream, delimiter="\t")
        writer.writerow(["package", "version", "source", "source_version", "license_notice"])
        writer.writerows(rows)
    shutil.copytree("/usr/share/common-licenses", notices / "common-licenses")
    (output / "source-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    python_packages = sorted(
        ({"name": dist.metadata["Name"], "version": dist.version} for dist in importlib.metadata.distributions()),
        key=lambda row: (row["name"] or "").lower(),
    )
    (output / "python-packages.json").write_text(json.dumps(python_packages, indent=2) + "\n")
    (output / "THIRD-PARTY-NOTICES").write_text(
        render_third_party_notices(args.project_notices.read_text(), package_notices)
    )
    checksums = []
    for path in sorted(output.rglob("*")):
        if path.is_file():
            checksums.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(output)}")
    (output / "SHA256SUMS").write_text("\n".join(checksums) + "\n")


if __name__ == "__main__":
    main()
