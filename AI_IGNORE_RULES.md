# AI 智能忽略规则生成 — 技术方案

## 概述

VeritasSync 提供"自然语言生成忽略规则"功能：用户在 WebUI 中用自然语言描述想忽略的文件（如"忽略所有测试文件，但保留 main_test.cpp"），系统自动生成 `.veritasignore` 规则。

核心实现采用 **LLM + 轻量级 RAG** 架构，全部用 C++ 实现，无外部依赖（不需要 Python、Node.js 或向量数据库）。

## 架构

```
用户输入: "忽略所有测试文件"
         │
         ▼
┌─────────────────────────────────────────────┐
│           NLFilterGenerator::generate()      │
│                                             │
│  1. 目录摘要 ──→ build_directory_summary()  │  ← RAG 第一层
│     压缩文件树为按目录聚合的统计信息          │
│                                             │
│  2. 关键词采样 ──→ search_relevant_samples() │  ← RAG 第二层
│     从文件树中搜索匹配样本 + 同级对比文件     │
│                                             │
│  3. 组装 Prompt ──→ build_llm_prompt()       │
│     用户需求 + 目录摘要 + 文件样本 + 已有规则  │
│                                             │
│  4. 调用 LLM ──→ generate_from_llm()         │
│     OpenAI 兼容 API (阿里百炼/DeepSeek/...)  │
│                                             │
│  5. 解析 & 验证 ──→ extract_rules / validate │
│     JSON 结构化解析 + 大括号提取法              │
└─────────────────────────────────────────────┘
         │
         ▼
输出: "tests/\n**/*_test.cpp"
```

## 为什么不用 Agent / Function Calling / 微服务？

| 方案 | 结论 | 原因 |
|------|------|------|
| **Agent（多步推理）** | ❌ 不需要 | 这是单轮任务——用户说一句话，返回一组规则。不需要多步决策 |
| **Function Calling** | ❌ 不需要 | 我们比 LLM 更清楚什么时候该搜文件，直接在 C++ 里调就行 |
| **Python 微服务** | ❌ 不需要 | 增加部署复杂度，C++ 里 ~250 行代码就解决了 |
| **向量数据库 RAG** | ❌ 不需要 | 文件路径不需要语义搜索，关键词匹配 + 目录聚合足够 |
| **LLM + 轻量 RAG** | ✅ 采用 | 单次 API 调用，预处理在本地完成，简单高效 |

**核心原则**：能在本地确定性完成的事（扫目录、搜关键词、聚合统计），绝不丢给 LLM 去"推理"。LLM 只负责它擅长的事——理解自然语言意图，生成 glob 模式。

## RAG 实现细节

### 第一层：目录摘要（解决文件树过长问题）

**问题**：10 万文件的项目，完整路径列表可能几 MB，远超 LLM 上下文窗口。

**解法**：`build_directory_summary()` 将文件树压缩为按目录聚合的统计信息。

```
输入：50000 个文件的完整路径列表（~5MB）

输出（<3KB）：
Total: 8542 files

(root)/          3 files (.md, .txt, .json)
src/             8 files (.cpp×5, .h×3)
src/net/        12 files (.cpp×7, .h×5)
src/p2p/         9 files (.cpp×6, .h×3)
tests/          25 files (.cpp×25)
build/         156 files (.obj×78, .pdb×77, .exe×1)
node_modules/ 8342 files (.js×5420, .json×1893, .md×612, .ts×417)
```

**安全阀**：
- `MAX_SCAN_FILES = 50000`：超过 5 万文件停止扫描
- `max_chars = 3000`：摘要文本超过 3KB 截断
- 自动跳过 `.git/`、`.veritas.db` 等内部目录

**实现**：`NLFilterGenerator::build_directory_summary()` (~90 行 C++)

### 第二层：关键词采样（解决深层混杂文件问题）

