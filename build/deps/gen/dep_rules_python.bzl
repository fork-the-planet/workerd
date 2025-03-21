# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_archive")

TAG_NAME = "1.2.0"
URL = "https://github.com/bazel-contrib/rules_python/releases/download/1.2.0/rules_python-1.2.0.tar.gz"
STRIP_PREFIX = "rules_python-1.2.0"
SHA256 = "2ef40fdcd797e07f0b6abda446d1d84e2d9570d234fddf8fcd2aa262da852d1c"
TYPE = "tgz"

def dep_rules_python():
    http_archive(
        name = "rules_python",
        url = URL,
        strip_prefix = STRIP_PREFIX,
        type = TYPE,
        sha256 = SHA256,
    )
