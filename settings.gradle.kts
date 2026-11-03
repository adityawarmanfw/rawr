pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "RawrCam"
include(":app")
// Headless GPU test APK: its preBuild needs a full release native build, so opt in with -Prawr.gpuTests=true.
if (providers.gradleProperty("rawr.gpuTests").orNull == "true") include(":gpu-tests")
