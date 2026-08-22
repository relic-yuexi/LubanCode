#!/usr/bin/env bash
# 检查 docs/interview 目录、仓库内链接与官方配置 Skill 路由。

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
DOCS_ROOT="$REPO_ROOT/docs"
INTERVIEW_ROOT="$REPO_ROOT/interview"
CATALOG="$DOCS_ROOT/catalog.txt"
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/lubancode-docs-check.XXXXXX")
trap 'rm -rf "$WORK_DIR"' EXIT HUP INT TERM

ERRORS="$WORK_DIR/errors"
: >"$ERRORS"
checked_links=0

report_error() {
    printf '%s\n' "$1" >>"$ERRORS"
}

catalog_value() {
    awk -v kind="$1" '$1 == kind { print $2 }' "$CATALOG"
}

write_relative_markdown() {
    root=$1
    output=$2
    if [ ! -d "$root" ]; then
        : >"$output"
        return
    fi
    find "$root" -type f -name '*.md' -print | while IFS= read -r path; do
        printf '%s\n' "${path#"$root"/}"
    done | LC_ALL=C sort -u >"$output"
}

report_set_difference() {
    expected=$1
    actual=$2
    missing_message=$3
    extra_message=$4

    comm -23 "$expected" "$actual" | while IFS= read -r path; do
        [ -n "$path" ] && report_error "$missing_message$path"
    done
    comm -13 "$expected" "$actual" | while IFS= read -r path; do
        [ -n "$path" ] && report_error "$extra_message$path"
    done
}

check_catalog_kind_unique() {
    kind=$1
    count=$(awk -v kind="$kind" '$1 == kind { count++ } END { print count + 0 }' "$CATALOG")
    if [ "$count" -ne 1 ]; then
        report_error "docs/catalog.txt: $kind 须恰有一行，现有 $count 行"
    fi
}

