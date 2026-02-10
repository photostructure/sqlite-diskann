#!/usr/bin/env bash
# Update vendored SQLite to latest version
#
# Usage: ./scripts/update-sqlite.sh [version]
#   If no version specified, downloads latest from sqlite.org

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENDOR_DIR="$SCRIPT_DIR/../vendor/sqlite"

# Get latest version or use provided version
if [ $# -eq 0 ]; then
  echo "Fetching latest SQLite version..."
  # Parse download page for latest amalgamation version
  LATEST_URL=$(curl -s https://www.sqlite.org/download.html | grep -o 'sqlite-amalgamation-[0-9]*\.zip' | head -1)
  if [ -z "$LATEST_URL" ]; then
    echo "Error: Could not determine latest SQLite version"
    exit 1
  fi
  VERSION=$(echo "$LATEST_URL" | grep -o '[0-9]*')
  YEAR=$(echo "$LATEST_URL" | sed 's/.*href="\/\([0-9]*\)\/.*/\1/' || echo "2026")
  # Extract year from download page or default to current year
  YEAR=$(curl -s https://www.sqlite.org/download.html | grep -o '202[0-9]/sqlite-amalgamation' | head -1 | cut -d'/' -f1 || echo "2026")
else
  VERSION=$1
  # Guess year based on version (SQLite versioning: MAJOR.MINOR.PATCH -> 3MMMMPPP)
  YEAR="2026"  # Update this logic if needed
fi

DOWNLOAD_URL="https://www.sqlite.org/$YEAR/sqlite-amalgamation-$VERSION.zip"

echo "Downloading SQLite amalgamation version $VERSION from $DOWNLOAD_URL..."

# Create temp directory
TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

# Download and extract
cd "$TMP_DIR"
curl -sL "$DOWNLOAD_URL" -o sqlite.zip
unzip -q sqlite.zip

# Find extracted directory
EXTRACTED_DIR=$(find . -type d -name "sqlite-amalgamation-*" | head -1)
if [ -z "$EXTRACTED_DIR" ]; then
  echo "Error: Could not find extracted SQLite directory"
  exit 1
fi

# Backup old version
if [ -d "$VENDOR_DIR" ]; then
  echo "Backing up old SQLite files..."
  OLD_VERSION=$(grep "SQLITE_VERSION " "$VENDOR_DIR/sqlite3.h" 2>/dev/null | head -1 | grep -o '"[^"]*"' || echo "unknown")
  echo "  Old version: $OLD_VERSION"
  mkdir -p "$VENDOR_DIR.backup"
  cp "$VENDOR_DIR"/*.{c,h} "$VENDOR_DIR.backup/" 2>/dev/null || true
fi

# Copy new files
echo "Installing new SQLite files..."
mkdir -p "$VENDOR_DIR"
cp "$EXTRACTED_DIR"/sqlite3.c "$VENDOR_DIR/"
cp "$EXTRACTED_DIR"/sqlite3.h "$VENDOR_DIR/"
cp "$EXTRACTED_DIR"/sqlite3ext.h "$VENDOR_DIR/"

# Verify version
NEW_VERSION=$(grep "SQLITE_VERSION " "$VENDOR_DIR/sqlite3.h" | head -1 | grep -o '"[^"]*"')
echo "✓ Updated to SQLite $NEW_VERSION"

# File sizes
echo ""
echo "File sizes:"
ls -lh "$VENDOR_DIR"/*.{c,h} | awk '{print "  " $9 ": " $5}'

echo ""
echo "Done! SQLite updated to version $NEW_VERSION"
echo ""
echo "Next steps:"
echo "  1. Test the build: make clean && make test"
echo "  2. Commit the changes: git add vendor/sqlite && git commit"
