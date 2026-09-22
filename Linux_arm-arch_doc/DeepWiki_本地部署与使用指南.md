## DeepWiki 本地部署与使用指南（基于当前环境）

本文总结你在本机（Ubuntu 虚拟机）把 `deepwiki-open` 跑起来所需的关键步骤和注意事项，按“源码部署”为主，顺带提一下 Docker 方式。

---

## 一、基础环境准备

- **操作系统**：Ubuntu（虚拟机即可）
- **已安装**：
  - Git（用于拉代码）
  - Node.js + npm（前端 Next.js）
  - Python 3.11/3.12（后端 FastAPI）
  - `curl`（方便测试 API）

建议在 `$HOME` 下工作，对应路径：

```bash
cd ~
git clone https://github.com/AsyncFuncAI/deepwiki-open.git
cd deepwiki-open
```

---

## 二、后端部署（Python / FastAPI）

### 1. 创建并激活虚拟环境

```bash
cd ~/deepwiki-open
python3 -m venv venv
source venv/bin/activate
```

后续所有后端相关命令都建议在激活的 venv 中执行（命令行前会有 `(venv)`）。

### 2. 安装后端依赖

项目的后端使用 `pyproject.toml` 管理依赖，可以直接用 `pip` 安装：

```bash
cd ~/deepwiki-open

pip install \
  "fastapi>=0.95.0" \
  "uvicorn[standard]>=0.21.1" \
  "pydantic>=2.0.0" \
  "google-generativeai>=0.3.0" \
  "tiktoken>=0.5.0" \
  "adalflow>=0.1.0" \
  "numpy>=1.24.0" \
  "faiss-cpu>=1.7.4" \
  "langid>=1.1.6" \
  "requests>=2.28.0" \
  "jinja2>=3.1.2" \
  "python-dotenv>=1.0.0" \
  "openai>=1.76.2" \
  "ollama>=0.4.8" \
  "aiohttp>=3.8.4" \
  "boto3>=1.34.0" \
  "websockets>=11.0.3" \
  "azure-identity>=1.12.0" \
  "azure-core>=1.24.0"
```

> 说明：这一步等价于原来 `api/requirements.txt` 的效果，只是现在项目改成了 `pyproject.toml` 管理。

---

## 三、配置向量模型与环境变量

DeepWiki 由两部分模型组成：

- **生成模型**：写 Wiki / 问答（在 `generator.json` 中配置）
- **Embedding 模型**：构建向量数据库、实现检索（在 `embedder.json` + `.env` 中配置）

这里给出两种典型配置方式，并单独说明“生成模型”的配置。

### 1. 使用 OpenAI 兼容 Embedding 服务（通用）

如果你有任意 **OpenAI 兼容** 的接口（包括千问 DashScope、Phytium 自研代理等），只要遵守 `/v1/embeddings` 协议，就可以通过 `OpenAIClient` 使用。

#### （1）`.env` 配置

示例（以 OpenAI 官方为例，DashScope 请替换成其兼容地址和 Key）：

```bash
DEEPWIKI_EMBEDDER_TYPE=openai

# OpenAI 兼容接口
OPENAI_API_KEY=你的_API_Key
OPENAI_BASE_URL=https://api.openai.com/v1

PORT=8001
SERVER_BASE_URL=http://localhost:8001
```

对于千问 DashScope，形如：

```bash
OPENAI_API_KEY=你的_DashScope_Key
OPENAI_BASE_URL=https://dashscope.aliyuncs.com/compatible-mode/v1
```

#### （2）`api/config/embedder.json`

嵌入模型配置示例（按你实际模型名与维度修改）：

