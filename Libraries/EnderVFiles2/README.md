# EnderVFiles2

EnderVFiles2 是 EnderEngine 的资源打包 / 加密 / 挂载 / 读取静态库。实现位于 `EnderVFiles2` 命名空间，使用 C++20，构建目标为静态库，MSVC 下使用 `/MD`（Debug `/MDd`）。

当前环境已经确认可直接使用：

- OpenSSL 3.5.8：`ThirdParty/OpenSSL/include`、`ThirdParty/OpenSSL/lib/libcrypto.lib`
- zlib 1.3.2：`ThirdParty/zlib/include`、`ThirdParty/zlib/lib/z[d].lib`

> 说明：本目录中的 zlib 实际把头文件放在 `ThirdParty/zlib/include/zlib.h`，并不存在 `zlib/zlib.h` 子目录。实现使用 `__has_include(<zlib/zlib.h>)` 优先兼容你要求的写法，找不到时自动回退到 `<zlib.h>`。

## 目录

```text
include/EnderVFiles2/            公共 API 与内部 Detail 头文件
src/                            实现
xmake.lua                       静态库目标（由根 xmake.lua 的 includes 引入）
```

## 接入 Xmake

根 `xmake.lua` 已经包含：

```lua
includes("Libraries/EnderVFiles2")
```

本库目标名为 `EnderVFiles2`。使用方只需要：

```lua
target("YourGame")
    set_kind("binary")
    add_deps("EnderVFiles2")
```

`xmake.lua` 会把 `libcrypto`、`z`（Debug 为 `zd`）以及包含目录沿静态目标继承给使用方。

单独构建本目标时：

```powershell
xmake f -m release
xmake build EnderVFiles2
```

## 快速使用

### 打包

```cpp
#include <EnderVFiles2/EnderVFiles2.hpp>

EnderVFiles2::PackerConfig config;
config.inputDirectory = "Assets";
config.outputDirectory = "Packed";
config.packageName = "GameAssets";
config.password = "your-password";       // 空字符串表示不加密
config.volumeSize = 1024ull * 1024ull * 1024ull; // 0 表示单卷
config.compressionLevel = EnderVFiles2::CompressionLevel::Default;
config.ioBufferSize = 2ull * 1024ull * 1024ull;
config.bufferAlignment = 64;              // 可按外部大页分配器调整到 2 MiB
config.memoryResource = myEngineMemoryResource;
config.overwriteExisting = false;

EnderVFiles2::Packer packer(std::move(config));
auto result = packer.pack();
if (!result)
{
    // result.error().code / result.error().message
    return;
}
```

输出布局：

```text
Packed/GameAssets.evfidx             索引
Packed/GameAssets.evfvol0000         数据卷 0
Packed/GameAssets.evfvol0001         数据卷 1
...
```

打包器扫描输入目录、生成包含根目录的完整目录树（空目录也会保留），按路径排序写入索引。每个文件完整落在一个卷中；当卷达到目标大小时，下一个文件开始新卷。单个文件大于 `volumeSize` 时不会被拆分，而是独占一个超目标大小的卷。

写入顺序为：体积数据写入临时文件 → 生成并写入索引 → 最后提交（先卷、后索引），因此读取方不会看到“索引已存在但卷不完整”的最终包。索引是最终包的权威入口。

### 挂载与同步读取

```cpp
EnderVFiles2::ReaderConfig readerConfig;
readerConfig.password = "your-password";
readerConfig.memoryResource = myEngineMemoryResource;
readerConfig.threadPool = myEngineThreadPool;
readerConfig.cacheCapacityBytes = 256ull * 1024ull * 1024ull; // 0 关闭缓存
readerConfig.bufferAlignment = 64;

EnderVFiles2::Reader reader(std::move(readerConfig));
auto mountResult = reader.mount("Packed/GameAssets.evfidx");
if (!mountResult)
{
    return;
}

auto bytes = reader.read("Textures/Grass.png");
if (bytes)
{
    EnderVFiles2::ByteSpan view = bytes.value()->bytes();
    // view 在 SharedBuffer 存活期间有效；缓存命中也返回共享所有权。
}
```

