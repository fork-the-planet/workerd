# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_file")

TAG_NAME = "v8.2.0"
URL = "https://github.com/bazelbuild/buildtools/releases/download/v8.2.0/buildifier-darwin-arm64"
SHA256 = "e08381a3ed1d59c0a17d1cee1d4e7684c6ce1fc3b5cfa1bd92a5fe978b38b47d"

def dep_buildifier_darwin_arm64():
    http_file(
        name = "buildifier-darwin-arm64",
        url = URL,
        executable = True,
        sha256 = SHA256,
    )
