// tests/test_music_vibe.c —— 律动可视化的主机测试(-DMUSIC_VIBE_NO_LVGL)。
//
// 钉死:LUT 色带合法、音符映射与邻段外溢、满幅钳制、按帧衰减、峰帽跟随
// 与 0.55/s 回落、以及三条省渲染铁律里的核心——属性差分(值不变零 setter)。
#include "music_vibe.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    // ---- LUT:32 项,首尾深蓝闭环,中段走暖色 ----
    music_vibe_init();
    const uint16_t c0 = music_vibe_test_lut(0);
    const uint16_t c31 = music_vibe_test_lut(31);
    assert(c0 == c31);                        // 闭环
    // RGB565 提取:深蓝 = 蓝>红
    const uint16_t b0 = c0 & 0x1F, r0 = (c0 >> 11) & 0x1F;
    assert(b0 > r0);
    // 黄区(约 idx 12-14)红绿高蓝低
    const uint16_t cy = music_vibe_test_lut(13);
    const uint16_t ry = (cy >> 11) & 0x1F, gy = (cy >> 5) & 0x3F, by = cy & 0x1F;
    assert(ry > 20 && gy > 40 && by < ry / 2);
    printf("ok  LUT 32 项闭环,锚点色正确\n");

    // ---- 音符映射:pitch 36 → 段 0;邻段外溢 0.35/0.45 ----
    music_vibe_init();
    music_vibe_note_on(36, 127);              // 主段 +0.8,右邻 +0.45
    const uint16_t main = music_vibe_test_band(0);
    const uint16_t right = music_vibe_test_band(1);
    assert(main > 200 && main <= 256);        // 0.8×256 ≈ 205
    assert(right > 110 && right < 120);       // 0.45×256 ≈ 115
    music_vibe_note_on(36, 127);              // 再来一次:右邻继续外溢
    assert(music_vibe_test_band(1) > right);
    // 左邻:pitch 44 → 段 2
    music_vibe_init();
    music_vibe_note_on(44, 127);
    assert(music_vibe_test_band(1) > 80 && music_vibe_test_band(3) > 100);
    // 边界钳制:pitch<36 → 段 0;pitch 79 → 段 9((79-36)/4=10→9)
    music_vibe_init();
    music_vibe_note_on(20, 127);
    assert(music_vibe_test_band(0) > 200);
    music_vibe_note_on(79, 127);
    assert(music_vibe_test_band(9) > 200);
    printf("ok  音符映射与外溢\n");

    // ---- 满幅钳制:疯狂连打不超过 1.0 ----
    for (int k = 0; k < 50; k++) music_vibe_note_on(48, 127);
    for (int i = 0; i < 10; i++) assert(music_vibe_test_band(i) <= 256);
    printf("ok  满幅钳制\n");

    // ---- 差分铁律:稳定输入,第二帧零 setter ----
    music_vibe_init();
    music_vibe_enable(3);                     // A+B 全开
    music_vibe_note_on(48, 100);
    (void)music_vibe_test_setters();          // 清零点(读不清,用差值)
    const uint32_t s1 = music_vibe_test_setters();
    music_vibe_tick(50);
    const uint32_t s2 = music_vibe_test_setters();
    // 首帧:几何从 0xFF 强制态落位,setter 大量发生
    assert(s2 > s1);
    // 数据静止(衰减仍在):衰减让 band 变化 → 有更新;构造“无变化”场景:
    // band 已 0 时再 tick,全部值稳定 → setter 应为 0
    for (int k = 0; k < 40; k++) music_vibe_tick(50);   // 衰减到 0
    const uint32_t sa = music_vibe_test_setters();
    music_vibe_tick(50);
    const uint32_t sb = music_vibe_test_setters();
    assert(sb == sa);                         // 静止帧:一个 setter 都不调
    printf("ok  属性差分:静止帧零 setter(%u 帧前后 %u==%u)\n",
           (unsigned)1, (unsigned)sa, (unsigned)sb);

    // ---- 峰帽:跟随向上,0.55/s 回落 ----
    music_vibe_init();
    music_vibe_enable(3);
    music_vibe_note_on(48, 127);              // 段 3 主段
    music_vibe_tick(50);
    const uint16_t cap0 = music_vibe_test_cap(3);
    assert(cap0 == music_vibe_test_band(3));  // 立即跟随
    for (int k = 0; k < 20; k++) music_vibe_tick(50);   // 1 秒无音符
    const uint16_t cap1 = music_vibe_test_cap(3);
    const uint16_t band1 = music_vibe_test_band(3);
    assert(band1 < cap1);                     // 帽悬在 band 上
    assert(cap1 < cap0);                      // 且在回落
    const uint16_t expect_drop = (uint16_t)(0.55 * 256);   // ≈141 q8/s
    assert(cap0 - cap1 > expect_drop / 3 && cap0 - cap1 < expect_drop * 2);
    printf("ok  峰帽跟随与 0.55/s 回落(%u→%u, band %u)\n",
           cap0, cap1, band1);

    printf("全部通过\n");
    return 0;
}