`read` 返回 `Result<SharedBuffer>`。`SharedBuffer` 是 `std::shared_ptr<const Buffer>`：返回缓存命中时不会复制数据；读取结果也会按配置写入 LRU 缓存。`Buffer` 的内存来自外部 `std::pmr::memory_resource`。

可用的目录查询：

```cpp
reader.exists("Textures");
reader.isDirectory("Textures");
auto info = reader.stat("Textures/Grass.png");

reader.listDirectory("Textures",
    [](std::string_view name, bool isDirectory, std::uint64_t size)
    {
        // name 是直接子项名称，回调期间有效
    });
```

### 资源加载扩展点

`load` / `loadAsync` 在 `read` / `readAsync` 成功后调用用户特化的 `ResourceLoader<Resource>`：

```cpp
struct Texture
{
    std::vector<std::byte> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

namespace EnderVFiles2
{
    template <>
    struct ResourceLoader<Texture>
    {
        static Texture load(const ResourceLoadContext& context)
        {
            Texture texture;
            // context.path: 虚拟路径
            // context.data: 已验证、已解密的完整文件字节
            // 在这里解码
            return texture;
        }
    };
}
auto texture = reader.load<Texture>("Textures/Grass.png");
if (texture)
{
    Texture& value = texture.value();
}
```

异步版本：

```cpp
EnderVFiles2::AsyncTask<Texture> task = reader.loadAsync<Texture>("Textures/Grass.png");

// 可选：先做别的工作
if (!task.isReady())
{
    // ...
}

auto result = task.get(); // 阻塞等待并取走结果，只能消费一次
```

### 外部线程池

线程池通过 concept 约束，不需要继承任何接口；只要提供可调用的 `submit(std::function<void()>)` 即可：

```cpp
class EngineThreadPool
{
public:
    void submit(std::function<void()> task)
    {
        m_pool.enqueue(std::move(task));
    }

private:
    YourThreadPool m_pool;
};

static_assert(EnderVFiles2::ThreadPool<EngineThreadPool>);

readerConfig.threadPool = engineThreadPool;
```

`ReaderConfig::threadPool` 的类型是 `ThreadPoolRef`，它只保存外部线程池的地址和一个类型擦除的调用入口，不拥有线程池。调用方必须保证线程池在 `Reader` 以及所有 `readAsync` / `loadAsync` 任务完成之前一直存活。线程池未设置时，异步 API 会返回 `ThreadPoolUnavailable` 错误。

### 外部内存分配器

库中所有大块数据（文件内容、索引、压缩/解压临时缓冲、缓存、`ReaderState`、异步任务状态）都通过 `std::pmr::memory_resource` 分配。外部可以实现自己的 `std::pmr::memory_resource`，在内部选择 2 MiB / 1 GiB 大页。库不会假设分配大小是 2 MiB 的整数倍，但建议：

- 用 2 MiB 大小的 arena 作为元数据/小对象的上游；
- 用大页服务大文件缓冲；
- `bufferAlignment` 可以设置为 2 MiB 以配合大页，默认 64；
- 该 allocator 必须比 `Reader`、所有 `SharedBuffer`、所有异步任务活得更久。

`PackerConfig::memoryResource` 同样生效。

## 安全设计

有密码时：

- 索引整体使用 **AES-256-GCM** 加密并认证，文件名、目录结构、每个文件的元数据都不以明文出现。
- 密码通过 **PBKDF2-HMAC-SHA256**（默认 300,000 次，随机 32 字节 salt）派生主密钥。
- 每个文件用随机 96-bit nonce，通过 **HKDF-SHA256** 派生独立文件密钥；AES-256-GCM 的 AAD 绑定包 UUID、路径、原始大小、存储大小、压缩/加密方式。
- 索引 GCM 的 AAD 绑定索引头（版本、KDF 参数、salt、nonce、包 UUID、大小、索引摘要）。
- 每个卷有独立头（magic/version/卷号/包 UUID/flags/CRC32），挂载时校验。
- GCM tag 验证失败、卷头不匹配、索引摘要不匹配都会在数据暴露前返回错误。

