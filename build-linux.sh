#!/usr/bin/env bash

# Simple Junocash Linux Build Script
# Quick compilation for local development

set -e

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}Building Junocash for Linux...${NC}\n"

# Get number of CPU cores
JOBS=${1:-$(nproc)}

echo -e "${GREEN}Using $JOBS parallel jobs${NC}\n"

# Build dependencies
echo -e "${GREEN}Building dependencies...${NC}"
cd depends
make -j"$JOBS"
cd ..

# Clean previous build
echo -e "\n${GREEN}Cleaning previous build...${NC}"
./zcutil/clean.sh || true

# Generate configure script
echo -e "\n${GREEN}Generating configure script...${NC}"
./autogen.sh

# Configure
echo -e "\n${GREEN}Configuring...${NC}"
CONFIG_SITE="$PWD/depends/x86_64-pc-linux-gnu/share/config.site" ./configure --disable-tests

# Build
echo -e "\n${GREEN}Building Junocash...${NC}"
make -j"$JOBS"

echo -e "\n${GREEN}Build complete!${NC}"
echo -e "Binaries are in:"
echo -e "  ./src/junocashd"
echo -e "  ./src/junocash-cli"
echo -e "  ./src/junocash-tx"
