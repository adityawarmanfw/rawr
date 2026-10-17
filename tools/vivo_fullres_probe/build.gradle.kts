// Standalone probe for vivo full-size RAW sensor modes. The package name is deliberate: the vivo HAL only
// honours vivo.control.forceSensorMode / VivoCameraManager for allow-listed packages (see tools/vivo_fullres_probe/README.md).
plugins { id("com.android.application") version "8.10.1" }

android {
    namespace = "com.rawr.probe"
    compileSdk = 36
    defaultConfig {
        applicationId = (findProperty("probePackage") as String?) ?: "android.camera.cts"
        minSdk = 33
        targetSdk = 36
        versionCode = 1
        versionName = "1"
        // -PprobeProcess=<name>: run the app in a process of that name (vivo's in-process checks read the process name)
        manifestPlaceholders["probeProcess"] = (findProperty("probeProcess") as String?) ?: applicationId!!
    }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
}
