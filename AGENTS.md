# SpinelOS 开发与协作指南 (AGENTS.md)

本文件面向新开启的 AI 会话与仓库开发者，约定 SpinelOS 的架构边界、构建测试流程、热部署方式、Git 提交规范及关键避坑事项。

---

## 1. 项目架构与核心约束

- **系统底座**：Arch Linux (`archiso`) + `labwc` (wlroots Wayland) + PipeWire / WirePlumber + NetworkManager + Fcitx5 (Wayland IME)。
- **四核心自研原生组件**（定义于 [desktop/CMakeLists.txt](desktop/CMakeLists.txt)）：
  - `spinel-shell` (`SPINEL_HAS_WEBKIT=0`)：常驻 C++20 外壳（纯 `GTK4 + Cairo + gtk4-layer-shell`），负责顶栏、Dock、桌面、控制中心、`Alt+Space` 启动器、OSD 与剪贴板。**严禁在 `spinel-shell` 中引入 WebKitGTK**，保持低内存常驻。
  - `spinel-webui` (`SPINEL_HAS_WEBKIT=1`)：按需拉起的 WebKitGTK 6.0 应用窗口，对应 `desktop/ui/index.html?mode=...`（`settings`、`store`、`games`、`doctor`、`monitor`、`installer`、`welcome`），关闭窗口即销毁并释放内存。
  - `spinel-terminal`：原生 GTK4 + VTE4 多标签终端（支持 `-d/--working-directory` 与 `-e/--`）。
  - `spinel-files`：原生 GTK4 文件管理器（`GtkListView`/`GtkGridView` + `GListStore` 虚拟化 + `opendir`/`fstatat` 异步扫描），同时提供 `--file-manager1`（`org.freedesktop.FileManager1` D-Bus 服务）、内置归档压缩/解压与可执行文件快捷操作。
  - `spinel-tests`：无 WebKit 的 C++ 单元测试目标（CTest `spinel_unit_tests`）。
- **模块边界约定**：
  - 全自研优先，不再引回已替换的第三方桌面组件（如 `thunar`、`thunar-archive-plugin`）。
  - **应用商店（[desktop/src/app_store.cpp](desktop/src/app_store.cpp)）全部采用宿主机原生安装**（`pacman` 或 `native` 宿主机解包/官方 `.pacman`/`AppImage --appimage-extract`），不要新增依赖 Flatpak 运行库（如额外 `nvidia-*` 扩展）的目录应用。
  - **监控与诊断独立窗口化**：进程监控与系统日志独立为「系统监视器」（`?mode=monitor`），健康体检独立为「Spinel Doctor」（`?mode=doctor`），不要再塞回系统设置侧边栏（`#app-settings`）。
  - **界面视觉规范**：涉及 UI 与主题变动请遵循 [docs/UI_SPEC.md](docs/UI_SPEC.md)。

---

## 2. 构建、语法检查与单元测试规范

宿主机为 macOS（命令行优先用 `grep -rn` 等通用工具；不要假设 `rg` 存在），所有 Linux 二进制构建与单测统一在 Docker `spinelos-builder` 容器中执行：

```bash
# 1. C++20 全目标编译 + 单元测试（修改 C++ / CMake 后必跑）
docker run --rm --platform linux/amd64 -v "$PWD":/workspace -w /workspace spinelos-builder \
  bash -c "cmake -S desktop -B build/linux-amd64 -DCMAKE_BUILD_TYPE=Release && cmake --build build/linux-amd64 -j4 && ctest --test-dir build/linux-amd64 --output-on-failure -V"

# 2. 前端 JS 语法检查（修改 desktop/ui/js/*.js 后必跑）
for f in desktop/ui/js/*.js; do node --check "$f"; done

# 3. 部署 / 安装脚本语法检查（修改脚本后必跑）
bash -n scripts/dev-deploy.sh
```

- 若修改了 [docker/Dockerfile.builder](docker/Dockerfile.builder)，先重建镜像：
  `docker build --platform linux/amd64 -t spinelos-builder -f docker/Dockerfile.builder .`
- 构建完整 Live ISO：`./scripts/build-iso.sh`（产物输出至 `out/spinelos-*.iso`）。

---

## 3. 热部署与镜像一致性规范

- **常规热部署（C++ / UI / desktop / D-Bus FileManager1 / MIME）**：
  ```bash
  ./scripts/dev-deploy.sh <user>@<ip>
  ```
- **仅改前端 `desktop/ui/`**：
  ```bash
  ./scripts/dev-deploy.sh <user>@<ip> --ui-only
  ```
