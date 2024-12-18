# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_archive")

TAG_NAME = "0.8.3"
URL = "https://github.com/astral-sh/ruff/releases/download/0.8.3/ruff-x86_64-unknown-linux-gnu.tar.gz"
STRIP_PREFIX = "ruff-x86_64-unknown-linux-gnu"
SHA256 = "19eb5ce0cf1151d5ef58372633c342e1c000f4bfa8877b3d4ba0f0191b4bf839"
TYPE = "tgz"

def dep_ruff_linux_amd64():
    http_archive(
        name = "ruff-linux-amd64",
        url = URL,
        strip_prefix = STRIP_PREFIX,
        type = TYPE,
        sha256 = SHA256,
        build_file_content = "filegroup(name='file', srcs=glob(['**']))",
    )
