# Pool Predictor

Real-time Android overlay app for billiards path prediction.

This repository uses:

- Kotlin for Android UI, screen capture, and overlay rendering
- C++ + OpenCV for the predictor
- JNI for native communication
- GitHub Actions for building the APK

## Build with GitHub Actions

Push this repository to GitHub and open the Actions tab.

The workflow will:

1. Download OpenCV Android SDK
2. Build the Android app
3. Upload `app-debug.apk` as an artifact

## Local Codespaces Build

Optional:

```bash
INSTALL_ANDROID=1 ./create-pool-predictor.sh
```

Then:

```bash
cd PoolPredictor
./scripts/setup_opencv.sh
gradle assembleDebug
```

If Gradle is not installed, GitHub Actions can build the APK instead.
