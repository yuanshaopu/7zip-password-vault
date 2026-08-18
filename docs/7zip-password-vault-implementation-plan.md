# 7-Zip 密码箱 + 批量解压自动测试 + 成功后删除压缩包

## 实现方案（基于 7-Zip 26.02 官方源码）

> 本文档为“自用 fork”级别的修改方案：不直接构建，只给出每步怎么改、改哪里、为什么。
> 许可证提醒：7-Zip 为 LGPL，自编译自用不受限制；若分发需遵守 LGPL 条款。

---

## 1. 功能需求（已确认）

| 编号 | 功能 | 说明 |
|---|---|---|
| F1 | 密码箱 | 可存放、删除密码；支持显示/隐藏；数据加密存储 |
| F2 | 批量解压按格式分组自动测试 | 一次批量解压中，相同格式的压缩包只选一个代表包，用密码箱候选依次测试；命中后该密码作为整组密码 |
| F3 | 组密码失效时重新测试 | 组内其它包用组密码解压失败时，对该包单独重新测试密码箱（策略 B，已确认） |
| F4 | 解压成功后自动删除压缩包 | 可选开关：解压成功才删除，失败保留 |

### F2 的精确流程

1. 用户一次选中多个压缩包执行解压（7-Zip 原版即支持批量，走 `UI\Common\Extract.cpp` 的 `Extract()` 循环）。
2. 按**检测到的格式**（`FormatIndex`，不是扩展名）分组。
3. 每组选**一个代表包**：
   - 打开成功后，扫描条目找第一个 `kpidEncrypted = true` 的条目，对该条目做**单文件测试解压**（测试模式、不写盘）；
   - 加密文件头的 7z 读不出列表，则以**逐个候选试打开**作为测试；
   - 组内无任何加密条目的包不参与测试。
4. 候选全部失败 → 正常弹原版密码框。

---

## 2. 已核实的源码事实（设计依据）

| 事实 | 位置 | 影响 |
|---|---|---|
| 批量解压是单次调用 `Extract()` 内循环多个压缩包 | `CPP/7zip/UI/Common/Extract.cpp`（`Extract()`，按 `arcPaths` 循环） | 分组缓存、代表测试、删除逻辑都集中在此函数，一处改动全端生效 |
| 打开后能拿到检测格式与解压对象 | `arcLink.Arcs.Back().FormatIndex` / `.Archive` | 分组键用真实格式 |
| 每个条目有加密标志 | `kpidEncrypted` + 现成工具 `Archive_GetItemBoolProp()` | 可预先定位“第一个加密条目”，无需试错 |
| 引擎要密码是“拉取式、不重试” | `CryptoGetTextPassword` 只被调用一次/加密文件夹；错误表现为数据/CRC 错误 | 自动测试必须在引擎外做“试跑”，不能指望引擎重试 |
| 注入密码即可免弹框 | `CryptoGetTextPassword` 逻辑为 `if (!PasswordIsDefined) { 弹框 }` | 试测时设置 `PasswordIsDefined = true; Password = 候选` 即可 |
| 原版解压对话框已有密码框 | `IDD_EXTRACT` 中 `IDE_EXTRACT_PASSWORD`（`ExtractDialog.rc`） | 自动测试开关可放在此对话框；用户预填密码时优先于密码箱 |
| 原版**没有**“解压后删除压缩包” | 全 UI 树仅有压缩侧 `DeleteAfterCompressing` | F4 为全新功能 |
| 回调自带重置/成功判定 | `CExtractCallbackImp::Init()`、`IsOK()` | 每次试测前重置计数，用 `IsOK()` 判断候选是否命中 |

---

## 3. 总体架构

