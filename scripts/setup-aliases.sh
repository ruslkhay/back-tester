#!/bin/bash
# Setup aliases and functions for common development tasks
# Source this file: source scripts/setup-aliases.sh

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ═══════════════════════════════════════════════════════════════════════════════
# Build Commands
# ═══════════════════════════════════════════════════════════════════════════════

# cbb: cmake build abbreviated - build the project with flexible parameters
alias cbb='cmake --build build -j'
alias tests='cmake --build build --target back-tester-tests && build/bin/test/back-tester-tests'
alias bench='cmake --build build --target back-tester-bench && build/bin/bench/back-tester-bench'