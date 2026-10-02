// 宿主兼容入口；词表与实现归 text，旧调用仍用同一份状态。
#pragma once

#include "text/i18n.hpp"

namespace lubancode::cli {
using text::tr;
using text::trf;
using text::TrFormat;
using text::CurrentLanguage;
using text::SetLanguage;
using text::AvailableLanguages;
using text::HasLanguage;
using text::LanguageDisplayName;
using text::LoadLanguagePacksFromDir;
using text::MapLocaleToLanguage;
using text::DetectSystemLanguage;
namespace i18n_detail = text::i18n_detail;
}  // namespace lubancode::cli
