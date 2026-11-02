plugins { id("com.android.application") }

val productionJni = tasks.register<Sync>("stageProductionJni") {
    dependsOn(":app:mergeBenchmarkNativeLibs")
    from(project(":app").layout.buildDirectory.dir("intermediates/merged_native_libs/benchmark/mergeBenchmarkNativeLibs/out/lib"))
    into(layout.buildDirectory.dir("generated/productionJni"))
}
tasks.matching { it.name == "preBuild" }.configureEach {
    dependsOn(productionJni, ":app:generateFilmAssets")
}

android {
    namespace = "com.rawr.gputest"
    compileSdk = 36
    buildToolsVersion = "36.0.0"
    ndkVersion = "29.0.14206865"
    defaultConfig {
        applicationId = "com.rawr.gputest"
        minSdk = 33
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk { abiFilters += "arm64-v8a" }
    }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "3.31.6" } }
    buildTypes {
        debug {
            externalNativeBuild { cmake { arguments += "-DCMAKE_BUILD_TYPE=Release" } }
        }
    }
    packaging { jniLibs.useLegacyPackaging = true }
    sourceSets.getByName("main") {
        jniLibs.srcDir(layout.buildDirectory.dir("generated/productionJni"))
        assets.srcDir(project(":app").file("src/main/assets"))
        assets.srcDir(project(":app").layout.buildDirectory.dir("generated/filmAssets"))
    }
}

dependencies {
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
}
