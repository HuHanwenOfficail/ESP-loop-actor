以下是 ESP-Loop-Actor 项目的 JSON 配置文件完整规则，适用于 v2.0 及以上版本固件。

```markdown
# ESP-Loop-Actor JSON 配置文件规范 v2.0

## 顶层结构

```json
{
  "meta": { /* 全局元数据 */ },
  "steps": [ /* 步骤数组 */ ]
}
```

## `meta` 元数据对象

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 否 | 流程名称（仅用于日志） |
| `version` | string | 否 | 配置文件版本号 |
| `base_resolution` | object | **是** | 基准分辨率，用于坐标映射 |
| `base_resolution.width` | int | **是** | 基准宽度（如 1920） |
| `base_resolution.height` | int | **是** | 基准高度（如 1080） |
| `default_pre_delay_ms` | int | 否 | 默认动作前延时（毫秒），未填时默认为 20 |
| `default_post_delay_ms` | int | 否 | 默认动作后延时（毫秒），未填时默认为 80 |
| `loop` | object | 否 | 循环设置 |
| `loop.enabled` | bool | 否 | 是否启用循环，默认 `true` |
| `loop.reset_position` | object | 否 | 每轮循环结束后的复位坐标（逻辑坐标） |
| `loop.reset_position.x` | int | 否 | 默认 -10（会被钳位为 0） |
| `loop.reset_position.y` | int | 否 | 默认 -10（会被钳位为 0） |
| `loop.reset_position_after_each_loop` | bool | 否 | 每次循环后是否自动复位，默认 `true` |

> 当 `loop.enabled = true` 且 `reset_position_after_each_loop = true` 时，固件会在步骤列表末尾自动追加一个复位动作（`LOOP_RESET`），将鼠标移至复位坐标。

## `steps` 步骤数组

每个步骤是一个对象，通用字段如下：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `id` | int | 否 | 步骤编号（仅日志） |
| `name` | string | 否 | 注释名称 |
| `pre_delay_ms` | int | 否 | 执行本动作前的等待时间（ms），未填则继承 `default_pre_delay_ms` |
| `post_delay_ms` | int | 否 | 执行本动作后的等待时间（ms），未填则继承 `default_post_delay_ms` |
| `ui_hint` | string | 推荐 | 显示在屏幕第 3 行的动作提示（仅支持 ASCII 字符） |
| `action` | object | **是** | 具体动作定义，必须包含 `type` 字段 |

## 动作类型 (`action`) 详细定义

### 1. 鼠标移动 `mouse_move`

```json
"action": {
  "type": "mouse_move",
  "x": 200,          // 逻辑 X 坐标（可为负数，钳位为 0）
  "y": 400,          // 逻辑 Y 坐标（可为负数，钳位为 0）
  "jitter_enabled": true,      // 可选，是否启用轨迹抖动，默认 false
  "jitter_strength": 12,       // 抖动强度（分段数），默认 10
  "h_min": -15, "h_max": 15,  // 水平随机偏移范围（像素），默认 -5 ~ 5
  "v_min": -10, "v_max": 10   // 垂直随机偏移范围（像素），默认 -5 ~ 5
}
```

- **坐标映射**：物理坐标 = `(逻辑坐标 × 实际分辨率) / 基准分辨率`，负数钳位为 0，最大 65535。
- **平滑移动**：默认采用 32 步线性插值移动（若无抖动），每步约 8ms，移动过程肉眼可见。
- **抖动**：启用后，移动路径中会随机加入偏移，最终精确到达目标。

### 2. 鼠标点击 `mouse_click`

```json
"action": {
  "type": "mouse_click",
  "button": "LEFT",            // "LEFT"、"RIGHT"、"MIDDLE"
  "click_type": "CLICK"        // "CLICK"（单击）、"DOUBLE_CLICK"（双击）、
                               // "PRESS"（只按下）、"RELEASE"（只释放）
}
```

> 点击动作在当前鼠标位置执行（不改变坐标）。

### 3. 鼠标拖拽 `mouse_drag`

```json
"action": {
  "type": "mouse_drag",
  "start": { "x": 100, "y": 100 },   // 起点逻辑坐标
  "end": { "x": 300, "y": 300 },      // 终点逻辑坐标
  "button": "LEFT"                    // 按下哪个键拖拽
}
```

> 在起点按下指定键，平滑移动至终点，释放按键。

### 4. 键盘输入 `keyboard_input`

有两种模式：从文件读取（`"file"`）或直接字符串（`"literal"`）。

#### 4.1 从文件读取

```json
"action": {
  "type": "keyboard_input",
  "input_mode": "file",
  "line_index": "AUTO_INCREMENT"   // 固定行号 (int) 或 "AUTO_INCREMENT"
}
```

- `line_index` 为 `"AUTO_INCREMENT"` 时：自动使用全局当前行号读取 `data.txt` 对应行，输入后行号 +1 并存入 NVS（断电续传）。
- 若为整数（如 `1`）：固定读取指定行（1‑based）。

#### 4.2 直接字符串输入

```json
"action": {
  "type": "keyboard_input",
  "input_mode": "literal",
  "literal_text": "Hello, World!"
}
```

#### 4.3 键盘随机延迟（可选）

可在键盘输入动作中添加随机延迟参数，使每个字符间隔在指定范围内随机变化：

```json
"random_delay_min_ms": 30,
"random_delay_max_ms": 80
```

> 字符间等待时间在 `[min, max]` 范围内随机选取，未设置则默认 5ms。

### 5. 组合快捷键 `keyboard_hotkey`

```json
"action": {
  "type": "keyboard_hotkey",
  "modifiers": ["CTRL", "SHIFT"],   // 修饰键数组（CTRL/SHIFT/ALT/GUI）
  "key": "C"                        // 主键（单个字符，如 A、TAB、ESC 等）
}
```

> 发送按键组合，例如 `Ctrl+Shift+C`。

### 6. 纯延时 `wait`

```json
"action": {
  "type": "wait",
  "duration_ms": 1000   // 等待毫秒数
}
```

## 内部处理逻辑

1. **解析流程**：
   - 读取 JSON 文件，使用 cJSON 解析。
   - 提取 `meta` 中的基准分辨率、默认延时、循环设置。
   - 遍历 `steps` 数组，根据 `action.type` 构建内部指令表（`step_t` 结构体）。
   - 若 `loop.enabled = true` 且 `reset_position_after_each_loop = true`，自动在指令表末尾追加 `ACT_LOOP_RESET`，坐标取自 `loop.reset_position`。

2. **执行引擎**：
   - 维护全局行号 `current_line`（从 NVS 读取，断电续传）。
   - 遇到 `keyboard_input` 且 `line_index = 0xFFFF`（即 `"AUTO_INCREMENT"`）时，使用当前行号读取 `data.txt`，输入后行号 +1 并写 NVS。
   - 遇到 `LOOP_RESET` 时，平滑移动至复位坐标（物理 0,0）。

3. **坐标映射**：
   - 逻辑坐标 → 物理坐标：`phys = (log * current_display) / base`。
   - 负数钳位为 0，超过 65535 钳位至 65535。

4. **移动平滑**：
   - 鼠标移动和拖拽均采用线性插值分段发送，默认 32 步、每步 8ms，可观察。
   - 若启用抖动，步数由 `jitter_strength` 决定，中间加入随机偏移。

5. **按键模式**：
   - **单击 BOOT** → STEP 模式，执行一次完整步骤序列后停止，行号 +1。
   - **双击 BOOT**（500ms 内）→ AUTO 模式，连续循环执行，直至行号超出总行数或用户干预。
   - **长按 BOOT**（>2 秒）→ RESET，立即停止所有动作，行号归 1，NVS 清零，鼠标复位至 (0,0)。
   - **三击 BOOT** → 临时显示网络信息（Wi‑Fi SSID、密码、IP 地址），持续 4 秒后恢复。

## 示例 JSON

以下是一个包含所有动作类型的完整示例（配合 Web 编辑器可图形化生成）：

```json
{
  "meta": {
    "name": "全功能测试",
    "version": "2.0",
    "base_resolution": { "width": 1920, "height": 1080 },
    "default_pre_delay_ms": 20,
    "default_post_delay_ms": 100,
    "loop": {
      "enabled": true,
      "reset_position": { "x": -10, "y": -10 },
      "reset_position_after_each_loop": true
    }
  },
  "steps": [
    {
      "id": 1,
      "ui_hint": "平滑移动至400,300",
      "action": { "type": "mouse_move", "x": 400, "y": 300 }
    },
    {
      "id": 2,
      "ui_hint": "左键双击",
      "action": { "type": "mouse_click", "button": "LEFT", "click_type": "DOUBLE_CLICK" },
      "post_delay_ms": 200
    },
    {
      "id": 3,
      "ui_hint": "抖动拖拽至800,600",
      "action": {
        "type": "mouse_drag",
        "start": { "x": 400, "y": 300 },
        "end": { "x": 800, "y": 600 },
        "button": "LEFT"
      }
    },
    {
      "id": 4,
      "ui_hint": "从文件输入当前行(随机延迟)",
      "action": {
        "type": "keyboard_input",
        "input_mode": "file",
        "line_index": "AUTO_INCREMENT",
        "random_delay_min_ms": 40,
        "random_delay_max_ms": 100
      }
    },
    {
      "id": 5,
      "ui_hint": "输入回车",
      "action": { "type": "keyboard_input", "input_mode": "literal", "literal_text": "\n" }
    },
    {
      "id": 6,
      "ui_hint": "Ctrl+C 复制",
      "action": { "type": "keyboard_hotkey", "modifiers": ["CTRL"], "key": "C" }
    },
    {
      "id": 7,
      "ui_hint": "等待1秒",
      "action": { "type": "wait", "duration_ms": 1000 }
    }
  ]
}
```

> 配套的 `data.txt` 每行一条文本，行数可大于 1。设备将自动循环执行上述步骤，每次从下一行读取数据输入。长按 BOOT 键可重置行号。屏幕实时显示进度、当前动作和模式。
```