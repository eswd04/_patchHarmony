# OnePlus 15 compat 重启排查与匹配构建

## 已确认的代码回归

用户描述为：模块加载成功，启动容器或执行其他操作时手机重启。

- `f7fe1cf` 改动的是主模块，增加 inline hook 开关并默认关闭。
- `11206f6` 给 compat 增加 `inline_hook` 参数和 `dlc_inline_hooks_on`
  安装检查，但没有在 init 中把参数值赋给实际开关。
- `74e8c0a`、`513d40a` 升级 KernCall/HooKern，使新的 API 可以编译；
  compat 开关仍是 BSS 中的 false，`find_task_by_vpid` hook 始终返回
  `-EOPNOTSUPP`。旧版本即使传 `inline_hook=1` 也不能解决这个赋值缺失。
- compat init 忽略 `dlc_ghost_init()` 返回值，继续打印 `ready` 并返回 0。
  因此 insmod 成功并不代表 vendor 保护已经生效。
- `9464caf` 已把默认值改回 true，并在 init 中传递参数到实际开关。
  本次在此基础上补充失败传播和清理。

这个确定存在的回归与触发时机一致：vendor 模块再次收到 NULL task 时，
原先由 compat 避免的解引用错误就可能重现。没有取得当次 panic 堆栈，
所以尚不能认定它是该手机每次重启的唯一原因。手机 pstore 当前为空，
可读取的 dmesg 已没有该次加载/崩溃记录。

## 本次修复

- `ghost=1` 时必须启用 inline hook；关闭开关直接拒绝加载。
- entry probe 的错误或非 plain entry 直接拒绝安装。
- ghost 初始化或 hook 安装失败向 insmod 返回错误，清理 hook engine，
  不再打印误导性的 `ready`。
- 失败清理和卸载均等待未恢复的 hook，避免仍被引用的模块代码被释放。
- wrapper 直接读取 HooKern 在发布 detour 前已设置的 `h->orig`，修复原先
  等 hook 返回后再赋另一个 original 指针的短暂窗口；该窗口内旧 wrapper
  会直接返回 NULL，仍可能伤及 vendor 调用者。

验证命令：

```sh
python3 tests/run-compat-unit.py
python3 tests/run-userns-unit.py
```

回归测试直接编译生产 init/probe 代码，覆盖开关传递、hook/probe 失败、
异常 entry、回滚及显式关闭 ghost。将 init 替换回 `513d40a` 进行反向验证时，
测试会失败，确认可检出该回归。

## 从手机提取内核用于构建

只读提取目录：`out/op15/live/`。当前 boot slot 为 `_b`，镜像内版本与
手机 `uname -r` 一致：

```text
6.12.23-android16-5-g188c695beb6d-ab14367805-4k
```

已有 `boot.img`、未压缩的 `kernel.payload`、配置、BTF 和 live kallsyms。
boot v4 的 kernel payload 从偏移 4096 开始，其长度来自 header 的
kernel_size；本次为 39,889,408 字节。

符号和 CRC 恢复可复现：

```sh
python3 scripts/extract-oneplus15-kernel.py \
  out/op15/live/kernel.payload out/op15/live/kallsyms.txt out/op15/live
```

脚本逐个核对 Image 中解压出的符号名与 live kallsyms，包括压缩索引和
markers 校验，再从两个 PREL32 export table 读取名字、namespace 和对应 CRC。
它支持本机 6.12 Image 布局；不是所有内核格式的通用提取器。

