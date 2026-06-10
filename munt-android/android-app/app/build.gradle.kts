plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.example.muntforandroid"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.example.muntforandroid"
        minSdk = 29          // Android 10 (LG Velvet)
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            // 릴리즈: arm64-v8a 전용 (LG Velvet 타겟, 빌드 속도 최적화)
            // armeabi-v7a는 디버그 빌드에서만 포함
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++11"
                arguments += "-DANDROID_STL=c++_shared"
            }
        }
    }

    signingConfigs {
        create("release") {
            storeFile = file("C:\\Users\\tiram\\munt-android-key.jks")
            storePassword = "munt2024"
            keyAlias = "munt_android"
            keyPassword = "munt2024"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        viewBinding = false
    }
}

dependencies {
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.constraintlayout)
    implementation(libs.androidx.core.ktx)
    implementation(libs.material)
    implementation("com.github.mik3y:usb-serial-for-android:3.7.3")
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
}
