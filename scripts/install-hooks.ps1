# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
[CmdletBinding()]
param([string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path)
$ErrorActionPreference = 'Stop'
git -C $Root config core.hooksPath githooks
if ($LASTEXITCODE -ne 0) { throw 'failed to configure Git hooks' }
Write-Output 'Git hooks enabled'
