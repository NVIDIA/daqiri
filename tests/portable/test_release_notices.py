# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from importlib.metadata import Distribution, PackagePath
from pathlib import Path
from types import SimpleNamespace


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
    copy_python_notices,
    find_notice_files,
    find_package_notice_files,
    render_third_party_notices,
    select_python_artifact,
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


def test_python_notices_preserve_same_named_files_in_different_directories(tmp_path):
    installed = tmp_path / "site-packages"
    metadata = installed / "example-1.0.dist-info"
    metadata.mkdir(parents=True)
    documents = {
        "example/vendor/first/LICENSE": "First dependency license\n",
        "example/vendor/second/LICENSE": "Second dependency license\n",
        "example-1.0.dist-info/licenses/NOTICE": "Package notice\n",
        "example/module.py": "# Not a notice\n",
    }
    for relative, content in documents.items():
        origin = installed / relative
        origin.parent.mkdir(parents=True, exist_ok=True)
        origin.write_text(content)
    (metadata / "RECORD").write_text(
        "".join(f"{relative},,\n" for relative in documents)
    )
    destination = tmp_path / "notices/python/example"

    copy_python_notices(Distribution.at(metadata), destination)

    retained = {
        path.relative_to(destination).as_posix(): path.read_text()
        for path in destination.rglob("*")
        if path.is_file()
    }
    assert retained == {
        relative: content
        for relative, content in documents.items()
        if relative != "example/module.py"
    }


@pytest.mark.parametrize("entry", ["../LICENSE", "/tmp/LICENSE"])
def test_python_notices_reject_paths_outside_destination(tmp_path, entry):
    dist = SimpleNamespace(files=[PackagePath(entry)])

    with pytest.raises(RuntimeError, match="Invalid Python notice path"):
        copy_python_notices(dist, tmp_path / "notices")


def test_python_source_collection_does_not_substitute_unknown_binary_wheels():
    wheel = {"packagetype": "bdist_wheel", "filename": "unknown-py3-none-manylinux_2_25_aarch64.whl"}
    with pytest.raises(RuntimeError, match="Expected one matching Python artifact"):
        select_python_artifact("unknown-oss-source", [wheel], "aarch64")


def test_cupti_binary_retention_requires_exact_native_architecture():
    arm = {"packagetype": "bdist_wheel", "filename": "nvidia_cuda_cupti-13.4.92-py3-none-manylinux_2_25_aarch64.whl"}
    amd = {"packagetype": "bdist_wheel", "filename": "nvidia_cuda_cupti-13.4.92-py3-none-manylinux_2_25_x86_64.whl"}
    assert select_python_artifact("nvidia-cuda-cupti", [arm, amd], "aarch64") == arm
    with pytest.raises(RuntimeError, match="Expected one matching Python artifact"):
        select_python_artifact("nvidia-cuda-cupti", [amd], "aarch64")
    with pytest.raises(RuntimeError, match="Expected one matching Python artifact"):
        select_python_artifact("nvidia-cuda-cupti", [arm, arm], "aarch64")
