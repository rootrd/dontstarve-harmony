import dmhost from 'libdmhost.so';
import fs from '@ohos.file.fs';
import { PadBtnCfg, loadPadLayout } from '../model/PadLayout';

// 改写产物全清单（rawfile 同名拷贝进沙箱）；.patch.log 不需要
// 饥荒 Don't Starve Together（Klei 自研引擎 + FMOD + Lua）：模块 main / sdk / subsdk0 / rtld
// Don't Starve Together (Klei engine + FMOD + Lua): modules main / sdk / subsdk0 / rtld
const FILES: string[] = [
  'main.patched.bin', 'sdk.patched.bin', 'subsdk0.patched.bin', 'rtld.patched.bin',
  'build_src.sha256',
  'main.cuts.tsv', 'main.fixups.tsv', 'main.gates.tsv', 'main.obs.tsv', 'main.params.tsv', 'main.sites.tsv',
  'sdk.cuts.tsv', 'sdk.fixups.tsv', 'sdk.gates.tsv', 'sdk.obs.tsv', 'sdk.params.tsv', 'sdk.sites.tsv',
  'subsdk0.cuts.tsv', 'subsdk0.fixups.tsv', 'subsdk0.gates.tsv', 'subsdk0.obs.tsv', 'subsdk0.params.tsv', 'subsdk0.sites.tsv',
  'rtld.cuts.tsv', 'rtld.fixups.tsv', 'rtld.gates.tsv', 'rtld.obs.tsv', 'rtld.params.tsv', 'rtld.sites.tsv',
];

@Entry
@Component
struct Index {
  private xcompController: XComponentController = new XComponentController();
  @State statusText: string = 'D2R 初始化…';
  // 状态字只在启动没走通时压屏（以前恒显，那行「镜像就绪 / dmhost.start=0」正好盖在游戏对话框中间）
  // The status line only goes on screen when startup did not work; it used to show always and sat right over the game's dialog
  @State showStatus: boolean = true;
  // 屏幕虚拟手柄（Switch）：布局来自 model/PadLayout.ets，沙箱里有存盘就用存盘
  // The on-screen Switch pad: layout from model/PadLayout.ets, a saved file in the sandbox wins
  @State padBtns: PadBtnCfg[] = [];
  @State showPad: boolean = true;
  @State padPressed: string[] = [];
  @State padW: number = 2848 / 3.4617;   // 窗口尺寸（vp），onAreaChange 里落实测；先给测试机 2848x1276 / 密度 3.4617 的估算值 | window size in vp, measured by onAreaChange; seeded with the test phone's numbers
  @State padH: number = 1276 / 3.4617;
  @State stickActive: boolean = false;
  @State stickKX: number = 0;
  @State stickKY: number = 0;
  // 软键盘（游戏 swkbd 小程序请求）：宿主把请求挂在这儿，页面轮 kbdPoll 取走 → 弹带 TextInput 的框 → kbdAnswer 回填
  // The software keyboard (the game's swkbd applet request): the host parks it there, the page polls kbdPoll, shows a TextInput dialog and hands the text back via kbdAnswer
  @State kbdOn: boolean = false;
  @State kbdText: string = '';
  @State kbdTitle: string = '';
  @State kbdGuide: string = '';
  @State kbdMax: number = 500;

  aboutToAppear(): void {
    // 不在这里碰 dmhost：libdmhost.so 的首次加载要由 XComponent(libraryname) 触发，
    // 这样模块注册函数里的 exports 才带得上 __NATIVE_XCOMPONENT_OBJ__（表面回调才接得住）。
    this.statusText = '等待表面…';
    this.padBtns = loadPadLayout(getContext(this).filesDir);
    // 100 ms 轮一次软键盘请求：这条路不需要即时送达（人在打字），省掉 threadsafe function 的 env 生命周期账
    // Poll the keyboard request every 100 ms: this path needs no instant delivery (a human is typing), which buys out the threadsafe-function env lifetime bookkeeping
    setInterval((): void => {
      if (this.kbdOn) return;
      let req: string = '';
      try { req = dmhost.kbdPoll() as string; } catch (e) { return; }   // 模块还没加载（游戏没起）：这一拍什么都不做 | module not loaded yet (the game is not up): this tick does nothing
      if (!req || req.length === 0) return;
      const f: string[] = req.split('\t');
      this.kbdText = f.length > 0 ? f[0] : '';
      this.kbdTitle = f.length > 1 ? f[1] : '';
      this.kbdGuide = f.length > 3 ? f[3] : '';
      this.kbdMax = f.length > 4 ? (parseInt(f[4], 10) || 500) : 500;
      this.kbdOn = true;
      console.info('D2R 软键盘弹出：初始「' + this.kbdText + '」上限 ' + this.kbdMax);
    }, 100);
  }

