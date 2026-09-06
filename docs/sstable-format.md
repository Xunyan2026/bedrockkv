# BedrockKV SSTable 文件格式（v1，阶段 2 定稿）

> 状态：**已实现**（`include/bedrockkv/sstable.h`）。改格式必须先改本文档并升级
> magic number。对照阅读：leveldb `table/format.h`、`table/block_builder.cc`。

## 1. 文件总布局

```
 offset 0
 +---------------------+
 | Data Block 0        |
 +---------------------+
 | Data Block 1        |
 +---------------------+
 | ...                 |
 +---------------------+
 | Filter Block        |   每个 data block 一个 Bloom 过滤器
 +---------------------+
 | Index Block         |   每个 data block 一条: (块内最大内部键 → 块句柄)
 +---------------------+
 | Footer (44 B)       |   句柄 + 条目数 + 全文件 CRC + magic
 +---------------------+   ← 文件尾
```

所有多字节整数一律 **小端 (LE)**。所有"块"复用同一套 entry 编码
（见 §2），包括 Index Block。

## 2. Block 编码（前缀压缩 + restart point）

块内 entry 按内部键严格递增排列（比较器见 §6）：

```
 entry := [shared u32][non_shared u32][value_len u32][key_delta][value]
```

- `shared`：与**上一个 entry** 的键的最长公共前缀长度；`key_delta` 是去掉
  前缀后的剩余字节。同一 user key 的多个版本前缀极长，压缩率高。
- **restart point**：每 16 个 entry 强制 `shared=0` 存一次完整键，其在本块内
  的偏移记录在块尾的重启数组里：

```
 [entries ...][restart_offset_0 u32]...[restart_offset_{n-1} u32][num_restarts u32]
```

作用（面试考点）：前缀压缩省空间；restart 点让块内查找可以先对重启数组
二分、再线性扫描一小段，把最坏 O(n) 的解压查找变成 O(n/16) 次比较 +
O(16) 次扫描——**不需要解压整个块**。

## 3. 内部键 (Internal Key)

```
 internal_key := user_key ++ tag(u64 LE)      tag := (seq << 8) | value_type
```

`value_type`：`0x0` 墓碑 / `0x1` 值。与 MemTable 的编码思想一致，但 SST 里
键**不带长度前缀**——块的 entry 头自带 `non_shared`，天然定界。

## 4. Filter Block（Bloom，目标假阳性 1%）

每个 data block 一个过滤器，键取该块的 **user key**（不含 tag）。

```
 [filter_0 bytes][filter_1 bytes]...[filter_{n-1}]
 [filter_start_0 u32]...[filter_start_{n-1} u32][filter_start_n u32][array_start u32]
```

单个过滤器（leveldb 的 `bloom.cc` 布局）：

```
 [bits 数组: ceil(n*bits_per_key/8) 字节][k u8]
```

- `bits_per_key = 10` → `m/n = 10`，`k = round(ln2 · 10) = 7`；
  假阳性率 ≈ `(1 - e^{-7/10})^7 ≈ 0.82%`。
- 哈希：`h1 = Crc32(key, seed=0xbc9f1d34)`，`delta = rotl32(h1, 15)`，
  双重哈希模拟 k 个独立探针：`h_{i+1} = h_i + delta (mod m)`。
- **无假阴性**是硬保证（只可能误报"可能存在"，绝不漏报"存在"）——
  读路径据此安全跳过整块。
- 空过滤器（0 键）→ `KeyMayMatch` 恒返回 true（保守）。

## 5. Index Block 与 Footer

Index Block：普通 block 编码，entry 为

```
 key   = 该 data block 的最大内部键（精确值，非分隔符）
 value = [block_offset u64][block_size u32]
```

用精确最大键 + "找第一个 ≥ 目标的索引项"即可正确定位：目标块 i 满足
`max_key(i-1) < target ≤ max_key(i)`，first-≥ 语义恰好选中它。

Footer（44 字节，固定在文件尾）：

```
 [filter_offset u64][filter_size u32]     12B
 [index_offset   u64][index_size   u32]   12B
 [num_entries    u64]                      8B
 [file_crc32     u32]                      4B   ← 覆盖 [0, footer_start)
 [magic          8B]                       8B   ← "BRKVSST1" (0x42 0x52 ...)
```

打开文件时先校验 magic，再流式计算全文件 CRC 比对 footer——任何一处
位翻转、截断都挡在读取之前。块级完整性由 CRC 全文件覆盖保证（leveldb
用块内压缩类型字段间接校验 + footer 无全文件 CRC；我们直接存全文件
CRC，更简单也更严）。

## 6. 内部键比较器

```
 a < b  ⇔  user(a) < user(b)
           或 user(a) == user(b) 且 tag(a) > tag(b)   （新版本在前）
```

同一 user key 的各版本按 seq 降序相邻存放 ⇒ 块内 Seek 到
`(user_key, tag=0xFF..FF)` 的第一个 ≥ 项必然是该 user key 的最新版本。

## 7. 阶段 2 的已知简化（有意为之，后续阶段回收）

| 简化 | 现状 | 回收点 |
|------|------|--------|
| 整文件读入内存 | `Table::Open` 一次 read + CRC 校验，之后块访问全在内存 | 阶段 2 读路径步骤换 pread + 分片 LRU Block Cache |
| Index 用精确最大键 | 比分隔符键略费空间 | 无正确性影响，优化项 |
| 无压缩 (无 Snappy) | value 原样存 | 可选优化 |
| Builder 单次成文 | 整文件在内存拼好后一次写出 | 大 SST 需流式写，优化项 |
