plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.example.poolpredictor"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    defaultConfig {
        applicationId = "com.example.poolpredictor"
        minSdk = 24
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        externalNativeBuild {
            cmake {
                // FIX: Use += to add to the MutableList instead of =
                cppFlags += "-std=c++17"
                cppFlags += "-fexceptions"
                cppFlags += "-frtti"
                arguments += "-DOpenCV_DIR=${rootDir}/third_party/opencv-android-sdk/sdk/native/jni"
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets {
        getByName("main") {
            jniLibs.srcDir("src/main/jniLibs")
            jniLibs.srcDir("${rootDir}/third_party/opencv-android-sdk/sdk/native/libs")
        }
    }

    packaging {
        jniLibs.useLegacyPackaging = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.9.0")
}