  private prepareAndStart(): void {
    try {
      const ctx = getContext(this);
      const filesDir: string = ctx.filesDir;
      let copied: number = 0;
      for (const m of FILES) {
        const dst: string = filesDir + '/' + m;
        if (fs.accessSync(dst)) continue;
        const data: Uint8Array = ctx.resourceManager.getRawFileContentSync(m);
        const f = fs.openSync(dst, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY);
        fs.writeSync(f.fd, data.buffer);
        fs.closeSync(f);
        copied++;
      }
      this.statusText = '镜像就绪（新拷 ' + copied + '/' + FILES.length + '）';
      console.info('D2R filesDir=' + filesDir + ' copied=' + copied);
      console.info('D2R resourceDir=[' + ctx.resourceDir + '] bundleCodeDir=[' + ctx.bundleCodeDir + ']');
      const ret: number = dmhost.start(filesDir, AppStorage.get<string>('wantParams') ?? '', ctx.resourceDir);
      this.statusText += ' / dmhost.start=' + ret;
      this.showStatus = ret !== 0;   // 起来了就撤字，画面交给游戏
      console.info('D2R ' + this.statusText);
    } catch (e) {
      this.statusText = '启动异常: ' + (e as Error).message;
      this.showStatus = true;
      console.error('D2R ' + this.statusText);
    }
  }

  private padKey(ev: KeyEvent): boolean {
    // 手柄键（十字 2012–2016、按键 2301–2315）交给宿主；不是手柄键（键盘 / 音量…）返回 false 让系统照常处理。1 = 抬起，其余 = 按下
    // Controller keys (D-pad 2012-2016, buttons 2301-2315) go to the host; non-controller keys (keyboard / volume...) return false so the system handles them as usual. 1 = up, anything else = pressed
    if (dmhost.padKey(ev.keyCode, ev.type === KeyType.Up ? 1 : 0) !== 1) return false;
    if (ev.type === KeyType.Down) console.info('D2R ' + dmhost.padDiag());   // 一行手柄诊断（原生侧同样有 LOG） | one controller diagnostic line (the host side logs it too)
    return true;
  }

  // ── 屏幕虚拟手柄（Switch 排法）──
  // 几何、触点记账与摇杆算法照 E:\StardewValley-HOS 的 pages/Index.ets（那份是 Xbox 键位，这里换成 Switch）；
  // 注入走本工程的 dmhost.padBtn / padAxis（那边是 SDL 虚拟手柄）。元素各自带 onTouch，空白处照旧穿透给游戏。
  // On-screen pad, Switch layout: geometry, touch bookkeeping and the stick math follow the Stardew project's Index.ets (that one is Xbox, this one is Switch);
  // injection goes through dmhost.padBtn / padAxis instead of SDL's virtual gamepad. Each element takes its own touch; empty space still falls through to the game.
  private padTouchMap: Map<number, string> = new Map();   // touchId → 元素 id：组件不保证收到 Up，根节点 Up 按号兜底松开 | a component is not guaranteed to get Up, so the root's Up releases by id
  private stickId: number = -1;                           // 正在推摇杆的手指（只认第一根） | the finger pushing the stick (only the first counts)
  private stickActId: string = '';
  private stickVX: number = 0;
  private stickVY: number = 0;

