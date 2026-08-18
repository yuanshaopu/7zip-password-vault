# 7-Zip Password Vault

基于 7-Zip 26.02 官方源码的自用 fork，在原有批量解压基础上新增三个功能：

## 功能

1. **密码箱（Password Vault）**
   - 工具 → 密码箱，可添加/删除/清空常用解压密码（上限 10 条）；
   - 数据经 Windows DPAPI 加密后存入注册表 `HKCU\Software\7-Zip\PasswordVault`，不存明文。

2. **批量解压按格式分组自动试密码**
   - 勾选“解压时自动尝试密码箱密码”后，同格式的压缩包只测一个代表包（对首个加密条目做 testMode 单文件测试，不写盘），命中后整组复用；
   - 组内某包密码不同导致解压失败时，对该包单独重新测试密码箱（F3）；
   - 加密文件头的 7z 通过逐个候选“试打开”匹配，全失败才弹原版密码框。

3. **解压成功后自动删除压缩包**
   - 勾选“解压成功后删除压缩包”后，解压成功才删除（含分卷）；失败、测试模式、取消均保留；
   - 每包独立判定：批处理中前面有包失败，不影响后面成功包的删除。

## 构建（Windows x86）

在 **x86 开发者命令提示符** 下依次构建：

```bat
cd CPP\7zip\Bundles\Fm
nmake O=o

cd ..\..\UI\GUI
nmake O=o

cd ..\..\Bundles\Format7zF
nmake O=o
```

把 `Bundles\Fm\o\7zFM.exe`、`UI\GUI\o\7zG.exe`、`Bundles\Format7zF\o\7z.dll` 放到同一目录运行。

> 注意：修改任何 `.rc` 资源文件后，需先删除对应 `o\` 目录下的 `resource.res` 再 nmake，否则资源不会更新（nmake 不追踪 .rc 的 include 依赖）。

## 主要改动文件

- `CPP/7zip/UI/Common/Extract.cpp` — 分组缓存、代表包测试、F3 重测、成功后删除
- `CPP/7zip/UI/Common/Extract.h` — `CExtractOptions` 新增两个开关
- `CPP/7zip/UI/Common/ZipRegistry.cpp/.h` — 密码箱 DPAPI 读写、开关持久化
- `CPP/7zip/UI/FileManager/PasswordVaultDialog.*` — 密码箱对话框（新增）
- `CPP/7zip/UI/FileManager/ExtractCallback.*` — 试测状态、错误静默、批次错误计数
- `CPP/7zip/UI/GUI/ExtractDialog.*` — 两个复选框
- `CPP/7zip/UI/Common/IFileExtractCallback.h` — 回调接口扩展

详细设计见 [docs/7zip-password-vault-implementation-plan.md](docs/7zip-password-vault-implementation-plan.md)。

## 许可

本项目沿用 7-Zip 的许可（LGPL，详见 `DOC/License.txt` 与 `DOC/copying.txt`），并包含 unRAR 许可限制（`DOC/unRarLicense.txt`）。分发（含源码与二进制）时请遵守 LGPL 条款。
