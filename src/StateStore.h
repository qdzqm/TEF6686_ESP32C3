#ifndef STATE_STORE_H
#define STATE_STORE_H

#include <Arduino.h>
#include <Wire.h>

/* ============================================================
 * 24C64BN EEPROM 均匀磨损(磨损均衡)状态存储模块
 *
 * 芯片: AT24C64 (8192 字节, 64 kbit, 2 字节词地址)
 * 地址: 0x50 (7 位)
 * 页大小: 32 字节
 * 单字节擦写寿命: ~1,000,000 次
 *
 * 设计: 把整片 EEPROM 划分为 N 个固定大小的槽(Slot)，
 *       数据轮询写入下一个槽，从而把擦写磨损均匀分摊到所有槽。
 *       每写完一轮(128 次保存)才回到起始地址，
 *       配合 1M 次/字节寿命，总寿命约 1.28 亿 次状态保存，
 *       远超本体寿命，杜绝"总写一个地址"导致提前报废。
 *
 * 每个槽带: 魔数 + 版本 + 顺序号(seq) + 校验和。
 *   - 顺序号 seq 用于上电时找到"最新"的槽(带 16 位回绕处理)。
 *   - 校验和用于识别"写到一半断电"导致的损坏槽，启动时自动跳过。
 * ============================================================
 */

#define EEPROM_ADDR   0x50
#define EEPROM_SIZE   8192      /* 24C64 = 8192 字节 */
#define EEPROM_PAGE   32        /* 页大小字节        */

#define SLOT_SIZE     64        /* 每个槽大小(64 % 32 == 0 页对齐) */
#define SLOT_COUNT    (EEPROM_SIZE / SLOT_SIZE)   /* 128 个槽 */

/* 槽内记录布局(共 64 字节) */
#define REC_MAGIC0    0x00
#define REC_MAGIC1    0x01
#define REC_VERSION   0x02
#define REC_RESERVED  0x03
#define REC_SEQ_LO    0x04
#define REC_SEQ_HI    0x05
#define REC_CHECKSUM  0x06
#define REC_DATA      0x07
#define REC_DATA_LEN  (SLOT_SIZE - REC_DATA)   /* 57 字节负载区 */

static const uint8_t MAGIC_FIRST  = 0xA5;
static const uint8_t MAGIC_SECOND = 0x5A;
static const uint8_t MAGIC_VERSION = 0x01;

/* 运行时的轮询游标 */
static uint16_t st_currentSlot = 0;
static uint16_t st_seq = 0;

/* ---------- 底层 I2C 读写(24C64, 2 字节地址) ---------- */

static bool eepromRead(uint16_t addr, uint8_t *buf, uint16_t len) {
    Wire.beginTransmission(EEPROM_ADDR);
    if (Wire.write((uint8_t)(addr >> 8)) != 1 ||
        Wire.write((uint8_t)(addr & 0xFF)) != 1) {
        Wire.endTransmission();
        return false;
    }
    if (Wire.endTransmission() != 0) return false;
    uint16_t got = Wire.requestFrom((int)EEPROM_ADDR, (int)len);
    if (got < len) return false;
    for (uint16_t i = 0; i < len; i++) buf[i] = (uint8_t)Wire.read();
    return true;
}

/* 单页页写(≤32 字节，避免跨页)。写完后等待芯片内部写周期。 */
static bool eepromWritePage(uint16_t addr, const uint8_t *buf, uint16_t len) {
    Wire.beginTransmission(EEPROM_ADDR);
    if (Wire.write((uint8_t)(addr >> 8)) != 1 ||
        Wire.write((uint8_t)(addr & 0xFF)) != 1) {
        Wire.endTransmission();
        return false;
    }
    for (uint16_t i = 0; i < len; i++) {
        if (Wire.write(buf[i]) != 1) { Wire.endTransmission(); return false; }
    }
    if (Wire.endTransmission() != 0) return false;
    delay(5);   /* 24C64 内部写周期 ~5ms */
    return true;
}

/* 广义写: 自动把 len 拆成多个页内小段写入(每段不超过写页阈值) */
static bool eepromWrite(uint16_t addr, const uint8_t *buf, uint16_t len) {
    const uint16_t CHUNK = 16;
    uint16_t off = 0;
    while (off < len) {
        uint16_t n = len - off;
        if (n > CHUNK) n = CHUNK;
        if (!eepromWritePage(addr + off, buf + off, n)) return false;
        off += n;
    }
    return true;
}

/* ---------- 槽位读写 ---------- */

static uint16_t slotOffset(uint16_t idx) { return idx * SLOT_SIZE; }

