# v1 backstop compilers

These are the four cross_cc v1 release packages, kept in ordinary Git as a
last-resort bootstrap compiler. Prefer an explicitly supplied/downloaded
`cross_cc` or an installed `cross_cc`; the matching host package here is the
fallback, not a required compiler version for everyday builds.

Extract the package for the **host running the build**, not the target being
compiled. Keep its executable and `lib/` resources together. The executable is
`cross_cc` (`cross_cc.exe` on Windows) inside `cross_cc-<host>/`.
The Intel Mac ZIP uses ZIP BZip2 compression for the compiler entry; extract it
with BSD tar (macOS's built-in `tar`), rather than a Deflate-only ZIP reader.

Source: <https://github.com/SamHSmith/cross_cc/releases/tag/v1>.
Some ZIPs are recompressed so each fits GitHub's 100 MiB ordinary-Git file limit.
Every entry's content, path, timestamp and Unix mode was checked against the
published package. No binaries or runtime sources were rebuilt or changed.
`SHA256SUMS` checks the ZIPs stored here; recompressed ZIP hashes differ from
the original published hashes below:

```text
8039bf579bebac46e12897e2c34933aac6d8d05c621d712cf78a990301d6f127  cross_cc-v1-darwin-arm64.zip
a7ff7f1131970b8d1d29e14d17b46d1d81e2a7228c97ddd4b24891b3c2bef638  cross_cc-v1-darwin-amd64.zip
bcb130ea5d43e2ffe77a6b58de6e43e49577bb745ce720e45783a585e7a9c573  cross_cc-v1-linux-amd64.zip
3460578a636425d8954f0c368f330ece3fff07dbab07aa47043e7770c9fa7e46  cross_cc-v1-windows-amd64.zip
```

Dependency license notices are included in each package. These backstops do not
replace the target LLVM static SDKs and other inputs needed to build cross_cc.
