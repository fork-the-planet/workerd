# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_file")

TAG_NAME = "v7.3.1"
URL = "https://github.com/bazelbuild/buildtools/releases/download/v7.3.1/buildifier-windows-amd64.exe"
SHA256 = "370cd576075ad29930a82f5de132f1a1de4084c784a82514bd4da80c85acf4a8"

def dep_buildifier_windows_amd64():
    http_file(
        name = "buildifier-windows-amd64",
        url = URL,
        executable = True,
        sha256 = SHA256,
    )
