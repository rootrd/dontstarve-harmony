export const start: (dir?: string, wantParams?: string, resourceDir?: string) => number;
// 手柄 / 触屏进 libdmhost.so 的四个入口（实现见 dm/ohos/pad_ohos.c）
// The four controller / touch entries into libdmhost.so (implementation in dm/ohos/pad_ohos.c)
export const padKey: (code: number, type: number) => number;      // 1 = 这个键归手柄（页面据此消费）| 1 = the key belongs to the controller (the page consumes it accordingly)
export const padAxis: (stick: number, x: number, y: number) => void;   // 0 左摇杆 / 1 右摇杆，[-1,1] 上为正 | 0 left stick / 1 right stick, [-1,1] up positive
export const padTouch: (action: number, x: number, y: number, w: number, h: number) => void;   // TouchType + 触点与组件尺寸 | TouchType plus the point and the component size
export const padDiag: () => string;
// 屏幕虚拟手柄：按下/松开某一颗游戏键（gp 编号见 ets/model/PadLayout.ets，与星露谷那份工程同一套约定）
// On-screen pad: press / release one game button (gp numbering in ets/model/PadLayout.ets, same convention as the Stardew project)
export const padBtn: (gp: number, down: number) => number;

