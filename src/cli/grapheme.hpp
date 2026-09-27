// 宿主兼容入口；Unicode 字素与列宽表归 text，不依赖编辑器。
#pragma once

#include "text/grapheme.hpp"

namespace lubancode::cli {
using text::GraphemeClass;
using text::GraphemeCluster;
using text::Utf8Grapheme;
using text::ClassifyCodepoint;
using text::GraphemeCodepointWidth;
using text::SplitGraphemes;
using text::SplitUtf8Graphemes;
using text::ClusterDisplayWidth;
using text::PrevGraphemeBoundary;
using text::NextGraphemeBoundary;
}  // namespace lubancode::cli
