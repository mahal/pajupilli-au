#!/bin/sh
# Build the pajupilli Audio Unit, install the app in ~/Applications and
# register it, then check it with auval.
set -e
cd "$(dirname "$0")"
xcodegen --quiet
xcodebuild -project Pajupilli.xcodeproj -scheme Pajupilli -configuration Release \
  -derivedDataPath build -allowProvisioningUpdates -quiet build
mkdir -p ~/Applications
rm -rf ~/Applications/Pajupilli.app
cp -R build/Build/Products/Release/Pajupilli.app ~/Applications/
open -g ~/Applications/Pajupilli.app   # registers the extension
sleep 3
osascript -e 'quit app "Pajupilli"' 2>/dev/null || true
auval -v aumu pjpl Pjpl