- **涉及系统软件包、Theme、GRUB、Plymouth、labwc、Portals 或 `start-spinel-desktop`/`spinel-install` 脚本**：必须追加 `--full`：
  ```bash
  ./scripts/dev-deploy.sh <user>@<ip> --full
  ```
- **四端一致性检查**：增删系统包、自启服务、桌面入口或二进制文件时，需同步确认以下 4 处是否一致：
  1. [archiso/packages.custom.x86_64](archiso/packages.custom.x86_64)
  2. [docker/build-inside-container.sh](docker/build-inside-container.sh)
  3. [archiso/airootfs/usr/local/bin/spinel-install](archiso/airootfs/usr/local/bin/spinel-install)
  4. [scripts/dev-deploy.sh](scripts/dev-deploy.sh)
- **账户无关性**：严禁硬编码 `/home/gamer` 或 `/home/allen`，一律通过 `home_dir()` 或 `$HOME` 读取。

---

## 4. Git 提交规范

1. **先验证后提交**：在**最后一次代码编辑之后**完成 Docker 构建与 `ctest`（全部通过），再执行提交。
2. **Commit Message 格式**：采用 Conventional Commits，格式为 `<type>(<scope>): <concise English summary>`：
   - `feat(...)`：新功能（如 `feat(files): ...`、`feat(monitor): ...`）
   - `fix(...)`：缺陷修复（如 `fix(launcher): ...`、`fix(proxy): ...`、`fix(icons): ...`）
   - `perf(...)`：性能优化（如 `perf(files,proxy): ...`）
   - `docs(...)` / `chore(...)` / `refactor(...)`
3. **精准暂存，禁止 `git add -A` / `git add .`**：
   - 提交前务必先看 `git status --short`，只 `git add` 本次实际改动的明确文件。
   - **如果工作区存在未提交的用户本地改动或实验代码，切勿私自还原、丢弃或顺带提交**。
4. **忽略产物**：`build/`、`desktop/build/`、`out/`、`.cache/`、`.env.local`、`*.iso`、`*.log` 已在 `.gitignore`，不要强制跟踪。

---

## 5. 关键技术与避坑事项

- **网络管理互斥**：Live ISO 制作（[docker/build-inside-container.sh](docker/build-inside-container.sh)）与装机（[spinel-install](archiso/airootfs/usr/local/bin/spinel-install)）必须保持禁用/清理 `systemd-networkd` 与 `iwd`，由 `NetworkManager` 独家接管有线/Wi-Fi 及静态 IP/DNS。
- **桌面标识与系统代理（Chromium / Clash / Monocloud）**：
  - `XDG_CURRENT_DESKTOP` 必须包含 `:GNOME`（当前为 `SpinelOS:GNOME`），并在 `start-spinel-desktop`、[common.cpp](desktop/src/common.cpp) `ensure_display_env()` 与 D-Bus activation 环境中兜底注入，这样热重启进程后浏览器和 Portal 也能识别桌面。
  - 当系统有 `gsettings` 时不要强绑 `--proxy-server=` 到 Chromium 命令行，使 `org.gnome.system.proxy`（由系统设置或 Monocloud/Clash 写入）可实时生效与热切换。
  - 在「系统设置」内：SpinelOS 自配 `~/.config/spinelos/proxy.conf` 优先，关闭时自动兜底读取外部 GSettings 代理并标注「外部代理生效」。
- **GTK4 Layer-Shell 浮层阴影与滚动**：
  - 透明浮窗（如 `Alt+Space` 启动器 [overlays.cpp](desktop/src/shell/overlays.cpp)）须调用 `gtk_window_set_decorated(win, FALSE)`，且内层面板外边距须大于 CSS `box-shadow` 模糊延伸半径，防止在 Wayland surface 边缘被硬裁切出矩形黑框。
  - 当输入框保留焦点时，键盘上下选择列表行必须同步调整 `GtkScrolledWindow` 的 `vadjustment`。
- **IPC 与进程调用安全**：
  - 命令路由集中在 [ipc_router.cpp](desktop/src/ipc_router.cpp)，新增命令需同时在 [spinel_tests.cpp](desktop/tests/spinel_tests.cpp) 补充单测。
  - 调用外部工具（`nmcli`、`udisksctl`、`wpctl`、`pacman`、`bsdtar`、`pkexec`）优先使用 `run_argv_*` 参数数组并对 IP/网段/设备路径/包名做严格校验，禁止直接拼接不可信字符串到 `sh -c`。
- **图标解析**：新装软件与便携软件依赖 [desktop_apps.cpp](desktop/src/desktop_apps.cpp) 多尺寸目录 + `GtkIconTheme`、`shell_core.cpp` 双向窗口匹配，以及 `spinel-files` 的 `app.asar` PNG 提取。
