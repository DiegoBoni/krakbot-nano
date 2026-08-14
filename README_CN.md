<div align="center">

  <a href="https://esp-claw.com/">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="./docs/src/assets/logos/logo-f.svg" />
      <source media="(prefers-color-scheme: light)" srcset="./docs/src/assets/logos/logo.svg" />
      <img alt="ESP-Claw logo" src="./docs/src/assets/logos/logo.svg" width="50%" />
    </picture>
  </a>

  <h1>KrakBot Nano 🦞 物联网设备 AI 智能体框架</h1>

  <p><em>KrakBot Nano 是 <a href="https://github.com/espressif/esp-claw">Espressif ESP-Claw</a> 的个人分支，面向 ESP32-S3 N16R8 DevKitC-1。</em></p>

  <h3>💬聊天即造物 · 🚀毫秒级响应 · 🧩智能可扩展 · 😋越用越懂你</h3>

  <p>
    <a href="https://www.espressif.com">
      <img src="https://img.shields.io/badge/runs_on-ESP32_Series-red?style=flat-square" alt="Runs on ESP32 Series" />
    </a>
    <a href="./LICENSE">
      <img src="https://img.shields.io/github/license/espressif/esp-claw?style=flat-square" alt="License" />
    </a>
  </p>

  <a href="https://esp-claw.com/zh-cn/">主页</a>
  |
  <a href="https://esp-claw.com/zh-cn/tutorial/">文档</a>
  |
  <a href="https://esp-claw.com/zh-cn/flash/">在线烧录</a>
  |
  <a href="https://esp-claw.com/zh-cn/reference-project/build-from-source/">源码编译</a>
  |
  <a href="./README.md">English</a>

</div>

**ESP-Claw** 是乐鑫推出的面向物联网设备的 **Chat Coding「聊天造物」** 式 AI Agent 框架，以对话定义设备行为，在乐鑫芯片上本地完成感知、决策与执行的完整闭环。ESP-Claw 自 OpenClaw 理念出发，用 C 语言重新实现，轻盈、智能、成长。仅需一块几美元的 ESP32 系列芯片，便可体验 ESP-Claw 的轻灵特性。