```
┌─ UI 层（7zFM / 7zG）─────────────────────────────┐
│ 密码箱对话框（新）    ExtractDialog（加两个开关） │
└──────────────┬──────────────────────────────────┘
┌─ 公共解压层 UI\Common\Extract.cpp ────────────────┐
│ 分组缓存 / 代表包测试 / 组密码注入 / 成功后删除   │
└──────┬──────────────────────────────┬────────────┘
┌──────▼──────────┐           ┌────────▼──────────┐
│ 回调层           │           │ 数据层            │
│ ExtractCallback  │           │ ZipRegistry       │
│ OpenCallback     │           │ + DPAPI 加密      │
└─────────────────┘           └───────────────────┘
```

各模块职责：

| 模块 | 文件 | 职责 |
|---|---|---|
| 数据层 | `UI/Common/ZipRegistry.h/.cpp` | 密码箱读写（DPAPI）、两个开关的持久化 |
| 密码箱 UI | `UI/FileManager/PasswordVaultDialog.*`（新建） | 增删、显示隐藏 |
| 解压对话框 | `UI/GUI/ExtractDialog.*` | 新增“自动尝试密码箱”“成功后删除压缩包”复选框 |
| 选项结构 | `UI/Common/Extract.h` | `CExtractOptions` 加字段 |
| 回调层 | `UI/FileManager/ExtractCallback.h/.cpp`、`OpenCallback.h/.cpp` | 试测状态、错误静默、候选注入 |
| 核心逻辑 | `UI/Common/Extract.cpp` | 分组、代表测试、删除 |

---

## 4. 详细实现步骤

### 4.1 数据层：密码箱（F1 的存储）

**文件**：`UI/Common/ZipRegistry.h/.cpp`

新增两个函数，存储到注册表 `HKCU\Software\7-Zip\PasswordVault`：

```cpp
// ZipRegistry.h
void Save_PasswordVault(const UStringVector &passwords);   // DPAPI 加密后写 REG_BINARY
void Read_PasswordVault(UStringVector &passwords);         // 读 REG_BINARY 并解密
```

实现要点：

- 把整个 `UStringVector` 序列化为 UTF-16 多字符串缓冲区，`CryptProtectData(..., CRYPTPROTECT_UI_FORBIDDEN, ...)` 加密后存入 `REG_BINARY`；读取时 `CryptUnprotectData` 解密再拆回。
- 需要链接 `crypt32.lib`（若工程未链）。
- 排序约定：最新使用的排最前（由写入方负责），读取方只负责还原。

为什么 DPAPI 而不是明文：密码箱是专门存放密码的地方，明文风险显著高于“历史记录”；DPAPI 无需额外密码、绑定当前 Windows 用户，成本低、收益直接。若第一阶段想先跑通，可先明文 `REG_MULTI_SZ`，但接口保持上面两个函数，后续替换实现不影响其它代码。

### 4.2 密码箱 UI（F1 的界面）

**新建文件**：`UI/FileManager/PasswordVaultDialog.h/.cpp/.rc/.res.h`

- 仿照现有 `ListViewDialog` 模式：一个列表（`SysListView32`）+ 按钮。
- 列表项默认显示 `****`；对话框内放“显示密码”复选框，勾选后切换明文（复用现有 `IDX_PASSWORD_SHOW` 机制）。
- 按钮：添加（弹一个输入框，可复用 `EditDialog`）、删除选中项、清空全部；删除/清空需确认。
- 数据流：打开时 `Read_PasswordVault()` 填充列表；任何修改后 `Save_PasswordVault()` 写回。
- 菜单入口：7zFM 主菜单新增“工具 → 密码箱”（菜单资源 + `App.cpp` 命令分发），命令 ID 在 `resource.h` 新增。

**注意**：新对话框的 `.rc` 需要被 `UI/FileManager/resource.rc` 或对应资源编译清单包含，否则资源不会进入 exe。

### 4.3 解压对话框新增两个开关（F4 + 自动测试总开关）

**文件**：`UI/GUI/ExtractDialog.rc`、`ExtractDialogRes.h`、`ExtractDialog.cpp`、`UI/Common/Extract.h`