check_links() {
    source_file=$1
    source_relative=${source_file#"$REPO_ROOT"/}
    source_directory=${source_file%/*}
    in_fenced_block=0
    link_re='!?\[[^]]*\]\(([^)]*)\)'

    while IFS= read -r source_line || [ -n "$source_line" ]; do
        trimmed=${source_line#"${source_line%%[![:space:]]*}"}
        case "$trimmed" in
            '```'*|'~~~'*)
                if [ "$in_fenced_block" -eq 0 ]; then
                    in_fenced_block=1
                else
                    in_fenced_block=0
                fi
                continue
                ;;
        esac
        [ "$in_fenced_block" -eq 0 ] || continue

        remaining=$source_line
        while [[ $remaining =~ $link_re ]]; do
            whole_link=${BASH_REMATCH[0]}
            target=${BASH_REMATCH[1]}
            link_prefix=${remaining%%"$whole_link"*}
            remaining=${remaining#"$link_prefix"}
            remaining=${remaining#"$whole_link"}

            target=${target#"${target%%[![:space:]]*}"}
            target=${target%"${target##*[![:space:]]}"}
            case "$target" in
                \<*\>*)
                    target=${target#<}
                    target=${target%%>*}
                    ;;
                *)
                    target=${target%%[[:space:]]*}
                    ;;
            esac

            case "$target" in
                ''|'#'*) continue ;;
            esac
            if [[ $target =~ ^[A-Za-z][A-Za-z0-9+.-]*: ]]; then
                continue
            fi

            checked_links=$((checked_links + 1))
            path_text=${target%%#*}
            path_text=${path_text%%\?*}
            path_text=${path_text//%20/ }
            [ -n "$path_text" ] || continue

            case "$path_text" in
                /*) candidate=$path_text ;;
                *) candidate=$source_directory/$path_text ;;
            esac
            if [ ! -e "$candidate" ]; then
                report_error "$source_relative: 断链: $target"
                continue
            fi

            if [ -d "$candidate" ]; then
                resolved=$(CDPATH= cd -- "$candidate" && pwd -P)
            else
                candidate_directory=${candidate%/*}
                candidate_name=${candidate##*/}
                resolved=$(CDPATH= cd -- "$candidate_directory" && printf '%s/%s\n' "$(pwd -P)" "$candidate_name")
            fi
            case "$resolved" in
                "$REPO_ROOT"|"$REPO_ROOT"/*) ;;
                *) report_error "$source_relative: 本地链接越出仓库: $target" ;;
            esac
        done
    done <"$source_file"
}

if [ ! -f "$CATALOG" ]; then
    echo "docs check failed: 缺 docs/catalog.txt" >&2
    exit 1
fi

awk '$1 == "DOC" { print $2 }' "$CATALOG" >"$WORK_DIR/docs-raw"
LC_ALL=C sort "$WORK_DIR/docs-raw" | uniq -d | while IFS= read -r page; do
    [ -n "$page" ] && report_error "docs/catalog.txt: 页面重复收录: $page"
done
LC_ALL=C sort -u "$WORK_DIR/docs-raw" >"$WORK_DIR/docs-expected"
write_relative_markdown "$DOCS_ROOT" "$WORK_DIR/docs-actual"
report_set_difference "$WORK_DIR/docs-expected" "$WORK_DIR/docs-actual" \
    "docs/catalog.txt: 收录页面不存在: " "docs/catalog.txt: 孤儿页面未收录: "

for page in "$DOCS_ROOT"/*.md; do
    [ -e "$page" ] || continue
    [ "$(basename -- "$page")" = "README.md" ] || \
        report_error "docs/: 顶层只留 README.md，页面须归模块: $(basename -- "$page")"
done

awk '$1 == "INTERVIEW" { print $2 }' "$CATALOG" | LC_ALL=C sort -u >"$WORK_DIR/interview-expected"
write_relative_markdown "$INTERVIEW_ROOT" "$WORK_DIR/interview-actual"
report_set_difference "$WORK_DIR/interview-expected" "$WORK_DIR/interview-actual" \
    "docs/catalog.txt: interview 收录页面不存在: " "docs/catalog.txt: interview 孤儿页面未收录: "

check_catalog_kind_unique SKILL_DIR
check_catalog_kind_unique SKILL_DOCS
check_catalog_kind_unique SKILL_MAP
skill_directory=$(catalog_value SKILL_DIR | sed -n '1p')
docs_relative=$(catalog_value SKILL_DOCS | sed -n '1p')
map_relative=$(catalog_value SKILL_MAP | sed -n '1p')
skill_root="$REPO_ROOT/$skill_directory"
skill_file="$skill_root/SKILL.md"
map_file="$skill_root/$map_relative"

if [ ! -f "$skill_file" ]; then
    report_error "官方 Skill 缺 SKILL.md: $skill_directory/SKILL.md"
else
    [ "$(sed -n '1p' "$skill_file")" = "---" ] || \
        report_error "skills/lubancode-config/SKILL.md: frontmatter 起始不合"
    grep -q '^name: lubancode-config$' "$skill_file" || \
        report_error "skills/lubancode-config/SKILL.md: name 不合"
    grep -q '^description: .' "$skill_file" || \
        report_error "skills/lubancode-config/SKILL.md: 缺 description"
    grep -q '../../docs' "$skill_file" || \
        report_error "skills/lubancode-config/SKILL.md: 没写官方 docs 相对根"
    if grep -q '\[TODO:' "$skill_file"; then
        report_error "skills/lubancode-config/SKILL.md: 仍有 TODO 占位符"
    fi
fi

if [ ! -d "$skill_root/$docs_relative" ]; then
    report_error "docs/catalog.txt: Skill docs 路径不存在: $docs_relative"
else
    resolved_docs=$(CDPATH= cd -- "$skill_root/$docs_relative" && pwd -P)
    canonical_docs=$(CDPATH= cd -- "$DOCS_ROOT" && pwd -P)
    [ "$resolved_docs" = "$canonical_docs" ] || \
        report_error "docs/catalog.txt: Skill docs 路径没指向 docs/: $resolved_docs"
fi

if [ ! -f "$map_file" ]; then
    report_error "官方 Skill 缺文档地图: $skill_directory/$map_relative"
    : >"$WORK_DIR/map-empty"
    map_file="$WORK_DIR/map-empty"
fi

write_relative_markdown "$skill_root/references" "$WORK_DIR/references-actual-relative"
sed "s#^#references/#" "$WORK_DIR/references-actual-relative" >"$WORK_DIR/references-actual"
printf '%s\n' "$map_relative" | LC_ALL=C sort -u >"$WORK_DIR/references-expected"
report_set_difference "$WORK_DIR/references-expected" "$WORK_DIR/references-actual" \
    "skills/lubancode-config/references: 文档地图不存在: " \
    "skills/lubancode-config/references: 多余副本，应只留文档地图: "

awk '$1 == "ROUTE" { print $2 }' "$CATALOG" | while IFS= read -r route; do
    if [ ! -f "$DOCS_ROOT/$route" ]; then
        report_error "docs/catalog.txt: Skill 路由目标不存在: $route"
    fi
    grep -Fq "\`$route\`" "$map_file" || report_error "Skill 文档地图漏路由: $route"
done

{
    printf '%s\n' "$REPO_ROOT/README.md" "$REPO_ROOT/README.en.md"
    find "$DOCS_ROOT" "$INTERVIEW_ROOT" "$skill_root" -type f -name '*.md' -print
} | LC_ALL=C sort -u >"$WORK_DIR/link-files"
while IFS= read -r source_file; do
    check_links "$source_file"
done <"$WORK_DIR/link-files"

if [ -s "$ERRORS" ]; then
    error_count=$(wc -l <"$ERRORS" | tr -d ' ')
    echo "docs check failed: $error_count 个问题" >&2
    sed 's/^/- /' "$ERRORS" >&2
    exit 1
fi

doc_count=$(wc -l <"$WORK_DIR/docs-actual" | tr -d ' ')
interview_count=$(wc -l <"$WORK_DIR/interview-actual" | tr -d ' ')
echo "docs check passed: docs $doc_count 页，interview $interview_count 页，本地链接 $checked_links 条"
