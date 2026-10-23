#!/bin/sh
set -e
cd "$(dirname "$0")/.."
rm -rf dist/cxps5 && mkdir -p dist/cxps5
cp -r src ui tools scripts third_party .github Makefile README.md config.ini.example dist/cxps5/
(cd dist && zip -qr cxps5-0.1.0-src.zip cxps5)
echo dist/cxps5-0.1.0-src.zip
