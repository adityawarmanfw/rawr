import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// The native asset is the canonical input; package it without a second tracked copy.
abstract class FilmAssetsTask : DefaultTask() {
    @get:InputFile abstract val sourceFile: RegularFileProperty
    @get:OutputDirectory abstract val outputDirectory: DirectoryProperty

    @TaskAction fun copyAsset() {
        val target = outputDirectory.file("spektrafilm/SpektraHanatos2025Spectra.f32").get().asFile
        target.parentFile.mkdirs()
        sourceFile.get().asFile.copyTo(target, overwrite = true)
    }
}
val filmAssets = tasks.register<FilmAssetsTask>("generateFilmAssets") {
    sourceFile.set(rootProject.layout.projectDirectory.file("native/spektrafilm/assets/SpektraHanatos2025Spectra.f32"))
    outputDirectory.set(layout.buildDirectory.dir("generated/filmAssets"))
}
androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(filmAssets, FilmAssetsTask::outputDirectory)
    }
}

android {
    namespace = "com.rawr.camera"
    compileSdk = 36
    buildToolsVersion = "36.0.0"

    defaultConfig {
        applicationId = "com.rawr.camera"
        minSdk = 33
        ndk { abiFilters += "arm64-v8a" }
        targetSdk = 36
        versionCode = 21
        versionName = "0.1.0"
    }

    ndkVersion = "29.0.14206865"

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    sourceSets {
        // Register Kotlin directories in the Android source model used by AGP and IDEs.
        getByName("main").java.srcDir("src/main/kotlin")
        getByName("debug").java.srcDir("src/debug/kotlin")
        getByName("test").java.srcDir("src/test/kotlin")
        // Preview/test fixtures belong in debug builds, never in the release APK. Release unit
        // tests compile the same sources directly because they do not inherit the debug variant.
        getByName("debug").java.srcDir("src/fixtures/kotlin")
        getByName("testRelease").java.srcDir("src/fixtures/kotlin")
    }

    // Lowest precedence: the git-ignored signing/release.properties (keystore path relative to the repo root).
    val localSigning = Properties().apply {
        rootProject.file("signing/release.properties").takeIf { it.isFile }?.reader()?.use { load(it) }
    }
    val releaseSigning = listOf("STORE_FILE", "STORE_PASSWORD", "KEY_ALIAS", "KEY_PASSWORD").associateWith { key ->
        providers.gradleProperty("rawr.signing.$key").orElse(providers.environmentVariable("RAWR_SIGNING_$key")).orNull
            ?: localSigning.getProperty("rawr.signing.$key")
    }
    require(releaseSigning.values.all { it == null } || releaseSigning.values.all { !it.isNullOrBlank() }) {
        "Provide all four RAWR_SIGNING_* values, or none for an unsigned release."
    }
    if (releaseSigning.values.all { it != null }) {
        signingConfigs.create("release") {
            storeFile = rootProject.file(requireNotNull(releaseSigning["STORE_FILE"]))
            storePassword = releaseSigning["STORE_PASSWORD"]
            keyAlias = releaseSigning["KEY_ALIAS"]
            keyPassword = releaseSigning["KEY_PASSWORD"]
        }
    }

    buildTypes {
        debug {
            applicationIdSuffix = ".debug"
            resValue("string", "app_name", "Rawr Debug")
        }
        release {
            isDebuggable = false
            isMinifyEnabled = true
            isShrinkResources = true
            ndk.debugSymbolLevel = "SYMBOL_TABLE"
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.findByName("release")
        }
    }

    packaging {
        // Keep dependency license metadata in the APK (Apache-2.0 requires the
        // license and notices be redistributed). pickFirsts keeps one copy to
        // avoid duplicate-resource merge failures, rather than dropping them.
        resources.pickFirsts += "/META-INF/{AL2.0,LGPL2.1}"
        // libadrenotools locates its hook .so files through ApplicationInfo.nativeLibraryDir.
        // They must be extracted there rather than loaded directly from the APK.
        jniLibs.useLegacyPackaging = true
    }
    // Film lookup tables are opened with AAssetManager.openFd and must remain uncompressed.
    androidResources { noCompress += "f32" }

    lint {
        abortOnError = true
        warningsAsErrors = true
        checkReleaseBuilds = true
        // This app intentionally supports arm64 phones; dependency upgrades are reviewed separately.
        disable += setOf("ChromeOsAbiSupport", "GradleDependency")
    }
}

kotlin {
    jvmToolchain(17)
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
        allWarningsAsErrors.set(true)
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2025.05.01"))
    implementation("androidx.activity:activity-compose:1.10.1")
    implementation("androidx.core:core-ktx:1.16.0")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.9.0")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.9.0")
    implementation("androidx.datastore:datastore-preferences:1.2.1")
    implementation("androidx.media3:media3-muxer:1.10.0")
    implementation("sh.calvin.reorderable:reorderable:3.0.0")

    debugImplementation("androidx.compose.ui:ui-tooling")
    testImplementation(kotlin("test"))
    // Exercise portable capture/renderer recipes on the JVM rather than Android stub methods.
    testImplementation("org.json:json:20260719")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.10.2")
}

// Keep Java compilation as strict as Kotlin compilation. There are currently no Java sources,
// but this protects future Android/application adapters from introducing unchecked warnings.
tasks.withType<org.gradle.api.tasks.compile.JavaCompile>().configureEach {
    // AGP's generated BuildConfig starts with a file-level documentation comment,
    // which javac 23+ reports as dangling. Keep user Java strict while excluding that
    // generated-source-only diagnostic only on compilers that recognize the lint key.
    val allLint = if (JavaVersion.current().isCompatibleWith(JavaVersion.VERSION_23)) {
        "-Xlint:all,-dangling-doc-comments"
    } else {
        "-Xlint:all"
    }
    options.compilerArgs.addAll(listOf(allLint, "-Werror"))
}

// Kotlin LSP's Android importer reads generated source providers and javac arguments.
// Its prepare task currently misses BuildConfig and nested test preparation (upstream #253).
// Wire the producers into that task only during language-server import; ordinary Gradle
// builds retain their task graph, and the editor keeps both main and test source sets.
if (providers.systemProperty("com.jetbrains.ls.imports.gradle").orNull == "true") {
    tasks.matching { it.name == "prepareKotlinIdeaImport" }.configureEach {
        dependsOn(tasks.matching {
            (it.name.startsWith("generate") && it.name.endsWith("BuildConfig")) ||
                it.name.startsWith("javaPreCompile") ||
                (it.name.startsWith("process") && it.name.endsWith("Resources"))
        })
    }
}