```json
{
  "embedder": {
    "client_class": "OpenAIClient",
    "initialize_kwargs": {
      "api_key": "${OPENAI_API_KEY}",
      "base_url": "${OPENAI_BASE_URL}"
    },
    "batch_size": 10,
    "model_kwargs": {
      "model": "text-embedding-v3",
      "dimensions": 1024,
      "encoding_format": "float"
    }
  },
  "retriever": {
    "top_k": 20
  },
  "text_splitter": {
    "split_by": "word",
    "chunk_size": 350,
    "chunk_overlap": 100
  }
}
```

> 关键：`model` 和 `dimensions` 必须和实际服务支持的 embedding 模型一致，可先用小脚本 / curl 测试返回向量长度。

### 2. 使用 Google Embedding（官方推荐之一）

如果你使用 Google Gemini：

```bash
DEEPWIKI_EMBEDDER_TYPE=google
GOOGLE_API_KEY=你的_Google_API_Key
```

此时 `embedder.json` 使用项目自带的 Google 配置即可，无需手动改模型名。

---

## 四、配置生成模型（大模型，用于写 Wiki + 问答）

Embedding 负责“找相关文档块”，而生成模型负责“看着这些块写 Wiki / 回答问题”。  
生成模型的配置主要在：

- `.env`：提供不同 provider 的 API Key
- `api/config/generator.json`：决定默认用哪个 provider / 哪个模型

### 1. 千问（DashScope）作为生成模型

你当前的 `.env` 已经把 DashScope 配成 OpenAI 兼容接口，可以顺便用于生成模型：

```bash
DEEPWIKI_EMBEDDER_TYPE=openai

OPENAI_BASE_URL=https://dashscope.aliyuncs.com/compatible-mode/v1
OPENAI_API_KEY=你的_DashScope_Key
DASHSCOPE_API_KEY=你的_DashScope_Key

PORT=8001
SERVER_BASE_URL=http://localhost:8001
```

在 `api/config/generator.json` 中，DashScope 一段大致如下（你已经有了）：

```json
{
  "default_provider": "dashscope",
  "providers": {
    "dashscope": {
      "default_model": "qwen-plus",
      "supportsCustomModel": true,
      "models": {
        "qwen-plus":  { "temperature": 0.7, "top_p": 0.8 },
        "qwen-turbo": { "temperature": 0.7, "top_p": 0.8 },
        "deepseek-r1": { "temperature": 0.7, "top_p": 0.8 }
      }
    },
    ...
  }
}
```

- **`default_provider`** 设为 `dashscope`，表示 Ask / 生成 Wiki 时优先走千问；
- **`default_model`** 写 `qwen-plus` 或 `qwen-turbo` 等 DashScope 文档里的聊天模型名；
- 实际调用时会通过 `OpenAIClient` 走 `OPENAI_BASE_URL` 指向的千问兼容接口。

> 总结：embedding 在 `embedder.json` + `.env`，生成模型在 `generator.json` + `.env`。两者都可以指向同一个服务（比如都用千问），也可以 embedding 用千问、生成用别的（如 Google）。

---

## 五、启动后端服务

在 venv 中执行：

```bash
cd ~/deepwiki-open
source venv/bin/activate
uvicorn api.api:app --host 0.0.0.0 --port 8001 --env-file .env
```

保持该终端窗口运行（不要 Ctrl+C），后端 API 将监听 `http://0.0.0.0:8001`。

> 注意：如果之前做过错误配置导致向量库损坏，可删除缓存后重建：
> ```bash
> rm -f ~/.adalflow/databases/*.pkl
> ```

---

## 六、前端部署（Next.js）

### 1. 安装前端依赖

```bash
cd ~/deepwiki-open
npm install
```

（只需执行一次，后续如有依赖变更再跑。）

### 2. 开发模式启动前端

```bash
cd ~/deepwiki-open
npm run dev
```

默认监听 `http://localhost:3000`。

### 3. 生产模式（可选）

如需生产模式运行：

```bash
cd ~/deepwiki-open
npm run build
npm start
```

---

## 七、在浏览器中使用 DeepWiki

1. 浏览器打开：
   - `http://localhost:3000`
