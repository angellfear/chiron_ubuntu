# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
[CmdletBinding()]
param([string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path)

$ErrorActionPreference = 'Stop'
$git = Get-Command git -ErrorAction Stop
if (-not (Test-Path (Join-Path $Root '.git'))) { throw 'not a Git repository' }

$forbiddenTracked = @('AGENTS.md', 'private/', 'local/', 'work/', 'out/', 'sources/', 'upstream/')
$tracked = @(& $git -C $Root ls-files)
foreach ($path in $tracked) {
    foreach ($prefix in $forbiddenTracked) {
        if ($path -eq $prefix.TrimEnd('/') -or $path.StartsWith($prefix)) {
            throw "forbidden tracked path: $path"
        }
    }
    $mode = (& $git -C $Root ls-files -s -- $path) -split '\s+' | Select-Object -First 1
    if ($mode -eq '120000') { throw "symlink must not be tracked: $path" }
    # Windows checkouts carry no execute bit (core.fileMode=false), so a script
    # added from here is recorded 0644 and reaches the image non-executable.
    # That stays invisible in a workspace whose files were chmod'ed by hand and
    # surfaces only on a clean rebuild, as every service script failing at once.
    # Anything carrying a shebang is meant to be run: require 0755 for it.
    if ($mode -eq '100644') {
        $first = (& $git -C $Root show ":$path" 2>$null | Select-Object -First 1)
        if ($first -like '#!*') {
            throw "script tracked without the execute bit: $path -- git update-index --chmod=+x '$path'"
        }
    }
}

$secretPatterns = @(
    'BEGIN (RSA|EC|OPENSSH|PGP) PRIVATE KEY',
    'github_pat_[A-Za-z0-9_]{20,}',
    'gh[pousr]_[A-Za-z0-9]{20,}',
    'AKIA[0-9A-Z]{16}',
    '(password|passwd|secret|token|api[_-]?key)\s*[:=]\s*[^< ]'
)
# Files that are allowed to match the patterns above, because what they carry
# is not a secret. Keep this list exact-path and short, and say why each entry
# is on it -- an allowlist that grows by guesswork is the same as no check.
#
#   etc/default/chiron-firstboot  the appliance's first-boot account. The
#                     image ships no account at all and creates one on first
#                     boot from this file; the password in it is a published
#                     default meant to be visible and changed, not a
#                     credential to anything.
#   sbin/chiron-firstboot  reads that file. What matches here is the shell
#                     idiom for defaulting an unset variable to the empty
#                     string -- there is no value in it at all.
#   etc/sudoers.d/50-chiron-ssh  a sudo rule, not a credential. What matches
#                     is the tag sudo itself uses to say that no password is
#                     asked for -- which is also why the file holds none.
#   etc/sudoers.d/50-chiron-evidence  same sudo rule for the evidence-collector
#                     desktop shortcut, and the same NOPASSWD tag is the only
#                     thing that matches. It holds no credential either.
$secretAllowlist = @(
    'rootfs/graphical/etc/default/chiron-firstboot',
    'rootfs/graphical/usr/local/sbin/chiron-firstboot',
    'rootfs/graphical/etc/sudoers.d/50-chiron-ssh',
    'rootfs/graphical/etc/sudoers.d/50-chiron-evidence'
)

$forbiddenReference = '2_' + 'armbian_duo'
# Phrases that name the integration this port was built from. The port stands
# on its own: a public file states what the board needs and why, not where the
# statement was inherited from. The word "legacy" on its own stays allowed --
# it has genuine technical uses here (a legacy V4L2 queue, the legacy HCI form,
# the legacy file I/O helpers, CONFIG_*LEGACY*) -- so what is matched is the
# phrasing that refers to a predecessor rather than to a thing.
#
# The paths are here for the same reason and are the half that is easy to
# miss: a sentence can be reworded and still leave the directory it came from
# quoted in brackets after it. Two survived exactly that way.
$forbiddenPhrases = @(
    'legacy project', 'legacy integration', 'legacy patch set',
    'legacy board tree', 'legacy tree', 'legacy DTB', 'legacy overlay',
    'legacy fragment', 'legacy session', 'legacy copies',
    'legacy desktop package set',
    '_deploy/', 'overlay_upper', 'android_grab', 'dependencies/patches',
    'reference/_'
)
# What is scanned is what can become public: the files Git tracks, plus the
# untracked ones it would not ignore. Anything Git ignores -- private, local,
# docs, the assembled image -- cannot reach a push, so asking Git for the list
# is both more accurate than matching path patterns and far faster than walking
# the tree, which used to read every byte of a gigabyte squashfs on each commit.
$publicPaths = @(& $git -C $Root ls-files --cached --others --exclude-standard) |
    Where-Object { $_ -ne 'scripts/check-repository.ps1' }
foreach ($path in $publicPaths) {
    $full = Join-Path $Root $path
    if (-not (Test-Path -LiteralPath $full)) { continue }

    if (-not ($secretAllowlist -contains $path)) {
        foreach ($pattern in $secretPatterns) {
            if (Select-String -LiteralPath $full -Pattern $pattern -Quiet -ErrorAction SilentlyContinue) {
                throw "possible secret in public file: $path"
            }
        }
    }

    if (Select-String -LiteralPath $full -SimpleMatch $forbiddenReference -Quiet -ErrorAction SilentlyContinue) {
        throw "predecessor reference in public file: $path"
    }
    foreach ($phrase in $forbiddenPhrases) {
        if (Select-String -LiteralPath $full -SimpleMatch $phrase -Quiet -ErrorAction SilentlyContinue) {
            throw "predecessor reference ($phrase) in public file: $path"
        }
    }
}

& $git -C $Root diff --check
if ($LASTEXITCODE -ne 0) { throw 'working-tree whitespace check failed' }
& $git -C $Root diff --cached --check
if ($LASTEXITCODE -ne 0) { throw 'staged whitespace check failed' }
Write-Output 'repository policy check passed'