**问题**：目录摘要只显示 `src/p2p/ 9 files (.cpp×6, .h×3)`，LLM 看不出哪些是测试文件。

```
src/p2p/PeerController.cpp        ← 不是测试
src/p2p/PeerController_test.cpp   ← 是测试
src/p2p/utils/KcpHelper_test.cpp  ← 深层测试
```

**解法**：`search_relevant_samples()` 从用户描述中提取关键词，在文件树中搜索匹配样本，并找出同目录下的不匹配文件作为对比。

**工作流程**：

```
1. 用户输入 "忽略测试文件"
              │
2. 关键词提取 ──→ "测试" 命中映射表 ──→ search_term = "test"
              │
3. 扫描文件树 ──→ 路径包含 "test" 的文件：
   ✓ tests/test_database.cpp
   ✓ tests/test_sync_logic.cpp
   ✓ src/p2p/PeerController_test.cpp
   ✓ src/p2p/utils/KcpHelper_test.cpp
              │
4. 同目录对比 ──→ 同在 src/p2p/ 但不含 "test" 的文件：
   ✗ src/p2p/PeerController.cpp
   ✗ src/p2p/P2PManager.cpp
              │
5. 格式化输出 ──→ 塞进 Prompt
```

**关键词映射表**（中英文双语）：

| 搜索词 | 触发关键词 |
|--------|-----------|
| test | test, 测试, spec, mock |
| build | build, 编译, 构建, dist, output, 产物 |
| log | log, 日志 |
| cache | cache, 缓存 |
| temp | temp, tmp, 临时 |
| doc | doc, 文档, readme |
| config | config, 配置, conf, setting |
| image | image, 图片, img, 图像, 照片 |
| video | video, 视频 |
| font | font, 字体 |

如果用户描述不匹配任何预定义关键词，自动提取描述中 ≥3 字符的英文单词作为搜索词。

**实现**：`NLFilterGenerator::search_relevant_samples()` (~150 行 C++)

## Prompt 组装

最终发给 LLM 的 Prompt 由 `build_llm_prompt()` 组装，包含四个部分：

```
┌──────────────────────────────────────────────────┐
│ ## 规则语法（.gitignore 兼容）                      │  ← 固定系统指令
│ - * 匹配任意字符                                    │
│ - ** 匹配任意深度                                   │
│ - ! 取反（白名单）                                  │
│ ...                                                │
│                                                    │
│ ## 输出格式要求（最高优先级）                         │  ← JSON 结构化约束
│ 1. 必须且只能输出一个合法的 JSON 对象                 │
│ 2. 绝对不要输出任何解释性文字                        │
│ 3. 结构：{"rules": [...], "explanation": "..."}     │
├──────────────────────────────────────────────────┤
│ ## 项目目录结构：                                    │  ← RAG: 目录摘要
│ Total: 85 files                                    │
│ src/p2p/  9 files (.cpp×6, .h×3)                   │
│ tests/   25 files (.cpp×25)                        │
│ ...                                                │
├──────────────────────────────────────────────────┤
│ ## 与用户需求相关的文件样本（本地搜索 "test"）：       │  ← RAG: 关键词采样
│ 匹配的文件 (应被忽略规则覆盖):                       │
│   ✓ tests/test_database.cpp                        │
│   ✓ src/p2p/PeerController_test.cpp                │
│ 同目录下不匹配的文件 (不应被误伤):                   │
│   ✗ src/p2p/PeerController.cpp                     │
├──────────────────────────────────────────────────┤
│ ## 用户已有的规则：                                  │  ← 避免重复
│ *.log                                              │
│ build/                                             │
├──────────────────────────────────────────────────┤
│ ## 用户需求：                                       │  ← 用户原始输入
│ 忽略所有测试文件                                    │
└──────────────────────────────────────────────────┘
```

## LLM 调用

### API 兼容性

使用 OpenAI 兼容格式（`/v1/chat/completions`），支持：

