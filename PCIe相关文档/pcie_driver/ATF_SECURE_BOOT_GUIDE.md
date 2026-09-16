# ATF 安全启动实现指南

## 概述

**安全启动（Secure Boot）** 是确保系统从可信固件启动的安全机制。在 ARM Trusted Firmware (ATF) 中，安全启动通过验证每个启动阶段的固件签名来确保系统完整性。

---

## 安全启动基本原理

### 1. 启动链（Boot Chain）

```
Boot ROM (不可变)
    ↓ 验证签名
BL1 (ATF Bootloader Stage 1)
    ↓ 验证签名
BL2 (ATF Bootloader Stage 2)
    ↓ 验证签名
BL31 (ATF Runtime Firmware)
    ↓ 验证签名
BL32 (Secure World OS, 如 OP-TEE)
    ↓ 验证签名
BL33 (Normal World Bootloader, 如 U-Boot)
    ↓ 验证签名
Linux Kernel
```

### 2. 密钥体系

```
Root of Trust (RoT)
    │
    ├─> Root Public Key (RPK) - 根公钥（烧录在 eFuse/OTP 中）
    │
    ├─> Certificate Chain (证书链)
    │   ├─> Root Certificate (根证书)
    │   ├─> Intermediate Certificate (中间证书)
    │   └─> Content Certificate (内容证书)
    │
    └─> Key Revocation List (密钥吊销列表)
```

---

## ATF 安全启动实现

### 1. 密钥管理

#### 密钥存储位置

- **eFuse/OTP**：根公钥（Root Public Key）通常烧录在芯片的 eFuse 或 OTP 中
- **Flash**：证书链和签名存储在 Flash 中
- **密钥吊销列表**：存储在 Flash 或 eFuse 中

#### 密钥格式

ATF 使用 **X.509 证书** 和 **RSA/PSS 签名**：

```
证书结构：
- Subject: 证书主体（如 "BL2"）
- Issuer: 证书颁发者（如 "Root CA"）
- Public Key: 公钥（RSA 2048/4096 bit）
- Signature: 证书签名
- Validity: 有效期
```

### 2. 签名验证流程

#### BL1 验证 BL2

```c
// 伪代码示例（基于 ATF 实现）

int bl1_verify_bl2(void)
{
    // 1. 从 Flash 读取 BL2 镜像
    image_info_t bl2_image;
    load_image_from_flash(&bl2_image, BL2_BASE);
    
    // 2. 读取 BL2 的证书和签名
    cert_t bl2_cert;
    signature_t bl2_sig;
    load_cert_and_signature(&bl2_cert, &bl2_sig, BL2_CERT_BASE);
    
    // 3. 验证证书链
    // 3.1 验证内容证书（Content Certificate）
    if (verify_cert_chain(&bl2_cert, &root_pub_key) != 0) {
        return -1;  // 证书验证失败
    }
    
    // 3.2 检查密钥是否被吊销
    if (is_key_revoked(&bl2_cert) != 0) {
        return -1;  // 密钥已被吊销
    }
    
    // 4. 验证 BL2 镜像签名
    // 4.1 计算 BL2 镜像的哈希值
    uint8_t image_hash[SHA256_DIGEST_SIZE];
    sha256_hash(bl2_image.data, bl2_image.size, image_hash);
    
    // 4.2 使用证书中的公钥验证签名
    if (rsa_verify_signature(image_hash, &bl2_sig, &bl2_cert.pub_key) != 0) {
        return -1;  // 签名验证失败
    }
    
    // 5. 验证通过，跳转到 BL2
    return 0;
}
```

#### BL2 验证后续阶段

```c
// BL2 验证 BL31/BL32/BL33

int bl2_verify_next_stage(image_id_t image_id)
{
    image_info_t image;
    cert_t cert;
    signature_t sig;
    
    // 1. 加载镜像和证书
    load_image_and_cert(image_id, &image, &cert, &sig);
    
    // 2. 验证证书链（使用 Root Public Key）
    if (verify_cert_chain(&cert, &root_pub_key) != 0) {
        return -1;
    }
    
    // 3. 检查密钥吊销
    if (is_key_revoked(&cert) != 0) {
        return -1;
    }
    
    // 4. 验证镜像签名
    uint8_t hash[SHA256_DIGEST_SIZE];
    sha256_hash(image.data, image.size, hash);
    
    if (rsa_verify_signature(hash, &sig, &cert.pub_key) != 0) {
        return -1;
    }
    
    return 0;
}
```

### 3. 密钥吊销检查

#### 吊销列表存储

```c
// 密钥吊销列表结构
typedef struct {
    uint32_t num_revoked_keys;      // 吊销密钥数量
    key_id_t revoked_keys[MAX_REVOKED_KEYS];  // 吊销密钥 ID 列表
} key_revocation_list_t;

// 检查密钥是否被吊销
int is_key_revoked(cert_t *cert)
{
    key_revocation_list_t *rev_list;
    key_id_t cert_key_id;
    
    // 1. 从 eFuse/Flash 读取吊销列表
    rev_list = get_revocation_list();
    
    // 2. 从证书中提取密钥 ID
    cert_key_id = extract_key_id(cert);
    
    // 3. 在吊销列表中查找
    for (int i = 0; i < rev_list->num_revoked_keys; i++) {
        if (rev_list->revoked_keys[i] == cert_key_id) {
            return 1;  // 密钥已被吊销
        }
    }
    
    return 0;  // 密钥未被吊销
}
```

