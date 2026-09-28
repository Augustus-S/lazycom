# 发布、安装与 GitHub Actions

本文按 2026-09-28 的 [发布工作流](../.github/workflows/release.yml)、
[安装规则](../cmake/Install.cmake)、[CPack 配置](../cmake/Packaging.cmake)
和 [AppImage 脚本](../packaging/build-appimage.sh) 整理。开发沿革与历史验证见
[history.md](history.md)。

## GitHub Actions

工作流名称为 `Build and prerelease`，触发规则如下：

| 事件 | 构建与打包 | GitHub 预发布 |
| --- | --- | --- |
| 任意分支 push | 构建该次 push 的末尾提交 | 整个构建矩阵成功后发布 |
| Pull request | 构建并保留 Actions 产物 | 不发布 |
| Actions 页面手动触发 | 构建所选 ref | 整个构建矩阵成功后发布 |
| 仅推送 tag | 不触发 | 不发布 |

runner 固定为 `ubuntu-24.04`，安装 GCC 13 和 Clang 18。生产构建矩阵为：

| Preset | 编译器 | 用途 |
| --- | --- | --- |
| `gcc-debug` | GCC 13 | Debug 编译 |
| `clang-debug` | Clang 18 | Clang 编译兼容性 |
| `gcc-release` | GCC 13 | Release 编译及全部安装包 |
| `gcc-debug-no-diagnostics` | GCC 13 | diagnostics 关闭时的编译 |
| `gcc-asan-ubsan` | GCC 13 | ASan/UBSan 插桩版本编译 |

工作流只执行生产构建、打包和附件校验，不运行应用、CTest、sanitizer 或安装
冒烟验证。`gcc-tsan` 是本地 configure/build preset，没有加入自动矩阵。
测试代码与执行入口由独立 `../lazycom-test` 仓库维护，须按明确指令使用。

Release job 下载 `release-linux-x86_64` artifact，核对 `SHA256SUMS` 后发布。
构建 job 使用 `contents: read`，发布 job 使用自动提供的 `GITHUB_TOKEN` 和
`contents: write`；仓库或组织策略须允许该权限，无需配置个人 token。
Actions artifact 保留 7 天；该期限不删除已经上传到 Releases 的附件。

包版本为 `<项目版本>~pre.<run_number>.<run_attempt>.g<commit前12位>`。
项目版本来自根 `CMakeLists.txt`，后缀在 configure 步骤注入。发布标签为
`build-<run_id>-<run_attempt>`，绑定构建提交，并设置 prerelease、`latest=false`。
工作流不自动删除旧预发布，也没有配置新 push 取消旧运行。

排查未生成预发布时，先确认触发事件，再查看五个构建 job；GCC Release 已上传
artifact 并不代表整个矩阵成功。若构建通过但发布失败，检查附件校验、Actions
写权限与仓库 tag rulesets。

## 发布产物与兼容范围

所有产物均为 x86_64，使用 Ubuntu 24.04、GCC 13 和 C++20 Release 构建。

| 格式 | 包名或文件名 | 使用方式 |
| --- | --- | --- |
| DEB | `lazycom_<版本>_amd64.deb` | 通过 apt 安装 |
| RPM | `lazycom-<版本>-1.x86_64.rpm` | 通过 dnf 安装 |
| AppImage | `lazycom-<版本>-x86_64.AppImage` | 可执行文件，支持解包运行 |
| tar.gz | `lazycom-<版本>-linux-x86_64.tar.gz` | 解压后保留完整目录树 |
| 校验文件 | `SHA256SUMS` | 校验上述四个附件 |

工作流发布说明列出的目标系统为 Debian 13、Ubuntu 24.04 / 26.04 和 Fedora 44。
这是目标范围，当前自动流程不提供这些系统的安装、启动或卸载验证结果。
真实 USB-UART 与长时性能验收也未由发布流程完成。

## 安装与运行

下载所需包及 `SHA256SUMS` 到同一目录，先校验已下载的附件：

```bash
sha256sum --check --ignore-missing SHA256SUMS
```

以下命令应在只包含一个对应版本安装包的目录执行：

```bash
# Debian / Ubuntu
sudo apt install ./lazycom_*.deb

# Fedora
sudo dnf install ./lazycom-*.rpm

# 安装后以普通用户运行
lazycom
```

可在应用菜单中打开 LazyCom，桌面项的 `Terminal=true` 会请求终端运行。
卸载分别使用 `sudo apt remove lazycom` 或 `sudo dnf remove lazycom`。
包不创建用户配置，也不修改设备权限、用户组、udev 规则或系统服务。

系统安装布局：

```text
/usr/bin/lazycom                         主程序
/usr/lib/lazycom/libserialport.so*       应用私有串口动态库
/usr/share/lazycom/                      文档、源码、许可、版本信息
/usr/share/applications/lazycom.desktop  终端应用启动项
/usr/share/icons/hicolor/scalable/apps/lazycom.svg
```

