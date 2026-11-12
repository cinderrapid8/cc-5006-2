# 分段速度曲线：验收说明与运行指南

本轮把体育慢放工具从单一恒定速度扩展为**分段速度曲线**。曲线定义在
“源时间 → 输出时间”的映射上，支持正常进入、急剧减速、静帧停留后再加速退出。

## 1. 设计要点

### 时间映射（`src/core/SpeedCurve.{h,cpp}`）

- 曲线由有序控制点 `SpeedKnot{ timeUs, speed, freezeDurationUs }` 构成，时间单位微秒。
- 普通段在源区间 `[timeUs, next.timeUs)` 上以局部速度 `speed` 播放，映射斜率
  `ds/du = 1/speed`。段长度按整数微秒计算（`llround`），边界可逐帧精确检查。
- `freezeDurationUs > 0` 是**唯一允许的非严格单调部分**：源时间停在该控制点，
  输出时间继续推进，在逆映射上表现为 `speed == 0` 的静帧段。
- 除静帧段外，`sourceToOutput` / `outputToSource` 均保持单调；帧规划器另加
  整数舍入钳制，保证后一输出帧的源位置不会回退。

### 拒绝与未支持模式（`SpeedCurve::validate`）

下列情况返回 `false` 并给出原因，而不是产生未定义结果：

| 输入 | 处理 |
| --- | --- |
| 负速度 | 拒绝（`rejected speed`） |
| 零速度 / 极小速度导致无限时长 | 拒绝；需要停顿请改用显式 `freezeDurationUs` |
| 控制点重合或逆序 | 拒绝（`coincide ... not ordered`） |
| 曲线未锚定区间两端 | 拒绝 |
| 输出时长非有限或超过 1 小时上限 | 拒绝（不支持无限时长曲线） |

合法速度范围 `[1e-4, 100]`，控制点源时间最小间隔 1 µs。

### 预览与导出共用同一映射（`src/core/FramePlanner.{h,cpp}`）

- `FramePlanner::plan(...)` 是唯一的取帧入口：界面预览缓存与 `Exporter` 渲染
  调用的是同一个函数、同一条 `SpeedCurve`，不存在第二套近似。
- 取样始终使用解码器建立的**真实每帧 PTS 索引**（VFR 按时间戳取样），
  平均帧率仅作展示，绝不参与时间计算。
- 每帧记录来源 `FrameProvenance`：`original | repeat | freeze | interpolated`，
  含 `sourceFrameA/B`、混合权重、源时间、所属曲线段与局部速度。
- 重复帧与插值帧都保留来源；插值帧同时记录相邻两源帧及权重。

### 恒定慢放成为特例

- `SpeedCurve::constant(speed, span)` 生成单速度两段曲线。
- 旧的 `ExportSettings.speed`（`hasCurve=false`）在导出时包装为恒定曲线，
  与历史实现逐帧一致（由 `constantCurveIsExactLegacySpecialCase` 锁定）。
- 项目文件升级到 `formatVersion=2`；v1 旧项目仍可加载，不含曲线字段，
  重开后恒定慢放的时长与取帧结果不变（`legacyV1ProjectLoadsAndRendersUnchanged`）。

### 音频（`src/core/Exporter.cpp`）

- **保持音高**使用真实支持该能力的组件：libavfilter `atempo` 滤镜
  （低于 0.5 的系数自动级联多个 `atempo=0.5`）。
- 每个曲线段建立**独立滤镜图**并在段末发送 EOF 完全排空，使算法延迟
  （algorithmic delay）不会跨越速度变化点或静帧边界。
- 不保持音高的 `Drop` 模式按段重采样（`swresample`），音高随速度变化。
- 静帧段音频政策独立设置 `AudioFreezePolicy`：`silence`（静音）或
  `hold-last-sample`（保持最后一个采样），写入 sidecar。

## 2. 构建与测试

工具链位于仓库内用户态 sysroot（`.sysroot/root`，已被 `.gitignore` 忽略）。