  private padRadiusVp(b: PadBtnCfg): number { return b.size * Math.min(this.padW, this.padH); }
  private padDiameterVp(b: PadBtnCfg): number { return this.padRadiusVp(b) * 2; }
  private padCenterXv(b: PadBtnCfg): number {
    const r = this.padRadiusVp(b);
    return Math.max(r, Math.min(this.padW - r, b.x * this.padW));   // 钳进屏幕，贴边也不出界 | clamp so an edge button never leaves the screen
  }
  private padCenterYv(b: PadBtnCfg): number {
    const r = this.padRadiusVp(b);
    return Math.max(r, Math.min(this.padH - r, b.y * this.padH));
  }
  private padById(id: string): PadBtnCfg | null {
    for (const b of this.padBtns) { if (b.id === id) { return b } }
    return null;
  }
  private padIsDown(id: string): boolean { return this.padPressed.indexOf(id) >= 0; }
  private padSetDown(id: string, down: boolean): void {
    if (down) {
      if (!this.padIsDown(id)) { this.padPressed = this.padPressed.concat([id]); }
    } else {
      this.padPressed = this.padPressed.filter((v: string): boolean => v !== id);
    }
  }
  /** gp → 游戏键（摇杆不走这条，见 stickApply） | gp -> game button (the stick does not come through here, see stickApply) */
  private padInject(b: PadBtnCfg, down: boolean): void {
    if (b.kind === 'stick') { if (!down) { this.stickRelease(); } return; }
    dmhost.padBtn(b.gp, down ? 1 : 0);
  }
  /** 摇杆求值：死区 0.10、区外线性重标定、量化 0.02 少跨语言调用；Y 取负（宿主按 Npad 语义「上为正」，屏幕 dy 朝下） */
  private stickApply(b: PadBtnCfg, wx: number, wy: number): void {
    const R = this.padRadiusVp(b);
    let dx = wx - this.padCenterXv(b);
    let dy = wy - this.padCenterYv(b);
    let m = Math.sqrt(dx * dx + dy * dy);
    if (m > R) { dx = dx / m * R; dy = dy / m * R; m = R; }   // 超出底盘钳到边缘，杆头跟着贴边 | clamp to the base edge and move the knob with it
    this.stickKX = dx * 0.62;
    this.stickKY = dy * 0.62;
    const dz = 0.10;
    const u = R > 0 ? m / R : 0;
    const out = u <= dz ? 0 : (u - dz) / (1 - dz);
    const q = (v: number): number => Math.round(v * 50) / 50;
    let vx = 0, vy = 0;
    if (out > 0 && m > 0) { vx = q(dx / m * out); vy = q(dy / m * out); }
    if (vx === this.stickVX && vy === this.stickVY) { return; }
    this.stickVX = vx; this.stickVY = vy;
    dmhost.padAxis(0, vx, -vy);
  }
  private stickRelease(): void {
    if (this.stickId < 0 && !this.stickActive) { return; }
    this.stickId = -1;
    this.stickActId = '';
    this.stickActive = false;
    this.stickKX = 0;
    this.stickKY = 0;
    this.stickVX = 0;
    this.stickVY = 0;
    dmhost.padAxis(0, 0, 0);   // 松手必回中，不然角色一直朝那个方向走 | always re-centre, otherwise the character keeps walking
  }
  private padElementTouch(b: PadBtnCfg, ev: TouchEvent): void {
    if (ev.changedTouches.length === 0) { return; }
    if (ev.type === TouchType.Down) {
      const t: TouchObject = ev.changedTouches[0];
      if (b.kind === 'stick') {
        if (this.stickId >= 0) { return; }
        this.stickId = t.id;
        this.stickActId = b.id;
        this.stickActive = true;
        this.stickApply(b, t.windowX, t.windowY);
      } else {
        this.padTouchMap.set(t.id, b.id);
        this.padSetDown(b.id, true);
        this.padInject(b, true);
      }
      return;
    }
    if (ev.type === TouchType.Move) {
      if (b.kind === 'stick' && this.stickId >= 0 && this.stickActId === b.id) {
        for (const tt of ev.touches) { if (tt.id === this.stickId) { this.stickApply(b, tt.windowX, tt.windowY); } }
      }
      return;
    }
    for (const t of ev.changedTouches) { this.padRootUp(t.id); }   // Up / Cancel
  }
  /** 元素自身与根节点的收尾都走这里：按 touchId 精确松，多指互不干扰 | both the element and the root finish here: release exactly by touchId, so multi-finger presses never cross-talk */
  private padRootUp(touchId: number): void {
    const id: string | undefined = this.padTouchMap.get(touchId);
    if (id !== undefined) {
      this.padTouchMap.delete(touchId);
      const b: PadBtnCfg | null = this.padById(id);
      this.padSetDown(id, false);
      if (b !== null) { this.padInject(b, false); }
    }
    if (touchId === this.stickId) { this.stickRelease(); }
  }