提取到的镜像不能代替源码、生成头文件与 Kbuild 工具。此处从官方 Android
仓库取得了[对应源码提交](https://android.googlesource.com/kernel/common/+/188c695beb6df4f4f85e647aedda9e76327686ac)，
使用与手机相同的 Clang r536225。
[外部模块构建文档](https://www.kernel.org/doc/html/latest/kbuild/modules.html)
说明了这些构建资料以及 `CONFIG_MODVERSIONS` 对 CRC 的要求。
本次恢复了 CRC 表，因此无需完整重建内核来生成该表，也没有替换手机内核。

本地源码/构建目录分别为 `/tmp/op15-kernel-src` 和 `/tmp/op15-kernel-build`，
编译器目录为 `/tmp/dlkm-ddk/opt/ddk/clang/clang-r536225/bin`。
准备环境时有以下明确调整：

- 手机配置为 Rust enabled；本机没有 Android Rust 编译器，olddefconfig
  关闭 Rust。这里只编译 C LKM，不生成替换内核。
- `tools/lib/bpf/libbpf.c` 两个只读字符串局部指针改为 const，以适配宿主
  glibc 头文件。该工具改动不进入模块代码。
- protected module 名单从该源码的 `modules.bzl` 生成；三个 droid 模块
  不属于这些 GKI 模块。手机的签名与 SELinux 策略未修改。
- 将提取的 `System.map`、`Module.symvers` 放入 build 目录。
  `vmlinux` 文件是原始 phone BTF，供模块 BTF 生成工具读取，不是重建的 ELF 内核。
- KERNELRELEASE 使用手机完整 release；这同时配合真实源码、CRC 和布局核对，
  没有对旧 6.12.76 模块进行二进制 vermagic 修改。

当前环境的重编译命令：

```sh
PATH=/tmp/dlkm-ddk/opt/ddk/clang/clang-r536225/bin:/tmp/dlkm-ddk/usr/bin:$PATH \
make KDIR=/tmp/op15-kernel-build VER=oneplus15-6.12.23 \
  ARCH=arm64 LLVM=1 LLVM_IAS=1 HOSTCC=gcc HOSTCXX=g++ \
  KERNELRELEASE=6.12.23-android16-5-g188c695beb6d-ab14367805-4k -j8
```

输出目录为 `out/oneplus15-6.12.23/`。三个模块编译/MODPOST/链接/BTF 生成成功，
模块构建日志没有 warning 或 error。全部强导入都有对应的匹配 CRC，额外检查
`module_layout`；计数分别为 main 194、compat 77、misc 108。

```sh
python3 scripts/check-module-crcs.py out/op15/live/Module.symvers \
  out/oneplus15-6.12.23/*.ko \
  --release 6.12.23-android16-5-g188c695beb6d-ab14367805-4k
```

编译器 sizeof/offsetof 探针的 105 项结果与手机原始 BTF 完全一致，涉及
task_struct、cred、nsproxy、pid、user_namespace、mm_struct、module、ns_common、
file、inode、super_block、fs_struct、pid_namespace、ipc_namespace。
结果在 `out/op15/live/abi-layout-verification.json`，构建来源和文件摘要在
`out/oneplus15-6.12.23/BUILD-INFO.json`。

旧的 `out/android16-6.12/` DDK 产物有空版本表，`module_layout` CRC 缺失，
不要用于本次手机测试。其 DDK 的 `module_layout` CRC 是 `0xe143d454`，
手机为 `0xe976b219`，仅更改 release 字符串不足以匹配。

## 手机验收

此前用户按该顺序加载后运行 smoke，发生下述 CFI panic。以下命令仅记录
当时的复现条件；不要继续使用旧文件。修复版尚未实机运行。

```sh
insmod droid_lkm.ko gate=0
insmod droid_lkm_compat.ko inline_hook=1 ghost=1
dmesg | grep -E 'droid_lkm_compat|find_task_by_vpid|lkmhook' | tail -40
# 确认出现 ghost: find_task_by_vpid hooked 和 ready，且 insmod 返回成功
insmod droid_lkm_misc.ko userns=1 xt=0 devtmpfs=0 verbose=1
cat /proc/droid_lkm_misc/status
# 当时旧 smoke 无参数运行全部测试，随后触发 CFI panic。
```

如果 compat 拒绝加载，保留错误日志，先不要启动依赖此保护的容器。
构建/CRC/布局校验不等于手机的 CFI、SELinux、vendor 调用路径或容器运行验收。
代理没有在手机执行 insmod/rmmod、刷写或重启；用户自行进行了上述测试。

## 首次加载反馈后的修正

用户报告 misc insmod 返回 EINVAL。只读 dmesg 确认失败点为
`mntns_operations unusable: -22`。当前 Image 的 `mntns_owner` 位于
`_text + 0x47625c`，符合 A64 指令的 4 字节对齐，但不符合 8 字节指针对齐。
owner 校验错误地使用 `sizeof(void *)` 检查代码地址，现改为 4 字节，并给
name/type/owner 三种校验失败各自增加日志。测试直接编译该生产 helper，
覆盖该实际偏移、未对齐地址和文本范围边界。

手机 `/sdcard/Linux/` 的三个文件摘要全部对应旧 `out/android16-6.12/`
产物，并非匹配构建。本次重新编译的匹配文件已复制到
`/data/local/tmp/dlkm-op15-6.12.23/`，附带 `SHA256SUMS`；手机侧校验三个
文件均为 OK。没有替换已加载的旧模块，没有执行新的 insmod。
该目录是此前交付位置，其文件已在用户 smoke 测试中触发 panic，不能继续使用。

## smoke 运行导致重启（2026-10-07）

用户指出 `/data/persist_log/DCS/de/minidump/` 的
`SYSTEM_LAST_KMSG@4c5f0e5fdcfcdf10060ff9963f654a03@PLK110_11.A.41_0410_202601040056@2026_10_07_13_55_10.dat.gz`。
已通过只读 ADB 导出至 `out/op15/live/crash-20261007-135510.dat.gz`。
它是 gzip 压缩的 tar，包含 `minidump.bin`，并非普通 gzip 文本日志。
提取的 panic 片段为 `out/op15/live/crash-20261007-135510.panic.txt`。

决定性的日志：

```text
CFI failure at droid_lkm_userns_map_write+0xdc/0x214 [droid_lkm_misc]
(target: file_ns_capable+0x0/0x44; expected type: 0x4e6ffc6d)
CPU: 1 UID: 0 PID: 18365 Comm: userns-smoke
droid_lkm_userns_map_write -> droid_lkm_userns_uid_map_write -> vfs_write
Kernel panic - not syncing: Oops - CFI: Fatal exception
```

内核 `file_ns_capable` 声明的首参数为 `const struct file *`，工程曾声明为
`struct file *`。KCFI 对这两个类型使用不同哈希；日志 x16 的实际哈希为
`0xa289c1d2`，x17 的调用者期望哈希为 `0x4e6ffc6d`。这是本次重启的直接
触发原因，与此前 compat 政策回归不是同一问题。已改为
`typeof(&file_ns_capable)`，从内核头文件直接继承完整函数类型，保留 CFI。

```sh
python3 scripts/check-userns-cfi.py out/op15/live/kernel.payload \
  out/op15/live/System.map out/oneplus15-6.12.23/src/misc/userns/userns_proc.o
```

检查直接读取手机 Image 中函数入口前的 KCFI type ID，与 ARM64 对象中两个
map 调用点的 MOVK/CMP 指令比较。修复后均为 `0xa289c1d2`；旧 DDK 编译的
proc 对象两个调用点均为 `0x4e6ffc6d`，检查失败，与 panic 记录一致。

另外修正 HooKern 的 ADR/ADRP 和返回式 BL 搬移错误。ADRP 漏乘 4096，ADR
未正确合并 immhi/immlo，BL 返回后会执行嵌入的地址字面量。手机两个 proc
lookup 首 20 字节内含 ADRP，确实受影响，但这不是上述 CFI panic 的直接
触发点。错误在旧依赖 `0a9a1af` 中已存在，不能归因于 `513d40a` 新引入。
`scripts/fix-hookern-inline.py` 在构建目录生成 overlay，三个模块共同编译该
修复副本，不修改忽略目录中的依赖源码。测试覆盖实际手机 ADRP 指令、
正负偏移边界、BL 返回后的跳转；未修正的上游代码作为负例必须失败。

User NS 的 trampoline 调用改从 hook.orig 直接 READ_ONCE 读取，消除安装返回
后才写另一份 orig 指针的窗口。所有可失败初始化结束前，NEWUSER 请求和
proc/nsfs 发布路径保持原生行为，避免模块初始化失败后留下模块内回调或
凭据；misc 退出/初始化失败清理增加 hk_exit_block，等待未恢复的 hook。

新 smoke 支持 --list、--case N、--trace、--all，并在访问状态、fork 和每项
运行前立即打印进度；无参数只显示用法。当前修复版模块和 smoke 已本地编译，
发布副本在 `out/oneplus15-6.12.23-cfi-fix/`，未在手机加载或执行。
尝试读取手机旧模块时 ADB 显示设备已断开，因此本轮也未推送新产物。

## 修复版实机结果（2026-10-07）

最终结果：相同三个模块二进制在 OnePlus 15 的目标内核上，10 项 smoke
均由用户逐项执行并反馈 PASS。验证期间仅更新测试程序诊断及 case 5 前置
条件，没有替换已加载的模块。代理没有执行任何测试项；未做一次性 --all、
压力或实际容器启动验证。构建记录保存于交付目录的 BUILD-INFO.json。

用户重新连接手机后，整组修复版已推送至 `/data/local/tmp/dlkm-op15-cfi-fix`，
手机侧三个模块与 smoke 的 SHA256 均为 OK。代理仅执行 --list，未加载模块
或运行测试项。用户随后按顺序加载 main、compat、misc，并运行
`./userns-smoke --trace --case 1`，返回 `PASS: identity maps / single writes`。

该结果覆盖 NEWUSER|FS、identity map 分段读取、非 identity 映射拒绝、嵌入 NUL
拒绝、uid/gid map 单次写入、宿主 UID 保持以及嵌套 NEWUSER 后读取缓存 inode
对应的新 namespace。此次 map 写入没有再触发此前的 CFI panic。
其余 smoke 项、组合 namespace、setns 和实际容器启动仍待逐项实机验证。

用户随后执行 case 2、case 3，均返回 PASS：setgroups 可用、deny 不可恢复且
后代继承限制；PR_CAPBSET_DROP/READ 正常，setresgid/setresuid 成功切换至
12345。前三项实机通过。准备 case 4 时，仅更新 smoke 的 mount/UTS 逐调用
进度与失败点输出并重新编译、推送，模块二进制保持上述已加载版本。

case 4 随后 PASS：组合 User/mount/UTS namespace、设置和读取 hostname、
tmpfs 挂载/卸载均成功。初版 case 5 的 IPC owner 检查通过，但未 fork 就
打开 pid_for_children，返回 ENOENT。Linux v6.12 的 pidns_for_children_get()
以及本工程 droid_lkm_pidns_get_for_children() 均在 child_reaper 为 NULL 时
拒绝返回 namespace；刚 unshare 的 namespace 尚未创建 PID 1，这一状态
符合原生语义。参考 [Linux v6.12 PID namespace 源码](https://github.com/torvalds/linux/blob/v6.12/kernel/pid_namespace.c)。

仅修正 smoke：确认首个 fork 前 pid_for_children 返回 ENOENT，然后通过
pipe 同步创建 PID 1 并保持其存活，校验 IPC/PID owner，再释放和回收子进程。
子进程还检查 getpid()==1，避免把宿主进程误当成 namespace init。
修正版重新编译、推送及摘要校验后，用户重新执行 case 5，返回 PASS：
fork 前 ENOENT、首子进程 PID 1、IPC/PID owner inode 身份与 User NS 一致，
以及释放/回收子进程均成功。未修改模块。

后续 case 6 至 case 10 均 PASS：clone/clone3 创建 User NS、setns/nsfs ioctl、
非 root 创建返回 EPERM、非法 flag 返回 EINVAL 且保留原 namespace、多线程
unshare 返回 EINVAL。下一步需要按用户实际使用方式启动容器验收。
