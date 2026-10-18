#!/usr/bin/env bash
set -euo pipefail

OPENCV_VERSION="4.9.0"
ZIP_NAME="opencv-${OPENCV_VERSION}-android-sdk.zip"
URL="https://github.com/opencv/opencv/releases/download/${OPENCV_VERSION}/${ZIP_NAME}"

mkdir -p third_party

if [ ! -d "third_party/opencv-android-sdk" ]; then
  echo "Downloading OpenCV Android SDK ${OPENCV_VERSION}..."
  curl -L -o "${ZIP_NAME}" "${URL}"

  echo "Extracting..."
  rm -rf third_party/opencv-tmp
  unzip -q "${ZIP_NAME}" -d third_party/opencv-tmp

  mv third_party/opencv-tmp/OpenCV-android-sdk third_party/opencv-android-sdk

  rm -rf "${ZIP_NAME}" third_party/opencv-tmp
fi

echo "OpenCV Android SDK ready at third_party/opencv-android-sdk"