无密码时：

- 索引不加密，但仍带 SHA-256 摘要和头部 CRC32。
- 文件数据带 CRC32 校验（可选关闭）。
- 这种模式只提供完整性检测，不提供机密性或抗篡改认证；需要安全性时必须设置密码。

其他硬性保护：

- 索引解析全程有边界检查、计数上限、路径长度上限、分卷上限。
- 拒绝绝对路径、`..`、反斜杠路径、控制字符、重复条目、无父目录条目。
- 解压前按索引声明的精确大小分配输出，解压后校验大小、zlib 流尾部和 CRC32。
- 校验卷内 `offset + storedSize`，防止越界读取。
- 打包时拒绝输出目录位于输入目录内，避免把输出卷扫进包。
- 符号链接默认跳过（计入 `skippedEntries`），不跟随目录链接。
- 密码、主密钥、文件密钥、索引密钥在生命周期结束或出错路径上清零。

## 异常策略

根据根工程 Release 配置，本库实现不使用 `throw` / `try` / `catch`，所有公开 API 通过 `Result<T>` / `AsyncTask<T>` 报告错误。资源加载特化函数必须自行处理失败并返回一个资源值；在异常被禁用的构建中不得抛出异常。若使用方在启用异常的 target 中编译特化，同步 `load` 会让异常穿过 API，异步 `loadAsync` 中的异常会从工作线程逃逸，因此仍建议把特化实现为 `noexcept` 且不使用异常。

## 限制与约定

- 路径为 UTF-8 逻辑路径，使用 `/`；读取 API 也接受 `\`，会归一化为 `/`。
- 包名和卷名必须是合法 UTF-8 文件名；索引中的文件路径是逻辑路径，不做文件系统提取。
- 索引路径最大 4096 字节；包名最大 200 字节；卷名最大 255 字节。
- 默认最大条目数 4,000,000；Reader 端默认最大卷数 8,192（可配置）。
- 默认最大单文件 8 GiB；Reader 可配置。Packer 端单文件格式上限为 1 TiB。
- 默认最大索引文件 1 GiB、索引 payload 512 MiB；Reader 可配置。
- 默认 KDF 迭代次数 300,000；Packer 与 Reader 的上限均为 2,000,000。
- 单卷内文件不跨卷。卷大小是目标值，不是硬上限。
- 索引加密与内容加密同时开启；有密码时总是索引加密 + 内容加密。
- 不提供明文私钥导入接口：密码是唯一入口。
- `Reader::mount` 成功后密码会被清零；需要重新挂载时请构造新的 `Reader`。
- `mount` / `unmount` 不是线程安全的；请在开始并发读取之前完成挂载，并在所有读取/异步任务结束后卸载。
- `readAsync` / `loadAsync` 返回的 `AsyncTask` 只能 `get()` 一次；重复 `get()` 返回 `InvalidState`。
- `ResourceLoadContext::data` 和 `path` 只在加载函数执行期间有效；需要保留时请自行复制。
- 外部 `memory_resource` 必须比 `Reader`、所有 `SharedBuffer`、所有异步任务状态活得更久；外部线程池必须比所有异步任务活得更久。

## 需要你编译验证的点

1. Xmake 目标能否以 `/MD`（Debug `/MDd`）编译，且 OpenSSL/zlib 的 include/link 路径在你的上层配置下正确。
2. 大文件（> `compressionThresholdBytes`）流式加密路径。
3. 多卷边界、跨卷读取、空文件、空目录。
4. 错误密码、损坏索引、损坏卷、截断文件能否返回明确错误而不是崩溃。
5. 外部线程池的 `submit` 生命周期与你的引擎线程池实现是否匹配。
6. 外部 `memory_resource` 的 2 MiB / 1 GiB 大页分配策略。