1. `ExtractDialog.rc` 的 `IDD_EXTRACT` 内新增两个复选框：
   - `IDX_EXTRACT_TRY_VAULT`：“解压时自动尝试密码箱密码”
   - `IDX_EXTRACT_DELETE_AFTER_OK`：“解压成功后删除压缩包”
2. `CExtractOptions`（`UI/Common/Extract.h`）加两个字段：

```cpp
bool TryPasswordVault;
bool DeleteAfterExtract;
```

3. `ExtractDialog::OnOK()` 把两个复选框状态写入 `options`（与现有 `ElimDup` 等字段同一模式）；`OnInit()` 从注册表读上次状态（持久化函数放在 `ZipRegistry.cpp` 的 `NExtract` 区，仿照 `Save_ShowPassword`）。
4. 选项传递链：`ExtractDialog` → `ExtractGUI` → `CThreadExtracting::ProcessVirt` → `Extract(...)`，因为字段加在 `CExtractOptions` 上，整条链无需改动。

### 4.4 回调层：试测状态与错误静默

**文件**：`UI/FileManager/ExtractCallback.h/.cpp`、`OpenCallback.h/.cpp`

两个回调类各加：

```cpp
bool PasswordTryMode;      // 正在试密码：错误消息静默、不弹框
UStringVector PasswordCandidates;
unsigned PasswordTryIndex;
```

行为修改：

- `CryptoGetTextPassword`（`ExtractCallback.cpp`）：现有 `if (!PasswordIsDefined) { 弹框 }` **一行不改**。试测时外层已注入 `PasswordIsDefined = true; Password = 候选`，它自然返回注入值。
- 所有报错入口（`AddError_Message`、`MessageError`、`OpenResult`、`Extract_OperationResult` 中的消息展示）：`if (PasswordTryMode) return;` —— 只跳过**展示**，错误计数照常累加，因为判定候选是否命中靠计数。
- `Open_CryptoGetTextPassword`（`ExtractCallback.cpp` 151 行委托给 `CryptoGetTextPassword`）与 `OpenCallback.cpp` 的 `Open_CryptoGetTextPassword` 同样处理。

为什么：7-Zip 引擎不重试密码，错误要到“整轮试跑结束”才能看到；如果试测期间的错误弹窗不静默，用户会看到最多 10 次“数据错误”轰炸。

### 4.5 核心逻辑：`Extract()` 内的分组与代表测试（F2 + F3）

**文件**：`UI/Common/Extract.cpp`

在 `Extract()` 主循环内维护一个**批处理内有效**的缓存：

```cpp
struct CGroupPsw { int FormatIndex; UString Password; bool Found; };
CObjectVector<CGroupPsw> groupCache;   // 每组只存一个结果
```

每个压缩包的处理流程（伪代码）：