**KrakBot Nano** 在此基础上专注适配单一开发板（ESP32-S3 N16R8 DevKitC-1），并新增了语音、多服务器 MCP、更严格的 Telegram 权限控制，以及更安全的默认配置，详见下方 [🦑 KrakBot Nano 新特性](#-krakbot-nano-新特性)。

<div align="center">
  <img alt="From traditional IoT to Edge Agent" src="./docs/static/from-traditional-iot-to-edge-agent.webp" width="90%" />
</div>

## 🦑 KrakBot Nano 新特性

在 ESP-Claw 已有能力之上，本分支新增：

- 🎙️ **语音输入** — 收到的 Telegram 语音消息会通过 Groq Whisper API 自动转写为文字并交给智能体处理，无需打字即可对话。
- 🔊 **语音回复** — 智能体的回复可通过 OpenAI TTS（多语言，含西班牙语）或 Groq TTS（免费额度，仅英语/阿拉伯语）合成为语音消息，可在新增的**语音**设置页选择音色、性别预设与模型。
- 🔌 **多服务器 MCP 与 Composio 集成** — MCP 客户端现在维护一份可持久化的远程 MCP 服务器注册表（URL、Endpoint、认证方式），并提供 `mcp_list_servers`/`mcp_list_tools`/`mcp_call_tool` 能力。新增的 **MCP** 设置页支持通过单个 [Composio](https://composio.dev) API Key 一键接入 Gmail、日历等 500+ 应用。
- 🔒 **Telegram 白名单** — 通过 `tgAllowedChatIds`（格式为 `id` 或 `id:name`，逗号分隔）限制机器人仅响应指定的会话/用户，留空则对所有人开放。
- 💰 **`/cost` 指令** — 在任意 IM 渠道输入 `/cost`，即可查看自启动以来的 LLM Token 用量及预估美元花费。
- 🇪🇸 **西班牙语界面** — 本地设备设置界面新增完整西班牙语翻译，与现有的中英文并列。
- 🛠️ **N16R8 开发板适配 + PlatformIO/pioarduino 构建** — `application/edge_agent/platformio.ini` 支持通过 VS Code 的 pioarduino 扩展针对 ESP32-S3 N16R8 DevKitC-1（16MB Flash，八线 PSRAM）进行构建、烧录与串口监视，作为纯 `idf.py` 流程之外的另一种选择。
- 🔐 **默认安全加固** — 本地配置/状态/文件 HTTP API 现在需要 HTTP Basic Auth（用户名 `admin` + 密码）验证；配网 Wi-Fi AP 也不再允许开放无密码，始终回退为 WPA2-PSK 密码。**两者出厂默认均为 `KrakBotNano2026`——由于该默认值已写入源码仓库，请务必在首次启动时通过网页配置向导修改。**

## 🌟核心特性

传统 IoT 只停留在连接层——设备能联网，却不能思考；能执行，却不能决策。ESP-Claw 将 Agent Runtime 下沉至乐鑫芯片，让芯片从被动的"执行端"转变为主动的"决策中心"。

<table align="center">
  <tr>
    <th><div align="center"> 💬 聊天造物 </div></th>
    <th><div align="center"> ⚙️ 事件驱动 </div></th>
  </tr>
  <tr>
    <th>
      <div align="center">
        IM 聊天 + Lua 动态加载
        <br />
        普通用户即可定义设备行为，无需编程
      </div>
    </th>
    <th>
      <div align="center">
        任意事件可触发 Agent Loop 等动作
        <br />
        最快毫秒级响应
      </div>
    </th>
  </tr>
  <tr>
    <th width="45%">
      <video src="https://github.com/user-attachments/assets/9ec020c2-d133-4ab5-adaa-28817415bc26" />
    </th>
    <th width="45%">
      <video src="https://github.com/user-attachments/assets/c3613e52-61a8-49c2-a224-4a436a4c9e3e" />
    </th>
  </tr>

  <tr>
    <td colspan="2"><!-- 分隔行 --></td>
  </tr>

  <tr>
    <th><div align="center"> 🧬 结构化记忆 </div></th>
    <th><div align="center"> 📤 MCP 通讯 </div></th>
  </tr>
  <tr>
    <th>
      <div align="center">
        有条理地沉淀记忆内容
        <br />
        隐私不上云
      </div>
    </th>
    <th>
      <div align="center">
        支持连接标准 MCP 设备
        <br />
        具备 Server/Client 双重身份
      </div>
    </th>
  </tr>
  <tr>
    <th width="45%">
      <video src="https://github.com/user-attachments/assets/1fe6d67c-469d-405d-a6ec-648aa2681b15" />
    </th>
    <th width="45%">
      <video src="https://github.com/user-attachments/assets/b582ac5b-dd14-486f-b531-495597aa2af6" />
    </th>
  </tr>

  <tr>
    <td colspan="2"><!-- 分隔行 --></td>
  </tr>

  <tr>
    <th><div align="center"> 🧰 开箱即用 </div></th>
    <th><div align="center"> 🧩 组件扩展 </div></th>
  </tr>
  <tr>
    <th>
      <div align="center">
        基于 Board Manager 快速配置
        <br />
        支持一键烧录
      </div>
    </th>
    <th>
      <div align="center">
        所有模块均可按需裁剪
        <br />
        也可自行添加组件适配
      </div>
    </th>
  </tr>
</table>

## 📦快速开始

<div align="center">
  <img src="docs/src/assets/images/claw-breadboard-photo.jpg" width="80%" alt="ESP-Claw on ESP32-S3 Breadboard" />
</div>

ESP-Claw 目前已适配基于 ESP32-S3、ESP32-P4、ESP32-C5、ESP32-S31 的多款开发版，例如面包板、M5Stack CoreS3 等。[已支持的开发版](./application/edge_agent/boards/)可以直接在线烧录：通过网页即可完成配置与烧录，无需额外编译固件或安装开发环境。

<div align="center">
  <a href="https://esp-claw.com/zh-cn/flash/">
    <img src="./docs/static/flash-via-browser-button.svg" width="200" />
  </a>
</div>

你也可以在本地编译 ESP-Claw。请参考[本地编译文档](https://esp-claw.com/zh-cn/tutorial/)适配、编译和烧录。未列于上述列表的开发版、ESP32-P4 等芯片还可通过本地编译烧录实现。

我们的[文档](https://esp-claw.com/zh-cn/tutorial/)中有可供参考的使用样例。

### 支持的平台

<div align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./docs/static/claw-providers-white.webp" />
    <source media="(prefers-color-scheme: light)" srcset="./docs/static/claw-providers-black.webp" />
    <img alt="Supported Platforms" src="./docs/static/claw-providers-black.webp" width="90%" />
  </picture>
</div>

**LLM**: ESP-Claw 现已支持 OpenAI 风格 API 和 Anthropic 风格 API，原生支持 OpenAI 提供的 GPT 系列模型、阿里云百炼提供的 Qwen 系列模型、Anthropic 提供的 Claude 系列模型、DeepSeek 官方 API 提供的 DeepSeek 模型等，也可以自定义 Endpoint。

> [!TIP]
>
> ESP-Claw 的自编程能力需要调用工具和遵循指令能力较强的模型，推荐使用 `gpt-5.4`、`qwen3.6-plus`、`claude4.6-sonnet`、`deepseek-v4-pro` 或类似性能的模型。

**IM**: ESP-Claw 支持 Telegram、QQ、飞书、微信四大聊天软件，并可扩展。

## 🔧开发计划

ESP-Claw 目前仍处于活跃开发阶段，欢迎向我们提交 Issue 反馈问题或提交 Feature 请求。也可以通过[在线问卷](https://fcn5wbhnyubf.feishu.cn/share/base/form/shrcndYcjbGFY1ymttTSyYoGIPh)告诉我们你的想法。

[点此查看我们的 TODO List](https://fcn5wbhnyubf.feishu.cn/wiki/SRlgwWUYei4WmykU8uMcUtzTnFf?table=tblWSgzWcyW7jv7B&view=vewaP9B0KX)，为你心仪的 Feature / 关注的 Issue 投票，我们可以更早实现/修复！

## 📷关注我们

如果这个项目对您有所帮助，欢迎点亮一颗星！⭐⭐⭐⭐⭐

### Star History

<div align="center">
  <a href="https://www.star-history.com/?repos=espressif%2Fesp-claw&type=date&legend=top-left">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=espressif/esp-claw&type=date&theme=dark&legend=top-left" />
      <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=espressif/esp-claw&type=date&legend=top-left" />
      <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=espressif/esp-claw&type=date&legend=top-left" />
    </picture>
  </a>
</div>

## 致谢

灵感来自 [OpenClaw](https://github.com/openclaw/openclaw)。

Agent Loop 和 IM 通讯等功能在 ESP32 上的实现参考了 [MimiClaw](https://github.com/memovai/mimiclaw)。
