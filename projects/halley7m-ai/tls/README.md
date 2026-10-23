# AI TLS 客户端

此模块使用固定的 Mbed TLS 4.1.1 完整发行包，含 TF-PSA-Crypto 1.1.1。它提供独立可构建的 TLS 字节流客户端；尚未接入实板 GMAC/lwIP，也未连接 DeepSeek。网络驱动、有效熵源和新鲜 UTC 满足条件后，主工程才能调用此模块。

## 配置与接入

- TLS 1.2/1.3 客户端，ECDHE、RSA/ECDSA、P-256/P-384、AES-128/256-GCM、SHA-256/384/512。按发行版配置检查要求保留 RSA basic/OAEP 支持；不启用 RSA 密钥交换、PSK、TLS 服务端、会话恢复、文件系统或平台 socket 实现。
- 强制 `MBEDTLS_SSL_VERIFY_REQUIRED`、CA 链、hostname/SNI 及证书日期检查。调用者提供生产 CA；测试 CA 不进入产品。输入 TLS record 保留 16 KiB 标准上限，输出 record 为 4 KiB。
- 所有 TLS 和 PSA 调用由同一个网络任务串行拥有，包括初始化/销毁。`ai_tls_platform_init()` 注册 entropy、UTC、单调毫秒回调并播种库内 CTR_DRBG；缺失、时间无效或熵失败返回 `AI_TLS_ERR_NOT_READY`。活跃客户端存在时不能关闭全局 PSA。
- `utc_seconds` 回调只在可信/新鲜时间可用时返回 0；适配器另检查 2026～2099 年及 `time_t` 可表达范围。库使用此 UTC 验证证书；不会以启动时间或固定日期替代。主机测试中的指定日期只用于固定证书用例。
- Mbed TLS 新版 `mbedtls_platform_get_entropy()` 要求输出的 `estimate_bits == 8 * output_size`。本模块传递端口的真实估计，拒绝不足或超出的估计，绝不把原始 DTRNG 字节数自动当作有效熵。DTRNG 探测成功仅证明可读取和明显故障未出现；在健康检测、熵评估及所需调理方式完成前，不注册生产熵回调。没有固定 seed、计时器 seed 或失败降级路径。
- `ai_tls_create()` 复制配置并解析 CA，`ai_tls_handshake()` 驱动握手，read/write/close_notify 返回 OK、WANT_READ、WANT_WRITE 或错误。BIO 必须有界、可重试；任务等待 socket 就绪或到期，不能忙循环。超时和网络切换由上层取消并 destroy；此模块不拥有 socket，不自动重发请求。
- write 遇 WANT 时保持相同缓冲区及长度；OK 后仅推进 `written` 字节。待完成写入期间 close_notify 返回 BUSY；关闭开始后禁止重新握手或写入，只允许继续关闭/销毁，防止库内部待发数据被误当成另一种操作完成。read 收到无 close_notify 的 EOF 作为截断错误处理；HTTP 层仍需独立检查 Content-Length/chunked/SSE 结束条件。错误后销毁客户端，原生错误与证书标志可通过 getter 记录。
- `ai_tls_random()` 提供同一 PSA DRBG 的随机字节，未初始化即失败。未来 lwIP 随机回调可由网络任务按其线程要求另行桥接；当前没有自动连接这两层。

集成只需 `add_subdirectory(tls)` 并链接目标 `ai_tls`，其 PUBLIC 依赖会带入配置宏和官方 include。MIPS 的 ABI/section flags 由主工程提供；独立构建也已设置同一 ABI。底层需要正确的 `calloc/free` 与 libc 时间转换；无额外 FreeRTOS 头文件依赖。

## 可复现验证

先按 `../network/README.md` 离线准备固定依赖，然后运行：

```powershell
python projects/halley7m-ai/tls/tests/check_build.py --mips
```

主机部分使用现有 MSVC 环境、BCrypt 系统随机源和 Python/OpenSSL 的 `127.0.0.1` TLS 服务端，不访问云端。测试证书来自完整发行包的 `framework/data_files`，用途仅限测试。覆盖未就绪拒绝、低熵估计拒绝、正常 RSA/ECDSA TLS 1.2/1.3、错误域名/CA/日期拒绝、短读短写与 WANT 重试及 close_notify。

仅交叉编译和体积检查：

```powershell
python projects/halley7m-ai/tls/tests/check_build.py --skip-host --mips
```

脚本构建静态库，再保留所有适配层公开入口执行 MIPS 可重定位链接。`build/ai-tls-mips/retained-size.txt` 给出保留代码/常量、data 与 bss，**不含 libc/libgcc 增量、CA 包或动态堆**，因此不能替代主工程最终 BIN 差值与握手堆峰值。实板集成后仍需测量这些值和握手耗时。

2026-10-23 已执行上述完整命令：MSVC 构建、未就绪检查及 8 组回环用例全部通过；XBurst2 GCC 7.2.0 MIPS 构建与可重定位链接通过。保留段为 `text=162702 B, data=8 B, bss=1100 B`，合计 163810 B。本机沙箱禁止回环 socket 连接，验证在批准的沙箱外运行，目标仅为 `127.0.0.1`。这些结果不表示实板 TLS 或 DeepSeek 请求已经成功。