程序使用 `$ORIGIN/../lib/lazycom` RUNPATH 加载私有动态库，不覆盖发行版的
libserialport。DEB 和 RPM 自动生成系统运行库依赖；私有串口库不会被声明为供其他
RPM 使用的公共库。LazyCom 主项目尚未声明许可证，RPM 的 License 元数据使用
`NOASSERTION`；各第三方组件保留自身许可。

AppImage 无需系统安装，在终端中执行：

```bash
chmod +x lazycom-*-x86_64.AppImage
./lazycom-*-x86_64.AppImage
```

如果环境不提供可用 FUSE，使用运行时自带的解包模式：

```bash
APPIMAGE_EXTRACT_AND_RUN=1 ./lazycom-*-x86_64.AppImage
```

AppImage 保留系统 glibc、libstdc++ 和 libgcc 依赖，运行基线为 Ubuntu 24.04
同代或更新的兼容系统。tar.gz 解压后运行 `usr/bin/lazycom`；移动时保留整个 `usr/`
目录树。二者都保留可替换的 libserialport shared library。

串口设备组可能为 `dialout`、`uucp` 或发行版自定义组，访问权限由管理员配置；
组成员变更后可能需要重新登录。遇到设备忙时检查 ModemManager、brltty 或其他
串口程序。配置、日志和快捷键说明见 `/usr/share/lazycom/Plan.md`。

## 打包与依赖来源

CPack 生成 DEB、RPM 和 tar.gz，`packaging/build-appimage.sh` 生成 AppImage。
AppImage 打包工具固定为 1.9.1，type2 runtime 固定为 20251108，二者下载后必须通过
脚本中固定的 SHA-256 校验；提供已缓存文件时仍校验。CMake configure/build 使用
仓库内固定依赖，不下载应用依赖。runner 工具安装和首次 AppImage 工具获取需要
联网；提供两个已缓存且校验一致的 AppImage 工具文件后，打包脚本不会重新下载。

## 本地打包

从仓库根目录，在 Ubuntu 24.04 构建环境中执行：

```bash
sudo apt install cmake ninja-build make gcc-13 g++-13 dpkg-dev rpm \
  file binutils curl desktop-file-utils squashfs-tools
cmake --preset gcc-release -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13
cmake --build --preset gcc-release
cpack --config build/gcc-release/CPackConfig.cmake -G DEB -B build/packages
cpack --config build/gcc-release/CPackConfig.cmake -G RPM -B build/packages
cpack --config build/gcc-release/CPackConfig.cmake -G TGZ -B build/packages
bash packaging/build-appimage.sh build/gcc-release build/packages
```

在 Fedora 本地可使用 CPack RPM，但该二进制的最低运行库版本由本机构建环境决定。
DEB 自动依赖分析要求在 Debian/Ubuntu 环境运行。默认本地包版本等于项目版本；
可在 configure 时传入 `LAZYCOM_PACKAGE_SUFFIX` 和 `LAZYCOM_BUILD_COMMIT`。
本地未设置 `LAZYCOM_BUILD_COMMIT` 时，包内 `COMMIT` 写入 `unknown`。
`APPIMAGE_TOOL_DIR` 可指向工具缓存目录，其中的文件名必须是
`appimagetool-x86_64.AppImage` 和 `runtime-x86_64`。

`Runtime` 安装组件仅支持 Linux 默认 vendored 模式，安装前必须先构建程序：

```bash
cmake --install build/gcc-release --component Runtime \
  --prefix "$PWD/build/release-bundle/usr" --strip
```

## 随附源码与动态库替换

`share/lazycom/DEPENDENCIES.lock` 记录依赖版本，`COMMIT` 记录构建提交，
`licenses/` 保留第三方许可；fmt 的完整许可位于 `fmt-license-and-header.h` 开头。
Catch2 由独立测试仓库保存，不进入生产构建或发布程序。

libserialport 的 LGPL 许可和对应源码位于 `share/lazycom/licenses/` 及
`share/lazycom/source/libserialport/`。用户可以修改源码，重新编译并替换动态库，
无需重新链接 LazyCom。复制源码到可写目录，从其上级目录执行：

```bash
mkdir libserialport-build
cd libserialport-build
../libserialport/configure --prefix="$PWD/install" --libdir="$PWD/install/lib" \
  --enable-shared --disable-static
make
make install
```

关闭 LazyCom 后，以生成的 `install/lib/libserialport.so*` 替换包内对应文件。
系统包位于 `/usr/lib/lazycom/`；AppImage 可通过 `--appimage-extract` 解包后替换
`squashfs-root/usr/lib/lazycom/` 中的文件，运行 `squashfs-root/AppRun`。
替换版本必须保留 `libserialport.so.0` SONAME 和所需符号；使用 0.1.2 或经过验证的
兼容版本。库文件名 `libserialport.so.0.1.1` 是上游 ABI 命名，不是源码版本号。