2. 在输入框中填写仓库路径：
   - 公网仓库：`owner/repo` 或 `https://github.com/owner/repo`
   - 本地仓库：`/home/fbt1709/Linux_arm-arch_doc` 这类绝对路径
3. 点击“生成 Wiki / Generate Wiki”：
   - 后端会：
     - 克隆或读取仓库；
     - 切分文档（TextSplitter）；
     - 调用 embedding 接口生成向量；
     - 保存到 `~/.adalflow/databases/*.pkl`；
     - 生成并缓存 Wiki 页面到 `~/.adalflow/wikicache`。
4. 生成完成后，可以在网页中：
   - 浏览自动生成的 Wiki 页面；
   - 使用搜索/问答功能（RAG）。

---

## 八、常见问题与排查思路

### 1. “No valid embeddings found in any documents”

说明本轮 embedding 全部失败或被判为空，常见原因：

- Embedding 接口 401 / 欠费 / 模型名错误；
- `OPENAI_BASE_URL` 写错（多/少 `/embeddings`）；
- 之前错误配置产生的旧 `.pkl` 被重复使用。

排查步骤：

1. 用独立脚本验证 embedding 是否通（类似 `test_dashscope_embed.py`）；
2. 确认 `.env` 与 `embedder.json` 中 `model`/`dimensions` 正确；
3. 删除旧向量库后重启后端，重新生成：
   ```bash
   rm -f ~/.adalflow/databases/*.pkl
   uvicorn api.api:app --host 0.0.0.0 --port 8001 --env-file .env
   ```

### 2. “UnsupportedProtocol: Request URL is missing an 'http://' or 'https://' protocol”

说明 OpenAI SDK 拿到的 `base_url` 形如 `${https://...}` 或空串。

- 确认 `embedder.json` 中是：
  ```json
  "base_url": "${OPENAI_BASE_URL}"
  ```
  而不是 `"${https://...}"` 或硬编码错误 URL；
- 确认 `.env` 中 `OPENAI_BASE_URL` 为完整前缀（不包含 `/embeddings`）；
- 删除旧 `.pkl` 后重启后端。

### 3. 401 / token_not_found_in_db（代理场景）

如果后端日志中出现：

```json
"token_not_found_in_db" 或 "Invalid proxy server token passed"
```

说明使用的是 LiteLLM 这类代理，要求的其实是代理自身的 Token，而不是模型厂商原始 Key。需要：

- 在代理平台控制台获取对应的“访问 Token”；
- 按其文档将该 Token 放入 `Authorization: Bearer <token>`；
- 再更新 `.env` 中的 `OPENAI_API_KEY`。

---

## 九、Docker 部署简要说明（可选）

如不想手动装 Python/Node.js，可使用官方 Docker 镜像：

```bash
docker pull ghcr.io/asyncfuncai/deepwiki-open:latest

docker run -p 8001:8001 -p 3000:3000 \
  -e GOOGLE_API_KEY=your_google_api_key \
  -e OPENAI_API_KEY=your_openai_api_key \
  -e DEEPWIKI_EMBEDDER_TYPE=google \
  -v ~/.adalflow:/root/.adalflow \
  ghcr.io/asyncfuncai/deepwiki-open:latest
```

容器启动后，同样通过 `http://localhost:3000` 访问前端界面，API 服务默认为 `http://localhost:8001`。

---

## 十、小结

1. 后端：建 venv → 安装依赖 → 配置 `.env` → 启动 `uvicorn`。  
2. 前端：`npm install` → `npm run dev`（或 `npm run build && npm start`）。  
3. Embedding：根据服务商（OpenAI/千问/自研代理）正确设置 `OPENAI_BASE_URL`、`OPENAI_API_KEY` 和 `embedder.json` 中的模型名与维度。  
4. 如果遇到向量相关错误，优先用独立脚本 + curl 验证 embedding 接口，再清空缓存重跑 DeepWiki。

