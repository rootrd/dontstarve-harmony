import fs from '@ohos.file.fs'

/**
 * 屏幕虚拟手柄（Switch 排法）的数据模型 + 默认布局 + 沙箱持久化。
 * 结构照 E:\StardewValley-HOS 的 model/PadLayout.ets（同一套 gp 编号与归一化坐标约定），
 * 差别两处：
 *   ① 注入通道换成本工程的 dmhost.padBtn / padAxis（那边是 SDL 虚拟手柄）；
 *   ② 键位按 Switch：面键 上 X、下 B、左 Y、右 A（Xbox 是 下 A、右 B、左 X、上 Y），
 *      LB/RB → L/R，LT/RT → ZL/ZR，BACK/START → －/＋。
 *      注意：padBtn 发的是「游戏键」，字母跟着 Switch 键帽画在 Switch 的位置上，
 *      所以 DM_PAD_LAYOUT 只影响实体手柄的键帽换算，动不了这套屏幕按键。
 *
 * 坐标约定（照搬模板）：x / y = 元素中心相对操作区宽高的 0-1；size = 半径相对 U=min(宽,高)，
 * 绘制直径 = 2*size*U。横屏 2848x1276 下 U=1276px，0.055 ≈ 半径 70px ≈ 直径 41vp。
 *
 * gp 编号（与星露谷工程一致，改的是含义那侧的映射表，见 dm/ohos/pad_ohos.c 的 v_map）：
 *   0-3 = A/B/X/Y   4 = －   6 = ＋   7/8 = LS/RS 按下   9/10 = L/R
 *   100/101 = ZL/ZR  200 = 左摇杆（300-303 = 十字 上/下/左/右）
 */
export interface PadBtnCfg {
  id: string
  label: string
  kind: string   // 'btn' | 'stick'
  gp: number
  x: number
  y: number
  size: number
  color: string
}

const S: number = 0.055     // 十字 / 肩 / －＋ 的半径比例
const F: number = 0.062     // 面键半径比例（比十字略大，好按）
const K: number = 0.125     // 左摇杆底盘半径比例

function cfg(id: string, label: string, kind: string, gp: number, x: number, y: number,
  size: number, color: string): PadBtnCfg {
  return { id: id, label: label, kind: kind, gp: gp, x: x, y: y, size: size, color: color }
}

/**
 * 默认布局 = Switch 横屏排法，左右手各一坨、中间留给游戏自己的 UI：
 *   左手：左摇杆（中）+ 十字（右下）+ ZL/L（上沿）
 *   右手：面键菱形（Switch 位：X 上、B 下、Y 左、A 右）+ R/ZR（上沿）
 *   中上：－ 与 ＋
 * 取值按 2848x1276 实机算：相邻键中心距 ≥ 直径，互不重叠；贴边的也不出界。
 */
export function defaultSwitchLayout(): PadBtnCfg[] {
  return [
    // 左摇杆 / 十字
    cfg('ls', 'LS', 'stick', 200, 0.084, 0.500, K, '#f5f5f5'),
    cfg('dup', '↑', 'btn', 300, 0.225, 0.582, S, '#f5f5f5'),
    cfg('ddown', '↓', 'btn', 301, 0.225, 0.818, S, '#f5f5f5'),
    cfg('dleft', '←', 'btn', 302, 0.172, 0.700, S, '#f5f5f5'),
    cfg('dright', '→', 'btn', 303, 0.278, 0.700, S, '#f5f5f5'),
    // 上沿：左 ZL / L，右 R / ZR（Switch 的 ZL 在最外、L 靠里）
    cfg('zl', 'ZL', 'btn', 100, 0.060, 0.259, S, '#cfe8ff'),
    cfg('l', 'L', 'btn', 9, 0.123, 0.259, S, '#f5f5f5'),
    cfg('r', 'R', 'btn', 10, 0.843, 0.259, S, '#f5f5f5'),
    cfg('zr', 'ZR', 'btn', 101, 0.780, 0.259, S, '#cfe8ff'),
    // 面键（Switch 位）：A 在右 = 确认，B 在下 = 返回
    cfg('x', 'X', 'btn', 2, 0.845, 0.571, F, '#f5f5f5'),
    cfg('b', 'B', 'btn', 1, 0.845, 0.829, F, '#ff8a8a'),
    cfg('y', 'Y', 'btn', 3, 0.787, 0.700, F, '#f5f5f5'),
    cfg('a', 'A', 'btn', 0, 0.903, 0.700, F, '#8ee06e'),
    // 中上：－（地图）与 ＋（暂停/物品）
    cfg('minus', '－', 'btn', 4, 0.456, 0.157, S, '#f5f5f5'),
    cfg('plus', '＋', 'btn', 6, 0.544, 0.157, S, '#f5f5f5')
  ]
}

const PAD_LAYOUT_FILE = 'pad_layout.json'

/** 加载：没有存盘或存盘空 = 用默认布局（布局文件可以直接 hdc file send 覆盖，改位置不用重编） */
export function loadPadLayout(filesDir: string): PadBtnCfg[] {
  try {
    const path = filesDir + '/' + PAD_LAYOUT_FILE
    if (!fs.accessSync(path)) { return defaultSwitchLayout() }
    const parsed: PadBtnCfg[] = JSON.parse(fs.readTextSync(path)) as PadBtnCfg[]
    if (parsed === null || parsed.length === 0) { return defaultSwitchLayout() }
    return parsed
  } catch (e) {
    return defaultSwitchLayout()
  }
}

export function savePadLayout(filesDir: string, list: PadBtnCfg[]): boolean {
  try {
    const f = fs.openSync(filesDir + '/' + PAD_LAYOUT_FILE,
      fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC)
    fs.writeSync(f.fd, JSON.stringify(list))
    fs.closeSync(f)
    return true
  } catch (e) {
    return false
  }
}
