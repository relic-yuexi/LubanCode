import java.io.FileInputStream
import java.util.Properties

plugins {
    id("com.android.application")
}

// release 签名接线(打包发布账 §三):密钥走 keystore.properties,
// 绝不进仓(.gitignore 挡)。keytool 生成命令与文件口径见
// examples/shells/README.md《Android 签名》。文件不在时 release 退回
// debug 签名并打横幅——参考壳能装能验;发布壳必须配真密钥。
val keystoreProperties = Properties()
val keystorePropertiesFile = rootProject.file("keystore.properties")
if (keystorePropertiesFile.exists()) {
    keystoreProperties.load(FileInputStream(keystorePropertiesFile))
} else {
    logger.lifecycle(
        "keystore.properties 不在(examples/shells/android/):assembleRelease " +
        "将退回 debug 签名,仅供本机验装,不可发布"
    )
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

    signingConfigs {
        create("release") {
            if (keystorePropertiesFile.exists()) {
                storeFile = rootProject.file(keystoreProperties.getProperty("store.file"))
                storePassword = keystoreProperties.getProperty("store.password")
                keyAlias = keystoreProperties.getProperty("key.alias")
                keyPassword = keystoreProperties.getProperty("key.password")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = if (keystorePropertiesFile.exists()) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
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
