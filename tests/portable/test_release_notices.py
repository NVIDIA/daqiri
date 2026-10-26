# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_repository_notice_covers_vendored_components():
    notice = (ROOT / "THIRD-PARTY-NOTICES").read_text()

    for component in ("yaml-cpp", "spdlog", "fmt"):
        assert f"Component: {component}" in notice

    assert (ROOT / "third_party/yaml-cpp/LICENSE").read_text().strip() in notice
    assert (ROOT / "third_party/spdlog/LICENSE").read_text().strip() in notice
    assert (
        ROOT / "third_party/spdlog/include/spdlog/fmt/bundled/fmt.license.rst"
    ).read_text().strip() in notice


import pytest

from scripts.release.collect_sources import (
    find_notice_files,
    find_package_notice_files,
    render_third_party_notices,
)


def test_notice_renderer_sorts_packages_and_documents(tmp_path):
    project_notice = "THIRD-PARTY NOTICES\n\nComponent: yaml-cpp\n"
    packages = [
        (
            ["zlib1g", "1.3", "zlib", "1.3"],
            [("NOTICE", "zlib notice\n"), ("copyright", "zlib copyright\n")],
        ),
        (
            ["ca-certificates", "20240203", "ca-certificates", "20240203"],
            [("copyright", "certificate copyright\n")],
        ),
    ]

    rendered = render_third_party_notices(project_notice, packages)

    assert rendered.startswith(project_notice)
    assert rendered.index("Package: ca-certificates") < rendered.index("Package: zlib1g")
    zlib_section = rendered[rendered.index("Package: zlib1g") :]
    assert zlib_section.index("Document: copyright") < zlib_section.index(
        "Document: NOTICE"
    )


def test_find_notice_files_uses_all_supported_names_and_requires_one(tmp_path):
    doc = tmp_path / "doc"
    (doc / "nested").mkdir(parents=True)
    (doc / "copyright").write_text("copyright\n")
    (doc / "nested/LICENSE.txt").write_text("license\n")
    (doc / "changelog").write_text("not attribution\n")

    assert [path.relative_to(doc).as_posix() for path in find_notice_files(doc)] == [
        "copyright",
        "nested/LICENSE.txt",
    ]

    with pytest.raises(RuntimeError, match="No attribution material"):
        find_notice_files(tmp_path / "missing")

    with pytest.raises(RuntimeError, match="Project attribution"):
        render_third_party_notices("", [])


def test_proprietary_mft_notices_are_retained_but_excluded_from_oss_attribution(
    tmp_path,
):
    mft_doc = tmp_path / "mft"
    mft_doc.mkdir()
    eula = mft_doc / "EULA"
    eula.write_text("MFT terms\n")

    assert find_package_notice_files("mft", mft_doc) == [eula]


def test_unclassified_packages_fail_closed(tmp_path):
    missing_doc = tmp_path / "missing"

    with pytest.raises(RuntimeError, match="No attribution material"):
        find_package_notice_files("unknown-oss-source", missing_doc)
