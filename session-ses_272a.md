# 查询已安装的两个MCP

**Session ID:** ses_272a8d6e0ffeYRVJzYpGVpj3aN
**Created:** 2026/4/15 02:53:12
**Updated:** 2026/4/15 02:53:45

---

## User

现在装了哪两个mcp?

---

## Assistant (Build · GLM-5.1 · 2.6s)

_Thinking:_

The user is asking about which two MCP (Model Context Protocol) servers are installed. Let me check the project structure to find MCP configuration.

**Tool: glob**

**Input:**
```json
{
  "pattern": "**/.claude/**"
}
```

**Output:**
```
No files found
```

**Tool: glob**

**Input:**
```json
{
  "pattern": "**/mcp*"
}
```

**Output:**
```
No files found
```

**Tool: glob**

**Input:**
```json
{
  "pattern": "**/.cursor/**"
}
```

**Output:**
```
No files found
```

---

## Assistant (Build · GLM-5.1 · 15.5s)

**Tool: glob**

**Input:**
```json
{
  "pattern": "**/*mcp*"
}
```

**Output:**
```
No files found
```

**Tool: glob**

**Input:**
```json
{
  "pattern": "**/.opencode*"
}
```

**Output:**
```
No files found
```

---

## Assistant (Build · GLM-5.1 · 14.5s)

根据当前环境信息，我看到工具列表中有两个 MCP 工具集：

1. **Tavily MCP** — 提供 `tavily_search`、`tavily_extract`、`tavily_crawl`、`tavily_map`、`tavily_research`、`tavily_skill` 等工具（用于网页搜索、抓取、研究等）

2. **Syslab MCP** — 提供 `syslab_evaluate_julia_code`、`syslab_run_julia_file`、`syslab_detect_syslab_toolboxes`、`syslab_restart_julia`、`syslab_read_syslab_skill`、`syslab_search_syslab_docs`、`syslab_read_syslab_doc`、`syslab_map_matlab_functions_to_julia` 等工具（用于 Julia/Syslab 运行时和文档查询）

---

