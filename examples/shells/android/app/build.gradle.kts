plugins {
    id("com.android.application")
}

android {
    namespace = "com.lubancode.console"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.lubancode.console"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    sourceSets {
        getByName("main") {
            // 参考前端四件套(含阶段 E 的触屏外套)不复制:assets 直指
            // examples/web-console——壳与浏览器页吃同一份代码,改一处三家同步。
            // 相对路径基准是本模块目录(app/):app → android → shells → examples,
            // 三级上跳才到 examples/web-console。
            assets.srcDir("../../../web-console")
        }
    }
}