```cpp
// 1. 打开（原有代码 arcLink.Open_Strict）
HRESULT openRes = arcLink.Open_Strict(op, openCallback);

// 2. 取分组键
int fmt = arcLink.Arcs.Back().FormatIndex;

// 3. 查组缓存
CGroupPsw *grp = FindGroup(groupCache, fmt);

if (options.TryPasswordVault)
{
  if (!grp->Found && 打开成功 && 需要密码)
  {
    // ---- 该包成为代表包：先测密码，再正式解压 ----
    UStringVector vault; Read_PasswordVault(vault);

    if (加密文件头导致打开失败(openRes != S_OK && arcLink.PasswordWasAsked))
    {
      // 路径 A：逐个候选“试打开”
      for (each pwd in vault)
      {
        openCallback->Password = pwd; openCallback->PasswordIsDefined = true;
        openCallback->PasswordTryMode = true;
        res = arcLink.Open_Strict(op, openCallback);      // 重新打开
        if (res == S_OK) { grp->Password = pwd; grp->Found = true; break; }
      }
    }
    else if (打开成功)
    {
      // 路径 B：找第一个加密条目，单条目测试
      UInt32 firstEncrypted = FindFirstEncrypted(arcLink.Arcs.Back().Archive);
      if (firstEncrypted 存在)
      {
        for (each pwd in vault)
        {
          ecs->Init();  // 重置错误计数（回调层）
          callback->Password = pwd; callback->PasswordIsDefined = true;
          callback->PasswordTryMode = true;
          res = archive->Extract(&firstEncrypted, 1, 1 /*testMode*/, ecs);
          if (res == S_OK && callback->IsOK()) { grp->Password = pwd; grp->Found = true; break; }
        }
      }
    }
  }

  if (grp->Found)
  {
    callback->Password = grp->Password;   // 注入组密码
    callback->PasswordIsDefined = true;
  }
  else
  {
    callback->PasswordIsDefined = false;  // 未命中：允许弹框
  }
}

// 4. 正式解压（原有 ExtractArchive(...)，testMode 由用户选项决定）
result = ExtractArchive(...);

// 5. F3：组密码失效时对该包重新测试
if (options.TryPasswordVault && grp->Found && result != S_OK && 有密码相关错误)
{
  // 该包独立重测：清缓存不生效，只对当前包重新执行“路径 A/B”的候选测试
  // 命中后重跑一次正式解压；未命中则 PasswordIsDefined=false 重跑弹框
}

// 6. F4：成功后删除（见 4.6）
```

关键函数说明：

- `FindFirstEncrypted()`：遍历 `GetNumberOfItems()`，用 `Archive_GetItemBoolProp(archive, i, kpidEncrypted, isEnc)` 找第一个加密条目；找不到返回无效索引。
- 单条目测试必须用 `testMode = 1` 且只传一个索引：只解码校验第一个加密文件、**不写盘**，这是“每组只测一个文件”与性能可控的核心。
- 试测前 `ecs->Init()` 重置错误计数；命中判定 = `res == S_OK && callback->IsOK()`。
- 加密文件头的 7z 读不出列表，只能走“试打开”；`arcLink.PasswordWasAsked` 是判定“失败源于密码”的可靠信号。

### 4.6 成功后删除压缩包（F4）

在 4.5 的第 6 步，满足以下**全部**条件才删除：

```cpp
if (options.DeleteAfterExtract
    && !options.TestMode            // 用户执行“测试”时不删除
    && result == S_OK
    && callback->IsOK()             // 无任何错误（含静默试测期间的错误）
    && !PasswordTryMode             // 绝不在试测阶段删除
    && 未被用户取消)
{
  DeleteArchiveFiles(arcPath, arcLink);   // 主文件 + 全部分卷
}
```

实现细节：

- `IsOK()` 目前在 `CExtractCallbackImp` 具体类上；建议把 `IsOK()` 提升为 `IExtractCallbackUI` 接口的虚方法（该接口已是非 COM 的纯 C++ 接口，改动小），供 `Extract.cpp` 调用。
- `DeleteArchiveFiles`：删除 `arcPath` 以及 `arcLink.VolumePaths` 中所有分卷（多卷压缩包必须整体删除，否则残留分卷无法使用）。
- 删除方式建议先永久删除（`DeleteFile`/`DeleteFileAlways` 风格）；可选升级为“移入回收站”（`SHFileOperation`），对用户更安全。
- 失败保留：任何错误（数据/CRC/打开失败/取消）都不会走到删除分支——这是 F4 的核心保证。

### 4.7 设置持久化

两个开关的状态存注册表 `HKCU\Software\7-Zip\Extraction`（复用 `NExtract` 区）：

- `TryPasswordVault`（bool）
- `DeleteAfterExtract`（bool）

仿照 `Save_ShowPassword / Read_ShowPassword` 提供读写函数，`ExtractDialog::OnInit/OnOK` 调用。

---

## 5. 边界情况与处理策略

