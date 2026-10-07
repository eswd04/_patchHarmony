# OnePlus 15：原厂内核的 User NS 兼容模式

**2026-10-07 实机反馈：此前交付的模块在 smoke 写 uid_map 时触发 CFI panic。
当前已修正并通过本地 CFI/CRC 检查，修复版 10 项 smoke 测试均已由用户
逐项在手机上执行并确认 PASS。第五项测试已补齐先创建 PID 1 的前置条件。
实际容器启动和长期运行仍待验收；未执行一次性 --all 或压力测试。
手机原目录的旧文件不要继续使用。证据和修正见
[崩溃记录](oneplus15-compat-reboot.md#smoke-运行导致重启2026-10-07)。**

本实现按用户确认的范围工作：保持原厂内核，通过 LKM 为有权限的 root
容器提供**全范围恒等 UID/GID 映射**。它不是 rootless User NS，也不是完整的
Linux User NS 安全隔离边界。容器 UID 0 与手机 UID 0 是同一个内核身份。

## 适配基线

用户提供的手机版本：

```text
6.12.23-android16-5-g188c695beb6d-ab14367805-4k
aarch64, SMP PREEMPT, Oct 31 2025
```

工程保存的 `out/op15/kernel` 是同系列的旧构建
`6.12.23-android16-5-ga8f88ad96df3-ab13929693-4k`。其
`out/op15/op15_config` 明确关闭 `CONFIG_USER_NS`、`CONFIG_PID_NS`，打开
`CONFIG_UTS_NS`、`CONFIG_NET_NS`、`CONFIG_KEYS` 和 `CONFIG_CFI_CLANG`。
旧镜像不能代替当前手机的符号、ABI 和运行验证。

现已只读导出当前手机的资料至 `out/op15/live/`：`boot.img`、
`kernel.payload`、`config.gz`、`kernel.config`、`vmlinux.btf`、`kallsyms.txt`。
从当前 Image 恢复的 `System.map` 和 `Module.symvers` 已校验全部 125,417 个
内核符号名，包含 8,870 个内核导出符号的真实 CRC。

官方源码参考为 [OnePlusOSS SM8850 common kernel](https://github.com/OnePlusOSS/android_kernel_common_oneplus_sm8850)，
分支 `oneplus/sm8850_b_16.0.0_oneplus_15`。对应模块/平台仓库为
[OnePlusOSS SM8850 kernel](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8850)。
实现使用实际解析到的符号地址；挂载 namespace 的私有 owner 字段沿用工程的
扫描与内核 accessor 回读验证，不写死手机的结构体偏移。

`CONFIG_USER_NS=n` 的 `make_kuid()`、`make_kgid()`、`from_kuid()`、
`from_kgid()`、`current_user_ns()` 和 `get_user_ns()` 在宿主中是内联 stub。
所以仅移植 `kernel/user_namespace.c` 或接受任意 map，无法让宿主所有
文件、凭据和 IPC 路径正确转换身份。本模式始终让完整 identity map 生效。

## 支持的接口

- `unshare(CLONE_NEWUSER)`，包括显式 `CLONE_FS`、与 mount/UTS 等
  namespace 的组合请求。保留内核的线程和 flag 检查；构造期间暂存新凭据，
  成功后提交，失败恢复原凭据。
- `clone()` 和 `clone3()` 的 `CLONE_NEWUSER`。与 `CLONE_THREAD` 或
  `CLONE_FS` 组合被拒绝；子进程凭据在运行之前安装。
- nsfs FD 的 `setns(fd, CLONE_NEWUSER)` 和 `setns(fd, 0)`。
  只允许进入有权限的后代，拒绝重新进入自己、进入祖先/兄弟和返回宿主。
  不支持 pidfd 的 User NS 多 namespace 原子加入。
- `/proc/<pid>/ns/user`，`NS_GET_NSTYPE`、`NS_GET_OWNER_UID`、
  `NS_GET_PARENT`、`NS_GET_USERNS`。父/owner 访问保留祖先可见性检查。
- `/proc/<pid>/{uid_map,gid_map,setgroups}` 的直接打开。
  缓存 inode 在每次 open 时读取任务的真实凭据，打开的文件保持 namespace
  快照，避免嵌套 unshare/setns 后继续使用旧 map。
- 每种 map 可成功写一次 `0 0 4294967295`。拒绝非 identity、部分映射、
  重复写入、嵌入 NUL 和初始 namespace 的 map 写入。
- `setgroups`，以及不可恢复的 `deny` 状态。`deny` 须在 gid_map 写入确认
  之前设置，状态由后代继承；原生和 16 位兼容 setgroups syscall 均检查该状态。
- 宿主 ID 转换保持恒等，setuid/setgid 系列保留原 syscall 和 LSM 检查。
  `ns_capable_setid` 修正使用宿主常量 namespace 的调用；
  `PR_CAPBSET_DROP` 在调用者自己的 namespace 中检查 `CAP_SETPCAP`。
- 新建 mount、UTS、net、cgroup、time、PID、IPC namespace 的 owner
  修正；已有共享对象的 owner 不被替换。

创建者须为 EUID 0，并在自己的 namespace 中拥有 `CAP_SYS_ADMIN`、
`CAP_SETUID`、`CAP_SETGID`、`CAP_SETFCAP`。同时检查 chroot 限制并调用
`security_create_user_ns()`，不会绕过 SELinux/其他 LSM。一般的宿主
`capable()` 检查仍然针对初始 namespace；不会把容器 capability 一律提升成宿主权限。

与原生 User NS 的有意差异：map 从创建时就生效，写入只是一次性确认，
创建者在子 namespace 内也可用子 namespace capability 确认 identity map。
不会模拟空 map 的 unmapped/overflow 行为。`unshare --map-root-user`
写入单 UID 映射，不符合本模式；需要完整 identity map 的 root 容器配置。
idmapped mounts、rootless/subuid/subgid 映射仍不支持。proc 的宿主 readdir
表不会枚举补充的 map 文件，直接按路径打开可用。

## 生命周期

原厂内核没有有效的 `get_user_ns()`/`put_user_ns()` 引用路径；任务退出
不能证明所有凭据、owner、延迟销毁对象、proc 缓存和 nsfs FD 已释放。
因此发布 namespace 或 proc/nsfs 回调后，会固定 `droid_lkm_misc`，对象保留
至重启。通过 `__symbol_get()` 固定提供 proc namespace 注册接口的主模块，
避免动态解析的注册/注销函数在主模块卸载后失效。

每次启动最多保留 1024 个 User NS 对象，耗尽后返回 `ENOSPC`。构造阶段
可能延迟销毁 owner 引用的失败请求也会保留对象。`status` 实时显示保留数。
使用后不能依靠 `release` 参数卸载 User NS；该参数只释放 devtmpfs 内部挂载。
恢复原厂行为的方式是重启，不强制卸载这两个模块。

## 构建与加载

`Makefile` 已启用 6.12 的 `droid_lkm_misc.ko`，移除对不存在的 xt 和
`userns_ids.c` 编译单元的引用。较早的目标继续构建主模块和适用的 compat
模块。User NS 的符号、hook 或 map 文件初始化失败会使 misc 加载失败并
回滚已安装的 hook；原生 User NS 内核则保留原生实现。

使用与手机构建匹配、已准备好的 ARM64 kernel build 目录：

```sh
make KDIR=/path/to/matching/kernel-build VER=oneplus15 \
  ARCH=arm64 LLVM=1 LLVM_IAS=1
```

本次编译验证使用公开镜像
`ghcr.io/ylarod/ddk-min:android16-6.12-20260313` 的 kernel build 目录与
Clang `r536225`，完成三个模块的编译、MODPOST、链接和 BTF 生成。
**该 DDK 的 kernel.release 是 `6.12.76-4k-gae4e2f4f997e-dirty`，不是手机的
6.12.23。`out/android16-6.12/*.ko` 是构建验证产物，不能认定可直接加载。
需要匹配手机的 vermagic、导出符号 CRC、配置与 ABI；仅修改 vermagic 不足以保证适配。**

最新匹配构建已输出至 `out/oneplus15-6.12.23/`，使用官方源码提交
`188c695beb6df4f4f85e647aedda9e76327686ac`、手机提取的导出 CRC 与 BTF。
三个模块的全部导入 CRC、`module_layout` CRC 和完整 vermagic 均已通过校验；
另用编译器计算的 105 个结构体大小/字段偏移对比手机 BTF，全部一致。
详细构建资料与实机测试顺序见 [compat 重启排查](oneplus15-compat-reboot.md)。

以下仅记录加载顺序。修复版尚未实机验收，不据此重复运行旧文件或全套 smoke。
使用时必须替换整组模块，并避免旧模块自动加载；不要强制卸载已有 User NS。

```sh
insmod droid_lkm.ko gate=0
# OnePlus vendor 兼容模块按原有需求加载
insmod droid_lkm_compat.ko
# 先只验收 User NS
insmod droid_lkm_misc.ko userns=1 xt=0 devtmpfs=0 verbose=1
cat /proc/droid_lkm_misc/status
```

加载时的 `ready` 表示符号/hook 安装成功，不等于运行验收通过。手机的 LSM
策略还可能拒绝创建 namespace 或挂载，日志会保留对应错误。

## 验证

宿主上的纯逻辑测试直接编译生产 map helper 和 identity parser，启用 ASan/UBSan：

```sh
python3 tests/run-userns-unit.py
```

ARM64 Android 验收程序源码为 `tests/userns_smoke.c`。使用 NDK 构建：

```sh
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang" \
  -static -pthread -Wall -Wextra -Werror -O2 tests/userns_smoke.c -o userns-smoke
```

可复现的构建入口会检查静态 ARM64 ELF 的 TLS segment 对齐：

```sh
ANDROID_NDK_HOME=/path/to/android-ndk scripts/build-userns-smoke.sh
```

静态构建必须使用 API 29 或更高版本的 CRT。此前 API 28 构建的 TLS 对齐为
8，会在 ARM64 Bionic 进入 main() 前被拒绝；API 29 构建已确认对齐为 64、
skew 为 0。参考 [Bionic ELF TLS 设计](https://android.googlesource.com/platform/bionic/+/HEAD/docs/elf-tls.md)。

本次已重新生成 `out/op15/userns-smoke`。新版还未推送至手机，手机上的旧程序
不具备本次增加的诊断选项。新版无参数只打印用法，不再立即运行全部测试：

```sh
./userns-smoke --list              # 仅列出测试，不访问模块或创建 namespace
./userns-smoke --trace --case 1    # 单项诊断，显式打印每个 CHECK 前的步骤
./userns-smoke --trace --all       # 全套运行，须先完成单项实机验收
```

每项 fork 前打印 START，子进程运行前打印 RUN，stdout/stderr 关闭缓冲。
程序逐项验证 map/部分读取/重复写入/嵌套缓存、setgroups deny、setresuid/gid、
bounding set、组合 unshare/tmpfs/UTS、PID/IPC owner、clone/clone3、setns/nsfs ioctl、非 root
拒绝、多线程拒绝与失败后的 namespace 保持。每项在独立子进程运行，带超时。
它会创建 `/data/local/tmp/dlkm-userns-*` 临时目录，并在测试末尾尝试清理。

此前已在手机验证 TLS 初始化；随后用户运行全套 smoke 触发 CFI panic，
并提供 minidump。日志确认触发在 uid_map 写入时的 file_ns_capable 调用。
当前修复版的两个 map 调用点 CFI 哈希与手机 Image 均为 `0xa289c1d2`，
用户随后在手机加载修复版三个模块，10 项 smoke 测试均逐项返回 PASS，
包括 map、setgroups、setid、mount/UTS、PID/IPC owner、clone/clone3、
setns/nsfs ioctl、非 root 拒绝、失败回滚和多线程拒绝。
实际容器启动与长期运行尚未验证，不能由这些结果推定全部容器行为均正常。