### 4. 证书链验证

```c
// 验证证书链（从内容证书到根证书）
int verify_cert_chain(cert_t *content_cert, rsa_pub_key_t *root_pub_key)
{
    cert_t *current_cert = content_cert;
    cert_t *parent_cert;
    
    // 从内容证书开始，逐级向上验证
    while (current_cert != NULL) {
        // 1. 获取父证书（颁发者）
        parent_cert = get_parent_cert(current_cert);
        
        if (parent_cert == NULL) {
            // 到达根证书，使用 Root Public Key 验证
            if (rsa_verify_cert_signature(current_cert, root_pub_key) != 0) {
                return -1;  // 根证书验证失败
            }
            break;
        }
        
        // 2. 使用父证书的公钥验证当前证书的签名
        if (rsa_verify_cert_signature(current_cert, &parent_cert->pub_key) != 0) {
            return -1;  // 证书链验证失败
        }
        
        // 3. 检查证书有效期
        if (is_cert_expired(current_cert) != 0) {
            return -1;  // 证书已过期
        }
        
        // 4. 继续验证上一级证书
        current_cert = parent_cert;
    }
    
    return 0;  // 证书链验证通过
}
```

---

## ATF 配置

### 1. 编译选项

```makefile
# Makefile 或 build 配置

# 启用安全启动
ENABLE_TRUSTED_BOOT=1

# 密钥文件路径
ROOT_KEY=keys/root_key.pem
CONTENT_KEY=keys/content_key.pem

# 签名工具
SIGN_TOOL=tools/cert_create/cert_create
```

### 2. 密钥生成

```bash
# 生成根密钥对
openssl genrsa -out root_key.pem 2048
openssl rsa -in root_key.pem -pubout -out root_pub_key.pem

# 生成内容密钥对
openssl genrsa -out content_key.pem 2048
openssl rsa -in content_key.pem -pubout -out content_pub_key.pem

# 使用 ATF 工具创建证书
cert_create -n "Root CA" -k root_key.pem -c root_cert.pem
cert_create -n "Content Cert" -k content_key.pem -c content_cert.pem \
    -s root_key.pem -S root_cert.pem
```

### 3. 镜像签名

```bash
# 签名 BL2 镜像
sign_image -k content_key.pem -c content_cert.pem \
    -i bl2.bin -o bl2_signed.bin

# 签名 BL31 镜像
sign_image -k content_key.pem -c content_cert.pem \
    -i bl31.bin -o bl31_signed.bin
```

---

## 实际代码示例（基于 ATF）

### 1. BL1 中的验证

```c
// bl1/bl1_main.c (简化版)

void bl1_main(void)
{
    // ... 初始化代码 ...
    
    // 验证 BL2
    if (bl1_plat_handle_pre_image_load(BL2_IMAGE_ID) != 0) {
        ERROR("BL1: Failed to handle pre-image load\n");
        panic();
    }
    
    // 加载 BL2 镜像
    if (load_auth_image(BL2_IMAGE_ID, bl2_base, bl2_size) != 0) {
        ERROR("BL1: Failed to load/authenticate BL2\n");
        panic();
    }
    
    // 跳转到 BL2
    bl1_run_bl2(bl2_base);
}
```

### 2. 镜像加载和验证

```c
// common/auth_mod.c (简化版)

int load_auth_image(unsigned int image_id, void *image_base, size_t image_size)
{
    image_info_t image_info;
    auth_img_desc_t *img_desc;
    
    // 1. 获取镜像描述符
    img_desc = FCONF_GET_PROPERTY(auth, config, img_auth_config, image_id);
    
    // 2. 加载镜像
    if (load_image(image_id, &image_info) != 0) {
        return -1;
    }
    
    // 3. 验证镜像
    if (auth_mod_verify_img(image_id, &image_info, img_desc) != 0) {
        return -1;
    }
    
    return 0;
}
```

### 3. 签名验证