```bash
source scripts/env.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

测试组成：

- `build/test_core`：21 个核心用例，含本轮新增 12 个曲线验收用例。
- `build/test_ui`：8 个界面用例，含曲线编辑器与对话框持久化。

## 3. 极窄减速区间的逐帧核对（本轮重点验收例）

### 自动化

`CoreTest::cliAcceptanceNarrowBandMappingChecksFrameByFrame` 与
`CoreTest::narrowDecelerationBandIsSampledFromRealVfrTimestamps` 自动执行下列路径。

### 手动复现

```bash
source scripts/env.sh
export QT_QPA_PLATFORM=offscreen

# 1) 生成带非均匀 PTS（VFR）与音轨的合成片段
build/srt_genclip -o /tmp/vfr.mkv -n 24 --audio

# 2) 极窄减速：源帧 5→6 之间只有一个真实帧间隙（约 50 µs），
#    在该间隙内以 0.02 倍速急剧减速，随后静帧 150 ms，再加速退出
build/srt_curve_demo --input /tmp/vfr.mkv --output /tmp/narrow.mkv \
    --fps 120 --slow-a 5 --slow-b 6 --freeze-us 150000 \
    --audio-mode atempo --freeze-audio hold

# 3) 容器级核对：实际 PTS 与 sidecar 逐帧一致
build/srt_probe verify /tmp/narrow.mkv

# 4) 映射级核对：分段结构、单调性、每一输出帧对应的源时间/源帧
build/srt_probe mapping /tmp/narrow.mkv.frames.json

# 5) 任意输出时刻反查源时间
build/srt_probe mapping /tmp/narrow.mkv.frames.json --at-us 500000
```

期望结果（以 24 帧 VFR 样例为例）：

- `mapping` 输出 4 个逆映射段：进入（speed=1）、极窄减速（speed=0.02）、
  静帧（speed=0，`srcStart==srcEnd`）、加速退出。
- `sourceTimeMonotone = true`。
- 源区间 `[270000, 320000)` µs 这个仅 50 µs 宽的间隙，在输出侧展开为
  2.5 秒、约 177 帧，全部按真实 PTS 落在该间隙内。
- 静帧段产生 18 帧（150 ms @120 fps），全部锁定同一个源帧。
- `verify` 报告 `ok=true, ptsMismatches=0, hasAudio=true`。

## 4. 其他边界用例对应关系

| 验收点 | 测试 |
| --- | --- |
| 急剧减速仍保持单调 | `mappingStaysMonotoneOutsideExplicitFreezes` |
| 两控制点几乎重合（1 µs） | `nearlyCoincidentControlPointsRemainFiniteAndOrdered` |
| 曲线恰好落在源帧间隙 | `narrowDecelerationBandIsSampledFromRealVfrTimestamps` |
| 负速度 / 零速度 / 无限时长被拒绝 | `curveRejectsNegativeZeroAndInfiniteSpeeds` |
| 静帧帧来源记录 | `freezeSegmentMarksFramesAndKeepsOriginRecord` |
| 预览规划 == 导出取帧 | `curvePreviewPlanEqualsExportedFrameSelection` |
| atempo 真实音高保持 + 静帧政策 | `segmentedAtempoAudioUsesRealFilterAndFreezePolicy` |
| 旧项目重开时长/取帧不变 | `legacyV1ProjectLoadsAndRendersUnchanged` |
| 曲线项目往返时序一致 | `savedProjectReopensWithIdenticalCurveTiming` |
| 恒定速度是曲线特例且逐帧等价 | `constantCurveIsExactLegacySpecialCase` |

## 5. 界面操作

导出对话框新增 “Segmented speed curve” 标签页：

- 拖动曲线上的控制点沿源时间轴移动（首尾锚点固定）；菱形标记表示含静帧停留。
- 表格可编辑每点之后的速度与该点静帧时长；非法输入时实时显示原因并禁止导出。
- 每次编辑后底部显示**实际输出长度**（微秒/秒/相对源时长倍数），该数值与
  导出器使用同一映射计算。
- 取消勾选曲线复选框即回到历史恒定慢放特例。