| 平台 | 端点 | 默认模型 |
|------|------|---------|
| **阿里百炼**（默认） | `https://dashscope.aliyuncs.com/compatible-mode` | `qwen-turbo` |
| DeepSeek | `https://api.deepseek.com` | `deepseek-chat` |
| OpenAI | `https://api.openai.com` | `gpt-4o-mini` |
| 任何兼容端点 | 自定义 URL | 自定义 |

### 请求参数

```json
{
  "model": "qwen-turbo",
  "temperature": 0.3,
  "max_tokens": 1024,
  "response_format": {"type": "json_object"},
  "messages": [{"role": "user", "content": "<组装后的 prompt>"}]
}
```

- `temperature: 0.3`：低随机性，生成确定性强的规则
- `max_tokens: 1024`：忽略规则通常不长
- `response_format: json_object`：API 层面强制 JSON 输出，废话根本出不来
- 超时：30 秒

### 响应解析：三层防线保证 100% 可解析

不同 LLM 厂商/版本的输出格式差异是工程落地的最大坑。我们通过三层防线确保解析绝不翻车：

**第一层：API 级别约束**

请求体中 `response_format: {"type": "json_object"}` 让 API 底层强制输出 JSON。阿里百炼、OpenAI、DeepSeek 的新版模型均支持此参数。如果 LLM 试图输出非 JSON 内容，API 会强行拒绝并重试。

**第二层：Prompt 中的死命令**

```
输出格式要求（最高优先级）：
1. 你必须且只能输出一个合法的 JSON 对象
2. 绝对不要输出任何解释性文字
3. 结构：{"rules": ["第一条规则", "第二条规则"], "explanation": "简要说明"}
```

**第三层：C++ 大括号提取法（Brace Extraction）**

即使某些旧模型不支持 `json_object` 模式，在 JSON 前后加了废话（如 `好的，JSON如下：\n{...}\n希望能帮到你！`），解析依然不会失败：

```cpp
// 1. 寻找 JSON 的绝对边界
size_t start = response.find_first_of('{');
size_t end   = response.find_last_of('}');
// 2. 裁剪出纯净 JSON
std::string json_str = response.substr(start, end - start + 1);
// 3. 强类型解析
auto j = nlohmann::json::parse(json_str);
// 4. 提取 rules 数组
for (const auto& item : j["rules"]) { ... }
```

**Fallback：逐行过滤**

如果 JSON 解析也失败（极端情况），退化为逐行启发式过滤：保留以 `*` `/` `!` `#` `.` 或 ASCII 字母开头且不含中文的行，丢弃解释性废话。

**LLM 输出格式示例**：

```json
{
  "rules": [
    "tests/",
    "**/*_test.cpp",
    "!**/integration_test.cpp"
  ],
  "explanation": "忽略 tests 目录和所有以 _test.cpp 结尾的文件，保留集成测试"
}
```

**为什么不用 Markdown 代码块**：

| 方案 | 可靠性 | 原因 |
|------|--------|------|
| ` ```rules ``` ` | ~80% | LLM 可能输出 ` ```gitignore ` 或不带代码块 |
| 逐行正则过滤 | ~90% | 启发式猜测，文件名恰好像自然语言时误判 |
| **JSON 结构化输出** | **~100%** | `{` `}` 边界确定 + 强类型解析 + API 级约束 |

## 配置

`config.json` 中需要配置：

```json
{
  "llm": {
    "api_url": "https://dashscope.aliyuncs.com/compatible-mode",
    "api_key": "sk-你的API密钥",
    "model": "qwen-turbo"
  }
}
```

其中 `api_url` 和 `model` 有默认值，只需填写 `api_key` 即可使用。

## 端到端示例

### 简单场景

```
用户输入: "忽略所有日志文件"

LLM 返回 JSON:
  {"rules": ["*.log", "*.log.*"], "explanation": "忽略所有日志文件"}

最终规则:
  *.log
  *.log.*