```c
// drivers/auth/mbedtls/mbedtls_crypto.c (简化版)

int verify_signature(void *data_ptr, unsigned int data_len,
                     void *sig_ptr, unsigned int sig_len,
                     void *sig_alg, void *pk_ptr, unsigned int pk_len)
{
    mbedtls_rsa_context rsa;
    mbedtls_md_context_t md_ctx;
    unsigned char hash[MBEDTLS_MD_MAX_SIZE];
    int ret;
    
    // 1. 初始化 RSA 上下文
    mbedtls_rsa_init(&rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
    
    // 2. 导入公钥
    ret = mbedtls_rsa_import_raw(&rsa, pk_ptr, pk_len, NULL, 0, NULL, 0, NULL, 0);
    if (ret != 0) {
        return -1;
    }
    
    // 3. 计算数据哈希
    mbedtls_md_init(&md_ctx);
    mbedtls_md_setup(&md_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&md_ctx);
    mbedtls_md_update(&md_ctx, data_ptr, data_len);
    mbedtls_md_finish(&md_ctx, hash);
    
    // 4. 验证签名
    ret = mbedtls_rsa_rsassa_pss_verify(&rsa, NULL, NULL,
                                        MBEDTLS_RSA_PUBLIC,
                                        MBEDTLS_MD_SHA256,
                                        MBEDTLS_MD_SHA256_DIGEST_LENGTH,
                                        hash, sig_ptr);
    
    mbedtls_rsa_free(&rsa);
    mbedtls_md_free(&md_ctx);
    
    return (ret == 0) ? 0 : -1;
}
```

---

## 密钥吊销实现

### 1. 吊销列表配置

```c
// 在设备树或配置文件中定义吊销列表

#define REVOKED_KEY_LIST_BASE  0x80000000  // Flash 中的地址
#define REVOKED_KEY_LIST_SIZE  1024        // 1KB

// 吊销列表结构
typedef struct {
    uint32_t magic;              // 魔数，用于验证
    uint32_t version;            // 版本号
    uint32_t num_keys;           // 吊销密钥数量
    key_id_t keys[];             // 密钥 ID 数组
} revoked_key_list_t;
```

### 2. 吊销检查实现

```c
// 检查密钥是否被吊销
int check_key_revocation(key_id_t key_id)
{
    revoked_key_list_t *rev_list;
    uint32_t i;
    
    // 1. 读取吊销列表
    rev_list = (revoked_key_list_t *)REVOKED_KEY_LIST_BASE;
    
    // 2. 验证魔数
    if (rev_list->magic != REVOKED_KEY_LIST_MAGIC) {
        ERROR("Invalid revocation list magic\n");
        return -1;
    }
    
    // 3. 遍历吊销列表
    for (i = 0; i < rev_list->num_keys; i++) {
        if (rev_list->keys[i] == key_id) {
            ERROR("Key %08x is revoked\n", key_id);
            return 1;  // 密钥已被吊销
        }
    }
    
    return 0;  // 密钥未被吊销
}
```

---

## 安全启动流程总结

### 完整流程

```
1. Boot ROM 启动
   ├─> 读取 Root Public Key（从 eFuse）
   ├─> 读取 BL1 镜像（从 Flash）
   └─> 验证 BL1 签名（使用 Root Public Key）

2. BL1 执行
   ├─> 读取 BL2 镜像和证书（从 Flash）
   ├─> 验证证书链（使用 Root Public Key）
   ├─> 检查密钥吊销
   ├─> 验证 BL2 签名
   └─> 跳转到 BL2

3. BL2 执行
   ├─> 验证 BL31/BL32/BL33（同样流程）
   └─> 跳转到下一阶段

4. 后续阶段
   └─> 每个阶段都验证下一阶段的签名
```

### 关键点

1. **Root of Trust**：根公钥存储在不可变的 eFuse 中
2. **证书链**：从内容证书到根证书的完整验证
3. **密钥吊销**：检查密钥是否在吊销列表中
4. **签名验证**：使用 RSA/PSS 算法验证镜像哈希
5. **失败处理**：验证失败时系统停止启动或进入安全模式

---

## 常见问题

### 1. 如何更新密钥？

- **根密钥**：一旦烧录到 eFuse，无法更改（除非芯片支持密钥更新）
- **内容密钥**：可以通过更新证书链来更换
- **密钥吊销**：通过更新吊销列表来吊销旧密钥

### 2. 性能影响？

- **启动时间**：每次验证会增加启动时间（通常几十到几百毫秒）
- **存储空间**：证书和签名占用额外 Flash 空间
- **计算开销**：RSA 签名验证需要一定的 CPU 时间

### 3. 如何调试？

- **日志输出**：启用 `LOG_LEVEL=40` 查看详细验证过程
- **错误码**：根据错误码定位问题
- **测试模式**：可以暂时禁用验证进行调试（生产环境必须启用）

---

## 参考文档

- [ARM Trusted Firmware Documentation](https://trustedfirmware-a.readthedocs.io/)
- [ATF Authentication Framework](https://trustedfirmware-a.readthedocs.io/en/latest/design/auth-framework.html)
- [ATF Secure Boot](https://trustedfirmware-a.readthedocs.io/en/latest/design/trusted-board-boot.html)

---

## 总结

ATF 的安全启动实现包括：

1. **密钥管理**：根公钥存储在 eFuse，证书链存储在 Flash
2. **签名验证**：每个启动阶段都验证下一阶段的签名
3. **证书链验证**：从内容证书到根证书的完整验证
4. **密钥吊销**：检查密钥是否在吊销列表中
5. **失败处理**：验证失败时系统停止启动

这是一个完整的安全启动机制，确保系统从可信固件启动，防止恶意代码执行。

