# CamSwap Fix（虚拟相机 · 修复版）

基于 [zensu357/Android-CamSwap-OpenSource](https://github.com/zensu357/Android-CamSwap-OpenSource) 的修复分支（版本线 `tuzi999`）。

> **使用前提：本模块必须搭配我们自己的 Xposed 框架（卡密版 LSPosed 模块）使用。**
> Xposed 特征检测与 native hook 检测已在框架侧解决，单独使用本模块不包含任何对抗检测的能力。

---

## 相比原版修复了什么

### 1. 音频 hook 改为"数据改写"，不再修改系统库代码（反检测核心）

原版用 Dobby inline hook 直接改写 `libaaudio.so` / `libOpenSLES.so` 函数入口的机器码，内存字节与磁盘文件不一致——内存完整性检测（字节比对）一扫一个准，这也是它会被风控定位的原因之一。

本版改为：

- **AAudio**（`AAudioStream_read` / `setDataCallback` / `openStream` / `close`）：改 GOT/PLT hook，只改调用方自己的 GOT 表项，系统库 `.text` 一个字节不动
- **OpenSL ES**：vtable 指针替换（堆/静态表数据改写），`slCreateEngine` 走 GOT hook
- 新增**完整性自检**（`CS-Integrity` 日志）：每次注入后自动做一次"内存 vs 磁盘"字节比对，两库均 `diff_bytes=0 verdict=PRISTINE`——即字节比对检测已抓不到痕迹
- Dobby 依赖整个移除，APK 更小，也不会再出现 `Dobby` 特征字符串

顺带修复：

- MTK MIUI 上 libaaudio 导出是 4 字节 `B <impl>` 跳转桩，原版 inline hook 覆写桩会打爆相邻函数（`setPerformanceMode`/`setDeviceId`），WebView 走音频路径即 SIGILL 闪退——GOT 方案天然绕开
- 目标应用崩溃 SDK（Bugly 类）在信号处理器里调 `dlopen` 与扫描锁自死锁：所有读写走 `/proc/self/mem`（越界/竞态返回错误而非崩溃）+ try-lock 防重入
- OpenSL ES `GetInterface` 不过滤 IID 的隐患：原版把录音器上取到的任意接口都当 BufferQueue 记录
- 新增**限时重扫窗口**：游戏引擎在 Application.onCreate 之后才加载音频库，其 GOT 是后绑定的，安装时的首扫抓不到；后台线程 30s 快扫 + 5min 慢扫覆盖晚加载的调用方，全部符号就位后自动停止

### 2. 删除"活体页面放行真实摄像头"补丁（三色光宽容）

原修复线曾加过一个补丁：前台 Activity 匹配 `com.dtf.face` / `faceverify` / `liveness` 等关键词时（敦煌 DTF 三色光活体页面），Camera1 虚拟相机整体让位、`takePicture` 出真图。当时的考虑是"三色光活体只有真摄像头对着屏幕才过得去"。

**本版已将该补丁完整删除**（4 处放行点 + Activity 名跟踪）。活体/人脸页面同样走虚拟相机，三色光由 `ScreenColorDetector` 应对——GLVideoRenderer 渲染时把检测到的红/绿/蓝闪烁色温作为环境光注入虚拟画面，让活体检测"看到"屏幕反光。

**注意：实测删除该补丁并不能解决下面"当前的问题"**——说明到达签到调原相机的原因不在活体放行，见下。

### 3. 拍照替换（动态防御）默认开启

`enable_photo_fake`（拍照时把当前虚拟画面帧编成 JPEG 覆盖真实照片）原版默认关闭，配置文件里也没有这个键，导致所有"拍一张照片"类功能实际用的是原相机。本版三处默认值翻为 `true`：

- Camera1 `takePicture` → 回调直接喂虚拟帧 JPEG
- Camera2 → JPEG `ImageReader.acquire*` 出图时把 buffer 覆写为当前虚拟画面（日志：`成功替换 JPEG 拍照照片为当前虚拟画面`）
- 虚拟画面不可用（推流未就绪）时安全回退，原样放行，不影响功能
- 设置页开关仍可单独关闭

---

## 当前的问题（优先级最高）

**到达位置后调原相机**：实测视频通话、实名认证等全部走网络推流（虚拟相机正常），唯独"到达签到"这一步用的仍是原相机。

- 三色光宽容补丁已删除（排除活体放行因素）→ 仍然原相机
- 拍照替换已默认开启（覆盖 JPEG 静态拍照路径）→ 待真机复测
- **优先怀疑 hook 覆盖问题**：签到页面的捕获路径可能走了未被 hook 的 API 面（未识别的 Surface 类型、独立进程/独立 Activity 的相机、或非 JPEG 的图像回传通道），需要用真机日志（模块管理器"虚拟相机日志"页可导出）对着替换决策日志逐条核对

## 使用

1. 刷入配套的卡密版 Xposed 框架模块（含 Xposed/Hook 检测对抗），激活卡密
2. 在框架中启用本模块并勾选目标应用作用域
3. 推流源配置 `media_source_type=stream`，`stream_url` 填 RTSP 地址（或本地视频文件模式）
4. 问题排查：框架管理器 → 日志 → 虚拟相机，可查看 hook 装载、完整性自检、拍照替换决策并导出

构建：Android Studio 打开工程，`gradlew :app:assembleRelease`（NDK r25，CMake 3.22+）。原生部分位于 `app/src/main/cpp/`。