Dry-Run: 📊 预计将忽略本地 12 个文件
```

### 复杂场景：深层混杂 + 白名单

```
用户输入: "忽略所有测试文件，但保留 integration_test.cpp"

RAG 注入（自动生成）:
  目录摘要: tests/ 25 files (.cpp×25), src/p2p/ 9 files (.cpp×6, .h×3)
  文件样本: ✓ tests/test_database.cpp, ✓ src/p2p/PeerController_test.cpp
  对比文件: ✗ src/p2p/PeerController.cpp

LLM 返回 JSON:
  {
    "rules": ["tests/", "**/*_test.cpp", "!**/integration_test.cpp"],
    "explanation": "忽略 tests 目录和所有 _test.cpp 文件，保留集成测试"
  }

最终规则:
  tests/
  **/*_test.cpp
  !**/integration_test.cpp

Dry-Run: 📊 预计将忽略本地 34 个文件
```

## 涉及的源文件

| 文件 | 职责 |
|------|------|
| `include/VeritasSync/storage/NLFilterGenerator.h` | 类定义、接口声明 |
| `src/storage/NLFilterGenerator.cpp` | 核心实现：RAG 预处理 + LLM 调用 + 响应解析 |
| `src/p2p/WebUI.cpp` | HTTP 路由 `POST /api/tasks/:id/ignore/generate` |
| `src/web/index.html` | 前端 UI：输入框 + 调用按钮 + 结果展示 |
| `include/VeritasSync/common/Config.h` | LLM 配置字段定义和默认值 |
| `src/storage/FileFilter.cpp` | `load_rules_from_string()` — Dry-Run 用的规则加载 |
| `tests/test_nl_filter.cpp` | 单元测试 |

## Dry-Run 沙盘推演

LLM 返回规则后，自动在本地文件树上试跑一遍 glob 匹配，统计将被忽略的文件数量，在 UI 上展示 **"📊 预计将忽略本地 34 个文件"**。

**实现**：
1. `dry_run_rules()` 用 `FileFilter::load_rules_from_string()` 加载规则（纯内存，不读写文件）
2. 遍历同步目录，对每个文件调用 `should_ignore()` 统计匹配数
3. 结果通过 API 返回 `affected_files` 字段，前端展示

**价值**：给用户确定性反馈。"忽略 34 个文件" 比 "规则已生成" 更有安全感——用户能判断规则是否过于宽泛或过于窄。

## 目录摘要截断策略

针对超大项目（如包含 `node_modules` 的前端项目），摘要生成采用**浅层优先 + 大目录折叠**策略：

- **浅层优先排序**：按目录深度升序输出，确保 `src/`、`tests/` 等关键顶层目录优先占用字符预算
- **大目录折叠**：文件数 >500 的目录只显示一行 `[large dir]`，不展开扩展名
- **字符上限**：`max_chars = 3000`，超出截断

效果：即使 `node_modules/` 有 8000 文件按字母排在 `src/` 前面，也不会吃光预算。

## 技术选型评估记录

在方案设计阶段，评估了以下 AI 技术的适用性：

| 技术 | 结论 | 理由 |
|------|------|------|
| **RAG（轻量级）** | ✅ 采用 | 目录摘要 + 关键词采样，让 LLM 看到真实项目结构 |
| **RAG（向量数据库）** | ❌ 不需要 | 文件路径不需要语义搜索，关键词匹配足够 |
| **Agent** | ❌ 不需要 | 单轮任务，不需要多步推理和工具调用循环 |
| **Function Calling** | ❌ 不需要 | 搜索时机确定，无需 LLM 自主决策调用 |
| **Fine-tuning** | ❌ 不值得 | 数据量不够，通用模型 + prompt 工程足够 |
| **Python 微服务** | ❌ 不需要 | ~250 行 C++ 解决，不增加部署复杂度 |
| **前端 JS Agent** | ❌ 不需要 | 复杂度收益比不合理 |
