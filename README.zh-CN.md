# DreamSFX

**用文本为 Unreal Engine 5.8 写程序化声音。**

一份 `.dsfx` 可以是菜单里两个音符的确认声，可以是钢琴和八音盒的合奏，也可以是按住按钮不断蓄力的法术。用文本编辑器写音符、信号链和触发规则，在 UE 里试听，再生成一个立体声 MetaSound Source。蓝图可以直接播放它，也可以用文件里起的参数名控制它。

[English](README.md) · [83 个内置预设](Resources/Presets/) · [DSL 语言参考](https://github.com/kisspread/SoundLab/blob/main/Docs/TextFormat.md) · [SoundLab 演示工程](https://github.com/kisspread/SoundLab)

## 一个声音长什么样

[听这声菜单确认音 · WAV](Docs/Audio/soft-confirm.wav) · [查看预设源码](Resources/Presets/UI/sine-confirm-soft.dsfx)

```text
sound benchmark.sine_confirm_soft
preview .125 s
tail .09 s
input transposeBy range -12 12 default 0
patch bell { tone=voice.sine(gainDb=-26,voiceLimit=4) }
track chime uses bell {
  tempo 180; velocity .7; gate .7; transpose transposeBy
  C5/32 G5/16
}
output chime.tone
```

正弦波先奏 C5，再奏 G5。想让上行的跨度小一点，把 `G5` 改成 `E5` 就能听出区别。想保留音程、把两个音一起移低，可以在音频组件上设置 `transposeBy`。另一个菜单要用低音版，也可以把这个值存进文本实例。

## 快速上手

1. 递归克隆演示工程和两个插件，用 UE 5.8 编译 **SoundLabEditor**，打开 `SoundLab.uproject`。

   ```sh
   git clone --recurse-submodules https://github.com/kisspread/SoundLab.git
   ```

   如果用自己的 UE 5.8 工程，把 **DreamSFX** 和 **EightBitMetaSound** 并排放在 `Plugins/` 下，启用 DreamSFX 和 MetaSound，再编译工程的 Editor 目标。EightBitMetaSound 提供复古合成器。[EightBitMetaSound 仓库](https://github.com/kisspread/EightBitMetaSound)

2. 打开 **Window → DreamSFX Sounds → Factory presets**，在 **UI** 分类下双击确认音，听声音、看波形。**Preview volume** 只调节试听音量。
3. 要做低音版，选中确认音，填好实例名，点 **Create project instance**。勾选 `transposeBy`，设为 `-5`，再点 **Save instance**。要从头写声音，就给 UTF-8 的 `.dsfx` 文件取一个唯一的声音 ID，放进工程的 `DSFX/`，然后选 **Project sounds**。
4. 点 **Build MetaSound asset**，到 `Content/DreamSFX/Graphs/` 找到生成的 Source。把它赋给 Audio Component，调用 **Play** 即可。打开资产，画布上能看到一个 **DreamSFX Graph Player**，连着公开参数、立体声输出和播放完成引脚。乐器和效果链在源文本里编辑。

## 可以从哪些声音开始

| 声音 | 值得听的地方 | 源文件 |
| --- | --- | --- |
| 复古方波 | 改 `dutyCycle`，每个周期里的脉冲宽度随之改变，同一个音的音色也会变。 | [复古方波](Resources/Presets/Retro/retro-square.dsfx) |
| 钢琴与八音盒 | 两轨共用一段乐句。钢琴晚半秒进入，低七个半音，走 3.2 秒的大厅混响。八音盒有独立的 1.2 秒小房间。 | [提灯二重奏](Resources/Presets/MusicBox/lantern-duet.dsfx) |
| 从白昼到黄昏 | 改 `worldZ` 听两层铺底音交叉淡入淡出，改 `airSpeed` 听迎面风声增强。单独试听风层的实例只需两行。 | [完整声景](Resources/Presets/Duskward/Scenes/tour.dsfx) · [风层实例](Resources/Presets/Duskward/Layers/wind.dsfx) |
| 蓄力爆炸 | 从符文、下落、冲击波和主爆，一直听到耳鸣。默认试听用 19 秒走完整个过程，游戏也能接管同一组输入。 | [完整法术](Resources/Presets/Magic/blastFull.dsfx) |

## 和 AI 一起写，用命令行试听

把听感要求和最接近的预设交给 AI。拿上面的确认音来说，可以只提一句“保留第一个音，把第二个音降到 E5，音量照旧”，然后直接看 `.dsfx` 的 diff。`DreamSFXGraphAssets` 可以编译这份文本、导出试听 WAV，并在 JSON 报告中给出诊断码、文件路径和可定位的行列号。

在 PowerShell 中把 `UE_ROOT` 环境变量设为引擎安装目录（包含 `Engine/` 的那一层）。从 SoundLab 根目录执行下面的命令，会校验并渲染上面的确认音。

```powershell
$editor = Join-Path $env:UE_ROOT 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$project = (Resolve-Path './SoundLab.uproject').Path
$presets = (Resolve-Path './Plugins/DreamSFX/Resources/Presets').Path
$out = Join-Path (Get-Location) 'Saved/DreamSFX/ReadmeAudition'
& $editor $project -run=DreamSFXGraphAssets "-SourceDir=$presets" `
    '-Id=benchmark.sine_confirm_soft' -ValidateOnly "-RenderDir=$out" `
    "-Report=$out/report.json" -unattended -nullrhi -nosound -nop4
```

到 `Saved/DreamSFX/ReadmeAudition/` 打开 WAV。同目录的 JSON 报告记录了输入范围、时长、峰值、RMS 和过载样本数。试听文件由 DreamSFXCore 以 48 kHz 立体声渲染。去掉 `-ValidateOnly` 会同时写入 MetaSound 资产。去掉 `-Id` 则处理整个源目录。

## 插件结构

| 模块 | 职责 |
| --- | --- |
| [DreamSFXCore](Source/DreamSFXCore/) | 编译 DSL，调度音符和规则，渲染立体声音频。 |
| [DreamSFXRuntime](Source/DreamSFXRuntime/) | 在生成的资产中保存声音程序，播放时加载。 |
| [DreamSFXMetaSound](Source/DreamSFXMetaSound/) | 提供 Graph Player，把 UE 的触发器和参数接入正在运行的声音。 |
| [DreamSFXEditor](Source/DreamSFXEditor/) | 提供预设浏览、波形试听、实例调参、资产生成和批处理命令。 |

DreamSFX **2.0.0 beta** 已在 **Win64 / UE 5.8** 上编译并运行验证。创作指南和语言规范随 SoundLab 工程维护。[创作指南](https://github.com/kisspread/SoundLab/blob/main/Docs/DreamSFX-QuickStart.md) · [DSL 语法与节点](https://github.com/kisspread/SoundLab/blob/main/Docs/TextFormat.md)
