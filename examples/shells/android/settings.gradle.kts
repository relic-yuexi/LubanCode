pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
    // AGP 版本在此统一钉死:模块 build.gradle.kts 里 id 不带版本,
    // 仓库也不随 gradle wrapper(没带),CI 用 gradle/actions/setup-gradle
    // 钉 gradle 版本(gradle 8.9)后从 google() 解析到这里。升 AGP 只动
    // 这一行,compileSdk/JDK 口径见 examples/shells/README.md。
    plugins {
        id("com.android.application") version "8.5.2"
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "lubancode-console-shell"
include(":app")
