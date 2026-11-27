# DreamSFX

DreamSFX 是一个面向 Unreal Engine MetaSound 的程序化声音插件。它让 AI 把自然语言描述写成可编辑的声音配方，配方用 `.dsfx` 领域专用语言（DSL）保存。

## 基本思路

1. 用自然语言描述想要的声音，比如「一个柔和短促的菜单确认音，音高向上跳一次」。
2. AI 把描述写成 `.dsfx` 文件。文本定义发声方式、音高、音量和事件时序。
3. DreamSFX 编译文本，在 Unreal Editor 中试听，也可以生成供游戏使用的 MetaSound Source。想改声音时，调整文本再试听。

下面的预设用两个正弦波音符做一声上行的确认音。

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

[确认音源码](Resources/Presets/UI/sine-confirm-soft.dsfx) · [更多 DSL 示例](Resources/Presets/)

## 在 UE 中使用

在 Unreal Engine 5.8 工程的 `Plugins/` 下并排安装 `DreamSFX` 和 `EightBitMetaSound`，启用 MetaSound 和 DreamSFX。打开 **Window → DreamSFX Sounds**，选择 **Factory presets**，双击声音试听。

声音实验室已将两个插件配置为 Git 子模块，可以递归克隆工程直接使用。 [SoundLab 宿主工程](https://github.com/kisspread/SoundLab) · [快速入门](https://github.com/kisspread/SoundLab/blob/main/Docs/DreamSFX-QuickStart.md)