  @Builder PadVisualCenter(b: PadBtnCfg) {
    if (b.kind === 'stick') {
      // 不用 Circle().fill/.stroke：那两个属性 API 标着 26.0.0 起，而本工程 compatible SDK 是 6.1.1(24)，
      // 编译只给警告、真机上可能整个不画。圆角容器 + border 同款外观，走的是老 API。
      // Circle().fill/.stroke are marked as since-26 APIs while this project targets 6.1.1(24): the build only warns and the shape may never paint on the device. Rounded containers with a border look the same and use the old APIs.
      Stack({ alignContent: Alignment.Center }) {
        Stack({ alignContent: Alignment.Center }) {
          Stack()
            .width(this.padDiameterVp(b) * 0.44)
            .height(this.padDiameterVp(b) * 0.44)
            .borderRadius(this.padDiameterVp(b) * 0.22)
            .backgroundColor(this.stickActive ? '#99f0e0a0' : '#77202028')
            .border({ width: 2, color: '#99ffffff' })
            .offset({ x: this.stickKX, y: this.stickKY })
        }
        .width(this.padDiameterVp(b))
        .height(this.padDiameterVp(b))
        .borderRadius(this.padDiameterVp(b) / 2)
        .backgroundColor('#3a202028')
        .border({ width: 2, color: '#55ffffff' })
      }
    } else {
      Text(b.label)
        .fontSize(this.padDiameterVp(b) * 0.32)
        .fontWeight(FontWeight.Bold)
        .fontColor(b.color)
        .textAlign(TextAlign.Center)
        .width(this.padDiameterVp(b))
        .height(this.padDiameterVp(b))
        .borderRadius(this.padDiameterVp(b) / 2)
        .backgroundColor(this.padIsDown(b.id) ? '#aa3a3a46' : '#66202028')
        .borderWidth(1)
        .borderColor('#73ffffff')
    }
  }

  @Builder PadViewAbs(b: PadBtnCfg) {
    Stack({ alignContent: Alignment.Center }) {
      this.PadVisualCenter(b)
    }
      .width(this.padDiameterVp(b) + 4)
      .height(this.padDiameterVp(b) + 4)
      .position({ x: this.padCenterXv(b) - this.padDiameterVp(b) / 2 - 2,
                  y: this.padCenterYv(b) - this.padDiameterVp(b) / 2 - 2 })
      .onTouch((ev: TouchEvent): void => { this.padElementTouch(b, ev); })
  }

  private padTouch(ev: TouchEvent): void {
    // 触屏当十字键：按下点是原点，手指挪出去的方向 = 十字键（原生里算，见 pad_ohos.c）；动作号 0 按下 / 1 抬起 / 2 移动 / 3 取消
    // Touch as D-pad: the press point is the origin, the direction the finger moves out = the D-pad key (computed natively, see pad_ohos.c); action codes 0 down / 1 up / 2 move / 3 cancel
    const t: TouchObject | undefined = ev.touches.length > 0 ? ev.touches[0] : ev.changedTouches[0];
    if (t === undefined) return;
    const act: number = ev.type === TouchType.Up ? 1 : ev.type === TouchType.Cancel ? 3 : ev.type === TouchType.Move ? 2 : 0;
    dmhost.padTouch(act, t.x, t.y, ev.target.area.width as number, ev.target.area.height as number);
  }

