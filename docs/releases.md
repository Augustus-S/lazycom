# 自动构建与安装包

`.github/workflows/release.yml` 在每次分支 push 时构建最新提交，并在生产构建和打包成功后
创建 GitHub 预发布。一次 push 包含多个提交时，只发布最后一个提交。Actions 页面
支持手动触发；Pull request 执行相同检查并保留产物，但不发布。仅推送 tag 不触发。

## 发布产物与兼容范围

所有产物均为 x86_64，使用 Ubuntu 24.04、GCC 13 和 C++20 Release 构建。

| 格式 | 包名或文件名 | 安装与运行验证目标 |
| --- | --- | --- |
| DEB | `lazycom_<版本>_amd64.deb` | Debian 13、Ubuntu 24.04、Ubuntu 26.04 |
| RPM | `lazycom-<版本>-1.x86_64.rpm` | Fedora 44 |
| AppImage | `lazycom-<版本>-x86_64.AppImage` | 上述四种系统，无 FUSE 的解包运行模式 |
| tar.gz | `lazycom-<版本>-linux-x86_64.tar.gz` | 与 DEB 相同的构建基线 |
| 校验文件 | `SHA256SUMS` | 覆盖上述四个附件 |

上述发行版是原有安装验证的目标。安装、启动和卸载验证由独立
`../lazycom-test` 仓库维护，仅在用户明确指令下执行。本仓库的自动发布不执行测试，
也不代表当前产物已在上述系统通过安装验证。

版本形式为 `0.1.0~pre.<run_number>.<run_attempt>.g<commit前12位>`，基础版本来自
`CMakeLists.txt`。递增的预发布编号支持包管理器升级，同一基础版本的正式版排在
预发布之后。GitHub 标签使用 `build-<run_id>-<run_attempt>`，绑定实际构建提交；
重新运行产生新预发布，不覆盖旧版，不标记为 Latest，也不因后续 push 取消旧构建。

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

## 构建和验证流程

生产构建矩阵使用五个 configure/build presets：`gcc-debug`、`clang-debug`、`gcc-release`、
`gcc-debug-no-diagnostics`、`gcc-asan-ubsan`。发布使用 GCC Release 产物。

CPack 生成 DEB、RPM 和 tar.gz，`packaging/build-appimage.sh` 生成 AppImage。
AppImage 打包工具固定为 1.9.1，type2 runtime 固定为 20251108，二者下载后必须通过
脚本中固定的 SHA-256 校验；提供已缓存文件时仍校验。CMake configure/build 使用
仓库内固定依赖，不下载应用依赖。runner 工具安装、容器镜像及首次 AppImage 工具
获取需要联网。

自动流程只执行生产构建、打包、附件校验与发布，不执行 CTest、安装验证或 TUI
冒烟检查。原验证脚本与手动测试工作流位于 `../lazycom-test`；测试结果必须关联
实际生产提交和产物。真实 USB-UART、TSan 运行检查及 8 小时性能/RSS 验收仍未完成。

构建 job 仅申请 `contents: read`；发布 job 使用自动提供的 `GITHUB_TOKEN`
并申请 `contents: write`，不需要个人 token。仓库或组织策略须允许 Actions 及该权限。
Actions 中间产物保留 7 天，Releases 附件不受此期限影响。

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
`APPIMAGE_TOOL_DIR` 可指向预先下载两个固定工具的缓存目录。

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
