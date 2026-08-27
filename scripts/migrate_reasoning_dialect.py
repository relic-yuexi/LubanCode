# 一次性迁移:给 catalog/providers.json 的每家 provider 落 reasoning_dialect
# (模型协议兼容实录矩阵单 P1)。规矩:
#   - 不凭模型名猜方言——方言挂 provider,来源是三份手册明文、仓库真机
#     实测注释、官方 API 文档;
#   - 手册/实测/官方文档背书的家 verified=true,聚合转发端一律 false;
#   - generic 家族方言与迁移前的 serializer 行为逐家等价(形状不变,
#     只是把它从"serializer 硬编码"挪进"目录声明");
#   - 修正现状的两处是手册背书的:dashscope chat 端 toggle 从 thinking.type
#     改为顶层 enable_thinking(G0 反例),minimax chat 端 on 值从 enabled
#     改为手册枚举 adaptive。
# 用法:python scripts/migrate_reasoning_dialect.py(在仓库根跑,幂等重写)。

import io
import json

PATH = 'catalog/providers.json'

CHAT_GENERIC = {"toggle": "thinking_type", "toggle_on": "enabled", "toggle_off": "disabled",
                "effort_path": "reasoning_effort", "delta": "reasoning_content",
                "replay": "never", "verified": False}
RESP_GENERIC = {"toggle": "enable_thinking_bool", "toggle_on": "true", "toggle_off": "false",
                "effort_path": "reasoning.effort", "delta": "reasoning_summary",
                "replay": "never", "verified": False}
ANTHROPIC_GENERIC = {"toggle": "thinking_type", "toggle_on": "enabled", "toggle_off": "disabled",
                     "budget_path": "thinking.budget_tokens", "delta": "anthropic_thinking_block",
                     "replay": "never", "signature_required": True, "verified": False}
GEMINI_GENERIC = {"toggle": "include_thoughts", "effort_path": "thinkingLevel",
                  "budget_path": "thinkingBudget", "delta": "gemini_thought_part",
                  "replay": "never", "verified": False}


def v(base, **kw):
    out = dict(base)
    out.update(kw)
    return out


# verified 名单依据:
#  dashscope 三家:OpenAI兼容-Chat/Responses/Anthropic 三份手册明文;
#  zai 四家:仓库真机实测(矩阵 A2/A3:GLM 双开/budget);
#  minimax 两家 chat:手册明文(thinking.type=adaptive/disabled);
#  minimax 两家 anthropic:仓库真机实测(M6.6 注释);
#  openai/anthropic/gemini:官方 API 文档形状。
DIALECTS = {
    # 手册(OpenAI兼容-Chat.md):reasoning_effort 参数适用于阿里云直供的
    # DeepSeek-V4/GLM 系(dashscope 家里这 7 只模型声明了 effort 档),
    # Qwen 系用 enable_thinking + thinking_budget。两条路径并存,落不落
    # 由各模型的档案决定。
    'dashscope': v(CHAT_GENERIC, toggle="enable_thinking_bool", toggle_on="true",
                   toggle_off="false", budget_path="thinking_budget", verified=True),
    'zai': v(CHAT_GENERIC, verified=True),
    'zai-coding': v(CHAT_GENERIC, verified=True),
    # minimax 家目录里没有 supports_effort 模型,手册也只有 thinking.type。
    'minimax': v(CHAT_GENERIC, toggle_on="adaptive", effort_path="none", verified=True),
    'minimax-global': v(CHAT_GENERIC, toggle_on="adaptive", effort_path="none", verified=True),
    'deepseek': v(CHAT_GENERIC, replay="tool_episode"),
    'moonshot': v(CHAT_GENERIC),
    'dashscope-responses': v(RESP_GENERIC, verified=True),
    'openai': v(RESP_GENERIC, verified=True),
    'dashscope-anthropic': v(ANTHROPIC_GENERIC, verified=True),
    'zai-anthropic': v(ANTHROPIC_GENERIC, verified=True),
    'zai-coding-anthropic': v(ANTHROPIC_GENERIC, verified=True),
    'minimax-anthropic': v(ANTHROPIC_GENERIC, verified=True),
    'minimax-global-anthropic': v(ANTHROPIC_GENERIC, verified=True),
    'anthropic': v(ANTHROPIC_GENERIC, verified=True),
    'gemini': v(GEMINI_GENERIC, verified=True),
}
WIRES = {'openai-chat-completions': CHAT_GENERIC, 'openai-responses': RESP_GENERIC,
         'anthropic-messages': ANTHROPIC_GENERIC, 'google-generate-content': GEMINI_GENERIC}


def clean(d):
    return {k: val for k, val in d.items() if val not in ('none', None)}


def main():
    with io.open(PATH, encoding='utf-8') as f:
        catalog = json.load(f)
    for pid, provider in catalog['providers'].items():
        wire = provider['wire']
        dialect = DIALECTS.get(pid, WIRES[wire])
        if pid not in DIALECTS:
            assert dialect is WIRES[wire], pid  # generic 方言须与 wire 对得上
        provider['reasoning_dialect'] = clean(dialect)
        # 模型级覆写:anthropic 家 wireDialect=="effort" 的模型声明
        # output_config.effort(Claude 新式 adaptive 路径)。
        for model in provider['models'].values():
            reasoning = model.get('reasoning')
            if not reasoning:
                continue
            if wire == 'anthropic-messages' and reasoning.get('wireDialect') == 'effort':
                reasoning['dialect'] = {"effort_path": "output_config.effort"}

    order = ['name', 'description', 'wire', 'base_url', 'key_env', 'default_model',
             'model_reasoning_effort', 'native_web_search', 'stream_usage', 'reasoning_replay',
             'reasoning_dialect', 'reasoning_delta_field', 'reasoning_replay_field', 'docs_url',
             'extra_body', 'extra_headers', 'models']
    catalog['providers'] = {pid: {k: p[k] for k in order if k in p}
                            for pid, p in catalog['providers'].items()}
    with io.open(PATH, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(catalog, f, ensure_ascii=False, indent=2)
        f.write('\n')
    print('migrated', len(catalog['providers']), 'providers')


if __name__ == '__main__':
    main()