  build() {
    Stack() {
      XComponent({
        id: 'd2r_surface',
        type: XComponentType.SURFACE,
        libraryname: 'dmhost',
        controller: this.xcompController
      })
        .onLoad(() => {
          const sid: string = this.xcompController.getXComponentSurfaceId();
          const rect = this.xcompController.getXComponentSurfaceRect();
          console.info('D2R surface ' + rect.surfaceWidth + 'x' + rect.surfaceHeight + ' sid=' + sid);
          // 表面已建（原生侧 OnSurfaceCreated 已跑）→ 再首次触碰 dmhost，保证 XComponent 先加载模块
          this.prepareAndStart();
        })
        .width('100%')
        .height('100%')
        .onTouch((ev: TouchEvent): void => { this.padTouch(ev); })   // 触屏当十字键 | touch as D-pad

      if (this.showStatus) {   // 贴左上角，别再压在游戏画面中间
        Text(this.statusText)
          .fontSize(12)
          .fontColor('#FF7A66')
          .position({ x: 12, y: 12 })
      }

      if (this.showPad) {
        ForEach(this.padBtns, (b: PadBtnCfg): void => { this.PadViewAbs(b) },
          (b: PadBtnCfg): string => b.id)
      }
      // 手柄开关常驻（收起来以后还得点得回来）
      Text(this.showPad ? '⊙' : '◎')
        .fontSize(20)
        .fontColor('#88ffffff')
        .position({ x: 6, y: 36 })
        .onTouch((ev: TouchEvent): void => { if (ev.type === TouchType.Up) { this.showPad = !this.showPad; } })

      if (this.kbdOn) {   // 游戏要软键盘：Switch 的小程序在这里换成一个真输入框（系统输入法）| the game asked for the applet keyboard: the Switch applet becomes a real input box (system IME)
        Column() {
          Text(this.kbdTitle.length > 0 ? this.kbdTitle : '输入文本')
            .fontSize(15).fontColor('#FFE0B2').margin({ bottom: 6 })
          TextInput({ text: this.kbdText, placeholder: this.kbdGuide })
            .width('70%').fontSize(16).defaultFocus(true)
            .maxLength(this.kbdMax > 0 ? this.kbdMax : 500)
            .onChange((v: string): void => { this.kbdText = v; })
            .onSubmit((): void => { dmhost.kbdAnswer(this.kbdText, 1); this.kbdOn = false; })   // 输入法那行的「确定 / 回车」直接交回 | the IME's own enter hands the text straight back
          Row({ space: 14 }) {
            Button('取消').backgroundColor('#4a3a28').fontColor('#e8d9bd')
              .onClick((): void => { dmhost.kbdAnswer(this.kbdText, 0); this.kbdOn = false; })
            Button('确定').backgroundColor('#8a5a22').fontColor('#fff3dd')
              .onClick((): void => { dmhost.kbdAnswer(this.kbdText, 1); this.kbdOn = false; })
          }.margin({ top: 10 })
        }
        .padding(14)
        .backgroundColor('#e6241a10')
        .borderRadius(10)
        .width('100%')
        .height('100%')
        .justifyContent(FlexAlign.Center)
        .alignItems(HorizontalAlign.Center)
      }
    }
    .onKeyEventDispatch((ev: KeyEvent): boolean => this.padKey(ev))   // 页面级按键分发：不挑焦点，蓝牙手柄的键都从这来 | page-level key dispatch: no focus needed, the Bluetooth controller's keys all arrive here
    .onAreaChange((_o: Area, n: Area): void => { this.padW = n.width as number; this.padH = n.height as number; })
    .onTouch((ev: TouchEvent): void => {   // 兜底：组件不保证收到 Up，按 touchId 精确松开 | fallback: a component may never get Up, so release precisely by touchId
      if (ev.type === TouchType.Up || ev.type === TouchType.Cancel) {
        for (const t of ev.changedTouches) { this.padRootUp(t.id); }
      }
    })
    .width('100%')
    .height('100%')
  }
}
