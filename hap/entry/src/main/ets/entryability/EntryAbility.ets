import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';

const DOMAIN = 0x0000;

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
    } catch (err) {
      hilog.error(DOMAIN, 'RAG1', 'Failed to set colorMode. Cause: %{public}s', JSON.stringify(err));
    }
    hilog.info(DOMAIN, 'RAG1', '%{public}s', 'Ability onCreate');
    // aa start --ps DM_xxx v 传进来的诊断开关，交给原生 Start 落进进程环境（见 app_ohos.c）
    try {
      AppStorage.setOrCreate('wantParams', JSON.stringify(want.parameters ?? {}));
      hilog.info(DOMAIN, 'RAG1', 'wantParams=%{public}s', JSON.stringify(want.parameters ?? {}));
    } catch (e) {
      hilog.error(DOMAIN, 'RAG1', 'wantParams failed: %{public}s', JSON.stringify(e));
    }
  }

  onDestroy(): void {
    hilog.info(DOMAIN, 'RAG1', '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(DOMAIN, 'RAG1', '%{public}s', 'Ability onWindowStageCreate');

    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'RAG1', 'Failed to load the content. Cause: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, 'RAG1', 'Succeeded in loading the content.');
    });

    try {
      const win = windowStage.getMainWindowSync();
      // Follow the sensor between the two landscape orientations
      // (must match module.json5; this runtime call overrides it).
      win.setPreferredOrientation(window.Orientation.LANDSCAPE);
      win.setWindowLayoutFullScreen(true);
      hilog.info(DOMAIN, 'RAG1', 'Auto-rotate landscape + fullscreen set');
    } catch (e) {
      hilog.error(DOMAIN, 'RAG1', 'Failed to set landscape/fullscreen. Cause: %{public}s', JSON.stringify(e));
    }
  }

  onWindowStageDestroy(): void {
    hilog.info(DOMAIN, 'RAG1', '%{public}s', 'Ability onWindowStageDestroy');
  }

  onForeground(): void {
    hilog.info(DOMAIN, 'RAG1', '%{public}s', 'Ability onForeground');
  }

  onBackground(): void {
    hilog.info(DOMAIN, 'RAG1', '%{public}s', 'Ability onBackground');
  }
}