| 情况 | 处理 |
|---|---|
| 组内代表包无加密条目 | 顺延到组内下一个含加密条目的包；组内全无加密 → 整组不测试 |
| 加密文件头的 7z | 走“试打开”路径；`PasswordWasAsked` 判定密码失败 |
| ZIP 一包多密码 | 首文件命中后，若后续文件失败 → 走 F3 对该文件重试密码箱（可做文件级扩展）；都不行再弹框 |
| 多卷压缩包 | 删除时删除全部卷；分卷按同一 `arcLink` 处理 |
| 嵌套压缩包（rar 内含 zip） | 分组键取最内层 `Arcs.Back().FormatIndex`（与实际解压对象一致） |
| 密码箱为空 / 开关关闭 | 完全走原版流程，零行为变化 |
| 用户取消 | 不删除当前包；试测状态清理 |
| 删除失败（占用/权限） | 记录错误但不中断后续批次 |
| 文件损坏但密码正确 | 被误判为候选失败（引擎无“密码错误”码）；代价是消耗候选，最终弹框——接受此折衷 |

---

## 6. 测试计划

### 构建

```bat
cd SRC\CPP\7zip
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
nmake
```

产物放独立目录，不覆盖日常安装。

### 用例矩阵

| # | 场景 | 预期 |
|---|---|---|
| T1 | 7z 组两个包，密码不同 | 代表包命中 A；第二个包 F3 重测命中 B；都成功 |
| T2 | 7z 组两个包，密码相同 | 只测第一个包，第二个直接用组密码，无弹框 |
| T3 | 加密文件头的 7z 组 | 走试打开，命中后整组直接解压 |
| T4 | 混合格式（zip + 7z） | 各自分组互不干扰 |
| T5 | 全部候选失败 | 正常弹密码框，无试测期错误弹窗 |
| T6 | 密码箱增删 | 添加后立即参与测试；删除后不再参与 |
| T7 | 成功解压 + 开关开 | 解压结束后压缩包（含分卷）被删除 |
| T8 | 密码错误 / 文件损坏 / 取消 | 压缩包保留 |
| T9 | “测试”模式 | 不删除任何压缩包 |
| T10 | 密码箱开关关 | 完全原版行为 |
| T11 | 预填解压对话框密码 | 优先使用预填密码，跳过密码箱测试 |

---

## 7. 风险与安全

- **明文密码风险**：密码箱必须 DPAPI 加密；本方案默认直接加密，不做明文版。
- **自动试密码的语义**：逐候选解密尝试仅限用户自己的密码箱，量级（≤ 建议上限 10 条）不构成暴力破解，但存储侧安全仍由 DPAPI 兜底。
- **自动删除风险**：删除是破坏性操作，必须满足“成功 + 非测试 + 非试测 + 未取消”全部条件；建议 MVP 阶段默认**关闭**该开关。
- **误判风险**：引擎不返回“密码错误”码，文件损坏会被当作候选失败；方案接受此折衷，若需精确区分需自行解码首文件并比对校验值（列为可选增强）。

---

## 8. 里程碑（建议 MVP 拆分）

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M1 | 数据层 + 密码箱 UI（F1） | 增删/显隐/持久化正常，重启后仍在 |
| M2 | 批量解压分组自动测试（F2 + F3） | T1–T6 通过 |
| M3 | 成功后删除开关（F4） | T7–T10 通过 |
| M4 | 可选增强：回收站删除、文件级 ZIP 重测、精确密码校验 | 按需 |

---

## 9. 默认已拍板的决定（如有异议请指出）

1. 代表包 = 组内第一个“含加密条目”的包；加密文件头包天然合格。
2. 组密码失效 → 对该包单独重新测试（策略 B）。
3. 密码箱上限：读取时截断为 10 条（与之前“最多 10 个”一致）。
4. 存储：DPAPI 加密 REG_BINARY。
5. 范围：先做 7zFM 文件管理器路径；7zG 共享公共层代码，改造量小，M2 后自然覆盖。
6. 删除开关默认关闭，用户在解压对话框中勾选启用。