static uint8_t slotChecksum(const uint8_t *data) {
    uint8_t sum = 0;
    for (uint16_t i = 0; i < REC_DATA_LEN; i++) sum = (uint8_t)(sum + data[REC_DATA + i]);
    return sum;
}

static bool seqNewer(uint16_t a, uint16_t b) {
    return (int16_t)((uint16_t)a - (uint16_t)b) > 0;
}

static bool slotCheckValid(const uint8_t *slot) {
    if (slot[REC_MAGIC0] != MAGIC_FIRST ||
        slot[REC_MAGIC1] != MAGIC_SECOND ||
        slot[REC_VERSION] != MAGIC_VERSION) return false;
    return slotChecksum(slot) == slot[REC_CHECKSUM];
}

static uint16_t slotSeqOf(const uint8_t *slot) {
    return (uint16_t)(((uint16_t)slot[REC_SEQ_HI] << 8) | slot[REC_SEQ_LO]);
}

/* ---------- 公共接口 ---------- */

/* st_open 定位到的"最新有效槽"缓存, 供 st_load 直接读取, 避免二次全表扫描 */
static uint16_t st_cachedBest = 0;
static bool     st_cachedFound = false;

/* 打开存储(启动时调用一次): 扫描全片, 定位最新槽位并缓存 */
static void st_open(void) {
    uint16_t best = 0;
    uint16_t bestSeq = 0;
    bool found = false;
    uint8_t slot[SLOT_SIZE];

    for (uint16_t i = 0; i < SLOT_COUNT; i++) {
        eepromRead(slotOffset(i), slot, SLOT_SIZE);   /* 每个槽只读一次 */
        if (slotCheckValid(slot)) {
            uint16_t s = slotSeqOf(slot);
            if (!found || seqNewer(s, bestSeq)) { best = i; bestSeq = s; found = true; }
        }
    }
    st_cachedBest = best;
    st_cachedFound = found;
    if (found) {
        st_seq = bestSeq;
        st_currentSlot = (uint16_t)((best + 1) % SLOT_COUNT);
    } else {
        st_seq = 0xFFFF;              /* 首个保存将从 seq=0 开始 */
        st_currentSlot = 0;
    }
}

/* 保存一份负载(轮询写入下一个槽, 均匀磨损) */
static bool st_save(const uint8_t *data) {
    uint16_t idx = st_currentSlot;
    st_seq++;

    uint8_t slot[SLOT_SIZE];

    /* 先把目标槽写满 0xFF(抹除旧记录), 避免读到半新半旧的损坏数据 */
    memset(slot, 0xFF, SLOT_SIZE);
    if (!eepromWrite(slotOffset(idx), slot, SLOT_SIZE)) return false;

    /* 组装新记录 */
    memset(slot, 0x00, SLOT_SIZE);
    slot[REC_MAGIC0] = MAGIC_FIRST;
    slot[REC_MAGIC1] = MAGIC_SECOND;
    slot[REC_VERSION] = MAGIC_VERSION;
    slot[REC_RESERVED] = 0x00;
    slot[REC_SEQ_LO] = (uint8_t)(st_seq & 0xFF);
    slot[REC_SEQ_HI] = (uint8_t)((st_seq >> 8) & 0xFF);
    memcpy(&slot[REC_DATA], data, REC_DATA_LEN);
    slot[REC_CHECKSUM] = slotChecksum(slot);

    if (!eepromWrite(slotOffset(idx), slot, SLOT_SIZE)) return false;

    st_currentSlot = (uint16_t)((idx + 1) % SLOT_COUNT);
    return true;
}

/* 读取最新一次保存的负载到 data。返回 true 表示有有效记录。
 * 优先复用 st_open 定位到的缓存槽, 避免再全片扫描。 */
static bool st_load(uint8_t *data) {
    uint16_t best;
    bool found;
    if (st_cachedFound) {
        best = st_cachedBest;      /* 复用 st_open 的结果 */
        found = true;
    } else {
        uint16_t bestSeq = 0;
        found = false;
        uint8_t tmp[SLOT_SIZE];
        for (uint16_t i = 0; i < SLOT_COUNT; i++) {
            eepromRead(slotOffset(i), tmp, SLOT_SIZE);
            if (slotCheckValid(tmp)) {
                uint16_t s = slotSeqOf(tmp);
                if (!found || seqNewer(s, bestSeq)) { bestSeq = s; best = i; found = true; }
            }
        }
        if (found) { st_cachedBest = best; st_cachedFound = true; }
    }
    if (!found) return false;

    uint8_t slot[SLOT_SIZE];
    eepromRead(slotOffset(best), slot, SLOT_SIZE);
    memcpy(data, &slot[REC_DATA], REC_DATA_LEN);
    return true;
}

#endif /* STATE_STORE_H */